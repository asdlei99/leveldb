#include <algorithm>
#include <stdio.h>

#include "version_set.h"
#include "filename.h"
#include "log_reader.h"
#include "log_write.h"
#include "memtable.h"
#include "table_cache.h"
#include "table_builder.h"
#include "merger.h"
#include "two_level_iterator.h"
#include "coding.h"
#include "logging.h"
#include "env.h"

namespace leveldb{

static const int kTargetFileSize = 2 * 1048576; //2M

static const int64_t kMaxGrandParentOverlapBytes = 10 * kTargetFileSize; // 20M

static const int64_t kExpandedCompactionByteSizeLimit = 25 * kTargetFileSize; // 50M

static double MaxBytesForLevel(int level)
{
	double result = 10 * 1048576.0; //�����10M
	while(level > 1){
		result *= 10;
		level --;
	}

	return result;
}

static uint64_t MaxFileSizeForLevel(int level)
{
	return kTargetFileSize; 
}

static int64_t TotalFileSize(const std::vector<FileMetaData*>& files)
{
	int64_t sum = 0;
	for(size_t i = 0; i < files.size(); i ++)
		sum += files[i]->file_size;

	return sum;
}

namespace{
std::string IntSetToString(const std::set<uint64_t>& s)
{
	std::string result = "{";
	for(std::set<uint64_t>::const_iterator it = s.begin(); it != s.end(); ++ it){
		result += (result.size() > 1) ? "," : "";
		result += NumberToString(*it);
	}

	result += "}";
	return result;
}
}//namesapce

Version::~Version()
{
	assert(refs_ == 0);
	prev_->next_ = next_;
	next_->prev_ = prev_;

	//ɾ�������version obj ��FileMeta
	for(int level = 0; level < config::kNumLevels; level ++){
		for(size_t i = 0; i < files_[level].size(); i ++){
			FileMetaData* f = files_[level][i];
			assert(f->refs > 0);

			f->refs --;
			if(f->refs <= 0)
				delete f;
		}
	}
}

//����kye���ڵ��ļ����
int FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files, const Slice& key)
{
	uint32_t left = 0;
	uint32_t right = files.size();
	
	//2�ֲ���
	while(left < right){
		uint32_t mid = (left + right) / 2;
		const FileMetaData* f = files[mid];
		if(icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0)
			left = mid + 1;
		else
			right = mid;
	}

	return right;
}

