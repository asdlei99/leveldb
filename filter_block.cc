#include "filter_block.h"
#include "filter_policy.h"
#include "coding.h"

namespace leveldb{

static const size_t kFilterBaseLg = 11; 
static const size_t kFilterBase = 1 << kFilterBaseLg; //2KB

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* p) : policy_(p)
{
}

void FilterBlockBuilder::StartBlock(uint64_t block_offset)
{
	uint64_t filter_index = (block_offset / kFilterBase);
	assert(filter_index >= filter_offsets_.size());
	while(filter_index > filter_offsets_.size()){ //ÿ2K����һ��filter
		GenerateFilter();
	}
}

void FilterBlockBuilder::AddKey(const Slice& key)
{
	Slice k = key;
	start_.push_back(keys_.size());
	keys_.append(k.data(), k.size());
}
/* �洢�ṹ��|filter1|filter2|...|filtern|filter size array|result size|Base log flag|*/
//�����������뵽result_���в���ΪSlice����
Slice FilterBlockBuilder::Finish()
{
	if(!start_.empty()) //����KEY�Ĺ���������
		GenerateFilter();

	const uint32_t array_offset = result_.size();
	for(size_t i = 0; i < filter_offsets_.size(); i++){ //������filter��ƫ����д��result_����
		PutFixed32(&result_, filter_offsets_[i]);
	}

	PutFixed32(&result_, array_offset); //�����ձ�����ݳ���д��result����,4�ֽ�
	result_.push_back(kFilterBaseLg); //1�ֽ�

	return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter()
{
	//����KEY�ĸ���
	const size_t num_keys = start_.size();
	if(num_keys == 0){
		filter_offsets_.push_back(result_.size()); 
		return;
	}
	//�����һ��key�ĳ��� + ԭ���ܵĳ���ѹ�뵽start_����󣬱��Ժ���for�ļ���
	start_.push_back(keys_.size());

	tmp_keys_.resize(num_keys);
	for(size_t i = 0; i < num_keys; i ++){
		const char* base = keys_.data() + start_[i];
		size_t length = start_[i + 1] - start_[i];
		tmp_keys_[i] = Slice(base, length); //����KEYS,����key����tmp_keys_����
	}

	filter_offsets_.push_back(result_.size());
	policy_->CreateFilter(&tmp_keys_[0], num_keys, &result_); //����һ�����������ձ�

	//��չ������ձ�Ĳ���
	tmp_keys_.clear();
	keys_.clear();
	start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy, const Slice& contents)
: policy_(policy), data_(NULL), offset_(NULL), num_(0), base_lg_(0)
{
	size_t n = contents.size();
	if(n < 5) //һ��������filters�ı����ĳ���4�ֽڣ�����һ����kFilterBaseLg��1�ֽ�
		return ;

	base_lg_ = contents[n - 1];
	uint32_t last_word = DecodeFixed32(contents.data() + n - 5); //����filters ���ձ�ӽ�βλ��
	if(last_word > n - 5) 
		return ;

	data_ = contents.data();
	offset_ = data_ + last_word;					//��ÿ��index��ʼ��λ��
	num_ = (n - 5 - last_word) / sizeof(uint32_t);	//����filter�ĸ���(��ӦFilterBlockBuilder::filter_offsets_)
}

//����������
bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key)
{
	uint64_t index = block_offset >> base_lg_;
	if(index < num_){
		uint32_t start = DecodeFixed32(offset_ + index * 4);     //filter�Ŀ�ʼλ��
		uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4); //filter�Ľ���λ��(��ʵ������һ��filter�Ŀ�ʼλ��)
		if(start <= limit && limit <=(offset_ - data_)){		//ȡ����������key���й���
			Slice filter = Slice(data_ + start, limit - start);
			return policy_->KeyMayMatch(key, filter);
		}
		else if(start == limit)
			return false;
	}

	return true;
}


}//leveldb

