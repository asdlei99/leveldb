#include "arena.h"

namespace leveldb{

static const int kBlockSize = 4096;

Arena::Arena()
{
	blocks_memory_ = 0;
	alloc_ptr_ = NULL;
	alloc_bytes_remaining_ = 0;
}

Arena::~Arena()
{
	for(size_t i = 0; i < blocks_.size(); i ++){
		delete []blocks_[i];
	}
}

char* Arena::AllocateFallback(size_t bytes)
{
	if(bytes > kBlockSize / 4){ // > 1KB,ֱ�ӷ����ͬ��С���ڴ�
		return AllocateNewBlock(bytes);
	}

	//�½�һ��4K���ڴ�ҳ
	alloc_ptr_ = AllocateNewBlock(kBlockSize);
	alloc_bytes_remaining_ = kBlockSize;

	//���ڴ�ҳ�Ϸ���bytes�Ĵ�С�ռ��������
	char* result = alloc_ptr_;
	alloc_ptr_ += bytes;
	alloc_bytes_remaining_ -= bytes;

	return result;
}

char* Arena::AllocateAligned(size_t bytes)
{
	const int align = (sizeof(void*) > 8) ? sizeof(void *) : 8; //�ж�CPU���������ֽ�����32λ���� 64λ����
	assert((align & (align - 1)) == 0);
	//����alloc_ptr_��align�����λ��
	size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
	size_t slop = (current_mod == 0 ? 0 : align - current_mod);
	size_t needed = bytes + slop; //����align����

	//�����ڴ�����
	char* result;
	if(needed <= alloc_bytes_remaining_){
		result = alloc_ptr_ + slop;
		alloc_ptr_ += needed;
		alloc_bytes_remaining_ -= needed;
	}
	else{
		result = AllocateFallback(bytes);
	}
	//������
	assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
	return result;
}

//�����µ�ҳ
char* Arena::AllocateNewBlock(size_t block_bytes)
{
	char* result = new char[block_bytes];
	blocks_memory_ += block_bytes;
	blocks_.push_back(result);
	return result;
}

}
