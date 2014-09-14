#include "table_builder.h"
#include <assert.h>
#include "comparator.h"
#include "env.h"
#include "filter_policy.h"
#include "options.h"
#include "filter_block.h"
#include "format.h"
#include "coding.h"
#include "crc32c.h"
#include "block_builder.h"

namespace leveldb{

struct TableBuilder::Rep{
	Options options;				//����ѡ��
	Options index_block_options;	//index����ѡ���optionsһ��
	WritableFile* file;				//�ļ�����
	uint64_t offset;				//�ļ�ƫ��
	Status status;					//������
	BlockBuilder data_block;		//��ǰ�����ݿ�
	BlockBuilder index_block;		//��ǰ��������
	std::string last_key;			//����KEY??
	int64_t num_entries;			//key value����
	bool closed;
	FilterBlockBuilder* filter_block;

	bool pending_index_entry;
	BlockHandle pending_handle;

	std::string compressed_output;		//��Ϊsnappy������ʱ�洢�ĵط�

	Rep(const Options& opt, WritableFile* f) : options(opt), index_block_options(opt),
		file(f), offset(0), data_block(&options), index_block(&options),
		num_entries(0), closed(false), 
		filter_block(opt.filter_policy == NULL ? NULL : new FilterBlockBuilder(opt.filter_policy)),
		pending_index_entry(false)
	{
		index_block_options.block_restart_interval = 1;
	}
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
	: rep_(new Rep(options, file))
{
	if(rep_->filter_block != NULL)
		rep_->filter_block->StartBlock(0); //����������
}

TableBuilder::~TableBuilder()
{
	assert(rep_->closed);
	delete rep_->filter_block;
	delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& opt)
{
	if(opt.comparator != rep_->options.comparator) //�Ƚ�����ƥ��
		return Status::InvalidArgument("changing comparator while building table");

	rep_->options = opt;
	rep_->index_block_options = opt;
	rep_->index_block_options.block_restart_interval = 1; //����block����KEY����

	return Status::OK();
}

Status TableBuilder::status() const
{
	return rep_->status;
}

bool TableBuilder::ok() const
{
	return status().ok();
}

//�������ݣ���
void TableBuilder::Abandon() 
{
	Rep* r = rep_;
	assert(!r->closed);
	r->closed = true;
}

void TableBuilder::Add(const Slice& key, const Slice& value)
{
	Rep* r = rep_;
	assert(r->closed);

	//ǰ��IO�����쳣�ˣ����ܼ�������
	if(!ok())
		return;

	if(r->num_entries > 0)
		assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0); //key�����rep�е�last key��

	if(r->pending_index_entry){
		assert(r->data_block.empty());
		r->options.comparator->FindShortestSeparator(&r->last_key, key); //�ҵ�r->last_key��С��ͬ���ַ�����С��key�����ı�last key,
		//����һ��������
		std::string handle_encoding;
		r->pending_handle.EncodeTo(&handle_encoding); //����pending handle
		r->index_block.Add(r->last_key, Slice(handle_encoding));//���������ݼ��뵽index block����
		r->pending_index_entry = false;
	}

	//��key���뵽��������
	if(r->filter_block != NULL)
		r->filter_block->AddKey(key);

	//��last_key = key
	r->last_key.assign(key.data(), key.size());
	r->num_entries ++;
	//���뵽���ݿ���
	r->data_block.Add(key, value);
	
