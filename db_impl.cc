#include "db_impl.h"
#include <algorithm>
#include <set>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <vector>

#include "builder.h"
#include "db_iter.h"
#include "dbformat.h"
#include "filename.h"
#include "log_reader.h"
#include "log_write.h"
#include "memtable.h"
#include "table_cache.h"
#include "version_edit.h"
#include "version_set.h"
#include "write_batch_internal.h"
#include "db.h"
#include "env.h"
#include "status.h"
#include "table.h"
#include "table_builder.h"
#include "port.h"
#include "block.h"
#include "two_level_iterator.h"
#include "coding.h"
#include "logging.h"
#include "mutexlock.h"

namespace leveldb{

const int kNumNonTableCacheFiles = 10;

struct DBImpl::Writer
{
	Status status;
	WriteBatch* batch;
	bool sync;
	bool done;
	port::CondVar cv;

	explicit Writer(port::Mutex* mu) : cv(mu){};
};

struct DBImpl::CompactionState
{
	Compaction* const compaction;
	SequenceNumber smallest_snapshot;

	struct Output
	{
		uint64_t number;
		uint64_t file_size;
		InternalKey smallest, largest;
	};

	std::vector<Output> outputs;

	WritableFile* outfile;
	TableBuilder* builder;

	uint64_t total_bytes;

	Output* current_output()
	{
		return &outputs[outputs.size() - 1];
	};

	explicit CompactionState(Compaction* c) : compaction(c), outfile(NULL), builder(NULL), total_bytes(0)
	{
	}
};

template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue)
{
	if(static_cast<V>(*ptr) > maxvalue)
		*ptr = maxvalue;

	if(static_cast<V>(*ptr) < minvalue)
		*ptr = minvalue;
}

//�����õļ��
Options SanitizeOptions(const std::string& dbname, const InternalKeyComparator* icmp,
						const InternalFilterPolicy* ipolicy, const Options& src)
{
	Options result = src;
	result.comparator = icmp;
	result.filter_policy = (src.filter_policy != NULL) ? ipolicy : NULL;
	
	ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000); //74 ~ 50000
	ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30); //64K ~ 256M
	ClipToRange(&result.block_size, 1 << 10, 4 << 20); //1K ~ 4M

	if(result.info_log == NULL){
		src.env->CreateDir(dbname);
		src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
		//����һ���ı�log�ļ�
		Status s= src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
		if(!s.ok()) //��־�ļ���ʧ��
			result.info_log = NULL;
	}

	if(result.block_cache == NULL)
		result.block_cache = NewLRUCache(8 << 20); //LRU CACHEΪ8M?�ǲ���̫С�ˣ�

	return result;
}

DBImpl::DBImpl(const Options& raw_opt, const std::string& dbname) : env_(raw_opt.env),
	internal_comparator_(raw_opt.comparator), internal_filter_policy_(raw_opt.filter_policy),
	options_(SanitizeOptions(dbname, &internal_comparator_, &internal_filter_policy_, raw_opt)),
	owns_info_log_(options_.info_log != raw_opt.info_log),
	owns_cache_(options_.block_cache != raw_opt.block_cache),
	dbname_(dbname), db_lock_(NULL), shutting_down_(NULL),
	bg_cv_(&mutex_), mem_(new MemTable(internal_comparator_)), imm_(NULL),
	logfile_(NULL), logfile_number_(0), log_(NULL), seed_(0),
	tmp_batch_(new WriteBatch)
{
	bg_compaction_scheduled_ = false;
	manual_compaction_ = NULL;
	//memtable ���ü���
	mem_->Ref();
	has_imm_.Release_Store(NULL);

	//����һ��table cache
	const int table_cache_size = options_.max_open_files - kNumNonTableCacheFiles;
	table_cache_ = new TableCache(dbname_, &options_, table_cache_size);

	versions_ = new VersionSet(dbname_, &options_, table_cache_, &internal_comparator_);
}

DBImpl::~DBImpl()
{
	mutex_.Lock();
	shutting_down_.Release_Store(this);
	while(bg_compaction_scheduled_){
		bg_cv_.Wait();
	}
	mutex_.Unlock();

	if(db_lock_ != NULL)
		env_->UnlockFile(db_lock_);

	delete versions_;
	if(mem_ != NULL)
		mem_->Unref();

	if(imm_ != NULL)
		imm_->Unref();

	delete tmp_batch_;
	delete log_;
	delete logfile_;
	delete table_cache_;

	if(owns_info_log_)
		delete options_.info_log;

	if(owns_cache_)
		delete options_.block_cache;
}

