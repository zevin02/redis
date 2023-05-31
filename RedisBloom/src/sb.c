/*
 * Copyright Redis Ltd. 2017 - present
 * Licensed under your choice of the Redis Source Available License 2.0 (RSALv2) or
 * the Server Side Public License v1 (SSPLv1).
 */

#include "sb.h"

#include "redismodule.h"

#define BLOOM_CALLOC RedisModule_Calloc
#define BLOOM_FREE RedisModule_Free

#include "bloom/bloom.h"
#include<execinfo.h>
#include <string.h>

bloom_hashval bloom_calc_hash64(const void *buffer, int len);

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// Core                                                                     ///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
#define ERROR_TIGHTENING_RATIO 0.5                          //收紧误差，
#define CUR_FILTER(sb) ((sb)->filters + ((sb)->nfilters - 1))
//size就是需要给bloom filter开辟的bit数，error_rate就是我们指定的出错率
static int SBChain_AddLink(SBChain *chain, uint64_t size, double error_rate) {
    printf("error_rate:%d,line:%d\n",error_rate,__LINE__);
    if (!chain->filters) {
        chain->filters = RedisModule_Calloc(1, sizeof(*chain->filters));//【分配一个SBLINK（包含一个布隆过滤器和元素个数的结构体）
    } else {
        chain->filters =
            RedisModule_Realloc(chain->filters, sizeof(*chain->filters) * (chain->nfilters + 1));//扩容
    }

    SBLink *newlink = chain->filters + chain->nfilters;//通过计算数组来计算下一个可以使用的链节
    newlink->size = 0;
    chain->nfilters++;
    return bloom_init(&newlink->inner, size, error_rate, chain->options);//初始化这个链接的布隆过滤器
}

void SBChain_Free(SBChain *sb) {
    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        bloom_free(&sb->filters[ii].inner);//释放掉bloom链表的每个链表中的布隆过滤器
    }
    RedisModule_Free(sb->filters);//释放过滤器链表
    RedisModule_Free(sb);//释放掉该元素
}
//往布隆过滤器中添加元素，成功返回1
static int SBChain_AddToLink(SBLink *lb, bloom_hashval hash) {
    printf("%d\n",__LINE__);

    if (!bloom_add_h(&lb->inner, hash)) {
        // Element not previously present?
        //当前元素，直接没有出现过，我们就添加，并且添加成功了
        lb->size++;//当前链节对应的布隆过滤器的元素添加
        return 1;
    } else {
        return 0;
    }
}

static bloom_hashval SBChain_GetHash(const SBChain *chain, const void *buf, size_t len) {
    if (chain->options & BLOOM_OPT_FORCE64) {//如果选项中是需要按照64位来实现，我们就需要按照64位来计算哈希
        return bloom_calc_hash64(buf, len);
    } else {
        return bloom_calc_hash(buf, len);//计算32位的哈希值
    }
}
//使用了两个哈希函数，但是在实际bit位设置上，是将两个哈希函数，再进行一个计算，生成一个新的哈希值
int SBChain_Add(SBChain *sb, const void *data, size_t len) {
    // Does it already exist?
    bloom_hashval h = SBChain_GetHash(sb, data, len);//根据data计算出相应的哈希值
    for (int ii = sb->nfilters - 1; ii >= 0; --ii) {//遍历该布隆过滤器中的所有过滤器，这里是从最近的一个链节开始遍历，缓存局部性，概率大，我们遍历的次数就减少了
        if (bloom_check_h(&sb->filters[ii].inner, h)) {//如果当前值已经存在，直接返回0
            return 0;
        }
    }
    //这里我们发现蛇腰设置的这个data还没有被设置过
    // Determine if we need to add more items?
    SBLink *cur = CUR_FILTER(sb);//先获得到当前最新的链节（布隆过滤器）
    if (cur->size >= cur->inner.entries) {//如果当前节点的个数超过了该布隆过滤器可以插入的最大值，那么就需要新生成一个链节
        if (sb->options & BLOOM_OPT_NO_SCALING) {//如果设置了禁止添加链节点，就直接返回,插入失败，不允许我们继续插入
            return -2;
        }
        //否则就需要生成一个新的链节点，error_tightening_ratio
        double error = cur->inner.error * ERROR_TIGHTENING_RATIO;//
        if (SBChain_AddLink(sb, cur->inner.entries * (size_t)sb->growth, error) != 0) {
            return -1;
        }
        cur = CUR_FILTER(sb);
    }

    int rv = SBChain_AddToLink(cur, h);//把当前的元素的哈希值添加进去
    if (rv) {
        sb->size++;
    }
    // printf("%d\n",__LINE__);
    // void* callstack[128];
    // int i, frames = backtrace(callstack, 128);
    // char** strs = backtrace_symbols(callstack, frames);

    // printf("Call trace:\n");
    // for (i = 0; i < frames; ++i) {
    //     printf("%s\n", strs[i]);//打印栈帧信息
    // }
    // free(strs);
    return rv;
}
//判断当前这个data的值是否存在
int SBChain_Check(const SBChain *sb, const void *data, size_t len) {//执行这个函数来查找元素是否存在
    bloom_hashval hv = SBChain_GetHash(sb, data, len);
    for (int ii = sb->nfilters - 1; ii >= 0; --ii) {
        if (bloom_check_h(&sb->filters[ii].inner, hv)) {//找到就返回1
            return 1;
        }
    }
    printf("%d\n",__LINE__);

    return 0;
}
//传建一个新的SBChain对象
SBChain *SB_NewChain(uint64_t initsize, double error_rate, unsigned options, unsigned growth) {
    if (initsize == 0 || error_rate == 0 || error_rate >= 1) {
        return NULL;
    }
    SBChain *sb = RedisModule_Calloc(1, sizeof(*sb));//在堆上分配一个内存空间
    sb->growth = growth;
    sb->options = options;
    double tightening = (options & BLOOM_OPT_NO_SCALING) ? 1 : ERROR_TIGHTENING_RATIO;//这个是用来减少失误率，如果选项不指定缩放，就设置成1
    if (SBChain_AddLink(sb, initsize, error_rate * tightening) != 0) {//默认最少有一个链接
        SBChain_Free(sb);
        sb = NULL;
    }
    // printf("%d\n",__LINE__);

    return sb;
}

