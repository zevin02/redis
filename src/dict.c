/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to disable
 * resizing and rehashing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
static dictResizeEnable dict_can_resize = DICT_RESIZE_ENABLE;
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *d);
static signed char _dictNextExp(unsigned long size);
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *d, dictType *type);

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, size_t len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset hash table parameters already initialized with _dictInit()*/
static void _dictReset(dict *d, int htidx)
{
    d->ht_table[htidx] = NULL;//指向null
    d->ht_size_exp[htidx] = -1;//2的-1次方，说明没有元素
    d->ht_used[htidx] = 0;//使用的元素为0
}

/* Create a new hash table */
//创建一个新的dict实例
dict *dictCreate(dictType *type)
{
    dict *d = zmalloc(sizeof(*d));//申请一个dict实例的空间

    _dictInit(d,type);//初始化dict实例中的各个字段
    return d;
}

/* Initialize the hash table */
//初始化dict里面各个字段
int _dictInit(dict *d, dictType *type)
{
    _dictReset(d, 0);//将两个hashtable的实例都设置成null
    _dictReset(d, 1);
    d->type = type;//将里面的类型设置为type类型
    d->rehashidx = -1;//rehashidx社会锁子成-1,表示rehash没有进行
    d->pauserehash = 0;//pauserehash=0表示，
    return DICT_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
int dictResize(dict *d)
{
    unsigned long minimal;

    if (dict_can_resize != DICT_RESIZE_ENABLE || dictIsRehashing(d)) return DICT_ERR;
    minimal = d->ht_used[0];
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    return dictExpand(d, minimal);
}

/* Expand or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if expand was performed, and DICT_ERR if skipped. */
//进行一个rehash
//0好哈希表容量不够了，就触发扩容机制，把数据都存到1号哈希表中，完成扩容后0号哈希表指针指向1号哈希表，1号哈希表再设置为null即可(为下次扩容进行操作)

int _dictExpand(dict *d, unsigned long size, int* malloc_failed)
{
    //这个下面进行一个检查逻辑
    if (malloc_failed) *malloc_failed = 0;

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size)
        return DICT_ERR;

    /* the new hash table */
    //

    dictEntry **new_ht_table;
    unsigned long new_ht_used;
    signed char new_ht_size_exp = _dictNextExp(size);//计算扩容后的长度，大于当前的容量并且第一个小于2^n次方的那个数

    /* Detect overflows */
    //异常的检查逻辑
    size_t newsize = 1ul<<new_ht_size_exp;
    if (newsize < size || newsize * sizeof(dictEntry*) < newsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    if (new_ht_size_exp == d->ht_size_exp[0]) return DICT_ERR;


    /* Allocate the new hash table and initialize all pointers to NULL */
    //开始申请扩容值后的ht_table的空间
    if (malloc_failed) {
        new_ht_table = ztrycalloc(newsize*sizeof(dictEntry*));//给新的hashtable分配出newsize个大小的空间
        *malloc_failed = new_ht_table == NULL;//
        if (*malloc_failed)
            return DICT_ERR;
    } else
        new_ht_table = zcalloc(newsize*sizeof(dictEntry*));

    new_ht_used = 0;//新的一个hashtable的已经使用的元素

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    //初始化新开辟的hstable
    if (d->ht_table[0] == NULL) {//如果第一个table中一个元素都没有，就根本不需要第二个桶
        d->ht_size_exp[0] = new_ht_size_exp;//
        d->ht_used[0] = new_ht_used;
        d->ht_table[0] = new_ht_table;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
    //否则就把新的元素放到第二个桶里面
    d->ht_size_exp[1] = new_ht_size_exp;
    d->ht_used[1] = new_ht_used;
    d->ht_table[1] = new_ht_table;
    d->rehashidx = 0;
    return DICT_OK;
}

/* return DICT_ERR if expand was not performed */
int dictExpand(dict *d, unsigned long size) {
    return _dictExpand(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
int dictTryExpand(dict *d, unsigned long size) {
    int malloc_failed;
    _dictExpand(d, size, &malloc_failed);
    return malloc_failed? DICT_ERR : DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
//redis的rehash并不会把整个key全都一次性进行，否则就会阻塞掉redis主进程
//redis的实现是在每次访问dict的时候都会对一部分的key进行一个rehash，这样就把rehash分配到了不同的请求中
//这样就不会出现长时间的阻塞，只是在每次请求的时候都分散一部分时间
//如在增删改查的时候，都会进行一个rehash
//分几次把数据搬运到第二个hashtable中


//n的意思就是一次rehash，最多能处理多少个槽位置
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. *///如果连续10个空槽位也直接返回
    if (dict_can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d)) return 0;
    if (dict_can_resize == DICT_RESIZE_AVOID && 
        (DICTHT_SIZE(d->ht_size_exp[1]) / DICTHT_SIZE(d->ht_size_exp[0]) < dict_force_resize_ratio))
    {
        return 0;
    }
    //最多移动n次，同时如果0号数组中没有元素了，也就直接返回
    while(n-- && d->ht_used[0] != 0) {//1.控制最多rehash多少个槽位
        dictEntry *de, *nextde;
        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        assert(DICTHT_SIZE(d->ht_size_exp[0]) > (unsigned long)d->rehashidx);
        //2.遇到多个空槽位就跳过
        while(d->ht_table[0][d->rehashidx] == NULL) {//如果当前的正在进行的rehashidx槽位为空,跳过空槽位
            d->rehashidx++;//rehashidx就往后移动
            if (--empty_visits == 0) return 1;//如果连续碰到10个空槽位，这次的rehash就结束了
        }
        //3.碰到一个非空的槽位，遍历这个链表下面的链表，对他进行一个rehash操作
        de = d->ht_table[0][d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        while(de) {
            //遍历当前这个槽位的链表
            uint64_t h;

            nextde = de->next;
            /* Get the index in the new hash table */
            h = dictHashKey(d, de->key) & DICTHT_SIZE_MASK(d->ht_size_exp[1]);
            de->next = d->ht_table[1][h];
            d->ht_table[1][h] = de;
            d->ht_used[0]--;
            d->ht_used[1]++;
            de = nextde;
        }
        d->ht_table[0][d->rehashidx] = NULL;//移动干净了，把这个槽位设置成null
        d->rehashidx++;//rehashidx索引位子自增
    }

    /* Check if we already rehashed the whole table... */
    //4.检查ht_tablep[0]中的全部kv是否都已经被移动完成了
    if (d->ht_used[0] == 0) {
        zfree(d->ht_table[0]);
        /* Copy the new ht onto the old one */
        //把0号数组的指针指向1号数组
        //全部进行一个拷贝 
        d->ht_table[0] = d->ht_table[1];
        d->ht_used[0] = d->ht_used[1];
        d->ht_size_exp[0] = d->ht_size_exp[1];
        _dictReset(d, 1);
        d->rehashidx = -1;//把rehash标志位取消掉
        return 0;//0说明不需要在rehash了
    }

    /* More to rehash... */
    return 1;//1代表还需要进行一个rehash
}

long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash in ms+"delta" milliseconds. The value of "delta" is larger 
 * than 0, and is smaller than 1 in most cases. The exact upper bound 
 * depends on the running time of dictRehash(d,100).*/
int dictRehashMilliseconds(dict *d, int ms) {
    if (d->pauserehash > 0) return 0;

    long long start = timeInMilliseconds();
    int rehashes = 0;

    while(dictRehash(d,100)) {
        rehashes += 100;
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some elements can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void _dictRehashStep(dict *d) {
    if (d->pauserehash == 0) dictRehash(d,1);//如果当前的pause字段为0的时候才能够进行rehash，每次rehash迁移最多只能迁移一个槽位上的所有元素
}

/* Add an element to the target hash table */
//往hash表里面添加元素
//key and value这里都有
//add函数这里只负责添加键值对，遇到key相同的，不会进行覆盖
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR;//如果entry为null，说明我们设置失败了
    dictSetVal(d, entry, val);//设置对应的val值
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
//不断往dict里面添加数据的同时，会检查dict需不需要进行扩容
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    int htidx;

    if (dictIsRehashing(d)) _dictRehashStep(d);//扩容相关的操作

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    
    //计算写入的key位于哪个槽位
    //这个地方会根据对应的dict类型，调用提前注册好的hash函数，进行计算hash值
    //
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)//如果当前元素已经存在了，我们就不需要再进行查找了
        return NULL;
    //现在获得了相应的index位置
    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */

    htidx = dictIsRehashing(d) ? 1 : 0;//判断是否正在进行rehash，如果正在rehash的话，就把htidx的值设置成1,这个就是我们需要操作的hashtable是哪个数组
    size_t metasize = dictMetadataSize(d);
    entry = zmalloc(sizeof(*entry) + metasize);//创建新的dictentry条目，我们要把这个进行添加到dict里面
    if (metasize > 0) {
        memset(dictMetadata(entry), 0, metasize);
    }
    //进行一个头插

    entry->next = d->ht_table[htidx][index];
    d->ht_table[htidx][index] = entry;
    d->ht_used[htidx]++;//第一个hashtable中存储的节点数增加

    /* Set the hash entry fields. */
    dictSetKey(d, entry, key);//设置key
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    entry = dictAddRaw(d,key,&existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    auxentry = *existing;
    dictSetVal(d, existing, val);
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is a helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
//
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    /* dict is empty */
    if (dictSize(d) == 0) return NULL;
    //同样也是要处理是否处于渐进式hash的状态，需要的话，就处理一个槽
    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);//计算hash值

    for (table = 0; table <= 1; table++) {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);//获得目标槽位
        he = d->ht_table[table][idx];//获得对应的桶
        prevHe = NULL;//记录前一个节点
        //遍历两个桶，同样如果处于渐进式hash的时候，两个桶都要比较
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht_table[table][idx] = he->next;
                if (!nofree) {//nofree决定是否释放，nofree=0代表要释放
                    dictFreeUnlinkedEntry(d, he);//he就是目标节点，释放掉目标节点
                }
                d->ht_used[table]--;//节点个数减少
                return he;//返回单前节点
            }
            prevHe = he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
//delete直接释放掉dictentry的空间
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
//unlink会直接把dictentry节点从dict中摘掉，然后直接返回
dictEntry *dictUnlink(dict *d, const void *key) {
    return dictGenericDelete(d,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);//释放掉key和val
    dictFreeVal(d, he);
    zfree(he);//再释放掉这个节点的内存
}

/* Destroy an entire dictionary */
int _dictClear(dict *d, int htidx, void(callback)(dict*)) {
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]) && d->ht_used[htidx] > 0; i++) {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d);

        if ((he = d->ht_table[htidx][i]) == NULL) continue;
        while(he) {
            nextHe = he->next;
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            zfree(he);
            d->ht_used[htidx]--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    zfree(d->ht_table[htidx]);
    /* Re-initialize the table */
    _dictReset(d, htidx);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *d)
{
    _dictClear(d,0,NULL);
    _dictClear(d,1,NULL);
    zfree(d);
}

//在dict中进行查找某个key，需要将两个ht_table都走一遍
//1.dict的所有的操作都会判断是否需要进行一个渐进式的rehash
//2.计算hash值，在ht_table[0]里面查找对应的槽位，然后遍历这个槽位挂着的链表
//如果处理rehash状态，还需要取ht_table[1]中找一遍

//dict的修改是直接调用find，获得对应的entry，并进行修改即可，不需要特定的一个modify函数
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;
    //如果dict里面一个元素都没有，这个函数就调用失败
    if (dictSize(d) == 0) return NULL; /* dict is empty */
    //判断是否处于rehash状态,rehashidx!=-1,就说明正在处理rehash状态
    if (dictIsRehashing(d)) _dictRehashStep(d);//我们就需要进行一个先rehash
    h = dictHashKey(d, key);//计算出来hash值
    for (table = 0; table <= 1; table++) {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);//计算到对应的桶的索引，第一个桶和第二个桶的总元素不同，获得在各自桶的下标位置
        he = d->ht_table[table][idx];//获得相应桶的拉链
        while(he) {//遍历我们找到的这个桶
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;//找到的话就直接返回
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;//如果没处于rehash状态，就直接返回，如果处于，同时第一个桶里面没找到对应的位置下标，我们就需要访问第二个桶
    }
    return NULL;
}

void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */

unsigned long long dictFingerprint(dict *d) {
    unsigned long long integers[6], hash = 0;
    int j;
    //把dict里面的每个字段进行都进行抑操作，获得一个long值 
    integers[0] = (long) d->ht_table[0];
    integers[1] = d->ht_size_exp[0];
    integers[2] = d->ht_used[0];
    integers[3] = (long) d->ht_table[1];
    integers[4] = d->ht_size_exp[1];
    integers[5] = d->ht_used[1];

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}
//创建一个不安全的迭代器
dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));//开辟一个迭代器内存

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