Status DBImpl::NewDB()
{
	VersionEdit new_db;
	new_db.SetComparatorName(user_comparator()->Name());
	new_db.SetLogNumber(0);
	new_db.SetNextFile(2);
	new_db.SetLastSequence(0);

	//MANIFEST-1�������
	const std::string manifest = DescriptorFileName(dbname_, 1);
	WritableFile* file;
	Status s= env_->NewWritableFile(manifest, &file);
	if(!s.ok())
		return s;

	{//��version edit�еĳ�ʼ��������Ϊ��־д�뵽MANIFEST-1����
		log::Writer log(file);
		std::string record;
		new_db.EncodeTo(&record);
		s = log.AddRecord(record);
		if(s.ok())
			s = file->Close();
	}

	delete file;
	if(s.ok())
		s = SetCurrentFile(env_, dbname_, 1);
	else
		env_->DeleteFile(manifest);

	return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const
{
	if(s->ok() || options_.paranoid_checks){
	}
	else{
		Log(options_.info_log, "Ignaring error %s", s->ToString().c_str());
		*s = Status::OK();
	}
}

//ɾ���������ļ�
void DBImpl::DeleteObsoleteFiles()
{
	if(!bg_error_.ok())
		return ;

	//��ȡ��ǰ��Ч���ļ�
	std::set<uint64_t> live = pending_outputs_;
	versions_->AddLiveFiles(&live);
	
	std::vector<std::string> filenames;
	env_->GetChildren(dbname_, &filenames);
	uint64_t number;
	FileType type;
	
	for(size_t i = 0; i < filenames.size(); i ++){
		if(ParseFileName(filenames[i], &number, &type)){ //��NUMBER����У�飬������ǵ�ǰ�汾��Ч��number�ͱ�ʾ����ɾ��
			bool keep = true;
			switch(type){
			case kLogFile:
				keep = ((number >= versions_->LogNumber()) || (number == versions_->PrevLogNumber()));
				break;

			case kDescriptorFile:
				keep = (number >= versions_->ManifestFileNumber());
				break;

			case kTableFile:
				keep = (live.find(number) != live.end());
				break;

			case kTempFile:
				keep = (live.find(number) !=live.end());
				break;

			case kCurrentFile:
			case kDBLockFile:
			case kInfoLogFile:
				keep = true;
				break;

			}

			//�ļ���Ҫɾ��
			if(!keep){
				if(type == kTableFile) //ɾ��table cache�ж�Ӧ�ĵ�Ԫ
					table_cache_->Evict(number);
			}

			Log(options_.info_log, "Delete type=%d #%lld\n", int(type), static_cast<unsigned long long>(number)); 
			//ɾ���ļ�
			env_->DeleteFile(dbname_ + "/" + filenames[i]);
		}
	}
}

Status DBImpl::Recover(VersionEdit* edit)
{
	mutex_.AssertHeld();
	//����һ��dbname��Ŀ¼
	env_->CreateDir(dbname_);
	assert(db_lock_ == NULL);

	//����һ���ļ���
	Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
	if(!s.ok())
		return s;

	//�ж�dbname/CURRENT�ļ��Ƿ����
	if(!env_->FileExists(CurrentFileName(dbname_))){
		if(options_.create_if_missing){ //�����ڣ�����options��������Ҫ�������ʹ���һ���µ�DB
			s = NewDB();
			if(s.ok())
				return s;
		}
		else
			return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
	}
	else{
		if(options_.error_if_exists)
			return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
	}

	//����version set��ȡ��־�ָ�
	s = versions_->Recover();
	if(s.ok()){
		SequenceNumber max_sequence(0);
		const uint64_t min_log = versions_->LogNumber();     
		const uint64_t prev_log = versions_->PrevLogNumber();

		std::vector<std::string> filenames;
		s = env_->GetChildren(dbname_, &filenames);
		if(!s.ok())
			return s;

		std::set<uint64_t> expected;
		versions_->AddLiveFiles(&expected);
		uint64_t number;
		FileType type;
		
		//�����ɺ���ļ����ڴ��м�¼��file meta Data��������һһ�Ա�
		std::vector<uint64_t> logs;
		for (size_t i = 0; i < filenames.size(); i++) {
			if (ParseFileName(filenames[i], &number, &type)) {
				expected.erase(number);
				if (type == kLogFile && ((number >= min_log) || (number == prev_log))) //���log �ļ���������
					logs.push_back(number);
			}
		}

		//�ڴ��е�meta data����ʣ��˵�������ϵ��ļ�����������
		if(!expected.empty()){
			char buf[50];
			snprintf(buf, sizeof(buf), "%d missing files; e.g.", static_cast<int>(expected.size()));
			return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
		}

		//�������ݿ���־�ļ��Ķ�ȡ�������м�¼�ָ�
		std::sort(logs.begin(), logs.end());
		for(size_t i = 0; i < logs.size(); i++){
			s = RecoverLogFile(logs[i], edit, &max_sequence);
			versions_->MarkFileNumberUsed(logs[i]); //���version set���ļ����
		}

		if(s.ok()){
			if(versions_->LastSequence() < max_sequence)
				versions_->SetLastSequence(max_sequence);
		}
	}

	return s;
}

Status DBImpl::RecoverLogFile(uint64_t log_number, VersionEdit* edit, SequenceNumber* max_sequence)
{
	struct LogReporter : public log::Reader::Reporter
	{
		Env* env;
		Logger* info_log;
		const char* fname;
		Status* status;

		virtual void Corruption(size_t bytes, const Status& s)
		{
			Log(info_log, "%s%s: dropping %d bytes; %s", (this->status == NULL ? "(ignoring error) " : ""), 
				fname, static_cast<int>(bytes), s.ToString().c_str());

			if(this->status != NULL && this->status->ok())
				*this->status = s;
		}
	};

	mutex_.AssertHeld();

	//��˳���ȡ��log�ļ�
	std::string fname = LogFileName(dbname_, log_number);
	SequentialFile* file;
	Status status = env_->NewSequentialFile(fname, &file);
	if(!status.ok()){
		MaybeIgnoreError(&status);
		return status;
	}

	//����һ��Log Reader
	LogReporter reporter;
	reporter.env = env_;
	reporter.info_log = options_.info_log;
	reporter.fname = fname.c_str();
	reporter.status = (options_.paranoid_checks ? &status : NULL);

	log::Reader reader(file, &reporter, true, 0);
	Log(options_.info_log, "Recovering log #%llu", (unsigned long long) log_number);

	//��������LOG�еļ�¼���ӵ�memtable����
	std::string scratch;
	Slice record;
	WriteBatch batch;
	MemTable* mem = NULL;

	while(reader.ReadRecord(&record, &scratch) && status.ok()){
		if(record.size() < 12){ //LOG�ļ�¼ͷΪ12���ֽ�
		   reporter.Corruption(record.size(), Status::Corruption("log record too small"));
			continue;
		}

		WriteBatchInternal::SetContents(&batch, record);

		if(mem == NULL){
			mem = new MemTable(internal_comparator_);
			mem->Ref();
		}
		//��¼д��memtable
		status = WriteBatchInternal::InsertInto(&batch, mem);
		MaybeIgnoreError(&status);
		if(!status.ok())
		break;

		//����max sequence
		const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) + WriteBatchInternal::Count(&batch) - 1;
		if(last_seq > *max_sequence)
			 *max_sequence = last_seq;

		if(mem->ApproximateMemoryUsage() > options_.write_buffer_size){ //�ڴ�ʹ�ôﵽ����
			status = WriteLevel0Table(mem, edit, NULL); 
			if(!status.ok())
				break;

			//д����ɺ󣬿����ͷŵ�mem���󣬵���һ��ѭ���ٿ���һ���µ�mem
			mem->Unref();
			mem = NULL;
		}
	}
	
	//д��level 0����
	if(status.ok() && mem != NULL)
		status = WriteLevel0Table(mem, edit, NULL);

	if(mem != NULL)
		mem->Unref();

	delete file;
	
	return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base)
{
	mutex_.AssertHeld();

	const uint64_t start_micros = env_->NowMicros();
	FileMetaData meta;
	meta.number = versions_->NewFileNumber();
	//��¼�������ɵ�file numner
	pending_outputs_.insert(meta.number);

	Iterator* iter = mem->NewIterator();
	Log(options_.info_log, "Level-0 table #%llu: started", (unsigned long long) meta.number);

	Status s;
	{
		mutex_.Unlock();
		//��memtable�е�����д�뵽meta file���У��������block�ļ���ʽ
		s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
		mutex_.Lock();
	}

	Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s", (unsigned long long) meta.number,
      (unsigned long long) meta.file_size, s.ToString().c_str());

	delete iter;
	//��file number����ɾ��,��Ϊ�Ѿ������д��
	pending_outputs_.erase(meta.number);

	int level = 0;
	if(s.ok() && meta.file_size > 0){
		const Slice min_user_key = meta.smallest.user_key();
		const Slice max_user_key = meta.largest.user_key();
		if(base != NULL)
			level = base->PickLevelForMemTableOutput(min_user_key, max_user_key); //ѡ��һ�����ʵĿ���Compact�Ĳ㣬
		//���뵽version edit����
		edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
	}

	//��¼Compaction��ͳ����Ϣ
	CompactionStats stats;
	stats.micros = env_->NowMicros() - start_micros;
	stats.bytes_written = meta.file_size;
	stats_[level].Add(stats);

	return s;
}

