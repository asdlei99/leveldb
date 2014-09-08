#include "env.h"

namespace leveldb{

void Log(Logger* info_log, const char* format, ...)
{
	if(info_log != NULL){
		va_list ap;
		va_start(ap, format);
		info_log->Logv(format, ap);
		va_end(ap);
	}
}

//������д���ļ�������ò������
static Status DoWriteStringToFile(Env* env, const Slice& data, const std::string& fname, bool should_sync)
{
	WritableFile* file;
	//���һ���ļ�д����
	Status s = env->NewWritableFile(fname, &file);
	if(!s.ok())
		return s;
	//�����ļ�׷��
	s = file->Append(data);
	if(s.ok() && should_sync){ //ͬ��д��Ӳ��
		s = file->Sync();
	}

	if(s.ok()){
		s = file->Close();
	}

	delete file;
	if(!s.ok()){ //дʧ�ܣ�ֱ��ɾ���ļ���
		env->DeleteFile(fname);
	}

	return s;
}

Status WriteStringToFile(Env* env, const Slice& data, const std::string& fname)
{
	return DoWriteStringToFile(env, data, fname, false);
}

Status WriteStringToFileSync(Env* env, const Slice& data, const std::string& fname) 
{
	return DoWriteStringToFile(env, data, fname, true);
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data)
{
	data->clear();

	SequentialFile* file;
	//���һ���ļ�������
	Status s = env->NewSequentialFile(fname, &file);
	if(!s.ok()){
		return s;
	}

	//ÿ�ζ�ȡ���ݿ��С
	static const int kBufferSize = 8192;
	char* space = new char[kBufferSize];
	//�������ݶ�ȡ
	while(true){
		Slice fragment;
		s = file->Read(kBufferSize, &fragment, space);
		if(!s.ok()){ //��ȡʧ��
			break;
		}
		//ƴ������
		data->append(fragment.data(), fragment.size());
		if(fragment.empty()){
			break;
		}
	}

	delete []space;
	delete file;

	return s;
}



}