static bool AfterFile(const Comparator* ucmp, const Slice* user_key, const FileMetaData* f)
{
	return (user_key != NULL && ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

static bool BeforeFile(const Comparator* ucmp, const Slice* user_key, const FileMetaData* f)
{
	return (user_key != NULL && ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

//��λsmallest_user_key ~ largest_user_key�Ƿ����ļ�files�ķ�Χ���е���
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp, bool disjoint_sorted_files, const std::vector<FileMetaData*>& files,
	const Slice* smallest_user_key, const Slice* largest_user_key)
{
	const Comparator* ucmp = icmp->user_comparator();
	if(!disjoint_sorted_files){
		for(size_t i = 0; i < files.size(); i ++){
			const FileMetaData* f = files[i];
			if(AfterFile(ucmp, smallest_user_key, f) || BeforeFile(ucmp, largest_user_key, f)){
				//û���ص����䣬���²���
			}
			else
				return true; //�ʹ��ļ����ص�
		}

		return false;
	}

	uint32_t index = 0;
	if(smallest_user_key != NULL){
		InternalKey small(*smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek); //����һ���ڲ�key(user_key + seq + type)
		index = FindFile(icmp, files, small.Encode()); //����small���ڵ��ļ����
	}

	//δ���ҵ���Ӧ���ļ����
	if(index >= files.size())
		return false;

	return !BeforeFile(ucmp, largest_user_key, files[index]);
}

//��files_��iter
class Version::LevelFileNumIterator : public Iterator
{
public:
	LevelFileNumIterator(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>* flist)
		: icmp_(icmp), flist_(flist), index_(flist->size())
	{
	}

	virtual bool Valid() const
	{
		return index_ < flist_->size();
	}

	virtual void Seek(const Slice& target)
	{
		index_ = FindFile(icmp_, *flist_, target);
	}

	virtual void SeekToFirst()
	{
		index_ = 0;
	};

	virtual void SeekToLast()
	{
		index_ = flist_->empty() ? 0 : flist_->size() - 1;
	}

	virtual void Next()
	{
		assert(Valid());
		index_ ++;
	}

	virtual void Prev()
	{
		assert(Valid());
		if(index_ == 0){
			index_ = flist_->size(); //��ʶΪ�Ƿ�
		}
		else{
			index_ --;
		}
	}

	Slice key() const
	{
		assert(Valid());
		return (*flist_)[index_]->largest.Encode();
	}

	Slice value() const
	{
		assert(Valid());
		EncodeFixed64(value_buf_, (*flist_)[index_]->number); //�ļ����
		EncodeFixed64(value_buf_+ 8, (*flist_)[index_]->file_size); //�ļ���С
		return Slice(value_buf_, sizeof(value_buf_));
	}

	virtual Status status() const {return Status::OK();};

private:
	const InternalKeyComparator icmp_;
	const std::vector<FileMetaData*>* const flist_;
	uint32_t index_;

	mutable char value_buf_[16];
};

static Iterator* GetFileIterator(void* arg, const ReadOptions& opt, const Slice& file_value)
{
	TableCache* cache = reinterpret_cast<TableCache*>(arg);
	if(file_value.size() != 16)
		return NewErrorIterator(Status::Corruption("FileReader invoked with unexpected value"));
	else//ͨ��table cache����һ��two level��iter
		return cache->NewIterator(opt, DecodeFixed64(file_value.data()), DecodeFixed64(file_value.data() + 8));

}

Iterator* Version::NewConcatenatingIterator(const ReadOptions& opt, int level) const
{
	return NewTwoLevelIterator(new LevelFileNumIterator(vset_->icmp_, &files_[level]), &GetFileIterator, vset_->table_cache_, opt);
}

void Version::AddIterators(const ReadOptions& opt, std::vector<Iterator*>* iters)
{
	//level 0��table cache������
	for(size_t i = 0; i < files_[0].size(); i ++){
		//���һ��table cache��iter
		Iterator* cache_iter = vset_->table_cache_->NewIterator(opt, files_[0][i]->number, files_[0][i]->file_size);
		iters->push_back(cache_iter);
	}

	//1 ~ level�㷵��tow level������
	for(int level = 1; level < config::kNumLevels; level ++){
		if(!files_[level].empty()){
			iters->push_back(NewConcatenatingIterator(opt, level));
		}
	}
}

namespace {
enum SaverState{
	kNotFound,
	kFound,
	kDeleted,
	kCorrupt,
};

struct Saver
{
	SaverState state;
	const Comparator* ucmp;
	Slice user_key;
	std::string* value;
};
}; //namespace

//��value����Saver��
static void SaveValue(void* arg, const Slice& ikey, const Slice& v)
{
	Saver* s = reinterpret_cast<Saver*>(arg);
	ParsedInternalKey parsed_key;
	if(!ParseInternalKey(ikey, &parsed_key)){
		s->state = kCorrupt;
	}
	else{
		if(s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0){
			//�ж�key�ǲ��Ǳ�ɾ����
			s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
			if(s->state == kFound)
				s->value->assign(v.data(), v.size());
		}
	}
}

static bool NewestFirst(FileMetaData* a, FileMetaData* b)
{
	return a->number > b->number;
}

void Version::ForEachOverlapping(Slice user_key, Slice internal_key, void* arg, bool (*func)(void*, int, FileMetaData*))
{
	const Comparator* ucmp = vset_->icmp_.user_comparator();

	//search level 0
	std::vector<FileMetaData*> tmp;
	tmp.reserve(files_[0].size());
	for(uint32_t i = 0; i < files_[0].size(); i ++){
		FileMetaData* f = files_[0][i];
		if(ucmp->Compare(user_key, f->smallest.user_key()) >= 0 && ucmp->Compare(user_key, f->largest.user_key()) <= 0)
			tmp.push_back(f);
	}

	if(!tmp.empty()){
		std::sort(tmp.begin(), tmp.end(), NewestFirst);
		for(uint32_t i = 0; i < tmp.size(); i ++){
			if(!(*func)(arg, 0, tmp[i]))
				return ;
		}
	}

	//����1 ~ level��
	for(int level =1; level < config::kNumLevels; level ++){
		size_t num_files = files_[level].size();
		if(num_files == 0)
			continue;

		//����2�ֲ��ҷ����в���
		uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
		if(index < num_files){
			FileMetaData* f = files_[level][index];
			if(ucmp->Compare(user_key, f->smallest.user_key()) < 0){ //user_key ��f->smallest��С����ʾӦ�����²��������??

			}
			else{
				if (!(*func)(arg, level, f))
					return;
			}
		}
	}
}

Status Version::Get(const ReadOptions& opt, const LookupKey& k, std::string* value, GetStats* stats)
{
	Slice ikey = k.internal_key();
	Slice user_key = k.user_key();

	const Comparator* ucmp = vset_->icmp_.user_comparator();
	Status s;

	stats->seek_file = NULL;
	stats->seek_file_level = -1;

	FileMetaData* last_file_read = NULL;
	int last_file_read_level = -1;

	std::vector<FileMetaData*> tmp;
	FileMetaData* tmp2;

	for(int level = 0; level< config::kNumLevels; level ++){
		size_t num_files = files_[level].size();
		if(num_files == 0)
			continue;

		FileMetaData* const* files = &files_[level][0];
		if(level == 0){ //0������

			tmp.reserve(num_files);
			for(uint32_t i = 0; i < num_files; i ++){ //�ҵ����д���user_key��file
				FileMetaData* f = files[i];
				if(ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
					ucmp->Compare(user_key, f->largest.user_key())<= 0)
					tmp.push_back(f);
			}

			if(tmp.empty())
				continue;
			std::sort(tmp.begin(), tmp.end(), NewestFirst);
			files = &tmp[0];
			num_files = tmp.size();
		}
		else{ //��������������ҵ�ikey���ڵ��ļ���
			uint32_t index = FindFile(vset_->icmp_, files_[level], ikey);
			if(index >= num_files){
				files = NULL;
				num_files = 0;
			}
			else{
				tmp2 = files[index];
				if(ucmp->Compare(user_key, tmp2->smallest.user_key()) < 0){
					files = NULL;
					num_files = 0;
				}
				else{
					files = &tmp2;
					num_files = 1;
				}
			}
		}

		for(uint32_t i = 0; i < num_files; ++ i){
			if(last_file_read != NULL && stats->seek_file == NULL){
				stats->seek_file = last_file_read;
				stats->seek_file_level = last_file_read_level;
			}

			FileMetaData* f = files[i];
			last_file_read = f;
			last_file_read_level = level;

			Saver saver;
			saver.state = kNotFound;
			saver.ucmp = ucmp;
			saver.user_key = user_key;
			saver.value = value;
			//��table cache�н���key value�Ӳ���
			s = vset_->table_cache_->Get(opt, f->number, f->file_size, ikey, &saver, SaveValue);
			if(!s.ok())
				return s;

			switch(saver.state){
			case kNotFound:
				break;
			case kFound:
				break;
			case kDeleted:
				s = Status::NotFound(Slice());
				break;
			case kCorrupt:
				s = Status::Corruption("corrupted key for ", user_key);
				return s;
			}
		}
	}
	return Status::NotFound(Slice());  // Use an empty error message for speed
}

//�����ļ���������seek�ĸ���
bool Version::UpdateStats(const GetStats& stats)
{
	FileMetaData* f = stats.seek_file;
	if(f != NULL){
		f->allowed_seeks --;
		if(f->allowed_seeks <= 0 && file_to_compact_ == NULL){ //����allowed seek��0�ˣ���ʾ��Ҫ����compact����
			file_to_compact_ = f;
			file_to_compact_level_ = stats.seek_file_level;
			return true;
		}
	}

	return false;
}

bool Version::RecordReadSample(Slice internal_key)
{
	struct State {
		GetStats stats;  // Holds first matching file
		int matches;

		static bool Match(void* arg, int level, FileMetaData* f) {
			State* state = reinterpret_cast<State*>(arg);
			state->matches++;
			if (state->matches == 1) {
				// Remember first match.
				state->stats.seek_file = f;
				state->stats.seek_file_level = level;
			}
			// We can stop iterating once we have a second match.
			return state->matches < 2;
		}
	};


	ParsedInternalKey ikey;
	if (!ParseInternalKey(internal_key, &ikey))
		return false;

	State state;
	state.matches = 0;
	//����user_key�ص��Ĵ���
	ForEachOverlapping(ikey.user_key, internal_key, &state, &State::Match);
	if(state.matches >= 2) //������һ���ص�
		return UpdateStats(state.stats);

	return false;
}

void Version::Ref()
{
	++ refs_;
}

void Version::Unref()
{
	assert(this != &vset_->dummy_versions_);
	assert(refs_ >= 1);

	-- refs_;
	if(refs_ == 0)
		delete this;
}

//level���Ƿ���{smallest_user_key, largest_user_key}���ص�
bool Version::OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key)
{
	return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level], smallest_user_key, largest_user_key);
}