void DBImpl::CompactMemTable()
{
	mutex_.AssertHeld();
	assert(imm_ != NULL);

	VersionEdit edit;
	Version* base = versions_->current();
	base->Ref();
	//��imm_д�뵽block table����
	Status s = WriteLevel0Table(imm_, &edit, base);
	base->Unref();

	if(s.ok() && shutting_down_.Acquire_Load())
		s = Status::IOError("Deleting DB during memtable compaction");

	if(s.ok()){
		edit.SetPrevLogNumber(0);
		edit.SetLogNumber(logfile_number_);
		s = versions_->LogAndApply(&edit, &mutex_);
	}

	//���������ļ�����
	if(s.ok()){
		imm_->Unref();
		imm_ = NULL;
		has_imm_.Release_Store(NULL);
		DeleteObsoleteFiles();
	}
	else
		RecordBackgroundError(s);
}
//����Compact ����
void DBImpl::CompactRange(const Slice* begin, const Slice* end)
{
	int max_level_with_files = 1;
	{
		MutexLock l(&mutex_);
		Version* base = versions_->current();
		for(int level = 1; level < config::kNumLevels; level ++){
			if(base->OverlapInLevel(level, begin, end))
				max_level_with_files = level;
		}
	}

	TEST_CompactMemTable();
	for(int level = 0; level < max_level_with_files; level++)
		TEST_CompactRange(level, begin, end);
}

