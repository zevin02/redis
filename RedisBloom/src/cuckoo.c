/*
 * Copyright Redis Ltd. 2017 - present
 * Licensed under your choice of the Redis Source Available License 2.0 (RSALv2) or
 * the Server Side Public License v1 (SSPLv1).
 */

#include "cuckoo.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

/*
#ifndef CUCKOO_MALLOC
#define CUCKOO_MALLOC malloc
#define CUCKOO_CALLOC calloc
#define CUCKOO_REALLOC realloc
#define CUCKOO_FREE free
#endif
*/
// int globalCuckooHash64Bit;

static int CuckooFilter_Grow(CuckooFilter *filter);

static int isPower2(uint64_t num) { return (num & (num - 1)) == 0 && num != 0; }
//这个函数是返回大于等于n的最小2的整数次幂，如6-》8
static uint64_t getNextN2(uint64_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n;
}
//cap就是最大能插入的容量
int CuckooFilter_Init(CuckooFilter *filter, uint64_t capacity, uint16_t bucketSize,
                      uint16_t maxIterations, uint16_t expansion) {
    memset(filter, 0, sizeof(*filter));
    filter->expansion = getNextN2(expansion);
    filter->bucketSize = bucketSize;
    filter->maxIterations = maxIterations;
    filter->numBuckets = getNextN2(capacity / bucketSize);//桶的个数
    if (filter->numBuckets == 0) {
        filter->numBuckets = 1;
    }
    assert(isPower2(filter->numBuckets));//判断是否是2的次方

    if (CuckooFilter_Grow(filter) != 0) {//添加cuckoo filter的节点
        return -1; // LCOV_EXCL_LINE memory failure
    }
    return 0;
}

void CuckooFilter_Free(CuckooFilter *filter) {
    for (uint16_t ii = 0; ii < filter->numFilters; ++ii) {
        CUCKOO_FREE(filter->filters[ii].data);//把每个桶进行一个
    }
    CUCKOO_FREE(filter->filters);
}
//增加cuckoo的大小，增加cuckoo的个数,扩容
static int CuckooFilter_Grow(CuckooFilter *filter) {
    SubCF *filtersArray =
        CUCKOO_REALLOC(filter->filters, sizeof(*filtersArray) * (filter->numFilters + 1));//这个就是subcf* filter，每次进来会进行一个扩容,subcf的数组

    if (!filtersArray) {
        return -1; // LCOV_EXCL_LINE memory failure
    }
    SubCF *currentFilter = filtersArray + filter->numFilters;//获得当前的cuckoo节点
    size_t growth = pow(filter->expansion, filter->numFilters);//根据expansion来计算每次多新开的cuckoo的桶的个数
    currentFilter->bucketSize = filter->bucketSize;//每个桶里面存储的元素个数
    currentFilter->numBuckets = filter->numBuckets * growth;//计算当前新节点的桶的个数
    //data里面存储的就是实际的长度
    currentFilter->data =
        CUCKOO_CALLOC((size_t)currentFilter->numBuckets * filter->bucketSize, sizeof(CuckooBucket));//开辟一个哈希数组,每个元素是一个桶,该桶最多能存的大小是256,开辟numbucket*bucket个长度的数组，每个元素都是一个大小为1字节的数组
    if (!currentFilter->data) {
        return -1; // LCOV_EXCL_LINE memory failure
    }

    filter->numFilters++;
    filter->filters = filtersArray;
    return 0;
}
//一个key对应两个哈希值
typedef struct {
    CuckooHash h1;//没有被处理过的哈希值
    CuckooHash h2;//同样没被处理过的哈希值
    CuckooFingerprint fp;//被处理过的哈希值，只能存储8位
} LookupParams;

static CuckooHash getAltHash(CuckooFingerprint fp, CuckooHash index) {
    return ((CuckooHash)(index ^ ((CuckooHash)fp * 0x5bd1e995)));//已知一个index，我们就可以计算出另一个index值
}