//Ϊmemtable�ҵ�compact��level
int Version::PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key)
{
	int level = 0;
	//���0�����ص�
	if(!OverlapInLevel(0, &smallest_user_key, &largest_user_key)){
		//����internal key��������
		InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
		InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));

		std::vector<FileMetaData*> overlaps;
		while(level < config::kMaxMemCompactLevel){
			if(OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)){
				break;
			}

			if(level + 2 < config::kNumLevels){
				GetOverlappingInputs(level + 2, &start, &limit, &overlaps);
				const int64_t sum = TotalFileSize(overlaps);
				if(sum > kMaxGrandParentOverlapBytes) //�ص����򳬹�20M
					break;
			}
		}
		level ++;
	}

	return level;
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inputs)
{
	assert(level >= 0);
	assert(level < config::kNumLevels);

	inputs->clear();
	Slice user_begin, user_end;
	if(begin != NULL)
		user_begin = begin->user_key();
	if(end != NULL)
		user_end = end->user_key();

	const Comparator* user_cmp = vset_->icmp_.user_comparator();

	for(size_t i = 0; i < files_[level].size();){
		FileMetaData* f = files_[level][i++];
		const Slice file_start = f->smallest.user_key();
		const Slice file_limit = f->smallest.user_key();

		if(begin != NULL && user_cmp->Compare(file_limit, user_begin) < 0){

		}else if(end != NULL && user_cmp->Compare(file_start, user_end) > 0){

		} //�������������ȫ���ص������
		else{
			inputs->push_back(f);
			if(level == 0){ //{begin, end}û�г���level 0�ķ�Χ��level 0�������㵽inputs����
				if(begin != NULL && user_cmp->Compare(file_start, user_begin) < 0){
					user_begin = file_start;
					inputs->clear();
					i = 0;
				}
				else if(end != NULL && user_cmp->Compare(file_limit, user_end) > 0){
					user_end = file_limit;
					inputs->clear();
					i = 0;
				}
			}
		}
	}
}

std::string Version::DebugString() const 
{
	std::string r;
	for (int level = 0; level < config::kNumLevels; level++) {
		r.append("--- level ");
		AppendNumberTo(&r, level);
		r.append(" ---\n");
		const std::vector<FileMetaData*>& files = files_[level];
		for (size_t i = 0; i < files.size(); i++) {
			r.push_back(' ');
			AppendNumberTo(&r, files[i]->number);
			r.push_back(':');
			AppendNumberTo(&r, files[i]->file_size);
			r.append("[");
			r.append(files[i]->smallest.DebugString());
			r.append(" .. ");
			r.append(files[i]->largest.DebugString());
			r.append("]\n");
		}
	}
	return r;
}

