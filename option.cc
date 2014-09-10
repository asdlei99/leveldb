#include "options.h"
#include "comparator.h"
#include "env.h"

namespace leveldb{

//�������ݿ��Ĭ������
Options::Options()
	: comparator(ByteWiseComparator())
	, create_if_missing(false)
	, error_if_exists(false)
	, paranoid_checks(false)
	, env(Env::Default())
	, info_log(NULL)
	, write_buffer_size(4 << 20) //4M
	, max_open_files(1000)
	, block_cache(NULL)
	, block_size(4096) //4K
	, block_restart_interval(16)
	, compression(kSnappyCompression) //Ĭ��snappyѹ��
	, filter_policy(NULL)
{
}

}//leveldb