static void getLookupParams(CuckooHash hash, LookupParams *params) {
    params->fp = hash % 255 + 1;//获得该元素对应的指纹8位的,这个指纹从1-256

    params->h1 = hash;//该value对应的第一个哈希值就是hash
    params->h2 = getAltHash(params->fp, params->h1);//第二个就是需要根据第一个和指纹进行生成
    // assert(getAltHash(params->fp, params->h2, numBuckets) == params->h1);
}

//获得当前桶存储元素的起始偏移量
static uint32_t SubCF_GetIndex(const SubCF *subCF, CuckooHash hash) {
    return (hash % subCF->numBuckets) * subCF->bucketSize;//因为每个桶存储两个元素，所以这个就可以计算得到当前元素在所有索引中的偏移量
}

static uint8_t *Bucket_Find(CuckooBucket bucket, uint16_t bucketSize, CuckooFingerprint fp) {
    for (uint16_t ii = 0; ii < bucketSize; ++ii) {
        if (bucket[ii] == fp) {
            return bucket + ii;
        }
    }
    return NULL;
}
//在当前的一个子cuckoo中找这个元素
static int Filter_Find(const SubCF *filter, const LookupParams *params) {
    uint8_t bucketSize = filter->bucketSize;
    uint64_t loc1 = SubCF_GetIndex(filter, params->h1);
    uint64_t loc2 = SubCF_GetIndex(filter, params->h2);
    //在计算出来的索引位置去查看是否存在这个元素
    return Bucket_Find(&filter->data[loc1], bucketSize, params->fp) != NULL ||
           Bucket_Find(&filter->data[loc2], bucketSize, params->fp) != NULL;
}

static int Bucket_Delete(CuckooBucket bucket, uint16_t bucketSize, CuckooFingerprint fp) {
    for (uint16_t ii = 0; ii < bucketSize; ii++) {
        if (bucket[ii] == fp) {
            bucket[ii] = CUCKOO_NULLFP;//在其中一个地方找到了，就把他给标记掉，这样就删除了元素
            return 1;
        }
    }
    return 0;
}

static int Filter_Delete(const SubCF *filter, const LookupParams *params) {
    uint8_t bucketSize = filter->bucketSize;
    uint64_t loc1 = SubCF_GetIndex(filter, params->h1);
    uint64_t loc2 = SubCF_GetIndex(filter, params->h2);
    //在其中一个桶发现并删除了就可以
    return Bucket_Delete(&filter->data[loc1], bucketSize, params->fp) ||
           Bucket_Delete(&filter->data[loc2], bucketSize, params->fp);
}
//查看该fp是否存在
static int CuckooFilter_CheckFP(const CuckooFilter *filter, const LookupParams *params) {
    for (uint16_t ii = 0; ii < filter->numFilters; ++ii) {//每个链表中进行查询，只要在其中一个链节的cuckoo中找到了，就直接返回
        if (Filter_Find(&filter->filters[ii], params)) {
            return 1;
        }
    }
    return 0;
}
//查看某个元素是否存在
int CuckooFilter_Check(const CuckooFilter *filter, CuckooHash hash) {
    LookupParams params;
    getLookupParams(hash, &params);
    return CuckooFilter_CheckFP(filter, &params);
}

static uint16_t bucketCount(const CuckooBucket bucket, uint16_t bucketSize, CuckooFingerprint fp) {
    uint16_t ret = 0;
    for (uint16_t ii = 0; ii < bucketSize; ++ii) {//每个桶就两个元素
        if (bucket[ii] == fp) {
            ret++;//匹配上就+
        }
    }
    return ret;
}

static uint64_t subFilterCount(const SubCF *filter, const LookupParams *params) {
    uint8_t bucketSize = filter->bucketSize;
    uint64_t loc1 = SubCF_GetIndex(filter, params->h1);//获得索引位置
    uint64_t loc2 = SubCF_GetIndex(filter, params->h2);

    return bucketCount(&filter->data[loc1], bucketSize, params->fp) +
           bucketCount(&filter->data[loc2], bucketSize, params->fp);
}

