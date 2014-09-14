#include "table.h"
#include "cache.h"
#include "comparator.h"
#include "env.h"
#include "filter_policy.h"
#include "options.h"
#include "filter_block.h"
#include "format.h"
#include "two_level_iterator.h"
#include "coding.h"

namespace leveldb{

struct Table::Rep
{
	Options options;		//����ѡ��
	Status status;			//������
	
	RandomAccessFile* file;	//�ļ����
	uint64_t cache_id;		//cache id

	FilterBlockReader* filter; //���������
	const char* filter_data;	

	BlockHandle metaindex_handle;
	Block* index_block;

	~Rep()
	{
		delete filter;
		delete []filter_data;
		delete index_block;
	}
};

//��Ӧtable builder�е�Finish����
Status Table::Open(const Options& options, RandomAccessFile* file, uint64_t size, Table** table)
{
	*table = NULL;
	if(size < Footer::kEncodedLength)
		return Status::InvalidArgument("file is too short to be an sstable");

	char footer_space[Footer::kEncodedLength];
	Slice footer_input;
	//��ȡ���kEncodedLength���ֽڣ������footer�Ĵ洢�ռ�
	Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input, footer_space);
	if(!s.ok())
		return s;

	//��footer�Ľ���
	Footer footer;
	s = footer.DecodeFrom(&footer_input);
	if(!s.ok())
		return s;

	//��index block�Ķ�ȡ
	BlockContents contents;
	Block* index_block = NULL;
	s = ReadBlock(file, ReadOptions(), footer.index_handle(), &contents);
	if(s.ok()){
		index_block = new Block(contents);
	}

	if(s.ok()){
		Rep* r = new Table::Rep;
		r->options = options;
		r->file = file;
		r->metaindex_handle = footer.metaindex_handle();
		r->index_block = index_block;
		r->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
		r->filter_data = NULL;
		r->filter = NULL;
		*table = new Table(r);
		//��ȡmeta index block
		(*table)->ReadMeta(footer);
	}
	else if(index_block != NULL){
		delete index_block;
	}

	return s;
}

//��meta index block�Ķ�ȡ
void Table::ReadMeta(const Footer& footer)
{
	if(rep_->options.filter_policy == NULL)
		return;

	//��ȡmeta index block������
	ReadOptions opt;
	BlockContents contents;
	if(!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()){
		return;
	}

	Block* meta = new Block(contents);

	//�Թ�������Ϣ�Ķ�ȡ
	Iterator* iter = meta->NewIterator(BytewiseComparator());
	std::string key = "filter.";
	key.append(rep_->options.filter_policy->name());
	iter->Seek(key);
	if(iter->Valid() && iter->key() == Slice(key)){
		ReadFilter(iter->value()); //�Թ��������ձ�Ķ�ȡ��������������
	}

	delete iter;
	delete meta;
}

void Table::ReadFilter(const Slice& filter_handle_value)
{
	Slice v = filter_handle_value;
	BlockHandle filter_handle;
	if(!filter_handle.DecodeFrom(&v).ok()){
		return ;
	}

	ReadOptions opt;
	BlockContents block;
	if(!ReadBlock(rep_->file, opt, filter_handle, &block).ok()){
		return ;
	}

	//�ж��Ƿ���Ҫ�ͷ�data,�����mmap��ʽ�Ͳ���Ҫ�ͷ�
	if(block.heap_allocated){
		rep_->filter_data = block.data.data();
	}
	//����һ������������
	rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}

Table::~Table()
{
	delete rep_;
}

static void DeleteBlock(void* arg, void * ignored)
{
	delete reinterpret_cast<Block*>(arg);
}

static void DeleteCachedBlock(const Slice& key, void* value)
{
	Block* block = reinterpret_cast<Block*>(value);
	delete block;
}

static void ReleaseBlock(void* arg, void* h)
{
	Cache* cache = reinterpret_cast<Cache*>(arg);
	Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
	cache->Release(handle);
}

