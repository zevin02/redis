/*
 * Copyright Redis Ltd. 2017 - present
 * Licensed under your choice of the Redis Source Available License 2.0 (RSALv2) or
 * the Server Side Public License v1 (SSPLv1).
 */

#pragma once

#include "bloom/bloom.h"
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Single link inside a scalable bloom filter */
typedef struct SBLink {
    struct bloom inner; //< Inner structure   过滤器的内部的结构
    size_t size;        // < Number of items in the link  当前链接布隆过滤器对应的元素个数
} SBLink;

/** A chain of one or more bloom filters */
//这个是一组布隆过滤器的集合
typedef struct SBChain {
    SBLink *filters;  //< Current filter 当前的过滤器，一串布隆过滤器（链表链接）
    size_t size;      //< Total number of items in all filters 在过滤器集合中一共有多少个元素,我们可以通过返回这个值来实现bf.card
    size_t nfilters;  //< Number of links in chain 当前的链节数
    unsigned options; //< Options passed directly to bloom_init  直接传递给bloom_init的选项
    unsigned growth;  //用于计算在扩展过滤器时候应该增加多少个位的参数
} SBChain;

/**
 * Create a new chain
 * initsize: The initial desired capacity of the chain
 * error_rate: desired maximum error probability.
 * options: Options passed to bloom_init.
 *
 * Free with SBChain_Free when done.
 */
SBChain *SB_NewChain(uint64_t initsize, double error_rate, unsigned options, unsigned growth);

/**
 * Create a new chain from a 'template'. This template will copy an existing
 * chain, but not its internal data - which is reset from scratch. This is
 * used when 'migrating' filters
 */
SBChain *SB_NewChainFromTemplate(const SBChain *template);

/** Free a created chain */
void SBChain_Free(SBChain *sb);

/**
 * Add an item to the chain
 * Returns 0 if newly added, nonzero if new.
 */
//这里的len就是对应data的字节大小
int SBChain_Add(SBChain *sb, const void *data, size_t len);

/**
 * Check if an item was previously seen by the chain
 * Return 0 if the item is unknown to the chain, nonzero otherwise
 */
int SBChain_Check(const SBChain *sb, const void *data, size_t len);

/**
 * Get an encoded header. This is the first step to serializing a bloom filter.
 * The length of the header will be written to in hdrlen.
 *
 * The chunk should be freed with SB_FreeEncodedHeader.
 */
//该函数获得编码的头部数据，这个序列化一个bloom filter(compress bloom filter to transfer in network)
char *SBChain_GetEncodedHeader(const SBChain *sb, size_t *hdrlen);
void SB_FreeEncodedHeader(char *s);//释放编码后的数据头部

#define SB_CHUNKITER_INIT 1
#define SB_CHUNKITER_DONE 0
/**
 * Get an encoded filter chunk. This filter should be called in a loop until it
 * returns NULL. It will return incremental chunks based on the value of `curIter`.
 *
 * Prior to calling this function, the target of curIter should be initialized to
 * SB_CHUNKITER_INIT. Once there are no more chunks left to serialize, NULL will
 * be returned and curIter will be set to SB_CHUNKITER_DONE.
 *
 * The `len` pointer indicates the length of the returned buffer.
 * maxChunkSize indicates the largest chunk size to be returned, ensuring that
 * len <= maxChunkSize.
 */
const char *SBChain_GetEncodedChunk(const SBChain *sb, long long *curIter, size_t *len,
                                    size_t maxChunkSize);

/**
 * Creates a new chain from the encoded parameters returned by SBChain_GetEncodedHeader.
 * This function will return NULL if the header is corrupt or in a format not understood
 * by this version of rebloom. In this case, the description is found in the
 * errmsg pointer.
 */
//该函数的主要作用是从给定的已编码的参数中创建一个新的bloom filter
SBChain *SB_NewChainFromHeader(const char *buf, size_t bufLen, const char **errmsg);

/**
 * Incrementally load the bloom filter with chunks returned from GetEncodedChunk.
 * This function returns 0 on success, and nonzero on failure - in which case errmsg
 * is populated.
 *
 * The (iter,buf,bufLen) arguments are equivalent to the returned values in the
 * GetEncodedChunk function.
 */
int SBChain_LoadEncodedChunk(SBChain *sb, long long iter, const char *buf, size_t bufLen,
                             const char **errmsg);
#ifdef __cplusplus
}
#endif