//创建一个安全的迭代器对象
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;//区别只是在于safe字段为1,其他都一样
    return i;
}

//进行迭代
dictEntry *dictNext(dictIterator *iter)//获得iter的entry的nextentry位置
{
    while (1) {
        if (iter->entry == NULL) {//首次调用，初始化迭代器
            if (iter->index == -1 && iter->table == 0) {//index=-1.说明是首次调用
                if (iter->safe)//如果处于安全的状态
                    dictPauseRehashing(iter->d);//设置rehash处在暂停状态
                else
                    iter->fingerprint = dictFingerprint(iter->d);//对于不安全的迭代器，就需要计算并初始化fingerprint字段
            }
            iter->index++;//把索引值设置成0,表示从0好下标处开始遍历
            if (iter->index >= (long) DICTHT_SIZE(iter->d->ht_size_exp[iter->table])) {//如果当前的index不合法
                if (dictIsRehashing(iter->d) && iter->table == 0) {//如果处理rehash状态，同时是一号桶
                    iter->table++;//说明第一个桶已经无法遍历了，我名需要进行对第二个桶进行遍历
                    iter->index = 0;//重新设置从1号桶的0号下标开始遍历
                } else {
                    //否则的话就说明已经是在最后哦一个位置了，就不需要遍历了，就直接返回即可
                    break;
                }
            }
            iter->entry = iter->d->ht_table[iter->table][iter->index];//entry指向当前条目，说明没问题，就指向当前条目
        } else {
            iter->entry = iter->nextEntry;//如果entry不为空，就指向下一个位置
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;//获得下一个节点
            return iter->entry;
        }
    }
    return NULL;//没找到的话就返回null
}

