#include "block.h"
#include <vector>
#include <algorithm>
#include "comparator.h"
#include "format.h"
#include "coding.h"
#include "logging.h"
#include "slice.h" 

namespace leveldb{

inline uint32_t Block::NumRestarts() const
{
	assert(size >= sizeof(uint32_t));
	return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents)
	: data_(contents.data.data()), size_(contents.data.size()), owned_(contents.heap_allocated)
{
	if(size_ < sizeof(uint32_t))
		size_ = 0;
	else {
		size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
		if(NumRestarts() > max_restarts_allowed)
			size_ = 0;
		else
			restart_offset_ = size_ - ((1 + NumRestarts()) * sizeof(uint32_t));
	}
}

Block::~Block()
{
	if(owned_)
		delete[] data_;
}

static const char* DecodeEntry(const char* p, cosnt char* limit, 
	uint32_t* shared, uint32_t* non_shared, uint32_t* value_length)
{
	if(limit - p < 3)
		return NULL;

	//��̬�洢��shared non_shared length,7λ�����洢��
	*shared = reinterpret_cast<const unsigned char*>(p)[0];
	*non_shared = reinterpret_cast<const unsigned char*>(p)[1];
	*value_length = reinterpret_cast<const unsigned char*>(p)[2];
	if((*shared | *non_shared | *value_length) < 128){
		p += 3; 
	}
	else {
		if ((p = GetVarint32Ptr(p, limit, shared)) == NULL) return NULL;
		if ((p = GetVarint32Ptr(p, limit, non_shared)) == NULL) return NULL;
		if ((p = GetVarint32Ptr(p, limit, value_length)) == NULL) return NULL;
	}
	//�Ϸ����жϣ�key + value�ĳ���һ��С�ڻ����entry���ĳ��ȣ�limit - p��
	if(static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)){
		return NULL;
	}

	return p;
}

class Block::Iter : public Iterator
{

private:
	const Comparator* const comparator_;		//key�Ƚ���
	const char* const data_;					//block���ݾ��
	uint32_t const restarts_;					//block restartƫ����
	uint32_t const num_restarts_;				//entries������

	uint32_t current_;							//��ǰָ��entry��λ��
	uint32_t restart_index_;					//��ǰblock��restart index��λ��
	std::string key_;							//��ǰentry��key
	Slice value_;								//��ǰentry��value
	Status status_;								//�ϴβ����Ĵ�����

private:
	inline int Compare(const Slice& a, const Slice& b) const 
	{
		return comparator_->Compare(a, b);
	}

	uint32_t NextEntryOffset() const
	{
		return (value_.data() + value_.size()) - data_;
	}

	uint32_t GetRestartPoint(uint32_t index)
	{
		assert(index < num_restarts_);
		return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
	}

	void SeekToRestartPoint(uint32_t index)
	{
		key_.clear();
		restart_index_ = index;

		uint32_t offset = GetRestartPoint(index); //����restarts��λ��
		value_ = Slice((char*)(data_ + offset), 0);
	}

public:
	Iter(const Comparator* comparator, const char* data, uint32_t restarts, uint32_t num_restarts)
		: comparator_(comparator), data_(data), restarts_(restarts), num_restarts_(num_restarts),
		  current_(restarts), restart_index_(num_restarts_)
	{
		assert(num_restarts_ > 0);
	}

	virtual bool Valid() const {return current_ < restarts_;}
	virtual Status status() const {return status_;}

	virtual Slice key() const
	{
		assert(Valid());
		return key_;
	}

	virtual Slice value() const 
    {
		assert(Valid());
		return value_;
	}

	virtual Next()
	{
		assert(Valid());
		ParseNextKey();
	}

	virtual void Prev()
	{
		assert(Valid());
		const uint32_t original = current_;
		while(GetRestartPoint() >= original){
			if(restart_index_ == 0){ //�ص���ʼλ��
				current_ = restarts_;
				restart_index_ = num_restarts_;
				return;
			}
			restart_index_ --;
		}
		
		//SEEK��restart_index_ָ���entryλ��
		SeekToRestartPoint(restart_index_);
		do{
		}while(ParseNextKey() && NextEntryOffset() < original);
	}

	//��λtarget��ΪKEY��block��λ��
	virtual void Seek(const Slice& target)
	{
		uint32_t left = 0;
		uint32_t rigtht = num_restarts_ - 1;
		//2�ֲ��ҷ���λĿ���λ��(index)
		while(left < right){
			uint32_t mid = (left + right + 1) / 2;
			//��λentryƫ����
			uint32_t region_offset = GetRestartPoint(mid);
			uint32_t shared, non_shared, value_length;
			//���KEY��ָ��
			const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_, &shared, &non_shared, &value_length);
			if(key_ptr == NULL || (shared != 0)){
				CorruptionError(); //����һ������
				return;
			}

			//����KEY�Ƚ�
			Slice mid_key(key_ptr, non_shared);
			if(Compare(mid_key, target) < 0)
				left = mid;
			else
				right = mid  - 1;
		}

		//��λ��ƥ�䵽��λ��
		SeekToRestartPoint(left);
		//����ƫ������
		while(true){
			if (!ParseNextKey())
				return;
			if (Compare(key_, target) >= 0)
				return;
		}
	}

	virtual void SeekToFirst()
	{
		SeekToRestartPoint(0); //��λ����ʼ
		ParseNextKey();
	}

	virtual void SeekToLast()
	{
		SeekToRestartPoint(num_restarts_ - 1); //��λ�����һ��entry
		while(ParseNextKey() && NextEntryOffset() < restarts_){
		}
	}

	void CorruptionError()
	{
		current_ = restart_;
		restart_index_ = num_restarts_;
		status_ = Status("bad entry in block");
		key_.clear();
		value_.clear();
	}

	bool ParseNextKey()
	{
		current_ = NextEntryOffset();
		const char* p = data_ + current_;
		const char* limit = data_ + restarts_;
		if(p > limit){
			current_ = restarts_;
			restart_index_ = num_restarts_;
		}

		//���entry key��ptr�����ݲ���
		uint32_t shared, non_shared, value_length;
		p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
		if(p == NULL || key_.size() < shared){
			CorruptionError();
			return false;
		}
		else{
			//����KEY��value
			key_.resize(shared);
			key_.append(p, non_shared);
			value_ = Slice(p + non_shared, value_length);
			while(restart_index_ + 1 < num_restarts_ && GetRestartPoint(restart_index_ + 1) < current_)
				++ restart_index_;

			return true;
		}
	}
};

//����һ��block::iter
Iterator* Block::NewIterator(const Comparator* cmp)
{
	if(size_ < sizeof(uint32_t))
		return NewErrorIterator(Status::Corruption("bad block contents"));

	const uint32_t num_restarts = NumRestarts();
	if(num_restarts == 0)
		return NewEmptyIterator();
	else
		return new Iter(cmp, data_, restart_offset_, num_restarts);
}
};