class VersionSet::Builder
{
private:
	struct BySmallestKey
	{
		const InternalKeyComparator* internal_comparator;
		bool operator()(FileMetaData* f1, FileMetaData* f2) const
		{
			int r = internal_comparator->Compare(f1->smallest, f2->smallest);
			if(r != 0)
				return r < 0;
			else
				return (f1->number < f2->number);
		}
	};

	typedef std::set<FileMetaData*, BySmallestKey> FileSet;

	struct LevelState
	{
		std::set<uint64_t> deleted_files;
		FileSet* added_files;
	};

	VersionSet* vset_;
	Version* base_;
	LevelState levels_[config::kNumLevels];

public:
	Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base)
	{
		base_->Ref();
		BySmallestKey cmp;
		cmp.internal_comparator = &vset_->icmp_;
		for(int level = 0; level < config::kNumLevels; level ++){
			levels_[level].added_files = new FileSet(cmp);
		}
	}

	~Builder()
	{
		for(int level = 0; level < config::kNumLevels; level ++){
			const FileSet* added = levels_[level].added_files;
			std::vector<FileMetaData *> to_unref;
			
			to_unref.reserve(added->size());
			for(FileSet::const_iterator it = added->begin(); it != added->end(); ++ it)
				to_unref.push_back(*it);

			//�ͷŵ�fileSet
			delete added;
			//�ͷ����ü���
			for(uint32_t i = 0; i < to_unref.size(); i ++){
				FileMetaData* f = to_unref[i];

				f->refs --;
				if(f->refs <= 0)
					delete f;
			}
		}

		base_->Unref();
	}

	//��edit�е�delete files��add files���뵽���㵱�У��ȴ�SaveTo����
	void Apply(VersionEdit* edit)
	{
		for(size_t i = 0; i < edit->compact_pointers_.size(); i ++){
			const int level = edit->compact_pointers_[i].first;
			vset_->compact_pointer_[level] = edit->compact_pointers_[i].second.Encode().ToString();
		}

		const VersionEdit::DeletedFileSet& del = edit->deleted_files_;
		for(VersionEdit::DeletedFileSet::const_iterator iter = del.begin(); iter != del.end(); ++ iter){
			const int level = iter->first;
			const uint64_t number = iter->second;
			levels_[level].deleted_files.insert(number); //��Ҫɾ�����ļ�number���뵽levels����
		}

		//�����µ��ļ���Ϣ����
		for(size_t i = 0; i < edit->new_files_.size(); i ++){
			const int level = edit->new_files_[i].first;
			FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
			f->refs = 1;

			// We arrange to automatically compact this file after
			// a certain number of seeks.  Let's assume:
			//   (1) One seek costs 10ms
			//   (2) Writing or reading 1MB costs 10ms (100MB/s)
			//   (3) A compaction of 1MB does 25MB of IO:
			//         1MB read from this level
			//         10-12MB read from next level (boundaries may be misaligned)
			//         10-12MB written to next level
			// This implies that 25 seeks cost the same as the compaction
			// of 1MB of data.  I.e., one seek costs approximately the
			// same as the compaction of 40KB of data.  We are a little
			// conservative and allow approximately one seek for every 16KB
			// of data before triggering a compaction.
			//��������SEEK�Ĵ���,ͨ��IO����������
			f->allowed_seeks = f->file_size / 16384; //16k����һ��SEEK
			if(f->allowed_seeks < 100) //��СΪ100
				f->allowed_seeks = 100;

			levels_[level].deleted_files.erase(f->number);
			levels_[level].added_files->insert(f);
		}
	}

	void SaveTo(Version* v)
	{
		BySmallestKey cmp;
		cmp.internal_comparator = &vset_->icmp_;
		for(int level = 0; level < config::kNumLevels; level ++){
			const std::vector<FileMetaData*>& base_files = base_->files_[level];
			std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
			std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
			
			const FileSet* added = levels_[level].added_files;
			v->files_[level]->reserve(base_files.size() + added->size());

			//base_->files_ ���added���µ�file,˳��(��С����)�ϲ�
			for(FileSet::const_iterator added_iter = added->begin(); added_iter != added->end(); ++ added_iter){
				for(std::vector<FileMetaData*>::const_iterator bpos  = std::upper_bound(base_iter, base_end, *added_iter, cmp);
					base_iter != bpos; ++ base_iter){
					MaybeAddFile(v, level, *base_iter);
				}
				MaybeAddFile(v, level, *added_iter);
			}

			//�ϲ�ʣ�µ�
			for(; base_iter != base_end; ++ base_iter)
				 MaybeAddFile(v, level, *base_iter);
		}
	}

	void MaybeAddFile(Version* v, int level, FileMetaData* f)
	{
		if(levels_[level].deleted_files.count(f->number) > 0){ //�Ѿ���deleted file set���У�����

		}
		else{
			std::vector<FileMetaData*>* files = &v->files_[level];
			if(level > 0 && !files->empty()){
				assert(vset_->icmp_.Compare((*files)[files->size()-1]->largest, f->smallest) < 0); //ÿ�α��볬��files�����Χ
			}

			//���뵽files����
			f->refs ++;
			files->push_back(f);
		}
	}
};