//完成迭代，就会释放掉迭代器
void dictReleaseIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)
            dictResumeRehashing(iter->d);//把rehash重新启动
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));//安全的迭代器会计算一下fingerprint的值，看看是否发生变化
    }
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;
    if (dictIsRehashing(d)) _dictRehashStep(d);
    if (dictIsRehashing(d)) {
        unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (randomULong() % (dictSlots(d) - d->rehashidx));
            he = (h >= s0) ? d->ht_table[1][h - s0] : d->ht_table[0][h];
        } while(he == NULL);
    } else {
        unsigned long m = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
        do {
            h = randomULong() & m;
            he = d->ht_table[0][h];
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    listlen = 0;
    orighe = he;
    while(he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while(listele--) he = he->next;
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
//随机获得一些key
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
    if (tables > 1 && maxsizemask < DICTHT_SIZE_MASK(d->ht_size_exp[1]))
        maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[1]);

    /* Pick a random point inside the larger table. */
    unsigned long i = randomULong() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= DICTHT_SIZE(d->ht_size_exp[1]))
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= DICTHT_SIZE(d->ht_size_exp[j])) continue; /* Out of range for this table. */
            dictEntry *he = d->ht_table[j][i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d,entries,GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    if (count == 0) return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
//scan可以用来在特定的游标位置上遍历大量的key方便（使用的是迭代遍历），大型的数据库中建议使用scan进行操作
//hscan用于迭代大型hash表，cursor是游标的启动位置，

//1. dict中支持分批迭代
//2. 分批迭代的时候，还允许其他命令触发渐进式rehash
//3. 只要数据在迭代的开始到结束，没有被删除，就一定会被迭代到，如果在迭代过程中，被删除掉，就不一定会迭代到了
//4. 在迭代的过程中可能返回重复的数据
unsigned long dictScan(dict *d,/*d是要进行扫描的dict实例*/
                       unsigned long v,/*v是槽位开始迭代的索引位置，每次dictscan函数的返回值，就是下依次scan传入的参数*/
                       dictScanFunction *fn,/*函数指针，回调函数，迭代到的每个节点都有这个函数来处理*/
                       dictScanBucketFunction* bucketfn,/*bucket是一个函数指针，在迭代到一个新槽位的时候，会触发相应的回调逻辑*/
                       void *privdata/*数组，是fn这个回调函数的参数，里面会放adlist链表，一个是当前迭代的对象是什么东西*/)
{
    int htidx0, htidx1;//ht_table数组的下标
    const dictEntry *de, *next;
    unsigned long m0, m1;

    if (dictSize(d) == 0) return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    dictPauseRehashing(d);//暂停rehash，把pause字段+1，但是两次scan调用期间是允许rehash的
    //根据是否处于rehash状态进入不同的分支
    if (!dictIsRehashing(d)) {
        //不在rehash状态
        htidx0 = 0;
        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);//m0是ht的掩码

        /* Emit entries at cursor */
        //处理bucketfn这个回调，处理单个槽位
        if (bucketfn) bucketfn(d, &d->ht_table[htidx0][v & m0]);
        de = d->ht_table[htidx0][v & m0];//获得对应的htidx在v索引位置的元素
        while (de) {//进行向后遍历
            next = de->next;
            fn(privdata, de);//调用fn回调，处理每个元素,获得每个元素
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        //这里的游标v使用的是翻转二进制迭代器的方式
        //000+1=100       1+1=010,右边是高位，左边是低位
        //这样可以遍历到所有的节点，从尾部开始遍历每个节点
        v |= ~m0;//v保留低位，高位全部都设置成1

        /* Increment the reverse cursor */
        v = rev(v);//进行对v的二进制的翻转
        v++;//递增1
        v = rev(v);//再对V进行翻转

    } else {
        //处于rehash的分支中
        htidx0 = 0;
        htidx1 = 1;

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (DICTHT_SIZE(d->ht_size_exp[htidx0]) > DICTHT_SIZE(d->ht_size_exp[htidx1])) {
            htidx0 = 1;
            htidx1 = 0;
        }
        //分别获得两个桶的掩码
        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);
        m1 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx1]);

        /* Emit entries at cursor */
        //触发bucketfn回调。处理单个槽位
        if (bucketfn) bucketfn(d, &d->ht_table[htidx0][v & m0]);
        de = d->ht_table[htidx0][v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);//处理每个节点的回调
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (bucketfn) bucketfn(d, &d->ht_table[htidx1][v & m1]);
            de = d->ht_table[htidx1][v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    dictResumeRehashing(d);//pause减少，允许渐进式的rehash

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * expands, we will check this allocation is allowed or not if the dict
 * type has expandAllowed member function. */
static int dictTypeExpandAllowed(dict *d) {
    if (d->type->expandAllowed == NULL) return 1;
    return d->type->expandAllowed(
                    DICTHT_SIZE(_dictNextExp(d->ht_used[0] + 1)) * sizeof(dictEntry*),
                    (double)d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]));
}

