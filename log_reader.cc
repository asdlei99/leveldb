#include <stdio.h>
#include "log_reader.h"
#include "env.h"
#include "coding.h"
#include "crc32c.h"

namespace leveldb{
namespace log{

Reader::Reporter::~Reporter()
{
}

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset)
	: file_(file), reporter_(reporter), checksum_(checksum),
	  buffer_(), eof_(false), end_of_buffer_offset_(0),
	  initial_offset_(initial_offset), backing_store_(new char[kBlockSize])
{
	
}

Reader::~Reader()
{
	delete []backing_store_;
}

bool Reader::SkipToInitialBlock()
{
	//�������ڿ��ƫ��
	size_t offset_in_block = initial_offset_ % kBlockSize;
	uint64_t block_start_location = initial_offset_ - offset_in_block; //initial_offset_ƫ��λ��������������ܳ���
	
	//ĩβ������0�����ģ����Я��ͷ��Ϣ��7�ֽڣ�
	if(offset_in_block > kBlockSize - 6){
		offset_in_block = 0;
		block_start_location += kBlockSize;
	}

	//ȷ��������������ƫ��
	end_of_buffer_offset_ = block_start_location;

	if(block_start_location > 0){
		Status skip_status = file_->Skip(block_start_location); //����Ƕ��ļ��з���block_start_location��ô����λ�ã�����о�seek�����ƫ����
		if(!skip_status.ok()){
			ReportDrop(block_start_location, skip_status);
			return false;
		}
	}
	
	return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch)
{
	if(last_record_offset_ < initial_offset_){
		if(!SkipToInitialBlock())
			return false;
	}

	scratch->clear();
	record->clear();
	bool in_fragment_record = false;

	uint64_t prospective_record_offset = 0;

	//32KΪһ���飬һ�����ж��fragment
	Slice fragment;
	while(true){
		uint64_t physical_record_offset = end_of_buffer_offset_ - buffer_.size();
		
		//��÷�Ƭ����
		const unsigned int record_type = ReadPhysicalRecord(&fragment);
		switch(record_type)
		{
		case kFullType:
			if(in_fragment_record){
				if(scratch->empty())
					in_fragment_record = false;
				else
					ReportCorruption(scratch->size(), "partial record without end(1)");
			}
			prospective_record_offset = physical_record_offset;
			scratch->clear();
			
			*record = fragment;
			last_record_offset_ = physical_record_offset;
			return true;

		case kFirstType:
			if(in_fragment_record){ //����fragment��ʶ����first middle last�ȶ��fragment��ɵ�һ��log��
				if(scratch->empty()){
					in_fragment_record = false;
				}
				else{
					ReportCorruption(scratch->size(), "partial record without end(2)");
				}
			}

			prospective_record_offset = physical_record_offset;
			scratch->assign(fragment.data(), fragment.size());
			in_fragment_record = true;
			break;

		case kMiddleType:
			if(!in_fragment_record) //��������ʶ��middle��һ��������
				ReportCorruption(fragment.size(), "missing start of fragmented record(1)");
			else
				scratch->append(fragment.data(), fragment.size());
			break;

		case kLastType:
			if(!in_fragment_record){ //LastTypeǰ��һ����first��middle
				ReportCorruption(fragment.size(), "missing start of fragmented record(2)");
			}
			else{
				scratch->append(fragment.data(), fragment.size());
				*record = Slice(*scratch);
				last_record_offset_ = prospective_record_offset;
				return true;
			}
			break;

		case kEof:
			if(in_fragment_record){ //�����������ģ������ļ���ȡ����ĩβ�����Ǹ����̴���
				ReportCorruption(scratch->size(), "partial record without end(3)");
				scratch->clear();
			}
			return false;

		case kBadRecord:
			if (in_fragment_record){
				ReportCorruption(scratch->size(), "error in middle of record");
				in_fragment_record = false;
				scratch->clear();
			}
			break;

		default:
			{
				char buf[40];
				snprintf(buf, sizeof(buf), "unknown record type %u", record_type);
				ReportCorruption((fragment.size() + (in_fragment_record ? scratch->size() : 0)), buf);
				in_fragment_record = false;
				scratch->clear();

				break;
			}
		}
	}

	return false;
}

uint64_t Reader::LastRecordOffset()
{
	return last_record_offset_;
}

void Reader::ReportCorruption(size_t bytes, const char* reason)
{
	ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(size_t bytes, const Status& reason)
{
	if(reporter_ != NULL && end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_){
		reporter_->Corruption(bytes, reason);
	}
}

unsigned int Reader::ReadPhysicalRecord(Slice* result)
{
	while(true){
		if(buffer_.size() < kHeaderSize){
			if(!eof_){
				buffer_.clear();
				//���ļ��ж���һ�����ݵ�buffer_�ڴ���
				Status s = file_->Read(kBlockSize, &buffer_, backing_store_);
				end_of_buffer_offset_ += buffer_.size();
				if(!s.ok()){ //��ȡʧ�ܣ�ֱ�ӷ���ĩβ��ʶ
					buffer_.clear();
					ReportDrop(kBlockSize, s);
					eof_ = true;
					return kEof;
				}
				else if(buffer_.size() < kBlockSize){//����ĩβ��
					eof_ = true;
				}
				continue;
			}
			else if(buffer_.size() == 0){ //��������������
				return kEof;
			}
			else{ //��������������
				size_t drop_size = buffer_.size();
				buffer_.clear();
				ReportCorruption(drop_size, "truncated record at end of file");
				return kEof;
			}
		}

		//��log ���ͷ���н���
		const char* header = buffer_.data();
		const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
		const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
		const unsigned int type = header[6];
		const uint32_t length = a | (b << 8);	
		if(kHeaderSize + length > buffer_.size()){ //kHeaderSize + length һ��Ҫ���ڻ��ߴ��ڻ����������ݿ鳤�ȣ��ֶ�fragment���п��ܻ���ڣ�
			size_t drop_size = buffer_.size();
			buffer_.clear();
			ReportCorruption(drop_size, "bad record length");
			return kBadRecord;
		}

		if(type == kZeroType && length == 0){
			buffer_.clear();
			return kBadRecord;
		}

		//У��LOG ���ݿ��SRC
		if(checksum_){
			uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
			uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
			if(actual_crc != expected_crc){
				size_t drop_size = buffer_.size();
				buffer_.clear();
				ReportCorruption(drop_size, "checksum mismatch");
			}
		}
		//�ƶ�slice�е����ݣ�ʣ�¶�������ݣ�Ȼ������Ӧ�ĳ���У��
		buffer_.remove_prefix(kHeaderSize + length);
		if(end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length < initial_offset_){
			result->clear();
			return kBadRecord;
		}

		//ƴ�շ��ؽ��
		*result = Slice(header + kHeaderSize, length);
		return type;
	}
}

};//log
};//leveldb