typedef struct __attribute__((packed)) {
    uint64_t bytes;
    uint64_t bits;
    uint64_t size;
    double error;
    double bpe;
    uint32_t hashes;
    uint64_t entries;
    uint8_t n2;
} dumpedChainLink;//每一个bloom filter链的元素

// X-Macro uses to convert between encoded and decoded SBLink
#define X_ENCODED_LINK(X, enc, link)                                                               \
    X((enc)->bytes, (link)->inner.bytes)                                                           \
    X((enc)->bits, (link)->inner.bits)                                                             \
    X((enc)->size, (link)->size)                                                                   \
    X((enc)->error, (link)->inner.error)                                                           \
    X((enc)->hashes, (link)->inner.hashes)                                                         \
    X((enc)->bpe, (link)->inner.bpe)                                                               \
    X((enc)->entries, (link)->inner.entries)                                                       \
    X((enc)->n2, (link)->inner.n2)


//压碎后的头部,没有内存对其的限制
typedef struct __attribute__((packed)) {
    uint64_t size;
    uint32_t nfilters;
    uint32_t options;
    uint32_t growth;
    dumpedChainLink links[];//这个还是一个柔性数组
} dumpedChainHeader;
//该函数是获得sbchain中的某个链节
static SBLink *getLinkPos(const SBChain *sb, long long curIter, size_t *offset) {
    // printf("Requested %lld\n", curIter);

    curIter--;//减1是为了从0开始的索引,cur是就是一个字节数
    SBLink *link = NULL;

    // Read iterator
    size_t seekPos = 0;//偏移量

    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        //计算链节和当前迭代器之间的偏移量，如果大于，就找到
        if (seekPos + sb->filters[ii].inner.bytes > curIter) {//找到了所需的链节点
            link = sb->filters + ii;
            break;
        } else {
            seekPos += sb->filters[ii].inner.bytes;//否则就需要往后
        }
    }
    if (!link) {
        return NULL;
    }

    curIter -= seekPos;//计算出在该链节中的偏移量
    *offset = curIter;//oiffset是在当前（要找的链节）节点的偏移量
    printf("%d\n",__LINE__);
    return link;//返回对应的链指针
}
//用于从bloom filter中获得编码后的数据块
//从curiter位置开始
const char *SBChain_GetEncodedChunk(const SBChain *sb, long long *curIter, size_t *len,
                                    size_t maxChunkSize) {
    // See into the offset.
    size_t offset = 0;//offset是为了看要查找的元素再当前节点的偏移量
    SBLink *link = getLinkPos(sb, *curIter, &offset);//获得当前的链节

    if (!link) {
        *curIter = 0;//如果已经不存在了，就设置成done
        return NULL;
    }

    *len = maxChunkSize;//先把大小初始化成maxchunksize
    size_t linkRemaining = link->inner.bytes - offset;//链节中剩余可以使用的字节数
    if (linkRemaining < *len) {
        *len = linkRemaining;//如果剩余的空间小于最大的，我们用小的就可以了
    }

    *curIter += *len;
    // printf("Returning offset=%lu\n", offset);
    printf("bfgetencodedchunk\n");
    return (const char *)(link->inner.bf + offset);//返回对应的布隆过滤器数据，从offset位置开始
}
//根据一个sbchain来进行一个编码,zhege 
char *SBChain_GetEncodedHeader(const SBChain *sb, size_t *hdrlen) {
    *hdrlen = sizeof(dumpedChainHeader) + (sizeof(dumpedChainLink) * sb->nfilters);//计算经过编码之后的大小
    dumpedChainHeader *hdr = RedisModule_Calloc(1, *hdrlen);//开辟一个总的链表
    hdr->size = sb->size;//总元素个数
    hdr->nfilters = sb->nfilters;//总的bloom filter节点个数
    hdr->options = sb->options;//选项
    hdr->growth = sb->growth;//如何增长

    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        dumpedChainLink *dstlink = &hdr->links[ii];//获得hdr中的某个链节
        SBLink *srclink = sb->filters + ii;//从这个链节点进行迁移

#define X(encfld, srcfld) encfld = srcfld;
        X_ENCODED_LINK(X, dstlink, srclink)//进行一个元素迁移
#undef X
    }
    printf("bfencodedheader\n");
    return (char *)hdr;//返回压缩后的数据
}

