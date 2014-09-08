#ifndef __LEVEL_DB_ARENA_H_
#define __LEVEL_DB_ARENA_H_

#include <vector>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

namespace leveldb {

class Arena
{
public:
	Arena();
	~Arena();

	char* Allocate(size_t bytes);
	char* AllocateAligned(size_t bytes);

	size_t MemoryUsage() const
	{
		return blocks_memory_ + blocks_.capacity() * sizeof(char *);
	};

private:
	char* AllocateFallback(size_t bytes);
	char* AllocateNewBlock(size_t block_bytes);

	Arena(const Arena&);
	void operator=(const Arena&);

private:
	char* alloc_ptr_;
	size_t alloc_bytes_remaining_;

	std::vector<char*> blocks_;
	size_t blocks_memory_;
};

//��Arena�еõ�һ���ڴ�
inline char* Arena::Allocate(size_t bytes)
{
	assert(bytes > 0);
	if(bytes <= alloc_bytes_remaining_){
		char* result = alloc_ptr_;
		alloc_ptr_ += bytes;
		alloc_bytes_remaining_ -=bytes;

		return result;
	}
	return AllocateFallback(bytes);
}

}

#endif