Status DBImpl::TEST_CompactMemTable()
{
	Status s = Write(WriteOptions(), NULL);
	if(s.ok()){
		MutexLock l(&mutex_);
		while(imm_ != NULL && bg_error_.ok())
			bg_cv_.Wait();

		if(imm_ != NULL)
			s = bg_error_;
	}

	return s;
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin, const Slice* end)
{
	assert(level > 0);
	assert(level + 1 < config::kNumLevels);

	InternalKey begin_storage, end_storage;

	ManualCompaction manual;
	manual.level = level;
	manual.done = false;
	if(begin == NULL)
		manual.begin = NULL;
	else{
		begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
		manual.begin = &begin_storage;
	}

	if(end == NULL)
		manual.end = NULL;
	else{
		end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
		manual.end = &end_storage;
	}

	//�ȴ�compact��ɣ�
	MutexLock l(&mutex_);
	while(!manual.done && !shutting_down_.Acquire_Load() && bg_error_.ok()){
		if(manual_compaction_ == NULL){
			manual_compaction_ = &manual;
			MaybeScheduleCompaction();
		}
		else
			bg_cv_.Wait();
	}

	if(manual_compaction_ == &manual)
		manual_compaction_ = NULL;
}

void DBImpl::RecordBackgroundError(const Status& s)
{
	mutex_.AssertHeld();
	if(bg_error_.ok()){
		bg_error_ = s;
		bg_cv_.SignalAll();
	}
}

void DBImpl::MaybeScheduleCompaction()
{
	mutex_.AssertHeld();

	//�Ѿ���ʼCompact scheduled
	if(bg_compaction_scheduled_){
	}
	else if(shutting_down_.Acquire_Load()){ //DB���ڱ�ɾ��
	}
	else if(!bg_error_.ok()){ //�Ѿ�����һ�����󣬲��ܽ���Compact
	}
	else if(imm_ == NULL && manual_compaction_ == NULL && !versions_->NeedsCompaction()){ //û����Ҫcompact��������������
	}
	else{
		bg_compaction_scheduled_ = true;
		env_->Schedule(&DBImpl::BGWork, this); //����һ���߳̽��ж�Ӧ��compact���൱�����ݿ�ĺ�̨�����߳�
	}
}

void DBImpl::BGWork(void* db)
{
	reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall()
{
	MutexLock l(&mutex_);
	assert(bg_compaction_scheduled_);

	if(shutting_down_.Acquire_Load()){
	}
	else if(!bg_error_.ok()){
	}
	else
		BackgroundCompaction(); //��̨Compact

	bg_compaction_scheduled_ = false;

	//�ٴμ��compact Schedule
	MaybeScheduleCompaction();
}

void DBImpl::BackgroundCompaction()
{
	mutex_.AssertHeld();

	//�ȶ�imm_ table����д���ļ�
	if(imm_ != NULL){
		CompactMemTable();
		return ;
	}

	Compaction* c;
	bool is_manual = (manual_compaction_ != NULL);
	InternalKey manual_end;
	if(is_manual){
		ManualCompaction* m = manual_compaction_;
		//ͨ��version set��Compact���м���õ�һ��Compaction
		c = versions_->CompactRange(m->level, m->begin, m->end);

		m->done = (c == NULL);
		if(c != NULL) //����ȷ��manual end
			manual_end = c->input(0, c->num_input_files(0) - 1)->largest;

		Log(options_.info_log,
			"Manual compaction at level-%d from %s .. %s; will stop at %s\n",
			m->level,
			(m->begin ? m->begin->DebugString().c_str() : "(begin)"),
			(m->end ? m->end->DebugString().c_str() : "(end)"),
			(m->done ? "(end)" : manual_end.DebugString().c_str()));
	}
	else //��������ֶ��ƶ�Compact��Χ�Ļ�����versions����Compact���õ�һ��Compaction
		c = versions_->PickCompaction();

	Status status;
	if(c == NULL){
	}
	else if (!is_manual && c->IsTrivialMove()){ //��Compact������,��Compact������ļ��ƶ�����һ��LEVEL
		assert(c->num_input_files(0) == 1);

		FileMetaData* f = c->input(0, 0);
		c->edit()->DeleteFile(c->level(), f->number);
		c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest, f->largest);

		status = versions_->LogAndApply(c->edit(), &mutex_);
		if(!status.ok()) //����һ������
			RecordBackgroundError(status);

		VersionSet::LevelSummaryStorage tmp;
		Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
			static_cast<unsigned long long>(f->number), c->level() + 1, static_cast<unsigned long long>(f->file_size),
			status.ToString().c_str(), versions_->LevelSummary(&tmp));
	}
	else{ //������ص����ݺϲ���
		CompactionState* compact = new CompactionState(c);
		status = DoCompactionWork(compact);
		if(!status.ok())
			RecordBackgroundError(status);

		CleanupCompaction(compact);
		c->ReleaseInputs();
		//ɾ�����������ļ�
		DeleteObsoleteFiles();
	}

	delete c;

	if(status.ok()){
	}
	else if(shutting_down_.Acquire_Load()){
	}
	else
		Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());

	if(is_manual){
		 ManualCompaction* m = manual_compaction_;
		 if(!status.ok())
			 m->done = true;

		 if(!m->done){
			 m->tmp_storage = manual_end;
			 m->begin = &m->tmp_storage;
		}
		manual_compaction_ = NULL;
	}
}

