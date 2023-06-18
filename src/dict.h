/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0
#define DICT_ERR 1

//dictentry就是节点的类型
typedef struct dictEntry {
    void *key;//节点的key
    //节点的value,这里的value是一个联合体，这里的节点值可以是任意类型的，具体使用哪一个值由节点的值来决定,同时只能其中一个有值
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    
    struct dictEntry *next;     /* Next entry in the same hash bucket. *///指向下一个hash节点，用于处理哈希冲突，在同一个桶里面，这个指针将他们挂起来，能够串起来元素
    void *metadata[];           /* An arbitrary number of bytes (starting at a
                                 * pointer-aligned address) of size as returned
                                 * by dictType's dictEntryMetadataBytes(). */
                                //metadata是一个空数组，可以存储任意类型的元数据，可以用来存储处了节点有关数据之外的额外数据，
} dictEntry;

typedef struct dict dict;

//dict相关的操作函数，以函数指针的形式存在
//dicttype有多个比较典型和重要的实现如:dbdicttype hashdicttype setdicttype


typedef struct dictType {
    //计算哈希值函数
    uint64_t (*hashFunction)(const void *key);
    //复制key的，进行一个深拷贝，别的地方修改掉这个key不会影响已经写入到dict里面的key
    void *(*keyDup)(dict *d, const void *key);
    //复制value
    void *(*valDup)(dict *d, const void *obj);
    //判断两个key是否相等
    int (*keyCompare)(dict *d, const void *key1, const void *key2);
    //销毁key
    //在写入的时候进行神拷贝，我们在释放的时候就要进行把拷贝出来的内存给释放掉，value同理
    void (*keyDestructor)(dict *d, void *key);
    //销毁value
    void (*valDestructor)(dict *d, void *obj);
    //判断dict是否还需要进行扩容
    int (*expandAllowed)(size_t moreMem, double usedRatio);
    /* Allow a dictEntry to carry extra caller-defined metadata.  The
     * extra memory is initialized to 0 when a dictEntry is allocated. */
    //用来计算metadata的柔性数组的长度
    size_t (*dictEntryMetadataBytes)(dict *d);
} dictType;

//计算hash表的元素个数
//exp=-1,说明hashtable中没有元素，我们把大小设置成0
#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1<<(exp))
//获得相应的掩码
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp))-1)

//这个就是对字典的定义
//最简单实现字典的有数组和链表，但是只能使用在元素不多的情况下
//为了兼顾效率和简单性，就使用哈希表，
//如果还希望实现排序的话，就可以使用平衡树

//每个字典使用两个哈希表，实现渐进式rehash

struct dict {
    dictType *type;//特定类型的处理函数，字典结构体需要根据具体的数据类型进行处理
    //这里有2个hash表是因为
    //0代表正在进行操作的哈希表
    //1可以用来进行扩容和缩容，在rehash前设置成null

    dictEntry **ht_table[2];//2个指向指针数组的指针，用于存储字典中的元素，这个数组就是hashtable，每个元素是hash表的节点，dictentry就是节点的类型
    unsigned long ht_used[2];//用于记录哈希表中已经使用的节点数，每当添加一个节点就要+1，每个哈希桶里面有几个元素

    //rehashidx的值表示当前正在进行rehash的散列表节点的索引，0代表从0位置开始进行rehash
    long rehashidx; /* rehashing not in progress if rehashidx == -1 ，如果=-1,说明rehash并没有进行，=0说明正在进行*/

    /* Keep small vars at end for optimal (minimal) struct padding */
    //=0说明rehash没有暂停，rehash可以正常进行
    int16_t pauserehash; /* If >0 rehashing is paused (<0 indicates coding error) pauserhash>0表示rehash操作停止，<0表示当前编码出错，这个变量用来控制rehash操作的暂停和恢复*/
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) 用来记录两个哈系桶的长度，实际上是记录2的n次方中n的值，hash table里面每次扩容都是2倍2倍的扩容，n=2,代表4,n=3代表8*/
};

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
//dict的迭代器
//使用迭代器来屏蔽底层的实现逻辑
typedef struct dictIterator {
    dict *d;//指向对应的dict对象
    long index;//当前迭代到的槽位
    int table, safe;
    //table是单前迭代到哪个dict，只能是0或1
    //safe表示当前迭代器是否处于安全模式迭代器，如果safe=1代表是安全迭代器，在迭代过程中，可以调用add，delete函数来修改dict实例
    //safe=0代表不是安全的，只能使用dictnext迭代数据，而不能修改dict

    //entry指向当前迭代到的节点
    //nextentry表示下一个可以进行迭代的节点
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    //fingerprint为安全模式迭代器设计一个指纹标识
    //这个就是在迭代器刚创建的时候，
    unsigned long long fingerprint;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(dict *d, dictEntry **bucketref);

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP      2
#define DICT_HT_INITIAL_SIZE     (1<<(DICT_HT_INITIAL_EXP))

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d), (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d), _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d), (entry)->key)

//设置key到entry的key字段
//如果有dup就使用特定的dup函数，否则就直接进行复制即可
#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d), _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d), key1, key2) : \
        (key1) == (key2))

#define dictMetadata(entry) (&(entry)->metadata)
#define dictMetadataSize(d) ((d)->type->dictEntryMetadataBytes \
                             ? (d)->type->dictEntryMetadataBytes(d) : 0)

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) (DICTHT_SIZE((d)->ht_size_exp[0])+DICTHT_SIZE((d)->ht_size_exp[1]))
#define dictSize(d) ((d)->ht_used[0]+(d)->ht_used[1])
#define dictIsRehashing(d) ((d)->rehashidx != -1)
#define dictPauseRehashing(d) (d)->pauserehash++
#define dictResumeRehashing(d) (d)->pauserehash--

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long) genrand64_int64())
#else
#define randomULong() random()
#endif

//dict大小调整机制
typedef enum {
    DICT_RESIZE_ENABLE,//启动
    DICT_RESIZE_AVOID,//避免
    DICT_RESIZE_FORBID,//禁止
} dictResizeEnable;

/* API */
dict *dictCreate(dictType *type);
int dictExpand(dict *d, unsigned long size);
int dictTryExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *dictAddOrFind(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
int dictDelete(dict *d, const void *key);
dictEntry *dictUnlink(dict *d, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, size_t len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);
void dictEmpty(dict *d, void(callback)(dict*));
void dictSetResizeEnabled(dictResizeEnable enable);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);
#endif

#endif /* __DICT_H */
