#ifndef __LEVEL_DB_OPTION_H_
#define __LEVEL_DB_OPTION_H_

namespace leveldb{

class Cache;
class Comparator;
class Env;
class Logger;
class FilterPolicy;
class Snapshot;

enum CompressionType
{
	kNoCompression		= 0x00,
	kSnappyCompression	= 0x01
};

struct Options
{
	//�Ƚ���
	const Comparator* comparator;
	//true - �������ݿⲻ���ڻ��߶�ʧ�����Զ�������
	bool create_if_missing;
	//true - �������ݿ���ڣ�������һ������
	bool error_if_exists;

	bool paranoid_checks;
	//PORT������������
	Env* env;
	
	//��־����
	Logger* info_log;

	size_t write_buffer_size;
	//���ļ��������Ŀ
	int max_open_files;
	//cache LRU CACHE
	Cache* block_cache;
	//���С
	size_t block_size;

	int block_restart_interval;
	//����ѹ������
	CompressionType compression;
	//����������bloom filter
	const FilterPolicy* filter_policy;

	Options();
};

//��ѡ��
struct ReadOptions
{
	//�����Ƿ����check sums��CRC��
	bool verfy_checksums;

	bool fill_cache;

	const Snapshot* snapshot;

	ReadOptions() : verfy_checksums(false), fill_cache(true), snapshot(NULL)
	{
	}
};

//дѡ��
struct WriteOptions
{
	//�ڴ��������ȫͬ����ʶ
	bool sync;
	WriteOptions() : sync(false){};
};

}//leveldb

#endif