uint64_t CuckooFilter_Count(const CuckooFilter *filter, CuckooHash hash) {
    LookupParams params;
    getLookupParams(hash, &params);
    uint64_t ret = 0;
    for (uint16_t ii = 0; ii < filter->numFilters; ++ii) {//到每个链表节点中
        ret += subFilterCount(&filter->filters[ii], &params);//在每个节点中查找是否有该元素
    }
    return ret;
}
//删除过滤器中的元素
int CuckooFilter_Delete(CuckooFilter *filter, CuckooHash hash) {
    LookupParams params;
    getLookupParams(hash, &params);
    for (uint16_t ii = filter->numFilters; ii > 0; --ii) {
        if (Filter_Delete(&filter->filters[ii - 1], &params)) {//删除成功
            filter->numItems--;//元素总个数减少
            filter->numDeletes++;//删除的个数增加
            //如果删除元素超过了元素总数的10%，就需要进行合并,因为有很多空间被闲置了
            if (filter->numFilters > 1 && filter->numDeletes > (double)filter->numItems * 0.10) {
                CuckooFilter_Compact(filter, false);
            }
            return 1;
        }
    }
    return 0;
}
//在特定的一个桶上,从
static uint8_t *Bucket_FindAvailable(CuckooBucket bucket, uint16_t bucketSize) {
    //从当前桶的起始位置往后走bucketsize个元素，查看哪个没有被占用
    for (uint16_t ii = 0; ii < bucketSize; ++ii) {//每个桶有bucketsize个元素
        if (bucket[ii] == CUCKOO_NULLFP) {//当前槽位没人用
            if(ii==1)
            {
                printf("ii=%d,line=%d",ii,__LINE__);
            }
            return &bucket[ii];//返回当前的槽位
        }
    }
    return NULL;
}
//在特定的一个过滤器中进行查看
static uint8_t *Filter_FindAvailable(SubCF *filter, const LookupParams *params) {
    uint8_t *slot;
    uint8_t bucketSize = filter->bucketSize;
    uint64_t loc1 = SubCF_GetIndex(filter, params->h1);//根据paramszhon计算需要插入的位置
    uint64_t loc2 = SubCF_GetIndex(filter, params->h2);//计算h2对应的索引位置
    //到该过滤器的实际存储的元素位置
    if ((slot = Bucket_FindAvailable(&filter->data[loc1], bucketSize)) ||
        (slot = Bucket_FindAvailable(&filter->data[loc2], bucketSize))) {
            //判断在data哈希表的第loc1中的桶中是否有需要的元素
        return slot;
    }
    return NULL;
}

static CuckooInsertStatus Filter_KOInsert(CuckooFilter *filter, SubCF *curFilter,
                                          const LookupParams *params);

static CuckooInsertStatus CuckooFilter_InsertFP(CuckooFilter *filter, const LookupParams *params) {
    for (uint16_t ii = filter->numFilters; ii > 0; --ii) {//从最后一个节点开始往前找
        uint8_t *slot = Filter_FindAvailable(&filter->filters[ii - 1], params);//计算可用的槽位
        if (slot) {
            *slot = params->fp;//把对应的fp插入进去
            filter->numItems++;//该元素的元素个数增加
            return CuckooInsert_Inserted;//有空槽位，就直接返回了
        }
    }
    //找了所有节点都没有位置
    // No space. Time to evict!,执行驱逐操作
    CuckooInsertStatus status =
        Filter_KOInsert(filter, &filter->filters[filter->numFilters - 1], params);//从最后一个链节点开始操作
    if (status == CuckooInsert_Inserted) {//插入成功的状态，
        filter->numItems++;//怎加当前元素的个数
        return CuckooInsert_Inserted;
    }
    //插入失败，不存在空余槽位
    if (filter->expansion == 0) {
        return CuckooInsert_NoSpace;//expansion=0说明不允许继续插入
    }
    //当前节点已经满了不允许继续插入了，同时允许我们进行扩容，我们就需要进行扩容,到新的节点上面操作
    if (CuckooFilter_Grow(filter) != 0) {
        return CuckooInsert_MemAllocFailed;
    }

    // Try to insert the filter again
    return CuckooFilter_InsertFP(filter, params);//再执行一次
}
//把该数据对应的hash值插入到filter中
CuckooInsertStatus CuckooFilter_Insert(CuckooFilter *filter, CuckooHash hash) {
    LookupParams params;
    getLookupParams(hash, &params);//根据该key对应的哈希值，生成他对应的两个哈希值，以及指纹
    return CuckooFilter_InsertFP(filter, &params);
}
//保证元素在Cuckkoo中的唯一性
CuckooInsertStatus CuckooFilter_InsertUnique(CuckooFilter *filter, CuckooHash hash) {
    LookupParams params;
    getLookupParams(hash, &params);
    if (CuckooFilter_CheckFP(filter, &params)) {//如果元素已经存在了
        return CuckooInsert_Exists;//返回已经存在
    }
    return CuckooFilter_InsertFP(filter, &params);
}