//��ȡһ���飬�������������
Iterator* Table::BlockReader(void* arg, const ReadOptions& opt, const Slice& index_value)
{
	Table* table = reinterpret_cast<Table*>(arg);
	Cache* block_cache = table->rep_->options.block_cache;
	Block* block = NULL;
	Cache::Handle* cache_handle = NULL;
	//������λ��
	BlockHandle handle;
	Slice input = index_value;

	Status s = handle.DecodeFrom(&input);
	if(s.ok()){
		BlockContents contents;
		if(block_cache != NULL){
			char cache_key_buffer[16];
			//id + offset = cache key
			EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
			EncodeFixed64(cache_key_buffer + 8, handle.offset());
			Slice key(cache_key_buffer, sizeof(cache_key_buffer));
			//��LRU CACHE����BLOCK
			cache_handle = block_cache->Lookup(key);
			if(cache_handle != NULL){//��CACHE���ҵ���
				block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
			}
			else{//��CACHE��û�ҵ����ڴ����ж�ȡ
				s = ReadBlock(table->rep_->file, opt, handle, &contents);
				if(s.ok()){
					s = ReadBlock(table->rep_->file, opt, handle, &contents);
					if(s.ok()){
							//���������
						block = new Block(contents);
						if(contents.cachable && opt.fill_cache) //���Ӵ����еõ���blockд�뵽lru cache����
							cache_handle = block_cache->Insert(key, block, block->size(), &DeleteCachedBlock);
					}
				}
			}
		}
		else{ //����ѡ����cache,ֱ�ӳ�����ж�ȡ
			s = ReadBlock(table->rep_->file, opt, handle, &contents);
			if(s.ok())
				block = new Block(contents);
		}
	}
	
	//����һ��block������
	Iterator* iter = NULL;
	if(block != NULL){
		iter = block->NewIterator(table->rep_->options.comparator);
		if(cache_handle == NULL) //cache��û��cache�Ĳ�ͬ��ʽ�ͷ�block
			iter->RegisterCleanup(&DeleteBlock, block, NULL);
		else
			iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
	}
	else
		iter = NewErrorIterator(s);

	return iter;
}
//����һ��two level iter
Iterator* Table::NewIterator(const ReadOptions& opt) const
{
	return NewTowLevelIterator(rep_->index_block->NewIterator(rep_->options.comparator), &Table::BlockReader,
		const_cast<Table*>(this), opt);
}

Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg, void (*saver)(void*, const Slice&, const Slice&))
{
	Status s;
	Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
	iiter->Seek(k);
	if(iiter->Valid()){
		Slice handle_value = iiter->value();
		FilterBlockReader* filter = rep_->filter;
		BlockHandle handle;

		//���������
		if(filter != NULL && handle.DecodeFrom(&handle_value).ok() && !filter->KeyMayMatch(handle.offset(), k)){
			//δ�ҵ�
		}
		else{
			//��ȡdata block������seek��KEY��λ�ã�������SAVE������������
			Iterator* block_iter = BlockReader(this, options, iiter->value());
			block_iter->Seek(k);
			if(block_iter->Valid())
				(*saver)(arg, block_iter->key(), block_iter->value());

			s = block_iter->status();
			delete block_iter;
		}
	}

	if(s.ok())
		s = iiter->status();

	delete iiter;

	return s;
}

//����key��Ӧbock��ƫ����offset
uint64_t Table::ApproximateOffsetOf(const Slice& key) const
{
	//��λ���������Ϣ
	Iterator* index_iter = rep_->index_block->NewIterator(rep_->options.comparator);
	index_iter->Seek(key);
	uint64_t result;
	//ͨ��������Ϣ��λƫ����
	if(index_iter->Valid()){
		BlockHandle handle;
		Slice input = index_iter->value();
		Status s = handle.DecodeFrom(&input);
		if(s.ok())
			result = handle.offset();
		else
			result = rep_->metaindex_handle.offset();
	}
	else{ //key�Ǳ�ʾindex block��KEY��ֱ�ӷ���meta block��ƫ����
		result = rep_->metaindex_handle.offset();
	}

	delete index_iter;
	return result;
}

};



