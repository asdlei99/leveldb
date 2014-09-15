#ifndef __LEVEL_DB_SKIPLIST_H_
#define __LEVEL_DB_SKIPLIST_H_

#include <assert.h>
#include <stdlib.h>
#include "port.h"
#include "arena.h"
#include "random.h"

namespace leveldb{

class Arena;


template<typename Key, class Comparator>
class SkipList
{
private:
	struct Node;

public:
	explicit SkipList(Comparator cmp, Arena* arena);
	void Insert(const Key& key);
	void Contains(const Key& key) const;

	class Iterator
	{
	public:
		Iterator(const SkipList* list);
		bool Valid() const;
		const Key& key() const;
		void Next();
		void Prev();
		void Seek(const Key& target);
		void SeekToFirst();
		void SeekToLast();

	private:
		const SkipList* list;
		Node* node_;
	};

private:
	enum{
		kMaxHeight = 12,
	};

	Comparator const compare_;
	Arena* const arena_;
	Node* const head_;

	port::AtomicPointer max_height_;

	inline int GetMaxHeight() const{
		return static_cast<int>(reinterpret_cast<intptr_t>(max_height_.NoBarrier_Load()));
	}

	Random rnd_;

private:
	Node* NewNode(const key& key, int height);
	int RandomHeigth();
	bool Equal(const Key& a, const Key& b) const 
	{
		return (compare_(a,b) == 0);
	}

	bool KeyIsAfterNode(const Key& key, Node* n) const;
	Node* FindGreaterOrEqual(const Key& key, Node** prev) const;
	Node* FindLessThan(const Key& key) const;
	Node* FindLast() const;

	SkipList(const SkipList&);
	void operator=(const SkipList&);
};

template<typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node
{
	explicit Node(const Key& k) : key(k){};

	Key const key;
	Node* Next(int n)
	{
		assert(n >= 0);
		return reinterpret_cast<Node*>(next_[n].Acquire_Load()); //���ڴ������µ���next_[n],����cpu cache��һ��
	}

	void SetNext(int n, Node* x)
	{
		next_[n].Release_Store(x); //��д���ڴ���
	}

	Node* NoBarrier_Next(int n)
	{
		assert(n >= 0);
		return reinterpret_cast<Node*>(next_[n].NoBarrier_Load()); //ֱ�Ӵӱ����������п��ܱ������ڴ��У��п��ܱ����ڼĴ�������CPU CACHE����
	}

	void NoBarrier_SetNext(int n, Node* x)
	{
		 next_[n].NoBarrier_Store(x);
	}

private:
	port::AtomicPointer next_[1];
};

//��������һ��NODE,
template<typename Key, class Comparator>
typedef SkipList<Key, Comparator>::Node*
	SkipList<Key, Comparator>::NewNode(const Key& key, int height)
{
	char* mem = arena_->AllocateAligned(sizeof(Node) + sizeof(port::AtomicPointer) * (height - 1));
	return new (mem) Node(key);
}

template<typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list)
{
	list_ = list;
	node_ = NULL;
}

template<typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const
{
	return Node != NULL;
}

template<typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const
{
	assert(Valid());
	return node_->key;
}

template<typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next()
{
	assert(Valid());
	node_ = node_->Next(0);
}

template<typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev()
{
	assert(Valid());
	node_ = list_->FindLessThan(node_->key);
	if(node_ == list_->head_){
		node_ = NULL;
	}
}

template<typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target)
{
	node_ = list_->FindGreaterOrEqual(target, NULL);
}

template<typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst()
{
	node_ = list_->head_->Next(0);
}

template<typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast()
{
	node_ = list_->FindLast();
	if(node_ == list_->head_)
		node_ = NULL;
}

template<typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeigth()
{
	static const unsigned int kBranching = 4;
	int height = 1;
	while(height < kMaxHeight && (rnd_.Next() % kBranching))
		height ++;

	assert(height > 0);
	assert(height <= kMaxHeight);
	return height;
}

//�ж�key�Ƿ���node�ĺ��棬��Ϊskiplist�������(n.key < key)
template<typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const
{
	return (n != NULL) && (compare_(n->key, key) < 0);
}

//�ҵ�key�������е�λ��,���ӶȽ���O(logN)
template<typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key, Node** prev) const
{
	Node* x = head_;
	int level = GetMaxHeight() - 1; //�������Ծ�ĵط���ʼ��
	while(true){
		Node* next = x->Next(level);
		if(KeyIsAfterNode(key, next)){ //key���Ǳ�next�󣬼��������
			x = next;
		}
		else{
			if(prev != NULL) //����ǰһ����Ľڵ㣬һ�齨���㼶��ϵ
				prev[level] = x;

			if(level == 0) //�Ѿ����������һ���ˣ���λ�þ���
				return next;
			else  //����һ������
				level --;
		}
	}
}

template<typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const
{
	Node* x = head_;
	int level = GetMaxHeight() - 1; //����߲�һֱ����ײ��ƶ�
	while(true){
		Node* next = x->Next(level);
		if(next == NULL){
			if(level == 0) //�Ѿ�����ײ�����һ��node
				return x;
			else
				level --;
		} 
		else 
			x = next;
	}
} 

template<typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
	: compare_(cmp), arena_(arena), head_(NewNode(0, kMaxHeight)),
	max_height_(reinterpret_cast<void*>(1)), rnd_(0xdeadbeef)
{
	for(int i = 0; i < kMaxHeight; i++)
		head_->SetNext(i, NULL);
}

template<typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key)
{
	Node* prev[kMaxHeight];
	//�ҵ�key�������е�λ��
	Node* x = FindGreaterOrEqual(key, prev);
	assert(x == NULL || !Equal(key, x->key));

	int height = RandomHeigth();
	if(height > GetMaxHeight()){
		for(int i = GetMaxHeight(); i < height; i ++)
			prev[i] = head_;

		//�����µ����ĸ߶�
		max_height_.NoBarrier_Store(reinterpret_cast<void*>(height));
	}

	//�½�һ��key node
	x = NewNode(key, height);
	for(int i = 0; i < height; i ++){
		x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i)); //��δ��skip list�У����Բ���ǿ�ƻ�д
		prev[i]->SetNext(i, x); //prev���������У�����ǿ��CPU��д�ڴ�
	}
}

template<typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const
{
	Node* x = FindGreaterOrEqual(key, NULL); //�ҵ���Ӧkey��skip listλ��
	if(x != NULL && Equal(key, x->key))
		return true;
	else
		return false;
}

};

#endif
