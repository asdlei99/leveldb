#include <string.h>
#include "coding.h"
#include "hash.h"

#ifndef FALLTHROUGH_INTENDED
#define FALLTHROUGH_INTENDED do{ }while(0)
#endif

namespace leveldb{
//HASH����
uint32_t Hash(const char* data, size_t size_t n, uint32_t seed)
{
	const uint32_t m = 0xc6a4a793;
	const uint32_t r = 24;
	const char* limit = data + n;
	uint32_t h = seed ^ (n * m);

	while(data + 4 <= limit){
		uint32_t w = DecodeFixed32(data);
		data += 4;
		h += w;
		h *= m;
		h ^= (h >> 16);
	}

	switch (limit - data) {
	case 3:
		h += data[2] << 16;
		FALLTHROUGH_INTENDED;
	case 2:
		h += data[1] << 8;
		FALLTHROUGH_INTENDED;
	case 1:
		h += data[0];
		h *= m;
		h ^= (h >> r);
		break;
	}

	return h;
}
}