VersionSet::VersionSet(const std::string& dbname, const Options* opt, TableCache* table_cache, const InternalKeyComparator* icmp)
	: env_(opt->env), dbname_(dbname), options_(opt), table_cache_(table_cache), icmp_(*icmp), next_file_number_(2), manifest_file_number_(0),
	last_sequence_(0), log_number_(0), prev_log_number_(0), descriptor_file_(NULL), descriptor_log_(NULL), dummy_versions_(this),
	current_(NULL)
{
	AppendVersion(new Version(this));
}

VersionSet::~VersionSet()
{
	current_->Unref();
	assert(dummy_versions_.next_ == &dummy_versions_);

	delete descriptor_log_;
	delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v)
{
	assert(v->refs_ == 0);
	assert(v != current_);

	if(current_ != NULL)
		current_->Unref();

	current_ = v;
	v->Ref();

	v->prev_ = dummy_versions_.prev_;
	v->next_ = &dummy_versions_;
	v->prev_->next_ = v;
	v->next_->prev_ = v;
}

Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu)
{
	//����edit��һЩ����
	if(edit->has_log_number_){
		assert(edit->log_number_ >= log_number_);
		assert(edit->log_number_ < next_file_number_);
	}
	else{
		edit->SetLogNumber(log_number_);
	}

	if(!edit->has_prev_log_number_)
		edit->SetPrevLogNumber(prev_log_number_);

	edit->SetNextFile(next_file_number_);
	edit->SetLastSequence(last_sequence_);

	//��current_ �� edit�ϲ���v����
	Version* v = new Version(this);
	{
		Builder builder(this, current_);
		builder.Apply(edit);
		builder.SaveTo(v);
	}
	Finalize(v);

	std::string new_manifest_file;
	Status s;
	if(descriptor_log_ == NULL){ //�����־�ļ���û�д���
		assert(descriptor_file_ == NULL);
		//���һ��//dbname/MANIFEST-number���ļ���
		new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
		edit->SetNextFile(next_file_number_);
		//��new_manifest_file�ļ�
		s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
		if(s.ok()) //����һ��log writer����
			descriptor_log_ = new log::Writer(descriptor_file_);
	}

	{
		mu->Unlock();
		
		if(s.ok()){
			//���edit��д��log writer����
			std::string record;
			edit->EncodeTo(&record); 
			s = descriptor_log_->AddRecord(record);
			if(s.ok())
				s = descriptor_file_->Sync();

			if(!s.ok())
				Log(options_->info_log, "MANIFEST write: %s\n", s.ToString().c_str());
		}

		//����һ��//dbname/CURRENT�ļ�������MANIFEST���ļ���д�뵽��һ��
		if(s.ok() && !new_manifest_file.empty())
			s = SetCurrentFile(env_, dbname_, manifest_file_number_);

		mu->Lock();
	}

	//��v��ӵ�������
	if(s.ok()){
		AppendVersion(v);
		log_number_ = edit->log_number_;
		prev_log_number_ = edit->prev_log_number_;
	}
	else{ //�ļ�����ʧ��,�ͷŵ��������ļ��;��
		delete v;
		if(!new_manifest_file.empty()){
			delete descriptor_log_;
			delete descriptor_file_;
			descriptor_log_ = NULL;
			descriptor_file_ = NULL;
			env_->DeleteFile(new_manifest_file);
		}
	}

	return s;
}

//ͨ����־�ָ�����
Status VersionSet::Recover()
{
	struct LogReporter : public log::Reader::Reporter
	{
		Status* status;
		virtual void Corruption(size_t bytes, const Status& s)
		{
			if (this->status->ok()) *this->status = s;
		}
	};

	//��ȡ/dbname/CURRENT������
	std::string current;
	Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
	if(s.ok())
		return s;

	//CURRENT�����ݴ�����߱��Ķ���
	if(current.empty() || current[current.size() - 1] !='\n')
		return Status:::Corruption("CURRENT file does not end with newline");

	current.resize(current.size() - 1);//ȥ�����\n
	//���DescriptorFileName,�������
	std::string dscname = dbname_ + "/" + current;
	SequentialFile* file;
	s = env_->NewSequentialFile(dscname, &file);
	if(!s.ok())
		return s;

	bool have_log_number = false;
	bool have_prev_log_number = false;
	bool have_next_file = false;
	bool have_last_sequence = false;

	uint64_t next_file = 0;
	uint64_t last_sequence = 0;
	uint64_t log_number = 0;
	uint64_t prev_log_number = 0;

	Builder builder(this, current_);
	{
		LogReporter reporter;
		reporter.status = &s;
		//����һ��log reader����
		log::Reader reader(file, &reporter, true, 0);

		Slice record;
		std::string scratch;
		while(reader.ReadRecord(&record, &scratch) && s.ok()){
			//���Զ�versionedit�Ķ�ȡ
			VersionEdit edit;
			s = edit.DecodeFrom(record);
			if(s.ok()){
				if (edit.has_comparator_ && edit.comparator_ != icmp_.user_comparator()->Name()) {
					s = Status::InvalidArgument(edit.comparator_ + " does not match existing comparator ", icmp_.user_comparator()->Name());
				}
			}

			//�����version edit,������뵽builder����
			if(s.ok())
				builder.Apply(&edit);

			if (edit.has_log_number_) {
				log_number = edit.log_number_;
				have_log_number = true;
			}

			if (edit.has_prev_log_number_) {
				prev_log_number = edit.prev_log_number_;
				have_prev_log_number = true;
			}

			if (edit.has_next_file_number_) {
				next_file = edit.next_file_number_;
				have_next_file = true;
			}

			if (edit.has_last_sequence_) {
				last_sequence = edit.last_sequence_;
				have_last_sequence = true;
			}
		}
	}

	delete file;
	file = NULL;

	if(s.ok()){
		if(!have_next_file){
			 s = Status::Corruption("no meta-nextfile entry in descriptor");
		}
		else if(!have_log_number){
			 s = Status::Corruption("no meta-lognumber entry in descriptor");
		}
		else if(!have_last_sequence){
			 s = Status::Corruption("no last-sequence-number entry in descriptor");
		}

		if(!have_prev_log_number)
			prev_log_number = 0;

		//��֤дһ��file numberһ�����κ���־�еĶ���
		MarkFileNumberUsed(prev_log_number);
		MarkFileNumberUsed(log_number);
	}

	if(s.ok()){ //����һ��Version �����뵽VersionSet����
		Version* v = new Version(this);
		builder.SaveTo(v);
		Finalize(v);

		AppendVersion(v);
		//����version����
		manifest_file_number_ = next_file;
		next_file_number_ = next_file + 1;
		last_sequence_ = last_sequence;
		log_number_ = log_number;
		prev_log_number_ = prev_log_number;
	}

	return s;
}

