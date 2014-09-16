#ifndef __LEVEL_DB_CACHE_H_
#define __LEVEL_DB_CACHE_H_

#include <stdint.h>
#include "slice.h"

namespace leveldb{

extern Cache* NewLRUCache(size_t capacity);

class Cache
{
public:
	Cache(){};
	virtual ~Cache(){};

	struct Handle {};

	virtual Handle* Insert(const Slice& key, void* value, size_t charge, void (*deleter)(const Slice& key, void* value)) = 0;
	virtual Handle* Lookup(const Slice& key) = 0;

	virtual void Release(Handle* handle) = 0;
	virtual void* Value(Handle* handle) = 0;
	virtual void Erase(const Slice& key) = 0;

	virtual uint64_t NewId() = 0;

private:
	//�о��Ƕ���ĺ�����leveldb�Ĵ�����û�ж�cache���⼸��������ʵ��
	void LRU_Remove(Handle* e);
	void LRU_Append(Handle* e);
	void LRU_Unref(Handle* e);

	struct Rep;
	Rep* rep_;

	//���ؿ����͸�ֵ
	Cache(const Cache&);
	void operator=(const Cache&);
};

};
#endif
