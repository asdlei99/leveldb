#include "table_cache.h"
#include "filename.h"
#include "env.h"
#include "table.h"
#include "coding.h"

namespace leveldb{

struct TableAndFile
{
	RandomAccessFile* file;
	Table* table;
};

static void DeleteEntry(const Slice& key, void* value)
{
	TableAndFile* tf = reinterpret_cast<TableAndFile *>(value);
	delete tf->table;
	delete tf->file;
	delete tf;
}

static void UnrefEntry(void* arg1 ,void* arg2)
{
	Cache* cache = reinterpret_cast<Cache*>(arg1);
	Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
	cache->Release(h); //LRU Cache�����ͷ�,������������ü�����Ϊ�ͷű�ʶ
}

TableCache::TableCache(const std::string& dbname, const Options* opt, int entries) : dbname_(dbname), options_(opt), env_(opt->env)
{
	cache_ = NewLRUCache(entries);
}

TableCache::~TableCache()
{
	delete cache_;
}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle)
{
	Status s;
	char buf[sizeof(file_number)];
	EncodeFixed64(buf, file_number);

	//ͨ���ļ���hash��ȷ��CACHE_handle,cache_������16��cache handler
	Slice key(buf, sizeof(buf));
	*handle = cache_->Lookup(key);
	//���cache��û�м�¼table����Ϣ�����ļ��е���table��Ϣ������¼��cache��
	if(handle == NULL){
		//����һ��//dbname/number.ldb�ļ���
		std::string fname = TableFileName(dbname_, file_number);
		RandomAccessFile* file = NULL;
		Table* table = NULL;

		s = env_->NewRandomAccessFile(fname, &file); //��һ�����д���ļ�
		if(!s.ok()){ //��ldb�ļ�ʧ��,���Դ�sst�ļ�
			std::string old_fname = SSTableFileName(dbname_, file_number);
			if(env_->NewRandomAccessFile(old_fname, &file).ok())
				s = Status.OK();
		}

		if(s.ok())
			s = Table::Open(*options_, file, file_size, &table); //���ļ��ж�ȡ���ݹ���TABALE

		if(!s.ok()){
			assert(table == NULL);
			delete file;
		}
		else{
			TableAndFile* tf = new TableAndFile();
			tf->file = file;
			tf->table = table;

			//����һ��cache��handle,����ΪTableAndFile,�൱�ڽ�table�Ļ�������Ԫ�������õ�cache����
			*handle = cache_->Insert(key, tf, 1, &DeleteEntry);
		}
	}

	return s;
}

//����һ��tow level iterator������
Iterator* TableCache::NewIterator(const ReadOptions& opt, uint64_t file_number, uint64_t file_size, Table** tableptr)
{
	if(tableptr != NULL)
		*tableptr = NULL;

	Cache::Handle* handle = NULL;
	Status s = FindTable(file_number, file_size, &handle); //�����һ��handle�����ü���
	if(!s.ok())
		return NewErrorIterator(s);

	//��cache�в��ҵ�value����TableAndFile�����ָ��
	Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
	Iterator* result = table->NewIterator(opt);  //����һ��two level iterator
	result->RegisterCleanup(&UnrefEntry, cache_, handle); //���ø��ͷŵ������Ļص���handle�����ü�������iter������ʱ�����UnrefEntry�ͷ�

	if(tableptr != NULL)
		*tableptr = table;

	return result;
}

//saver��һ��������arg,�ڶ���������k,��������������table�в��ҵ�value
Status TableCache::Get(const ReadOptions& opt, uint64_t file_number, uint64_t file_size, const Slice& k, void* arg, 
	void(*saver)(void*, const Slice&, const Slice&))
{
	Cache::Handle* handle = NULL;
	Status s = FindTable(file_number, file_size, &handle);
	if(s.ok()){
		Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
		s = t->InternalGet(opt, k ,arg, saver); //��table���в���һ��k��Ӧ��value,������saver���з���
		cache_->Release(handle);//�ͷ����ü���
	}

	return s;
}

void TableCache::Evict(uint64_t file_number)
{
	char buf[sizeof(file_number)];
	EncodeFixed64(buf, file_number);
	cache_->Erase(Slice(buf, sizeof(buf))); //��cache��ɾ��file_number��table, �����DeleteEntry��tableAndFile�ṹ�����ͷ�
}

};//leveldb



