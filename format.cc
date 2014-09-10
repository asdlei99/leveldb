#include "format.h"
#include "env.h"
#include "port.h"
#include "block.h"
#include "coding.h"
#include "crc32c.h"

namespace leveldb{

//��blockhandle���뵽dst����
void BlockHandle::EncodeTo(std::string* dst) const
{
	assert(offset_ != ~static_cast<uint64_t>(0));
	assert(size_  != ~static_cast<uint64_t>(0));
	PutVarint64(dst, offset_);
	PutVarint64(dst, size_);
}

//��input���н���block handle����Ϣ
Status BlockHandle::DecodeFrom(Slice* input)
{
	if(GetVarint64(input, &offset_) && GetVarint64(input, &size_)) //����ɹ�
		return Status::OK();
	else
		return Status::Corruption("bad block handle");
}

//��footer���б��뵽dst
void Footer::EncodeTo(std::string* dst) const
{
#ifndef NDEBUG
	const size_t orginal_size = dst->size();
#endif
	metaindex_handle_.EncodeTo(dst); //16�ֽ�
	index_handle_.EncodeTo(dst); //16�ֽ�

	dst->resize(2 * BlockHandle::kMaxEncodedLength);
	//д��һ��������
	PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu)); //4�ֽ�
	PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32)); //4�ֽ�

	assert(dst->size() == orginal_size + kEncodedLength);
}

//��input����footer����Ϣ
Status Footer::DecodeFrom(Slice* input)
{
	const char* magic_ptr = input->data() + kEncodedLength - 8;
	const uint32_t magic_lo = DecodeFixed32(magic_ptr);
	const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
	//������У��
	const uint64_t magic = (static_cast<uint64_t>(magic_hi) << 32 | static_cast<uint64_t>(magic_lo));
	if(magic != kTableMagicNumber){
		return Status::InvalidArgument("not an sstable(bad magic number)");
	}

	//����meta handler
	Status result = metaindex_handle_.DecodeFrom(input);
	if(result.ok()) //����handler
		result = index_handle_.DecodeFrom(input);

	if(result.ok()){
		const char* end = magic_ptr + 8;
		*input = Slice(end, input->data() + input->size() - end); //����input��ֵ��ȥ����ͷ��ֵ
	}
}

Status ReadBlock(RandomAccessFile* file, const ReadOptions& options, const BlockHandle& handle, BlockContents* result)
{
	result->data = Slice();
	result->cachable = false;
	result->heap_allocated = false;

	size_t n = static_cast<size_t>(handle.size());
	char* buf = new char[n + kBlockTrailerSize];
	//��file������ƫ��λ�ö�ȡn + kBlockTrailerSize���ȵ����ݵ�buf�У�������contents
	Slice contents;
	Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
	if (!s.ok()) {
		delete[] buf;
		return s;
	}

	const char* data = contents.data();
	if(options.verify_checksums){
		const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1)); //���CRC
		const uint32_t actual = crc32c::Value(data, n + 1); //����DATA��CRC
		if(actual != crc){ //CRCУ��
			delete []buf;
			s = Status::Corruption("block checksum mismatch");
			return s;
		}
	}

	switch(data[n]){
	case kNoCompression: //��ѹ������
		if(data != buf){ //MMAPģʽ
			delete []buf;
			result->data = Slice(data, n);
			result->heap_allocated = false;
			result->cachable = false;
		}
		else{ //preadģʽ
			result->data = Slice(buf, n);
			result->heap_allocated = true;
			result->cachable = true;
		}
		break;

	case kSnappyCompression:{ //snappyѹ��
			size_t ulength = 0;
			if(!port::Snappy_GetUncompressedLength(data, n, &ulength)){ //snappy ��ѹ����
				delete []buf;
				return Status::Corruption("corrupted compressed block contents");
			}

			//�������ݽ�ѹ
			char* ubuf = new char[ulength];
			if(!port::Snappy_Uncompress((data, n, ubuf))){
				delete []buf;
				delete []ubuf;
				return Status::Corruption("corrupted compressed block contents");
			}

			delete buf;

			result->data = Slice(ubuf, ulength);
			result->heap_allocated = true;
			result->cachable = true;
		}
		break;

	default:
		delete []buf;
		return Status::Corruption("bad block type");
	}

	return Status::OK();
}


} //leveldb
