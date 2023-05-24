/*
 *  Copyright (c) 2012-2017, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

#ifndef _BLOOM_H
#define _BLOOM_H
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ***************************************************************************
 * Structure to keep track of one bloom filter.  Caller needs to
 * allocate this and pass it to the functions below. First call for
 * every struct must be to bloom_init().
 *
 */
struct bloom {
    uint32_t hashes;//哈希函数的个数
    uint8_t force64;//总是强制64位哈希，即使很小，
    uint8_t n2;//2的指数，2^n2bit数组的空间大小
    uint64_t entries;//最多能增加的元素

    double error;//错误率
    double bpe;//每个元素占用的比特数(bit per element),bits/entries

    unsigned char *bf;//过滤器的字符串
    uint64_t bytes;//需要这么多的bite数所总共要占的字节数
    uint64_t bits;//位数,该布隆过滤器需要的位数
};

/** ***************************************************************************
 * Initialize the bloom filter for use.
 *
 * The filter is initialized with a bit field and number of hash functions
 * according to the computations from the wikipedia entry:
 *     http://en.wikipedia.org/wiki/Bloom_filter
 *
 * Optimal number of bits is:最佳比特数
 *     bits = (entries * ln(error)) / ln(2)^2,根据这个公式计算出一个位图中合理的位数
 *
 * bpe = -log_2(p) / ln(2)^2计算bpe
 * 
 * Optimal number of hash functions is:再计算出一个合理的哈希函数的个数
 *     hashes = bpe * ln(2)
 *
 * Parameters:
 * -----------
 *     bloom   - Pointer to an allocated struct bloom (see above).
 *     entries - The expected number of entries which will be inserted.
 *               Must be at least 1000 (in practice, likely much larger).
 *     error   - Probability of collision (as long as entries are not
 *               exceeded).
 *
 * Return:
 * -------
 *     0 - on success
 *     1 - on failure
 *
 */

// Do not round bit size to nearest power of 2. Instead, estimate bits
// accurately.,不要将位数向最进的2的幂次方进行四舍无入，而是要精确的计算位数
#define BLOOM_OPT_NOROUND 1

// Entries is actually the number of bits, not the number of entries to reserve
//表示传入函数的参数实际上是位数，而不是需要预流的元素个数
#define BLOOM_OPT_ENTS_IS_BITS 2

// Always force 64 bit hashing, even if too small,必须保证64位，即使这个数很小
#define BLOOM_OPT_FORCE64 4

// Disable auto-scaling. Saves memory，禁止自动的扩容，添加链节
#define BLOOM_OPT_NO_SCALING 8

int bloom_init(struct bloom *bloom, uint64_t entries, double error, unsigned options);

/** ***************************************************************************
 * Deprecated, use bloom_init()
 *
 */
int bloom_init_size(struct bloom *bloom, uint64_t entries, double error, unsigned int cache_size);

typedef struct {
    uint64_t a;
    uint64_t b;
} bloom_hashval;//计算两个哈希值

bloom_hashval bloom_calc_hash(const void *buffer, int len);

/** ***************************************************************************
 * Check if the given element is in the bloom filter. Remember this may
 * return false positive if a collision occured.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *     buffer - Pointer to buffer containing element to check.
 *     len    - Size of 'buffer'.
 *
 * Return:
 * -------
 *     0 - element is not present
 *     1 - element is present (or false positive due to collision)
 *    -1 - bloom not initialized
 *
 */
int bloom_check_h(const struct bloom *bloom, bloom_hashval hash);
int bloom_check(const struct bloom *bloom, const void *buffer, int len);

/** ***************************************************************************
 * Add the given element to the bloom filter.
 * The return code indicates if the element (or a collision) was already in,
 * so for the common check+add use case, no need to call check separately.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *     buffer - Pointer to buffer containing element to add.
 *     len    - Size of 'buffer'.
 *
 * Return:
 * -------
 *     0 - element was not present and was added
 *     1 - element (or a collision) had already been added previously
 *    -1 - bloom not initialized
 *
 */
int bloom_add_h(struct bloom *bloom, bloom_hashval hash);
int bloom_add(struct bloom *bloom, const void *buffer, int len);

/** ***************************************************************************
 * Print (to stdout) info about this bloom filter. Debugging aid.
 *
 */
void bloom_print(struct bloom *bloom);

/** ***************************************************************************
 * Deallocate internal storage.
 *
 * Upon return, the bloom struct is no longer usable. You may call bloom_init
 * again on the same struct to reinitialize it again.
 *
 * Parameters:
 * -----------
 *     bloom  - Pointer to an allocated struct bloom (see above).
 *
 * Return: none
 *
 */
void bloom_free(struct bloom *bloom);

/** ***************************************************************************
 * Returns version string compiled into library.
 *
 * Return: version string
 *
 */
const char *bloom_version();

#ifdef __cplusplus
}
#endif

#endif
