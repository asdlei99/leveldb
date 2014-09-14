#include "merger.h"
#include "comparator.h"
#include "iterator.h"
#include "iterator_wrapper.h"

namespace leveldb{

enum Direction{
	kForward,
	kReverse,
};
class MerginIterator : public Iterator
{
public:
	MerginIterator(const Comparator* cp, Iterator** children, int n);
	virtual ~MerginIterator();

	virtual bool Valid() const;
	virtual void SeekToFirst();
	virtual void SeekToLast();
	virtual void Seek(const Slice& target);
	virtual void Next();
	virtual void Prev();
	virtual Slice key() const;
	virtual Slice value() const;
	virtual Status status() const;

private:
	void FindSmallest();
	void FindeLargest();

private:
	const Comparator* comparator_;
	int n_;

	IteratorWrapper* children_;
	IteratorWrapper* current_;

	Direction direction_;
};

MerginIterator::MerginIterator(const Comparator* cp, Iterator** children, int n)
	: comparator_(cp), children_(new IteratorWrapper[n]), n_(n), current_(NULL), direction_(kForward)
{
	for(int i = 0; i < n; i ++){
		children_[i].Set(children[i]);
	}
}

MerginIterator::~MerginIterator()
{
	delete []children_;
}

bool MerginIterator::Valid() const
{
	return (current_ != NULL);
}

void MerginIterator::SeekToFirst()
{
	for(int i = 0; i < n_; i++) //���е�iters���ص�first
		children_[i].SeekToFirst();

	//��iters����ѡ��С����Ϊ��һ��
	FindSmallest();
	direction_ = kForward;
}

void MerginIterator::SeekToLast()
{
	for(int i = 0; i < n_; i ++) //���е�iters����λ��LAST
		children_[i].SeekToLast();

	//��iters����ѡ������Ϊ���һ��
	FindeLargest();
	direction_ = kReverse;
}

void MerginIterator::Seek(const Slice& target)
{
	for(int i = 0; i < n_; i ++)
		children_[i].Seek(target);

	FindSmallest();
	direction_ = kForward;
}

//ָ����һ��(key,value)�������ڴ��ڵ�ǰkey,�п����ڲ�ͬ��IteratorWrapper��
void MerginIterator::Next()
{
	assert(Valid());
	if(direction_ != kForward){ //�ж��Ǵ�ͷ��β
		for(int i = 0; i < n_; i++){
			IteratorWrapper* child = &children_[i];
			if(child != current_){
				child->Seek(key()); //��λkey��ֵ
				if(child->Valid() && (comparator_->Compare(key(), child->key()) == 0)){
					child->Next();
				}
			}
		}
		direction_ = kForward;
	}

	current_->Next();
	//�ڵ�ǰ���е�children_->key������С��
	FindSmallest();
}

//ָ����һ��(key,value)��������С�ڵ�ǰkey,�п����ڲ�ͬ��IteratorWrapper��
void MerginIterator::Prev()
{
	assert(Valid());
	if(direction_ != kReverse){
		for(int i = 0; i < n_; i ++){
			IteratorWrapper* child = &children_[i];
			if(child != current_){
				child->Seek(key());
				if(child->Valid())//��ǰ���ƶ�
					child->Prev();
				else //��λ�����
					child->SeekToLast();
			}
		}
		direction_ = kReverse;
	}

	current_->Prev();
	//�ڵ�ǰ����children_->key()��������
	FindeLargest();
}

Slice MerginIterator::key() const
{
	assert(Valid());
	return current_->key();
}

Slice MerginIterator::value() const
{
	assert(Valid());
	return current_->value();
}

Status MerginIterator::status() const 
{
	Status status;
	for(int i = 0; i < n_; i ++){
		status = children_[i].status(); 
		if(!status.ok()) //����Ƿ��д���,�д�����������
			break;
	}

	return status;
}

//�����е�children�����ҵ���С��KEY
void MerginIterator::FindSmallest()
{
	IteratorWrapper* smallest = NULL;
	for(int i = 0; i < n_; i ++){
		IteratorWrapper* child = &children_[i];
		if(child->Valid()){
			if(smallest == NULL)
				smallest = child;
			else if(comparator_->Compare(child->key(), smallest->key()) < 0) //�ҵ���С��
				smallest = child;
		}
	}
	current_ = smallest;
}

//�����е�children�����ҵ�����KEY
void MerginIterator::FindeLargest()
{
	IteratorWrapper* largest = NULL;
	for(int i = n_ - 1; i > 0; i --){
		IteratorWrapper* child = &children_[i];
		if(child->Valid()){
			if(largest == NULL)
				largest = child;
			else if(comparator_->Compare(child->key(), largest->key()) > 0)
				largest = child;
		}
	}
	current_ = largest;
}

Iterator* NewMergingIterator(const Comparator* comparator, Iterator** list, int n)
{
	assert(n >= 0);
	if(n == 0)
		return NewEmptyIterator();
	else if(n == 1)
		return list[0];
	else
		return new MerginIterator(comparator, list, n); //����һ��MeringIterator
}


}//leveldb







