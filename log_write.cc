#include "log_write.h"
#include <stdint.h>
#include "env.h"
#include "coding.h"
#include "crc32c.h"

namespace leveldb{
namespace log{

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0)
{
	for(int i = 0; i <= kMaxRecordType; i++){
		char t = static_cast<char>(i);
		type_crc_[i] = crc32c::Value(&t, 1);
	}
}

Writer::~Writer()
{
}

Status Writer::AddRecord(const Slice& slice)
{
	const char* ptr = slice.data();
	size_t left = slice.size();

	Status s;
	bool begin = true;
	do{
		const int leftover = kBlockSize - block_offset_;
		assert(leftover >=0);

		//32KΪһ��
		if(leftover < kHeaderSize){
			if(leftover > 0){
				assert(kHeaderSize == 7);
				dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover)); //�����7���ֽڣ�ȫ������0,��32k�Ŀ�����
			}
			//��������һ���µ�32K��block
			block_offset_ = 0;
		}

		assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

		//��������õ��ֽ�����
		const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
		const size_t fragment_length = (left < avail) ? left : avail;

		//ʣ�µĳ�����һ�δ洢slice
		/*|kFullType|*/
		//ʣ�µĳ����޷��洢slice�ͻᰴ�����·�ʽ�洢
		/*|kFistType|kMiddleType1|kMiddleType2|....|kLastType|*/
		RecordType type;
		const bool end = (left == fragment_length);
		if(begin && end)
			type = kFullType; //��ʼλ�ý���־һ��д���ļ���
		else if(begin)
			type = kFirstType;//��һ���־����ʾ��ʼ��
		else if(end)
			type = kLastType; //���һ���־����ʾ��־������
		else	
			type = kMiddleType; //�м���־�����ܶ��

		//����д��fragment
		s = EmitPhysicalRecord(type, ptr, fragment_length);
		//λ�ý���ǰ��
		ptr += fragment_length;
		left -= fragment_length;
		//������־��ʼ��־
		begin = false;
	}while(s.ok() && left > 0);

	return s;
}

/*��־��Ƭ��ʽ:|crc32|fragment size|type|data|*/
Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr, size_t n)
{
	assert(n <= 0xffff);
	assert(block_offset_ + kHeaderSize + n <= kBlockSize);

	//��ʽ����־ͷ
	char buf[kHeaderSize];
	//�����ȴ��
	buf[4] = static_cast<char>(n & 0xff);
	buf[5] = static_cast<char>(n >> 8);
	//���fragment type����־��Ƭ���ͣ�
	buf[6] = static_cast<char>(t);

	//ǰ��4�ֽ���CRCУ����,����CRC
	uint32_t crc = crc32c::Extend(type_crc_[t], ptr, n);
	crc = crc32c::Mask(crc);
	EncodeFixed32(buf, crc);
	
	Status s = dest_->Append(Slice(buf, kHeaderSize));
	if(s.ok()){
		s = dest_->Append(Slice(ptr, n));
		if(s.ok())
			s = dest_->Flush();
	}
	//�ı�����д���ƫ����
	block_offset_ += kHeaderSize + n;
	return s;
}

};

};