void DBImpl::CleanupCompaction(CompactionState* compact)
{
	mutex_.AssertHeld();
	if(compact->builder != NULL){
		compact->builder->Abandon();
		delete compact->builder;
	}
	else{
		assert(compact->outfile == NULL);
	}

	delete compact->outfile;
	//��pending��ɾ�����ڴ�����ļ�number
	for(size_t i = 0; i < compact->outputs.size(); i ++){
		const CompactionState::Output& out = compact->outputs[i];
		pending_outputs_.erase(out.number);
	}

	delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact)
{
	assert(compact != NULL);
	assert(compact->builder == NULL);

	uint64_t file_number;
	{
		//����һ���ļ����
		mutex_.Lock();
		file_number = versions_->NewFileNumber();
		pending_outputs_.insert(file_number);
		//��һ��Compact out���󣬲����뵽compact����
		CompactionState::Output out;
		out.number = file_number;
		out.smallest.Clear();
		out.largest.Clear();
		compact->outputs.push_back(out);

		mutex_.Unlock();
	};
	//����һ��table file��table builder
	std::string fname = TableFileName(dbname_, file_number);
	Status s = env_->NewWritableFile(fname, &compact->outfile);
	if(s.ok())
		compact->builder = new TableBuilder(options_, compact->outfile);

	return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact, Iterator* input)
{
	assert(compact != NULL);
	assert(compact->outfile != NULL);
	assert(compact->builder != NULL);

	const uint64_t output_number = compact->current_output()->number;
	assert(output_number != 0);

	Status s = input->status();
	const uint64_t current_entries = compact->builder->NumEntries();
	if(s.ok())
		s = compact->builder->Finish();
	else
		compact->builder->Abandon();

	//����Compact���ֽ���
	const uint64_t current_bytes = compact->builder->FileSize();
	compact->current_output()->file_size = current_bytes;
	compact->total_bytes += current_bytes;

	delete compact->builder;
	compact->builder = NULL;

	//����д�����
	if(s.ok())
		s = compact->outfile->Sync();

	if(s.ok())
		s = compact->outfile->Close();
	//�ͷ��ļ��������
	delete compact->outfile;
	compact->outfile = NULL;

	//У��table�Ƿ����
	if(s.ok() && current_entries > 0){
		Iterator* iter = table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
		s = iter->status();
		delete iter;

		if(s.ok()){
			Log(options_.info_log, "Generated table #%llu: %lld keys, %lld bytes",
			(unsigned long long) output_number,
			(unsigned long long) current_entries,
			(unsigned long long) current_bytes);
		}
	}

	return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact)
{
	mutex_.AssertHeld();

	Log(options_.info_log,  "Compacted %d@%d + %d@%d files => %lld bytes",
		compact->compaction->num_input_files(0),
		compact->compaction->level(),
		compact->compaction->num_input_files(1),
		compact->compaction->level() + 1,
		static_cast<long long>(compact->total_bytes));

	//ɾ�������Compact files,��Ϊ��Щ�ļ���Compact��
	compact->compaction->AddInputDeletions(compact->compaction->edit());

	const int level = compact->compaction->level();
	//Ϊversion edit������Ч��Compact files
	for(size_t i = 0; i < compact->outputs.size(); i++){
		const CompactionState::Output& out = compact->outputs[i];
		compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size, out.smallest, out.largest);
	}
	//��session set�ĸ���
	return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact)
{
	const uint64_t start_micros = env_->NowMicros();
	int64_t imm_micros = 0;

	Log(options_.info_log,  "Compacting %d@%d + %d@%d files",
		compact->compaction->num_input_files(0),
		compact->compaction->level(),
		compact->compaction->num_input_files(1),
		compact->compaction->level() + 1);

	assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
	assert(compact->builder == NULL);
	assert(compact->outfile == NULL);

	if(snapshots_.empty())
		compact->smallest_snapshot = versions_->LastSequence();
	else
		compact->smallest_snapshot = snapshots_.oldest()->number_;

	mutex_.Unlock();

	//���һ��merge iter
	Iterator* input = versions_->MakeInputIterator(compact->compaction);
	input->SeekToFirst();

	Status status;
	ParsedInternalKey ikey;
	std::string current_user_key;
	bool has_current_user_key = false;

	SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
	for(; input->Valid() && !shutting_down_.Acquire_Load(); ){
		if(has_imm_.NoBarrier_Load() != NULL){
			const uint64_t imm_start = env_->NowMicros();
			mutex_.Lock();
			if(imm_ != NULL){ //����Compact mem table����Ϊimm���п����зǳ���Χ���KEY VALUE����ɾ����key
				CompactMemTable();
				bg_cv_.SignalAll();
			}

			mutex_.Unlock();
			imm_micros += (env_->NowMicros() - imm_start);
		}

		//�ж��Ƿ�Compact����
		Slice key = input->key();
		if(compact->compaction->ShouldStopBefore(key) && compact->builder != NULL){
			status = FinishCompactionOutputFile(compact, input);
			if(!status.ok())
				break;
		}

		bool drop = false;
		if(!ParseInternalKey(key, &ikey)){ //key��һ���Ƿ���key
			current_user_key.clear();	  //��յ�key
			has_current_user_key = false; //���Ϊû�е�ǰ��key
			last_sequence_for_key = kMaxSequenceNumber;
		}
		else{
			//�ж��Ƿ������iter�ĵ�һ��key
			if(!has_current_user_key || user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) != 0){
				current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
				has_current_user_key = true;
				last_sequence_for_key = kMaxSequenceNumber;
			}

			//�ж�ֵ�Ƿ���Ҫ����
			if(last_sequence_for_key <= compact->smallest_snapshot) //�ж���key�ص������������Ǻ���ļ�¼������
				drop = true;
			else if(ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot
				&& compact->compaction->IsBaseLevelForKey(ikey.user_key)) //key��ɾ����
				drop = true;

			last_sequence_for_key = ikey.sequence;
		}

		if(!drop){
			if(compact->builder == NULL){ //�տ�ʼ����out file��out table��
				status = OpenCompactionOutputFile(compact);
				if(!status.ok())
					break;
			}

			if(compact->builder->NumEntries() == 0) //��¼out meta file��smallest
				compact->current_output()->smallest.DecodeFrom(key);

			//ÿһ�ζ���¼Ϊ�����Ϊ��֪����һ��Ϊ����
			compact->current_output()->largest.DecodeFrom(key);
			compact->builder->Add(key, input->value());
			
			//�ļ�����������ֵ��,�������block table
			if(compact->builder->FileSize() >= compact->compaction->MaxOutputFileSize()){
				status = FinishCompactionOutputFile(compact, input);
				if(!status.ok())
					break;
			}
		}
		//������һ����¼
		input->Next();
	}

	if(status.ok() && shutting_down_.Acquire_Load())
		status = Status::IOError("Deleting DB during compaction");

	if(status.ok() && compact->builder != NULL)
		status = FinishCompactionOutputFile(compact, input);

	if(status.ok())
		status =input->status();

	//ɾ����������
	delete input;
	input = NULL;

	//�����ļ�Compact�ĺ�ʱ�������ȥ��imm Compact�ĺ�ʱ
	CompactionStats stats;
	stats.micros = env_->NowMicros() - start_micros - imm_micros;
	//����input bytes ��output bytes
	for(int which = 0; which < 2; which ++)
		for(int i = 0; i < compact->compaction->num_input_files(which); i ++)
			stats.bytes_read += compact->compaction->input(which, i)->file_size;

	for(size_t i = 0; i < compact->outputs.size(); i ++)
		stats.bytes_written += compact->outputs[i].file_size;

	mutex_.Lock();
	stats_[compact->compaction->level() + 1].Add(stats);

	//��Compact ���meta files��������
	if(status.ok())
		status = InstallCompactionResults(compact);

	if(!status.ok())
		RecordBackgroundError(status);

	VersionSet::LevelSummaryStorage tmp;
	Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));

	return status;
}

