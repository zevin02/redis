/*
 * Copyright Redis Ltd. 2017 - present
 * Licensed under your choice of the Redis Source Available License 2.0 (RSALv2) or
 * the Server Side Public License v1 (SSPLv1).
 */

#pragma once

#include "murmur2/murmurhash2.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// Defines whether 32bit or 64bit hash function will be used.
// It will be deprecated in RedisBloom 3.0.

#define CUCKOO_BKTSIZE 2
#define CUCKOO_NULLFP 0
// extern int globalCuckooHash64Bit;

typedef uint8_t CuckooFingerprint;
typedef uint64_t CuckooHash;//64为的哈希值
typedef uint8_t CuckooBucket[1];//定义了一个CuckooBucket类型，他是一个具有1个元素的uint8_t数组类型，每个桶都是1个字节,这个==uint8_t*,
typedef uint8_t MyCuckooBucket;

typedef struct {
    uint64_t numBuckets : 56;//哈希表中的桶个数
    uint64_t bucketSize : 8;//每个哈希表能容纳的元素个数
    MyCuckooBucket *data;//一个数组，每个元素是一个uint8 a[];//相当于这样的一个数组,每个哈希数组中存储最大不超过8位的指纹，存储的元素个数就是numbucket*bucketsize
} SubCF;

typedef struct {
    //numbucket*bucketsize=最多能添加的元素个数
    uint64_t numBuckets;//表示哈希表中桶的个数,这个是一个基数，相当于最低的位置的节点的，后面节点可能会更多
    uint64_t numItems;//哈希表中元素的个数
    uint64_t numDeletes;//删除元素的个数
    uint16_t numFilters;//节点的个数
    uint16_t bucketSize;//每个桶里面能容纳的元素个数
    uint16_t maxIterations;//在插入元素的时候，最多能迭代的次数，超过这个次数就插入失败
    //我们可以平衡内存和性能，确保Cuckoo Filter具有足够的存储空间来存储数据，并且尽可能地减少内存浪费。
    uint16_t expansion;//在哈希表需要扩容的时候，需要扩容的比例因子,newbucket=numbucket*(expansion^nfilter),保证按比例增大
    SubCF *filters;//链表，每一个元素是一个cuckoo filter
} CuckooFilter;

#define CUCKOO_GEN_HASH(s, n) MurmurHash64A_Bloom(s, n, 0)//用0做种子

/*
#define CUCKOO_GEN_HASH(s, n)                       \
            globalCuckooHash64Bit == 1 ?            \
                MurmurHash64A_Bloom(s, n, 0) :      \
                murmurhash2(s, n, 0)
*/
typedef struct {
    uint64_t i1;
    uint64_t i2;
    CuckooFingerprint fp;
} CuckooKey;


typedef enum {
    CuckooInsert_Inserted = 1,
    CuckooInsert_Exists = 0,
    CuckooInsert_NoSpace = -1,
    CuckooInsert_MemAllocFailed = -2
} CuckooInsertStatus;//插入的状态

int CuckooFilter_Init(CuckooFilter *filter, uint64_t capacity, uint16_t bucketSize,
                      uint16_t maxIterations, uint16_t expansion);
void CuckooFilter_Free(CuckooFilter *filter);
CuckooInsertStatus CuckooFilter_InsertUnique(CuckooFilter *filter, CuckooHash hash);
CuckooInsertStatus CuckooFilter_Insert(CuckooFilter *filter, CuckooHash hash);
int CuckooFilter_Delete(CuckooFilter *filter, CuckooHash hash);
int CuckooFilter_Check(const CuckooFilter *filter, CuckooHash hash);
uint64_t CuckooFilter_Count(const CuckooFilter *filter, CuckooHash);//calculate the current items,计算某个元素在cuckoo中出现的次数
void CuckooFilter_Compact(CuckooFilter *filter, bool cont);
void CuckooFilter_GetInfo(const CuckooFilter *cf, CuckooHash hash, CuckooKey *out);
