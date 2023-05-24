/*
 *  Copyright (c) 2012-2017, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

/*
 * Refer to bloom.h for documentation on the public interfaces.
 */

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bloom.h"
#include "murmurhash2.h"

#define MAKESTRING(n) STRING(n)
#define STRING(n) #n

extern void (*RedisModule_Free)(void *ptr);
extern void * (*RedisModule_Calloc)(size_t nmemb, size_t size);

#define BLOOM_CALLOC RedisModule_Calloc
#define BLOOM_FREE RedisModule_Free

/*
#ifndef BLOOM_CALLOC
#define BLOOM_CALLOC calloc
#define BLOOM_FREE free
#endif
*/

#define MODE_READ 0
#define MODE_WRITE 1
//在buf中查看x位上的值是否被设置
//1代表已经存在，0代表插入成功
inline static int test_bit_set_bit(unsigned char *buf, uint64_t x, int mode) {
    uint64_t byte = x >> 3;
    uint8_t mask = 1 << (x % 8);//对应需要操作的索引bit位
    uint8_t c = buf[byte]; // expensive memory access,先获得对应的需要操作的字节

    if (c & mask) {//已经存在
        return 1;
    } else {//不存在，同时是write的模式，我们就需要进行写入
        if (mode == MODE_WRITE) {
            buf[byte] = c | mask;//写入，就是不存在，把这个bit设置成1,
        }
        return 0;
    }
}

bloom_hashval bloom_calc_hash(const void *buffer, int len) {
    bloom_hashval rv;
    rv.a = murmurhash2(buffer, len, 0x9747b28c);
    rv.b = murmurhash2(buffer, len, rv.a);
    return rv;
}

bloom_hashval bloom_calc_hash64(const void *buffer, int len) {
    bloom_hashval rv;
    rv.a = MurmurHash64A_Bloom(buffer, len, 0xc6a4a7935bd1e995ULL);//使用相同的哈希算法，第三个参数就是种子值
    rv.b = MurmurHash64A_Bloom(buffer, len, rv.a);
    return rv;
}

// This function is defined as a macro because newer filters use a power of two
// for bit count, which is must faster to calculate. Older bloom filters don't
// use powers of two, so they are slower. Rather than calculating this inside
// the function itself, we provide two variants for this. The calling layer
// already knows which variant to call.
//
// modExp is the expression which will evaluate to the number of bits in the
// filter.
// 
//我们设置hash个位置，每个hash位置都是根据计算出来的两个哈希值和目前所在的第几个哈希函数中
#define CHECK_ADD_FUNC(T, modExp)                                                                  \
    T i;                                                                                           \
    int found_unset = 0;                                                                           \
    const register T mod = modExp;                                                                 \
    for (i = 0; i < bloom->hashes; i++) {                                                          \
        T x = ((hashval.a + i * hashval.b)) % mod;                                                 \
        if (!test_bit_set_bit(bloom->bf, x, mode)) {                                               \
            if (mode == MODE_READ) {                                                               \
                return 0;                                                                          \
            }                                                                                      \
            found_unset = 1;                                                                       \
        }                                                                                          \
    }                                                                                              \
    if (mode == MODE_READ) {                                                                       \
        return 1;                                                                                  \
    }                                                                                              \
    return found_unset;
//这个地方使用2的次方来操作更快
static int bloom_check_add32(struct bloom *bloom, bloom_hashval hashval, int mode) {
    CHECK_ADD_FUNC(uint32_t, (1 << bloom->n2));//这里要求计算出来的取模后是一个32位
}

static int bloom_check_add64(struct bloom *bloom, bloom_hashval hashval, int mode) {
    CHECK_ADD_FUNC(uint64_t, (1LLU << bloom->n2));
}

// This function is used for older bloom filters whose bit count was not
// 1 << X. This function is a bit slower, and isn't exposed in the API
// directly because it's deprecated
//紧凑的，所以bits就是他的bit空间的元素个数，但是会更慢一点 
static int bloom_check_add_compat(struct bloom *bloom, bloom_hashval hashval, int mode) {
    CHECK_ADD_FUNC(uint64_t, bloom->bits)//插入成功返回0
}

