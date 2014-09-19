#include "builder.h"
#include "filename.h"
#include "dbformat.h"
#include "table_cache.h"
#include "version_set.h"
#include "db.h"
#include "env.h"
#include "iterator.h"

namespace leveldb{

Status BuilderTable(const std::string& dbname, Env* env, const Options& opt, TableCache* table_cache,
					Iterator* iter, FileMetaData* meta)
{
	Status s;
	meta->file_size = 0;
	//��λ�������ǰ��
	iter->SeekToFirst();
	//����һ��/dbname/number.ldb���ļ�
	std::string fname = TableFileName(dbname, meta->number);
	if(iter->Valid()){
		//��ldb�ļ����д�
		WritableFile* file;
		s = env->NewWritableFile(fname, &file); //��һ����д���ļ�
		if(!s.ok())
			return s;

		//����һ��table builder����
		TableBuilder* builder = new TableBuilder(opt, file);
		//ȷ����С��key
		meta->smallest.DecodeFrom(iter->key());
		for(; iter->Valid(); iter->Next()){
			Slice key = iter->key();
			//ȷ������KEY
			meta->largest.DecodeFrom(key);
			//��key valueд�뵽table builder���У�������̻�д�ļ���
			builder->Add(key, iter->value());
		}

		if(s.ok()){
			//���д��block index����Ϣ���ļ�
			s = builder->Finish();
			if(s.ok()){
				//����ļ��Ĵ�С
				meta->file_size = builder->FileSize();
				assert(meta->file_size > 0);
			}
		}
		else //�ļ�����ʧ���ˣ�builder table�����ļ�
			builder->Abandon();

		delete builder;
		//�ļ���page cache��д�뵽����
		if(s.ok())
			s = file->Sync();

		if(s.ok())
			s = file->Close();

		delete file;
		file = NULL;

		if(s.ok()){
			//У��table�Ƿ����
			Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size);
			s = it->status();
			delete it;
		}
	}

	//���iter�Ƿ���Ч
	if(!iter->status().ok())
		s = iter->status();

	
	if(s.ok() && meta->file_size > 0){

	}
	else//��������iter��Ч�������ɵ��ļ�ɾ��
		env->DeleteFile(fname);

	return s;
}

};