namespace {
struct IterState {
		port::Mutex* mu;
		Version* version;
		MemTable* mem;
		MemTable* imm;
};

static void CleanupIteratorState(void* arg1, void* arg2)
{
	IterState* state = reinterpret_cast<IterState*>(arg1);
	
	state->mu->Lock();

	state->mem->Unref();
	if (state->imm != NULL) 
		state->imm->Unref();

	state->version->Unref();
	state->mu->Unlock();

	delete state;
}

};

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options, SequenceNumber* last_snapshot, uint32_t* seed)
{
	IterState* cleanup = new IterState;
	mutex_.Lock();

	*last_snapshot = versions_->LastSequence();

	std::vector<Iterator*> list;
	//���memtable iterator
	list.push_back(mem_->NewIterator());
	mem_->Ref();
	if(imm_ != NULL){
		list.push_back(imm_->NewIterator());
		imm_->Ref();
	}

	versions_->current()->AddIterators(options, &list);
	Iterator* internal_iter = NewMergingIterator(&internal_comparator_, &list[0], list.size());
	versions_->current()->Ref();

	cleanup->mu = &mutex_;
	cleanup->mem = mem_;
	cleanup->imm = imm_;
	cleanup->version = versions_->current();
	internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, NULL);

	*seed = ++seed_;
	mutex_.Unlock();

	return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator()
{
	SequenceNumber ignored;
	uint32_t ignored_seed;
	return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes()
{
	MutexLock l(&mutex_);
	return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) 
{
	Status s;

	MutexLock l(&mutex_);
	SequenceNumber snapshot;

	//������snapshot���д�����ļ���Sequence number
	if (options.snapshot != NULL)
		snapshot = reinterpret_cast<const SnapshotImpl*>(options.snapshot)->number_;
	else
		snapshot = versions_->LastSequence();

	MemTable* mem = mem_;
	MemTable* imm = imm_;
	Version* current = versions_->current();

	mem->Ref();
	if(imm != NULL) 
		imm->Ref();

	current->Ref();

	bool have_stat_update = false;
	Version::GetStats stats;

	{
		mutex_.Unlock();
		//����һ����ѯKEY
		LookupKey lkey(key, snapshot);
		if (mem->Get(lkey, value, &s)) { //����mem table

		} 
		else if (imm != NULL && imm->Get(lkey, value, &s)) { // ����index mem table

		} 
		else {
			s = current->Get(options, lkey, value, &stats); //����sstable
			have_stat_update = true;
		}
		mutex_.Lock();
	}

	//����Ƿ����Compact
	if(have_stat_update && current->UpdateStats(stats))
		MaybeScheduleCompaction();

	//�ͷ����ü���
	mem->Unref();
	if (imm != NULL) 
		imm->Unref();

	current->Unref();

	return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& opt)
{
	SequenceNumber latest_snapshot;
	uint32_t seed;

	Iterator* iter = NewInternalIterator(opt, &latest_snapshot, &seed);

	return NewDBIterator(this, user_comparator(), iter, 
		(opt.snapshot != NULL ? reinterpret_cast<const SnapshotImpl*>(opt.snapshot)->number_ : latest_snapshot), seed);
}

//��block ��io seek�ļ��
void DBImpl::RecordReadSample(Slice key)
{
	MutexLock l(&mutex_);

	if(versions_->current()->RecordReadSample(key))
		MaybeScheduleCompaction();
}

const Snapshot* DBImpl::GetSnapshot()
{
	MutexLock l(&mutex_);
	return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* s)
{
	MutexLock l(&mutex_);
	return snapshots_.Delete(reinterpret_cast<const SnapshotImpl*>(s));
}

Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val)
{
	return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& opt, const Slice& key)
{
	return DB::Delete(opt, key);
}