	//����������ݺ��block size
	const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
	if(estimated_block_size >= r->options.block_size){ //�������ÿ�Ĵ�С����Ҫ�Կ�Flush
		Flush();
	}
}

//�����ݽ���flush�̻���������
void TableBuilder::Flush()
{
	Rep* r = rep_;
	assert(r->closed);

	//�д���IO����
	if(!ok())
		return;

	//���ݿ��ǿյ�
	if(r->data_block.empty())
		return;
	
	//��pending index entry����У��
	assert(!r->pending_index_entry);

	WriteBlock(&r->data_block, &r->pending_handle);
	if(ok()){
		r->pending_index_entry = true; //д���־��
		r->status = r->file->Flush();
	}

	//�����¿�һ���������Ķ�
	if(r->filter_block != NULL){
		r->filter_block->StartBlock(r->offset);
	}
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle)
{
	assert(ok());
	Rep* r = rep_;
	Slice raw = block->Finish(); //�����ݿ���д���������Ƹ�raw

	Slice block_contents;
	CompressionType type = r->options.compression;
	switch(type){
	case kNoCompression: //��ѹ��
		block_contents = raw;
		break;

		//snappyѹ��
	case kSnappyCompression:{
			std::string* compressed = &r->compressed_output;
			if(port::Snappy_Compress(raw.data(), raw.size(), compressed) &&
				compressed->size() < raw.size() - (raw.size() / 8u)){
				block_contents = *compressed;
			}
			else{ //ѹ��ʧ�ܣ�����ѹ��ģʽд��
				block_contents = raw;
				type = kNoCompression;
			}
			break;
		}
	}

	WriteRawBlock(block_contents, type, handle);
	r->compressed_output.clear();
	block->Reset(); //��д��Ŀ����λ�����½����µ�����
}

void TableBuilder::WriteRawBlock(const Slice& block_contents, CompressionType type, BlockHandle* handle)
{
	Rep* r = rep_;
	//�����������λ�ü�¼, pending handle
	handle->set_offset(r->offset);
	handle->set_size(block_contents.size());
	//����д���ļ�page cache
	r->status = r->file->Append(block_contents);
	if(r->status.ok()){
		//����formatβ����
		char trailer[kBlockTrailerSize];
		trailer[0] = type; //��¼ѹ��ģʽ
		//����CRC�����뵽trailer��
		uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
		crc = crc32c::Extend(crc, trailer, 1);
		EncodeFixed32(trailer + 1, crc32c::Mask(crc));
		//βд��
		r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
		if(r->status.ok())
			r->offset += block_contents.size() + kBlockTrailerSize; //�����ļ�ƫ����
	}
}

Status TableBuilder::Finish()
{
	Rep* r = rep_;
	Flush(); //��д���ļ�

	assert(!r->closed);
	r->closed = true;

	BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;
	
 // Write filter block
  if (ok() && r->filter_block != NULL) {
		WriteRawBlock(r->filter_block->Finish(), kNoCompression, &filter_block_handle);
  }

	//д��meta index block,��Ҫ�ǹ��˵����ֺ͹�������λ��
	if(ok()){
		BlockBuilder meta_index_block(&r->options);
		if(r->filter_block != NULL){
			//����һ��������key
			std::string key = "filter.";
			key.append(r->options.filter_policy->name());

			std::string handle_encoding;
			filter_block_handle.EncodeTo(&handle_encoding);
			meta_index_block.Add(key, handle_encoding);
		}

		WriteBlock(&meta_index_block, &metaindex_block_handle);
	}

	//write index block
	if(ok()){
		if(r->pending_index_entry){ //�����һ�����offset����д��index block
			r->options.comparator->FindShortSuccessor(&r->last_key); //�ҵ�һ��������r->last_key���key
			std::string handle_encoding;
			r->pending_handle.EncodeTo(&handle_encoding); //��pending handle��λ�ý��б���
			r->index_block.Add(r->last_key, Slice(handle_encoding)); //��Ϊkey value���뵽index block��
			r->pending_index_entry = false;
		}
		//��������Ϣд���ļ���
		WriteBlock(&r->index_block, &index_block_handle);
	}

	//��metaindex_block_handle index_block_handle�����ݽ����ļ�д�� footer����д��
	if(ok()){
		Footer footer;
		footer.set_metaindex_handle(metaindex_block_handle);
		footer.set_index_handle(index_block_handle);
		std::string footer_encoding;
		footer.EncodeTo(&footer_encoding);
		r->status = r->file->Append(footer_encoding);
		if(r->status.ok())
			r->offset += footer_encoding.size();
	}

	return r->status;
}

uint64_t TableBuilder::NumEntries() const
{
	return rep_->num_entries;
}

uint64_t TableBuilder::FileSize() const
{
	return rep_->offset;
}

};//leveldb