/* Expand the hash table if needed */
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    if (DICTHT_SIZE(d->ht_size_exp[0]) == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if (!dictTypeExpandAllowed(d))
        return DICT_OK;
    if ((dict_can_resize == DICT_RESIZE_ENABLE &&
         d->ht_used[0] >= DICTHT_SIZE(d->ht_size_exp[0])) ||
        (dict_can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]) > dict_force_resize_ratio))
    {
        return dictExpand(d, d->ht_used[0] + 1);
    }
    return DICT_OK;
}

/* TODO: clz optimization */
/* Our hash table capability is a power of two */
static signed char _dictNextExp(unsigned long size)
{
    unsigned char e = DICT_HT_INITIAL_EXP;

    if (size >= LONG_MAX) return (8*sizeof(long)-1);
    while(1) {
        if (((unsigned long)1<<e) >= size)
            return e;
        e++;
    }
}

/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table. */
//计算处key的索引位置
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL;//如果要查找的key已经存在了，我们即把这个已经存在的可以的entry条目赋值给existing，并进行返回

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR)//这个是进行扩容使用的
        return -1;

    //然后就是在两个hash桶中进行搜索对应的key
    //正常情况下我们都是只使用一个hash table，而只有在扩容的时候才会使用第二个hashtable
    
    for (table = 0; table <= 1; table++) {
        //这个烟码的话，只题取到元素的低位,这样就能保证哈希值映射到0~size-1的位置上，相当于%运算
        
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        /* Search if this slot does not already contain the given key */
        he = d->ht_table[table][idx];//获得idxhash table中第idx索引位置的entry元素
        while(he) {//如果he不为空
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                //如果key和哈希桶里面的key相同，或者key和he的key相同
                if (existing) *existing = he;//如果existind存在，就把existing设置成这个he的entry
                return -1;//key存在的场景下，返回的是-1,而不是相应的槽位的索引
            }
            he = he->next;//
        }
        if (!dictIsRehashing(d)) break;//搜索完第一个hashtable的时候，判断是否处在扩容状态，如果不是，就直接break出去，不去对第二个table进行查找
    }
    return idx;//找到了对应的hashtable的桶的编号

}