Status DBImpl::Write(const WriteOptions& opt, WriteBatch* my_batch)
{
	//����һ��writer����
	Writer w(&mutex_);
	w.batch = my_batch;
	w.sync = opt.sync;
	w.done = false;

	MutexLock l(&mutex_);
	writers_.push_back(&w);
	//�ȴ�ǰ���д��ɣ�
	while(!w.done && &w != writers_.front())
		w.cv.Wait();

	if(w.done)
		return w.status;

	Status status = MakeRoomForWrite(my_batch == NULL);
	uint64_t last_sequence = versions_->LastSequence();
	Writer* last_writer = &w;

	if(status.ok() && my_batch != NULL){
		WriteBatch* updates = BuildBatchGroup(&last_writer); //����һ���������µĶ���
		WriteBatchInternal::SetSequence(updates, last_sequence + 1);
		last_sequence += WriteBatchInternal::Count(updates);

		{
			mutex_.Unlock();
			//��¼д������־
			status = log_->AddRecord(WriteBatchInternal::Contents(updates));

			bool sync_error = false;
			if(status.ok() && opt.sync){
				status = logfile_->Sync();
				if(!status.ok())
					sync_error = true;
			}
			//������д��mem table
			if(status.ok())
				status = WriteBatchInternal::InsertInto(updates, mem_);

			mutex_.Lock();
			if(sync_error)
				RecordBackgroundError(status);
		}

		if(updates == tmp_batch_)
			tmp_batch_->Clear();

		versions_->SetLastSequence(last_sequence);
	}

	//֪ͨǰ���д�����
	while(true){
		Writer* ready = writers_.front();
		writers_.pop_front();
		if(ready != &w){
			ready->status = status;
			ready->done = true;
			ready->cv.Signal();
		}

		if(ready == last_writer)
			break;
	}

	if(!writers_.empty())
		writers_.front()->cv.Signal();

	return status;
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer)
{
	assert(!writers_.empty());
	Writer* first = writers_.front();
	WriteBatch* result = first->batch;
	assert(result != NULL);

	size_t size = WriteBatchInternal::ByteSize(first->batch);

	size_t max_size = 1 << 20; //1M
	if(size <= (128<<10))
		max_size = size + (128<<10); //128K + size

	*last_writer = first;
	std::deque<Writer*>::iterator iter = writers_.begin();
	++iter;

	//ƴ�ն��writer����
	for(; iter != writers_.end(); ++iter){
		Writer* w = *iter;
		if(w->sync && !first->sync)
			break;

		if(w->batch != NULL){
			size += WriteBatchInternal::ByteSize(w->batch);
			if(size > max_size) //�պõ���д���
				break;
			
			//��һ��batch
			if(result == first->batch){
				result = tmp_batch_;
				assert(WriteBatchInternal::Count(result) == 0);
				WriteBatchInternal::Append(result, first->batch);
			}

			WriteBatchInternal::Append(result, w->batch);
		}
		*last_writer = w;
	}

	return result;
}