void SB_FreeEncodedHeader(char *s) { RedisModule_Free(s); }
//从一个已经编码好的sbchain中解压缩，进行创建一个sbchain
SBChain *SB_NewChainFromHeader(const char *buf, size_t bufLen, const char **errmsg) {
    const dumpedChainHeader *header = (const void *)buf;//buf中就是压缩之后的头数据
    if (bufLen < sizeof(dumpedChainHeader)) {//字节长度不相等，直接返回失败即可
        *errmsg = "ERR received bad data"; // LCOV_EXCL_LINE
        return NULL;                       // LCOV_EXCL_LINE
    }

    if (bufLen != sizeof(*header) + (sizeof(header->links[0]) * header->nfilters)) {
        *errmsg = "ERR received bad data"; // LCOV_EXCL_LINE
        return NULL;                       // LCOV_EXCL_LINE
    }

    SBChain *sb = RedisModule_Calloc(1, sizeof(*sb));
    sb->filters = RedisModule_Calloc(header->nfilters, sizeof(*sb->filters));
    sb->nfilters = header->nfilters;
    sb->options = header->options;
    sb->size = header->size;
    sb->growth = header->growth;

    for (size_t ii = 0; ii < header->nfilters; ++ii) {
        SBLink *dstlink = sb->filters + ii;
        const dumpedChainLink *srclink = header->links + ii;
#define X(encfld, dstfld) dstfld = encfld;
        X_ENCODED_LINK(X, srclink, dstlink)
#undef X
        dstlink->inner.bf = RedisModule_Alloc(dstlink->inner.bytes);
        if (sb->options & BLOOM_OPT_FORCE64) {
            dstlink->inner.force64 = 1;
        }
    }
    printf("bfnewchainfromheader\n");
    return sb;
}
//将压缩之后的数据块重新添加到SBChain中，这边默认，这个sb就已经存在了
//相当于在本地的第9个字节的位置，把buf数据全部导入到新布隆中
int SBChain_LoadEncodedChunk(SBChain *sb, long long iter, const char *buf, size_t bufLen,
                             const char **errmsg) {
    // Load the chunk
    size_t offset;
    iter -= bufLen;

    SBLink *link = getLinkPos(sb, iter, &offset);//在iter索引位查找
    if (!link) {
        *errmsg = "ERR invalid offset - no link found"; // LCOV_EXCL_LINE
        return -1;                                      // LCOV_EXCL_LINE
    }

    if (bufLen > link->inner.bytes - offset) {
        *errmsg = "ERR invalid chunk - Too big for current filter"; // LCOV_EXCL_LINE
        return -1;                                                  // LCOV_EXCL_LINE
    }

    // printf("Copying to %p. Offset=%lu, Len=%lu\n", link, offset, bufLen);
    printf("bfloadencodedchunk\n");
    memcpy(link->inner.bf + offset, buf, bufLen);//拷贝布隆过滤器过去,拷贝整个buf数据
    return 0;//成功返回0
}