static void swapFPs(uint8_t *a, uint8_t *b) {
    uint8_t temp = *a;
    *a = *b;
    *b = temp;
}

static CuckooInsertStatus Filter_KOInsert(CuckooFilter *filter, SubCF *curFilter,
                                          const LookupParams *params) {
    uint16_t maxIterations = filter->maxIterations;
    uint32_t numBuckets = curFilter->numBuckets;
    uint16_t bucketSize = filter->bucketSize;
    CuckooFingerprint fp = params->fp;//获得指纹，现在要插入元素的指纹

    uint16_t counter = 0;//执行替换的次数
    uint32_t victimIx = 0;//被替换的指纹索引
    uint32_t ii = params->h1 % numBuckets;//ii计算索引位

    while (counter++ < maxIterations) {
        uint8_t *bucket = &curFilter->data[ii * bucketSize];//操纵真正的桶的元素
        swapFPs(bucket + victimIx, &fp);//通过交换操作，就把fp插入进去了，而不需要进行赋值,
        //较换之后，将原来这个位置的值放到
        ii = getAltHash(fp, ii) % numBuckets;//计算另一个哈希位置
        // Insert the new item in potentially the same bucket
        uint8_t *empty = Bucket_FindAvailable(&curFilter->data[ii * bucketSize], bucketSize);//
        if (empty) {//如果当前元素有空槽位
            // printf("Found slot. Bucket[%lu], Pos=%lu\n", ii, empty - curFilter[ii]);
            // printf("Old FP Value: %d\n", *empty);
            // printf("Setting FP: %p\n", empty);
            *empty = fp;//把这个fp插入到他另一个哈希位置
            return CuckooInsert_Inserted;
        }
        //没有空位置的话，我们就强制在这个桶里面选择一个元素进行交换,victix+1这样就保证不会相同
        victimIx = (victimIx + 1) % bucketSize;//ictimtx表示当前元素被添加到桶中的位置，如果刚才这个位置的第一个位置大概率已经被交换过了，+1,保证了下次要交换的时候
    }

    // If we weren't able to insert, we roll back and try to insert new element in new filter,无法插入，就需要执行替换策略，将当前桶的第
    counter = 0;
    //随机挑选一个桶进行替换，防止过多的元素插入到同一个桶里面
    while (counter++ < maxIterations) {
        victimIx = (victimIx + bucketSize - 1) % bucketSize;
        ii = getAltHash(fp, ii) % numBuckets;
        uint8_t *bucket = &curFilter->data[ii * bucketSize];
        swapFPs(bucket + victimIx, &fp);//随机挑选一个桶来进行替换
    }

    return CuckooInsert_NoSpace;
}

#define RELOC_EMPTY 0
#define RELOC_OK 1
#define RELOC_FAIL -1

/**
 * Attempt to move a slot from one bucket to another filter
 */