void dictEmpty(dict *d, void(callback)(dict*)) {
    _dictClear(d,0,callback);
    _dictClear(d,1,callback);
    d->rehashidx = -1;
    d->pauserehash = 0;
}

void dictSetResizeEnabled(dictResizeEnable enable) {
    dict_can_resize = enable;
}

uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++) {
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        heref = &d->ht_table[table][idx];
        he = *heref;
        while(he) {
            if (oldptr==he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dict *d, int htidx) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (d->ht_used[htidx] == 0) {
        return snprintf(buf,bufsize,
            "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]); i++) {
        dictEntry *he;

        if (d->ht_table[htidx][i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = d->ht_table[htidx][i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %lu\n"
        " number of elements: %lu\n"
        " different slots: %lu\n"
        " max chain length: %lu\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        htidx, (htidx == 0) ? "main hash table" : "rehashing target",
        DICTHT_SIZE(d->ht_size_exp[htidx]), d->ht_used[htidx], slots, maxchainlen,
        (float)totchainlen/slots, (float)d->ht_used[htidx]/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        if (l >= bufsize) break;
        l += snprintf(buf+l,bufsize-l,
            "   %ld: %ld (%.02f%%)\n",
            i, clvector[i], ((float)clvector[i]/DICTHT_SIZE(d->ht_size_exp[htidx]))*100);
    }

    /* Unlike snprintf(), return the number of characters actually written. */
    if (bufsize) buf[bufsize-1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,d,0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,d,1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST
#include "testhelp.h"

#define UNUSED(V) ((void) V)

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int compareCallback(dict *d, const void *key1, const void *key2) {
    int l1,l2;
    UNUSED(d);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(dict *d, void *val) {
    UNUSED(d);

    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = sprintf(buf,"%lld",value);
    s = zmalloc(len+1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0)

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int flags) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType);
    long count = 0;
    int accurate = (flags & REDIS_TEST_ACCURATE);

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3],NULL,10);
        }
    } else {
        count = 5000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,stringFromLongLong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(dict);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}
#endif
