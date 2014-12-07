
#include <map>

template<class K,class V> 
class DoubleLinkedValue
{
public:
	DoubleLinkedValue(K &key,V &_value) : key(key),value(_value), prev(NULL), next(NULL) {}
	V value;
	K key;
	DoubleLinkedValue *prev, *next;	
};


template<class K,class V> 
class LRUCache{

public:

	LRUCache(int max, int(*sizeOfFunction)(V &_value)) : maxSize(max), begin(NULL), end(NULL), crrSize(0)
	{
		if (sizeOfFunction == NULL)
			sizeOfFunction = &this->sizeOfDefault;
		this->sizeOfFunction = sizeOfFunction;
	}

	static int sizeOfDefault(V &_value) {
		return 1;
	}

	V get(K &t) {
		DoubleLinkedValue<K, V> *a = map[t];
		if (a == NULL)
			return NULL;
		unlink(a);
		link(a); // MOVE TO FRONT
		return a->value;
	}
	
	V put(K &k,V &v) {
		DoubleLinkedValue<K, V> *a = new DoubleLinkedValue<K, V>(k, v);
			link(a);
		map[k] = a;
		if (crrSize > maxSize) 
			return popFront();
		return NULL;
	}
	
	V popFront() 
	{
		if (begin == NULL)
			return NULL;
		DoubleLinkedValue<K, V> *a = begin;
		V t = a->value;
		map.erase(a->key);
		unlink(a);
		delete a;
		return t;
	}

	void link(DoubleLinkedValue<K,V>* t) 
	{
		crrSize += sizeOfFunction(t->value);
		if (end != NULL) {
			end->next = t;
		}
		t->next = NULL;
		t->prev = end;
		end = t;
		if (begin == NULL)
			begin = end;
	}

	void unlink(DoubleLinkedValue<K,V>* t) {
		crrSize -= sizeOfFunction(t->value);
		if (t->prev != NULL)
			t->prev->next = t->next;
		else
			begin = t->next;
		if (t->next != NULL)
			t->next->prev = t->prev;
		else
			end = t->prev;
	}

	int(*sizeOfFunction)(V &_value);
	int maxSize;
	std::map<K, DoubleLinkedValue<K, V>*> map;
	DoubleLinkedValue<K,V>* begin;
	DoubleLinkedValue<K,V>* end;
	int crrSize;
};