void VersionSet::MarkFileNumberUsed(uint64_t number)
{
	if(next_file_number_ <= number)
		next_file_number_ = number + 1;
}

//�ж�level�Ƿ���Ҫcompaction
void VersionSet::Finalize(Version* v)
{
	int best_level = -1;
	double best_score = -1;
	
	for(int level = 0; level < config::kNumLevels - 1; level++){
		double score;
		if(level == 0){
			//��level 0�Ĵ����У�������BUFFER�е��ļ����������ֽڵ�ԭ����2����
			//(1) ���write buffer̫����̫���compactions����������
			//(2)level 0��ÿ�ζ���ʱ���ϲ����ݣ����������˷�ֹ̫����С�ļ�����
			score =v->files_[level].size() / static_cast<double>(config::kL0_CompactionTrigger);
		}
		else{
			const uint64_t level_bytes = TotalFileSize(v->files_[level]);
			score = static_cast<double>(level_bytes) / MaxBytesForLevel(level);
		}

		if(score > best_score){
			best_level = level;
			best_score = score;
		}
	}

	v->compaction_level_ = best_level;
	v->compaction_score_ = best_score;
}

Status VersionSet::WriteSnapshot(log::Writer* log)
{
	VersionEdit edit;
	edit.SetComparatorName(icmp_.user_comparator()->Name());

	//����ϲ���
	for(int level = 0; level = config::kNumLevels; level++){
		if(!compact_pointer_[level].empty()){
			InternalKey key;
			//����compaction pointers
			key.DecodeFrom(compact_pointer_[level]);
			edit.SetCompactPointer(level, key);
		}
	}

	//д��version��������ļ���Ϣ
	for(int level = 0; level < config::kNumLevels; level ++){
		const std::vector<FileMetaData*>& files = current_->files_[level];
		for(size_t i = 0; i < files.size(); i ++){
			const FileMetaData* f = files[i];
			edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
		}
	}

	std::string record;
	edit.EncodeTo(&record);
	return log->AddRecord(record);
}

int VersionSet::NumLevelFiles(int level) const
{
	assert(level >= 0);
	assert(level < config::kNumLevels);
	return current_->files_[level].size();
}

const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const
{
	assert(config::kNumLevels == 7);
	snprintf(scratch->buffer, sizeof(scratch->buffer),
		"files[ %d %d %d %d %d %d %d ]", 
		int(current_->files_[0].size()),
		int(current_->files_[1].size()),
		int(current_->files_[2].size()),
		int(current_->files_[3].size()),
		int(current_->files_[4].size()),
		int(current_->files_[5].size()),
		int(current_->files_[6].size()));

	return scratch->buffer;
}

//����ikey��version��ƫ��λ��
uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey)
{
	uint64_t result = 0;

	for(int level = 0; level < config::kNumLevels; level ++){
		const std::vector<FileMetaData*>& files = v->files_[level];
		for(size_t i = 0; i < files.size(); i++){
			if(icmp_.Compare(files[i]->largest, ikey) <= 0) //ikey��largest���󣬽���file size���뵽ƫ���ϣ�˵��ikey�����������ļ���
				result += files[i]->file_size;
			else if(icmp_.Compare(files[i]->smallest, ikey) > 0){
				if(level > 0)
					break;
			}
			else { //ikey��files[i]��
				Table* tableptr;
				//���table�ϵ�ƫ����
				Iterator* iter = table_cache_->NewIterator(ReadOptions(), files[i]->number, files[i]->file_size, &tableptr);
				if(tableptr != NULL)
					result += tableptr->ApproximateOffsetOf(ikey.Encode());

				delete iter;
			}
		}
	}

	return result;
}
//���version set�����ļ������
void VersionSet::AddLiveFiles(std::set<uint64_t>* live)
{
	for(Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_){
		for(int level = 0; level < config::kNumLevels; level ++){
			const std::vector<FileMetaData*>& files = v->files_[level];
			for(size_t i = 0; i < files.size(); i ++)
				live->insert(files[i]->number);
		}
	}
}