Status DBImpl::MakeRoomForWrite(bool force)
{
	mutex_.AssertHeld();
	assert(!writers_.empty());

	bool allow_delay = !force;
	Status s;
	while(true){
		if(!bg_error_.ok()){
			s = bg_error_;
			break;
		}
		else if(allow_delay && versions_->NumLevelFiles(0) >= config::kL0_CompactionTrigger){ //Sleep 1��
			mutex_.Unlock();
			env_->SleepForMicroseconds(1000);
			allow_delay = false;
			mutex_.Lock();
		}
		else if(!force && (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) //��ǿ��ת��imm��write bufferû������
			break;
		else if(imm_ != NULL){ //��ǰimm_�Ѿ����ڣ��ȴ���Compact
			Log(options_.info_log, "Current memtable full; waiting...\n");
			bg_cv_.Wait();
		}
		else if(versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger){ //Level 0�ļ�̫��
			Log(options_.info_log, "Too many L0 files; waiting...\n");
			bg_cv_.Wait();
		}
		else{ //mem tableҪ����ת�Ƶ�imm
			assert(versions_->PrevLogNumber());
			uint64_t new_log_number = versions_->NewFileNumber();
			WritableFile* lfile = NULL;
			//����һ����־�ļ�
			s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
			if(!s.ok()){ //������־�ļ�ʧ��
				versions_->ReuseFileNumber(new_log_number);
				break;
			}

			delete log_;
			delete logfile_;
			logfile_ = lfile;
			//�����µ�LOG�ļ�
			logfile_number_ = new_log_number;
			log_  = new log::Writer(lfile);
			//��mem_ת�Ƶ�imm�У���ΪmemҪ��Ϊtable Compact���ļ��У�Ϊ�˲�Ӱ��д����ת�Ƶ�imm����
			imm_ = mem_;
			has_imm_.Release_Store(imm_);

			//���´���һ��mem table
			mem_ = new MemTable(internal_comparator_);
			mem_->Ref();
			force = false;

			//���Compact
			MaybeScheduleCompaction();
		}
	}

	return s;
}

//�ⲿ״̬��Ϣ��ѯ�ӿ�
bool DBImpl::GetProerty(const Slice& property, std::string* value)
{
	value->clear();

	MutexLock l(&mutex_);

	Slice in = property;
	Slice prefix("leveldb.");

	if(!in.starts_with(prefix))
		return false;

	in.remove_prefix(prefix.size());
	
	if(!in.starts_with("num-files-at-level")){ //level����ļ���
		in.remove_prefix(strlen("num-files-at-level"));

		uint64_t level;
		bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
		if(!ok || level >= config::kNumLevels)
			return false;
		else{
			char buf[100];
			snprintf(buf, sizeof(buf), "%d",
				versions_->NumLevelFiles(static_cast<int>(level)));
			*value = buf;
			return true;
		}
	}
	else if(in == "stats"){ //Compact״̬��Ϣ
		char buf[200];
		snprintf(buf, sizeof(buf),
			"                               Compactions\n"
			"Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
			"--------------------------------------------------\n"
			);
		value->append(buf);
		for (int level = 0; level < config::kNumLevels; level++) {
			int files = versions_->NumLevelFiles(level);
			if (stats_[level].micros > 0 || files > 0) {
				snprintf(
					buf, sizeof(buf),
					"%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
					level,
					files,
					versions_->NumLevelBytes(level) / 1048576.0,
					stats_[level].micros / 1e6,
					stats_[level].bytes_read / 1048576.0,
					stats_[level].bytes_written / 1048576.0);
				value->append(buf);
			}
		}
		return true;
	}
	else if(in == "sstables"){ //��ǰ��sstable
		*value = versions_->current()->DebugString();
		return true;
	}

	return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes)
{
	Version* v;
	{
		//��õ�ǰ��version
		MutexLock l(&mutex_);
		versions_->current()->Ref();
		v = versions_->current();
	}

	for(int i = 0; i < n; i ++){
		InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
		InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);

		uint64_t start = versions_->ApproximateOffsetOf(v, k1);
		uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
		sizes[i] = (limit >= start ? limit - start : 0);
	}

	{
		MutexLock l(&mutex_);
		v->Unref();
	}
}

Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value)
{
	WriteBatch batch;
	batch.Put(key, value);
	return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, const Slice& key)
{
	WriteBatch batch;
	batch.Delete(key);
	return Write(opt, &batch);
}

DB::~DB()
{

}

//�����ݿ�
Status DB::Open(const Options& opt, const std::string& dbname, DB** dbptr)
{
	*dbptr = NULL;

	DBImpl* impl = new DBImpl(opt, dbname);
	impl->mutex_.Lock();

	VersionEdit edit;
	//��ȡ��־���ָ�version��mem table
	Status s = impl->Recover(&edit);
	if(s.ok()){
		//��һ����־�ļ�
		uint64_t new_log_number = impl->versions_->NewFileNumber();
		WritableFile* lfile;
		s = opt.env->NewWritableFile(LogFileName(dbname, new_log_number), &lfile);
		if(s.ok()){
			edit.SetLogNumber(new_log_number);
			impl->logfile_ = lfile;
			impl->logfile_number_ - new_log_number;
			//����һ��log writer
			impl->log_ = new log::Writer(lfile);
			//��־д��һ��manifest�ļ��������п���
			s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
		}

		//ɾ���������ļ�(�п������ϴε���־�ļ�)������̽�ԵĽ���Compact File
		if(s.ok()){
			impl->DeleteObsoleteFiles();
			impl->MaybeScheduleCompaction();
		}
	}
	
	impl->mutex_.Unlock();
	if(s.ok())
		*dbptr = impl;
	else
		delete impl;

	return s;
}

Snapshot::~Snapshot()
{
}

Status DestroyDB(const std::string& dbname, const Options& options)
{
	Env* env = options.env;
	std::vector<std::string> filenames;

	env->GetChildren(dbname, &filenames);
	if(filenames.empty())
		return Status::OK();

	FileLock* lock;
	const std::string lockname = LockFileName(dbname);
	Status result = env->LockFile(lockname, &lock);
	if(result.ok()){
		uint64_t number;
		FileType type;
		for(size_t i = 0; i < filenames.size(); i ++){
			if(ParseFileName(filenames[i], &number, &type) && type != kDBLockFile){ //��ɾ��block file
				Status del =  env->DeleteFile(dbname + "/" + filenames[i]);
				if(result.ok() && !del.ok())
					result = del;
			}
		}

		//�ļ�ɾ��
		env->UnlockFile(lock);
		env->DeleteFile(lockname);
		env->DeleteDir(dbname);
	}

	return result;
}

};


