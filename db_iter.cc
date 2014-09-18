#include "db_iter.h"
#include "filename.h"
#include "dbformat.h"
#include "env.h"
#include "iterator.h"
#include "port.h"
#include "logging.h"
#include "mutexlock.h"
#include "random.h"

namespace leveldb{

#if 0
static void DumpInternalIter(Iterator* iter) 
{
	for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
		ParsedInternalKey k;
		if (!ParseInternalKey(iter->key(), &k)) {
			fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
		} else {
			fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
		}
	}
}

#endif

namespace {

class DBIter : public Iterator
{
public:
	enum Direction
	{
		kForward,
		kReverse
	};

	DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s, uint32_t seed)
		: db_(db), user_comparator_(cmp), iter_(iter), sequence_(s),
		direction_(kForward), rnd_(seed), bytes_counter_(RandomPeriod())
	{
	}

	virtual ~DBIter()
	{
		delete iter_;
	}

	virtual bool Valid() const {return valid_;};
	virtual Slice key() const
	{
		assert(valid_);
		return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
	};

	virtual Slice value() const
	{
		assert(valid_);
		return (direction_ == kForward) ? iter_->value() : saved_value_;
	};


	virtual Status status() const
	{
		if(status_.ok())
			return iter_->status();
		else 
			return status_;
	}

	virtual void Next();
	virtual void Prev();
	virtual void Seek(const Slice& target);
	virtual void SeekToFirst();
	virtual void SeekToLast();

private:
	DBIter(const DBIter&);
	void operator=(const DBIter&);

	void FindNextUserEntry(bool skipping, std::string* skip);
	void FindPrevUserEntry();
	bool ParseKey(ParsedInternalKey* key);

	inline void SaveKey(const Slice& k, std::string* dst)
	{
		dst->assign(k.data(), k.size());
	}

	inline void ClearSavedValue()
	{
		if(saved_value_.capacity() > 1048576){ //1M���ݣ�����swap
			std::string empty;
			swap(empty, saved_value_); //Ч�ʸ��ߣ��ڲ���û���ͷ��ڴ棬ֻ�������������㣬swapʹ��xor��������
		}
		else
			saved_value_.clear();
	}

	ssize_t RandomPeriod()
	{
		return rnd_.Uniform(2 * config::kReadBytesPeriod);
	}

private:
	DBImpl* db_;
	const Comparator* const user_comparator_;
	Iterator* const iter_;
	SequenceNumber const sequence_;

	Status status_;
	std::string saved_key_;
	std::string saved_value_;
	Direction direction_;

	bool valid_;
	Random rnd_;
	ssize_t bytes_counter_;
};

inline bool DBIter::ParseKey(ParsedInternalKey* ikey)
{
	Slice k = iter_->key();
	ssize_t n = k.size() + iter_->value().size();
	bytes_counter_ -= n;

	//��Ϊÿ�δӴ����ж�ȡ1M�������൱һ��io seek,����û��ȡ1M�������У�����һ�ζ�version��seek�������жϣ����ݵ�ǰ��key���ж�
	while(bytes_counter_ < 0){
		bytes_counter_ += RandomPeriod(); //�������һ��1 ~ 2M������ƽ����1M
		db_->RecordReadSample(k);
	}

	//��key������ikey����
	if(!ParseInternalKey(k, ikey))
		return false;
	else 
		return true;
}

void DBIter::Next()
{
	assert(valid_);

	if(direction_ == kReverse){
		direction_ = kForward;
		if(!iter_->Valid())
			iter_->SeekToFirst();
		else
			iter_->Next();

		if(!iter_->Valid()){
			valid_ = false;
			saved_key_.clear();
			return ;
		}
	}
	else
		SaveKey(ExtractUserKey(iter_->key()), &saved_key_);

	FindNextUserEntry(true, & saved_key_);
}

void DBIter::FindNextUserEntry(bool skipping, std::string* skip)
{
	assert(iter_->Valid());
	assert(direction_ == kForward);
	do{
		ParsedInternalKey ikey;
		if(ParseKey(&ikey) && ikey.sequence <= sequence_){ //��KEYת��Ϊuser key�����ж�sequence
			switch(ikey.type){
			case kTypeDeletion: //ɾ����־
				SaveKey(ikey.user_key, skip); //����user_key��skip��
				skipping = true;
				break;

				//��ѯ����ֵ��Ч
			case kTypeValue:
				if(skipping && user_comparator_->Compare(ikey.user_key, *skip) <= 0){ //������������ͬ���߱��Լ���ģ�һ�����skip < ikey
				}
				else{ //�ҵ����Լ���ģ���Ϊ��next entry
					valid_ = true;
					saved_key_.clear();
					return ;
				}
				break;
			}
		}

		iter_->Next();
	}while(iter_->Valid());

	saved_key_.clear();
	valid_ = false;
}

void DBIter::Prev()
{
	assert(iter_->Valid());
	if(direction_ == kForward){
		assert(iter_->Valid());
		SaveKey(ExtractUserKey(iter_->key()), &saved_key_);

		while(true){
			iter_->Prev();

			if(!iter_->Valid()){
				valid_ = false;
				saved_key_.clear();
				ClearSavedValue();
				return;
			}

			if(user_comparator_->Compare(ExtractUserKey(iter_->key()), saved_key_) < 0)
				break;
		}
		direction_ = kReverse;
	}

	 FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry()
{
	assert(direction_ == kReverse);
	
	ValueType value_type = kTypeDeletion;
	if(iter_->Valid()){
		do{
			ParsedInternalKey ikey;
			if(ParseKey(&ikey) && ikey.sequence <= sequence_){
				if((value_type != kTypeDeletion) && user_comparator_->Compare(ikey.user_key, saved_key_) < 0) //�Ѿ���ǰһ��entry
					break;

				value_type = ikey.type;
				if(value_type == kTypeDeletion){ //��ɾ��
					saved_key_.clear();
					ClearSavedValue();
				}
				else{
					Slice raw_value = iter_->value();
					if(saved_value_.capacity() > raw_value.size() + 1048576){ //saved_value���ٵĿռ�ܴ�
						std::string empty;
						swap(empty, saved_value_);
					}

					SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
					saved_value_.assign(raw_value.data(), raw_value.size());
				}
			}
			iter_->Prev();
		}while(iter_->Valid());
	}

	if(value_type == kTypeDeletion){
		valid_ = false;
		saved_key_.clear();
		ClearSavedValue();
		direction_ = kForward;
	}
	else
		valid_ = true;
}

void DBIter::Seek(const Slice& target)
{
	direction_ = kForward;
	ClearSavedValue();
	saved_key_.clear();
	//����һ��Internalkey
	AppendInternalKey(&saved_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
	//iter seek
	iter_->Seek(saved_key_);
	if(iter_->Valid())
		FindNextUserEntry(false, &saved_key_);
	else
		valid_ = false;
}

void DBIter::SeekToFirst()
{
  direction_ = kForward;
  ClearSavedValue();

  iter_->SeekToFirst();
  if (iter_->Valid())
    FindNextUserEntry(false, &saved_key_);
  else
    valid_ = false;
}

void DBIter::SeekToLast()
{
	direction_ = kReverse;
	ClearSavedValue();

	iter_->SeekToLast();
	FindPrevUserEntry();
}

}//namespace

Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator, Iterator* internal_iter,
	SequenceNumber sequence, uint32_t seed) {
		return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

};