//���current version��level���ļ���С
int64_t VersionSet::NumLevelBytes(int level) const
{
	assert(level >= 0);
	assert(level < config::kNumLevels);
	return TotalFileSize(current_->files_[level]);
}

//��ȡcurrent_��level֮������ص�(level��level + 1�Ƚ�)���ֽ���
int64_t VersionSet::MaxNextLevelOverlappingBytes()
{
	int64_t result = 0;

	std::vector<FileMetaData *> overlaps;
	for(int level = 0; level < config::kNumLevels -1; level ++){
		for(size_t i = 0; i < current_->files_[level].size(); i ++){
			const FileMetaData* f = current_->files_[level][i];
			current_->GetOverlappingInputs(level + 1, &f->smallest, &f->largest, &overlaps);
			
			const int64_t sum = TotalFileSize(overlaps);
			if(sum > result)
				result = sum;
		}
	}

	return result;
}

//���inputs�ļ����е��е�interkey��Χ
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest, InternalKey* largest)
{
	assert(!inputs.empty());

	smallest->Clear();
	largest->Clear();

	for(size_t i = 0; i < inputs.size(); i ++){
		FileMetaData* f = inputs[i];
		if(i == 0){
			*smallest = f->smallest;
			*largest = f->largest;
		}
		else{
			if(icmp_.Compare(f->smallest, *smallest) < 0)
				*smallest = f->smallest;

			if(icmp_.Compare(f->largest, *largest) > 0)
				*largest = f->largest;
		}
	}
}

void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1, const std::vector<FileMetaData*>& inputs2,
	InternalKey* smallest, InternalKey* largest)
{
	std::vector<FileMetaData*> all = inputs1;
	all.insert(all.end(), inputs2.begin(), inputs2.end());
	GetRange(all, smallest, largest);
}

//����һ��merge iter������
Iterator* VersionSet::MakeInputIterator(Compaction* c)
{
	ReadOptions options;

	options.verfy_checksums = options_->paranoid_checks;
	options.fill_cache = false;

	const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
	Iterator** list = new Iterator*[space];
	int num = 0;
	//�ռ����Ժϲ���iter����
	for(int which = 0; which < 2; which ++){
		if(!c->inputs_[which].empty()){
			if(c->level() + which == 0){
				const std::vector<FileMetaData*>& files = c->inputs_[which];
				for(size_t i = 0; i < files.size(); i ++)
					list[num ++] = table_cache_->NewIterator(options, files[i]->number, files[i]->file_size);
			}
			else{
				list[num ++] = NewTwoLevelIterator(new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]),
					&GetFileIterator, table_cache_, options);
			}
		}
	}
	
	assert(num <= space);
	Iterator* result = NewMergingIterator(&icmp_, list, num); //����һ��merge�ϲ���iter

	delete []list;
	//������tow level iter�ڷ��ص�result���ͷ�
	return result;
}

//��ѡ�ϲ������ݸ������score��ȷ���ϲ�
Compaction* VersionSet::PickCompaction()
{
	Compaction* c;
	int level;

	const bool size_compaction = (current_->compaction_score_ >= 1);
	const bool seek_compaction = (current_->file_to_compact_ != NULL);

	if(size_compaction){
		level = current_->compaction_level_;
		
		assert(level >= 0);
		assert(level + 1 < config::kNumLevels);
		c = new Compaction(level);

		//��current level������û���ںϲ����ϵ�file�ӵ�Compaction������
		for(size_t i = 0; i < current_->files_[level].size(); i++){
			FileMetaData* f = current_->files_[level][i];
			if(compact_pointer_[level].empty() || icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0){
				c->inputs_[0].push_back(f);
				break;
			}
		}

		//����������ϲ�����c���ǿյģ���current level��ĵ�һ��file�����ȥ
		if(c->inputs_[0].empty()){
			c->inputs_[0].push_back(current_->files_[level][0]);
		}
	}
	else if(seek_compaction){ //����compaction,ֱ�ӽ�file_to_compact_���뵽compaction����
		level = current_->file_to_compact_level_;
		c = new Compaction(level);
		c->inputs_[0].push_back(current_->file_to_compact_);
	}
	else
		return NULL;

	c->input_version_ = current_;
	c->input_version_->Ref();

	//memtable��
	if(level == 0){
		InternalKey smallest, largest;
		GetRange(c->inputs_[0], &smallest, &largest);
		current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
		assert(!c->inputs_[0].empty());
	}

	SetupOtherInputs(c);

	return c;
}