static double calc_bpe(double error) {
    static const double denom = 0.480453013918201; // ln(2)^2
    double num = log(error);

    double bpe = -(num / denom);
    if (bpe < 0) {
        bpe = -bpe;
    }
    return bpe;
}
//初始化布隆过滤器
int bloom_init(struct bloom *bloom, uint64_t entries, double error, unsigned options) {
    if (entries < 1 || error <= 0 || error >= 1.0) {//检查是否合法
        return 1;
    }

    bloom->error = error;
    bloom->bits = 0;
    bloom->entries = entries;//设置
    bloom->bpe = calc_bpe(error);

    uint64_t bits;//布隆过滤器中需要使用的比特个数
    //如果直接给出了比特数，更具这个值计算需要的哈希值的个数
    if (options & BLOOM_OPT_ENTS_IS_BITS) {//如果参数中给的是bits，而不是元素数，直接用这个值计算哈希函数个数
        // Size is determined by the number of bits
        if (/* entries == 0 || */ entries > 64) {
            return 1;
        }

        bloom->n2 = entries;//设置n2为entries数
        bits = 1LLU << bloom->n2;//计算出来需要使用的bits数bits=2^n2次方
        bloom->entries = bits / bloom->bpe;//计算可以容纳需要的元素个数bpe=bits/entry

    } else if (options & BLOOM_OPT_NOROUND) {//不往上进位,不需要变成2的次方,所以bits就是这个布隆过滤器空间可以添加的最大值
        // Don't perform any rounding. Conserve memory instead
        bits = bloom->bits = (uint64_t)(entries * bloom->bpe);//计算布隆过滤器需要的bits数
        bloom->n2 = 0;//不需要进位

    } else {
        //这个地方表示IS_BITS为假，同时需要向上进位，最接近的2次方
        double bn2 = logb(entries * bloom->bpe);//计算出log2(n)中的n，2的多少次方
        if (bn2 > 63 || bn2 == INFINITY) {
            return 1;
        }
        bloom->n2 = bn2 + 1;//向上进位
        bits = 1LLU << bloom->n2;

        // Determine the number of extra bits available for more items. We rounded
        // up the number of bits to the next-highest power of two. This means we
        // might have up to 2x the bits available to us.
        size_t bitDiff = bits - (entries * bloom->bpe);//计算多余出来的bits数
        // The number of additional items we can store is the extra number of bits
        // divided by bits-per-element
        size_t itemDiff = bitDiff / bloom->bpe;//这些多余的元素可以用来存储多余的元素
        bloom->entries += itemDiff;//加上可以存储的多余的元素
    }
    //如果不能被64整除，就需要向上去获得最接近的64的倍数，是为了保证是8的倍数
    if (bits % 64) {
        bloom->bytes = ((bits / 64) + 1) * 8;//计算布隆过滤器中需要使用的字节数
    } else {
        bloom->bytes = bits / 8;//可以被整除，就直接除8
    }
    bloom->bits = bloom->bytes * 8;//计算的字节数+1

    bloom->force64 = (options & BLOOM_OPT_FORCE64);
    bloom->hashes = (int)ceil(0.693147180559945 * bloom->bpe); // ln(2)，ceil是向上取整的函数ceil（2.3）=3
    bloom->bf = (unsigned char *)BLOOM_CALLOC(bloom->bytes, sizeof(unsigned char));//根据字节数来开辟空间
    if (bloom->bf == NULL) {
        return 1;
    }

    return 0;
}
//查看计算的哈希值是否已经存在
int bloom_check_h(const struct bloom *bloom, bloom_hashval hash) {
    if (bloom->n2 > 0) {//说明使用的是2的次方的
        if (bloom->force64 || bloom->n2 > 31) {
            //是否强制使用64位的哈希，或者位数组的大小是否大于2^31，就需要使用64位来进行操作
            return bloom_check_add64((void *)bloom, hash, MODE_READ);//按照64位来操作
        } else {
            return bloom_check_add32((void *)bloom, hash, MODE_READ);
        }
    } else {
        return bloom_check_add_compat((void *)bloom, hash, MODE_READ);//使用的检验
    }
}

int bloom_check(const struct bloom *bloom, const void *buffer, int len) {
    return bloom_check_h(bloom, bloom_calc_hash(buffer, len));
}
//把对应的hash值，添加到bloom当中,模式是写入的模式
int bloom_add_h(struct bloom *bloom, bloom_hashval hash) {
    if (bloom->n2 > 0) {
        if (bloom->force64 || bloom->n2 > 31) {
            return !bloom_check_add64(bloom, hash, MODE_WRITE);//写入模式进行操作，
        } else {
            return !bloom_check_add32(bloom, hash, MODE_WRITE);
        }
    } else {
        return !bloom_check_add_compat(bloom, hash, MODE_WRITE);
    }
}

int bloom_add(struct bloom *bloom, const void *buffer, int len) {
    return bloom_add_h(bloom, bloom_calc_hash(buffer, len));
}

void bloom_free(struct bloom *bloom) { BLOOM_FREE(bloom->bf); }

const char *bloom_version() { return MAKESTRING(BLOOM_VERSION); }