static int relocateSlot(CuckooFilter *cf, CuckooBucket bucket, uint16_t filterIx, uint64_t bucketIx,
                        uint16_t slotIx) {
    LookupParams params = {0};//生成一个新的param插入到前面的节点中
    if ((params.fp = bucket[slotIx]) == CUCKOO_NULLFP) {//zhesdd 
        // Nothing in this slot.
        return RELOC_EMPTY;//为空的话，这个槽位就不需要进行迁移
    }

    // Because We try to insert in sub filter with less or equal number of
    // buckets, our current fingerprint is sufficient
    //把当前槽位的元素平行迁移到前一个节点
    params.h1 = bucketIx;
    params.h2 = getAltHash(params.fp, bucketIx);

    // Look at all the prior filters and attempt to find a home
    for (uint16_t ii = 0; ii < filterIx; ++ii) {
        uint8_t *slot = Filter_FindAvailable(&cf->filters[ii], &params);//在每一个节点上看，是否有这两个位置是空闲的，那么就插入上去
        if (slot) {
            *slot = params.fp;//把之前的元素进行设置
            bucket[slotIx] = CUCKOO_NULLFP;//把之前的槽设置成空
            return RELOC_OK;//设置完当前槽就迁移完成了
        }
    }
    return RELOC_FAIL;//前面节点已经没有任何过滤器可以进行使用插入改slot了，那么就返回fatal
}

/**
 * Attempt to strip a single filter moving it down a slot
 */
//将当前节点的有用词槽位迁移到前面的节点
static uint64_t CuckooFilter_CompactSingle(CuckooFilter *cf, uint16_t filterIx) {
    SubCF *currentFilter = &cf->filters[filterIx];//获得当前的节点
    MyCuckooBucket *filter = currentFilter->data;//获得对应的哈希表
    int rv = RELOC_OK;
    //这个是在访问最后一个节点
    for (uint64_t bucketIx = 0; bucketIx < currentFilter->numBuckets; ++bucketIx) {//遍历当前哈希表中存储的所有元素
        for (uint16_t slotIx = 0; slotIx < currentFilter->bucketSize; ++slotIx) {
            int status = relocateSlot(cf, &filter[bucketIx * currentFilter->bucketSize], filterIx,
                                      bucketIx, slotIx);
            if (status == RELOC_FAIL) {//改节点的元素，往前迁移都被插满了，就无法再进行插入了
                rv = RELOC_FAIL;
            }
        }
    }
    // we free a filter only if it the latest one
    // 如果该节点全部都迁移完了，就需要将最后一个节点进行释放
    if (rv == RELOC_OK && filterIx == cf->numFilters - 1) {
        CUCKOO_FREE(filter);
        cf->numFilters--;
    }
    return rv;
}

/**
 * Attempt to move elements to older filters. If latest filter is emptied, it is freed.
 * `bool` determines whether to continue iteration on other filters once a filter cannot
 * be freed and therefore following filter cannot be freed either.
 */
//重新组织元素，减少内存空间
void CuckooFilter_Compact(CuckooFilter *cf, bool cont) {
    for (uint64_t ii = cf->numFilters; ii > 1; --ii) {//遍历所有的过滤器，将其中的元素重刑分配到更老的filter中，如何成功，当前的就需要被释放掉,
        if (CuckooFilter_CompactSingle(cf, ii - 1) == RELOC_FAIL && !cont) {//一直持续，直到没有槽位可以操作为止
            // if compacting failed, stop as lower filters cannot be freed.
            break;
        }
    }
    cf->numDeletes = 0;//把这个值设置成0
}

/* CF.DEBUG uses another function
void CuckooFilter_GetInfo(const CuckooFilter *cf, CuckooHash hash, CuckooKey *out) {
    LookupParams params;
    getLookupParams(hash, cf->numBuckets, &params);
    out->fp = params.fp;
    out->h1 = params.h1;
    out->h2 = params.h2;
    assert(getAltHash(params.fp, out->h1, cf->numBuckets) == out->h2);
    assert(getAltHash(params.fp, out->h2, cf->numBuckets) == out->h1);
}*/