void VersionSet::SetupOtherInputs(Compaction* c)
{
	const int level = c->level();

	InternalKey smallest, largest;
	GetRange(c->inputs_[0], &smallest, &largest);

	//�����level + 1���ص��������ص�����filesд��c->inputs_[1]����
	current_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);

	//���c->inputs_[0], c->inputs_[1]�����ļ����п�ʼ��key�ͽ�����key
	InternalKey all_start, all_limit;
	GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

	if(!c->inputs_[1].empty()){
		std::vector<FileMetaData*> expanded0;
		current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);

		//����ռ��С
		const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
		const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
		const int64_t expanded0_size = TotalFileSize(expanded0);

		//level ����ص��ļ���������inputs[0]���ļ�������inputs1_size(level + 1���ص�����) + expanded0_size(current level���ص�����) < 50M
		if(expanded0.size() > c->inputs_[0].size() && inputs1_size + expanded0_size < kExpandedCompactionByteSizeLimit){
			InternalKey new_start, new_limit;
			GetRange(expanded0, &new_start, &new_limit);
			
			std::vector<FileMetaData*> expanded1;
			current_->GetOverlappingInputs(level + 1, &new_start, &new_limit, &expanded1);

			//��ҪCompation���ļ�������c�������趨��һ��������ֻ�Ƿ�Χ�ı��ˣ�����������Χ
			if(expanded1.size() == c->inputs_[1].size()){
				Log(options_->info_log,
					"Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld bytes)\n",
					level,
					int(c->inputs_[0].size()),
					int(c->inputs_[1].size()),
					long(inputs0_size), long(inputs1_size),
					int(expanded0.size()),
					int(expanded1.size()),
					long(expanded0_size), long(inputs1_size));

				//��������smallest ��largest
				smallest = new_start;
				largest = new_limit;

				c->inputs_[0] = expanded0;
				c->inputs_[1] = expanded1;
				GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
			}
		}
	}

	//���current level + 2����{all_start, all_limit}���ص������ļ�����
	if(level + 2 < config::kNumLevels){
		current_->GetOverlappingInputs(level + 2, &all_start, &all_limit, &c->grandparents_);
	}

	//��¼һ��Compacting��־
	if(false){
		Log(options_->info_log, "Compacting %d '%s' .. '%s'",
			level, smallest.DebugString().c_str(), largest.DebugString().c_str());
	}

	//����Compact��
	compact_pointer_[level] = largest.Encode().ToString();
	c->edit_.SetCompactPointer(level, largest);
}

//�ϲ�ָ����current_����Χ
Compaction* VersionSet::CompactRange(int level, const InternalKey* begin, const InternalKey* end)
{
	//��������files
	std::vector<FileMetaData*> inputs;
	current_->GetOverlappingInputs(level, begin, end, &inputs);

	if(inputs.empty())
		return NULL;

	if(level > 0){
		const uint64_t limit = MaxFileSizeForLevel(level);
		uint64_t total = 0;
		//��inputs�������ļ���С���Ƿ񳬹�2M������2M�ͻ���Ϊ��inputs.size����1
		for(size_t i = 0; i < inputs.size(); i++){
			uint64_t s = inputs[i]->file_size; 
			total += s;
			if(total >= limit){ //����2M
				inputs.resize(i + 1);
				break;
			}
		}
	}

	//����һ��Compaction
	Compaction* c = new Compaction(level);
	c->input_version_ = current_;
	c->input_version_->Ref();
	c->inputs_[0] = inputs;

	SetupOtherInputs(c);
}

Compaction::Compaction(int level) : level_(level), max_output_file_size_(MaxFileSizeForLevel(level)),
	input_version_(NULL), grandparents_(0), seen_key_(false), overlapped_bytes_(0)
{
	for(int i = 0; i < config::kNumLevels; i++)
		level_ptrs_[i] = NULL;
}

Compaction::~Compaction()
{
	if(input_version_ != NULL)
		input_version_->Unref();
}

bool Compaction::IsTrivialMove() const
{
	//��Ϊ��һ���ǳ���ֵ�õĺϲ�
	return (num_input_files(0) == 1 && num_input_files(1) == 0 && TotalFileSize(grandparents_) <= kMaxGrandParentOverlapBytes);
}

void Compaction::AddInputDeletions(VersionEdit* edit)
{
	for(int which = 0; which < 2; which ++){
		for(size_t i = 0; i < inputs_[which].size(); i++)
			edit->DeleteFile(level_ + which, inputs_[which][i]->number);
	}
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key)
{
	const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();

	for(int lvl = level_ + 2; lvl < config::kNumLevels; lvl++){
		const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
		for(; level_ptrs_[lvl] < files.size();){
			FileMetaData* f = files[level_ptrs_[lvl]];
			
			//��user_key��f�ķ�Χ����
			if(user_cmp->Compare(user_key, f->largest.user_key()) < 0){
				if(user_cmp->Compare(user_key, f->smallest.user_key()) >= 0)
					return false;
				break;
			}

			level_ptrs_[lvl] ++;
		}
	}

	return true;
}

bool Compaction::ShouldStopBefore(const Slice& internal_key)
{
	const InternalKeyComparator* icmp = &input_version_->vset_->icmp_;
	//ɨ��internal_key�������е�grandparent files��С
	while(grandparent_index_ < grandparents_.size() && 
		icmp->Compare(internal_key, grandparents_[grandparent_index_]->largest.Encode()) > 0)
	{
		if(seen_key_)
			overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;

		grandparent_index_ ++;
	}

	seen_key_ = true;

	if(overlapped_bytes_ > kMaxGrandParentOverlapBytes){ //�ص����򳬹�20M,����ֹͣ,�½�һ��output?
		overlapped_bytes_ = 0;
		return true
	}
	else
		return false;
}

void Compaction::ReleaseInputs()
{
	if(input_version_ != NULL){
		input_version_->Unref();
		input_version_ = NULL;
	}
}

};//leveldb



