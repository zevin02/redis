/* Maxmemory directive handling (LRU eviction and other policies).
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "script.h"
#include <math.h>

/* ----------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------*/

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across performEvictions() calls.
 *
 * Entries inside the eviction pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * When an LFU policy is used instead, a reverse frequency indication is used
 * instead of the idle time, so that we still evict by larger value (larger
 * inverse frequency means to evict keys with the least frequent accesses).
 *
 * Empty entries have the key pointer set to NULL. */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255
//LRU缓冲池
//如Key: "user:1", Value: { "name": "John", "age": 25 }，对user:1这个key进行删除的时候，key中保存的就是他的值，而cached里面缓存的就是这个value

struct evictionPoolEntry {
    //LRU相关次策略中的空闲时间，LFU中的频次，ttl中的过期时间
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) */
    sds key;                    /* Key name. 对应一个候选的key，当内存淘汰的时候，就可以被淘汰出去*/
    sds cached;                 /* Cached SDS object for key name. 为了避免在淘汰的时候，再从数据库中获得key对应的值，当缓存池中的key被选中淘汰时，对应的value就会被缓存再cached中*/
    int dbid;                   /* Key DB number. 该key所在的db编号*/
};
//这个缓冲区是一个全局的，有序，长度固定的16位数组
//其中元素会按照idle字段进行从小到大进行排序，缓存池用来采样获取的key
static struct evictionPoolEntry *EvictionPoolLRU;

/* ----------------------------------------------------------------------------
 * Implementation of eviction, aging and LRU
 * --------------------------------------------------------------------------*/

/* Return the LRU clock, based on the clock resolution. This is a time
 * in a reduced-bits format that can be used to set and check the
 * object->lru field of redisObject structures. */
//全局的时钟是通过这个函数计算得到的
unsigned int getLRUClock(void) {
    return (mstime()/LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX;//获得低24位,除以lru时钟的分辨率（lru时钟的分辨率代表时钟的精确度或间隔，定义了时钟在多长时间进行递增一次即时钟值每经过1s就递增1）
}

/* This function is used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a system call. */
//LRU算法进行数据淘汰的时候，使用lruclock字段来进行，使用LRU_CLOCK来计算当前的LRU 时钟
unsigned int LRU_CLOCK(void) {
    unsigned int lruclock;
    if (1000/server.hz <= LRU_CLOCK_RESOLUTION) {
        //如果serverCron（）每秒至少执行一次，那么server中缓存的lruclock也是秒级别的
        //能够做到1s更新一次，那这个精确度就够，可以直接用
        atomicGet(server.lruclock,lruclock);
    } else {
        //如果当前做不到秒级别，就需要获得时间戳，来计算，精确度不够就用精确获得
        lruclock = getLRUClock();
    }
    return lruclock;
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. */
//进行缓存淘汰的过程中，计算value空闲了多久，redis可以找到最久没有被访问的key
//毫秒级时间戳
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();//当前LRU时钟，未被请求的最短时间
    if (lruclock >= o->lru) {//
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;//（lruclock-o->lru）：计算得到时钟经过的时间片数量，计算得到对象未被请求的时间跨度
    } else {
        //表示对象的LRU时钟发生了循环溢出
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) *
                    LRU_CLOCK_RESOLUTION;
    }
    //最后返回的都是对象未被请求的最短时间
}

/* LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by EVPOOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */

/* Create a new eviction pool. */
void evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep)*EVPOOL_SIZE);
    for (j = 0; j < EVPOOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
        ep[j].cached = sdsnewlen(NULL,EVPOOL_CACHED_SDS_SIZE);
        ep[j].dbid = 0;
    }
    EvictionPoolLRU = ep;
}

/* This is a helper function for performEvictions(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time bigger than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. */
//sampledict就是根据约定使用的dict或者expire（里面只存储了key和过期时间而不存储值对象）的其中一个，而keydict就是dict
void evictionPoolPopulate(int dbid, dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    dictEntry *samples[server.maxmemory_samples];//根据配置中的maxmemory_sample进行采样
    //随机获得几个samples,存储在samples中
    //每个数据库都获得采样数量
    //如果超过了，就需要不断的进行数据的和替换，最后概率就会比较高

    count = dictGetSomeKeys(sampledict,samples,server.maxmemory_samples);//从sampledict中获得一些元素，放到samples中
    for (j = 0; j < count; j++) {
        //遍历获得的sample数组
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];//
        key = dictGetKey(de);//获得这个key

        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. */
        //
        if (server.maxmemory_policy != MAXMEMORY_VOLATILE_TTL) {
            if (sampledict != keydict) de = dictFind(keydict, key);//如果当前是再expire中查询，那么他不会存储值只会存储过期时间，所以就需要在key对应的元素
            o = dictGetVal(de);//获得该entry中存储的value
        }

        /* Calculate the idle time according to the policy. This is called
         * idle just because the code initially handled LRU, but is in fact
         * just a score where an higher score means better candidate. */
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LRU) {
            //如果使用的是*lru策略，idle就是key的空闲时长
            idle = estimateObjectIdleTime(o);
        } else if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            /* When we use an LRU policy, we sort the keys by idle time
             * so that we expire keys starting from greater idle time.
             * However when the policy is an LFU one, we have a frequency
             * estimation, and we want to evict keys with lower frequency
             * first. So inside the pool we put objects using the inverted
             * frequency subtracting the actual frequency to the maximum
             * frequency of 255. */
            //如果是LFU，又因为pool是根据idle排序的，所以头节点idle小
            //反过来，如果频率高，那么相减就小在前面，而越大，访问的频次说明越低，就在后面
            idle = 255-LFUDecrAndReturn(o);//如果是LFU，就需要计算访问的频率
        } else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            /* In this case the sooner the expire the better. */
            //计算key的剩余过期时间
            //因为这个时候使用的expire字段
            idle = ULLONG_MAX - (long)dictGetVal(de);//de可以获得他剩余的过期时间，反过来就能够进行排序
        } else {
            serverPanic("Unknown eviction policy in evictionPoolPopulate()");
        }
        //无论那种策略，只要idle越大，对应的key就越应该被淘汰
        //将idle从小到大进行排序

        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        k = 0;
        while (k < EVPOOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;
               
        //根据空闲时间进行排序，选取插入到pool的位置
        if (k == 0 && pool[EVPOOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. */
            //如果找到的位置是0,且最后一个位置已经被占用了，就无法继续插入，继续下一个key的处理
            continue;
        } else if (k < EVPOOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. */
            // 如果找到的位置小于淘汰池的大小，且该位置为空，就插入到这个位置
        } else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  */
            //如果是插入在中间，我们就需要保证其有序性，所以需要进行移动
            //检查右侧是否有空闲，
            if (pool[EVPOOL_SIZE-1].key == NULL) {
                //如果最后一个位置空闲，则说明右侧有空间可以插入元素
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right. */

                /* Save SDS before overwriting. */
                //在右侧插入元素，
                sds cached = pool[EVPOOL_SIZE-1].cached;//保存被覆盖元素的cached
                //把当前位置全部向后移动，把这个空间让出来
                memmove(pool+k+1,pool+k,
                    sizeof(pool[0])*(EVPOOL_SIZE-k-1));//将该元素全部向右边移动
                pool[k].cached = cached;
            } else {
                /* No free space on right? Insert at k-1 */
                k--;
                //右边没有空间，则将插入位置向左移动一位，
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                sds cached = pool[0].cached; /* Save SDS before overwriting. */
                //第一个idle时间是最小的，所以可以舍弃
                if (pool[0].key != pool[0].cached) sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
                pool[k].cached = cached;//保证在释放掉第一个元素的时候，还可以访问到第一个元素的值
                //这个是为了舍弃较小空闲时间的第一个元素，并在k位置插入新的元素
            }
        }

        /* Try to reuse the cached SDS string allocated in the pool entry,
         * because allocating and deallocating this object is costly
         * (according to the profiler, not my fantasy. Remember:
         * premature optimization bla bla bla. */
        int klen = sdslen(key);
        if (klen > EVPOOL_CACHED_SDS_SIZE) {
            //如果key的长度大于255,就需要进行复制
            pool[k].key = sdsdup(key);//复制key到pool的位置上
        } else {
            //如果key的长度小，可以直接使用cached，而不需要使用key
            memcpy(pool[k].cached,key,klen+1);//复制cached
            sdssetlen(pool[k].cached,klen);//设置cached的长度
            pool[k].key = pool[k].cached;
        }
        pool[k].idle = idle;
        pool[k].dbid = dbid;
    }
}

/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) implementation.

 * We have 24 total bits of space in each object in order to implement
 * an LFU (Least Frequently Used) eviction policy, since we re-use the
 * LRU field for this purpose.
 *
 * We split the 24 bits into two fields:
 *
 *          16 bits      8 bits
 *     +----------------+--------+
 *     + Last decr time | LOG_C  |
 *     +----------------+--------+
 *
 * LOG_C is a logarithmic counter that provides an indication of the access
 * frequency. However this field must also be decremented otherwise what used
 * to be a frequently accessed key in the past, will remain ranked like that
 * forever, while we want the algorithm to adapt to access pattern changes.
 *
 * So the remaining 16 bits are used in order to store the "decrement time",
 * a reduced-precision Unix time (we take 16 bits of the time converted
 * in minutes since we don't care about wrapping around) where the LOG_C
 * counter is halved if it has an high value, or just decremented if it
 * has a low value.
 *
 * New keys don't start at zero, in order to have the ability to collect
 * some accesses before being trashed away, so they start at LFU_INIT_VAL.
 * The logarithmic increment performed on LOG_C takes care of LFU_INIT_VAL
 * when incrementing the key, so that keys starting at LFU_INIT_VAL
 * (or having a smaller value) have a very high chance of being incremented
 * on access.
 *
 * During decrement, the value of the logarithmic counter is halved if
 * its current value is greater than two times the LFU_INIT_VAL, otherwise
 * it is just decremented by one.
 * --------------------------------------------------------------------------*/

/* Return the current time in minutes, just taking the least significant
 * 16 bits. The returned time is suitable to be stored as LDT (last decrement
 * time) for the LFU implementation. */
//用于记录最近一次访问的时间（分钟级别，45天溢出一次）
unsigned long LFUGetTimeInMinutes(void) {
    return (server.unixtime/60) & 65535;//计算分钟级别时间，并且取低16位
}

/* Given an object last access time, compute the minimum number of minutes
 * that elapsed since the last access. Handle overflow (ldt greater than
 * the current 16 bits minutes time) considering the time as wrapping
 * exactly once. */
//计算已经过去的时间
unsigned long LFUTimeElapsed(unsigned long ldt) {
    unsigned long now = LFUGetTimeInMinutes();//获得当前的分钟数
    if (now >= ldt) return now-ldt;//当前时间大于上一次访问时间，直接计算过去了多少时间
    return 65535-ldt+now;//当前时间小于上一次访问时间，考虑到时间溢出，计算时间差
}

/* Logarithmically increment a counter. The greater is the current counter value
 * the less likely is that it gets really incremented. Saturate it at 255. */
//计数器的值月大，增加的概率就越小，该算法根据访问对象的频率来动态调整计数器的值,以准确的放映对象的使用频率
uint8_t LFULogIncr(uint8_t counter) {
    //counter有一定的概率增加
    if (counter == 255) return 255;//已经达到了上限，不再增加
    double r = (double)rand()/RAND_MAX;//计算一个0-1之间的随机数
    double baseval = counter - LFU_INIT_VAL;//计算一个基准值
    if (baseval < 0) baseval = 0;//b避免成负数

    double p = 1.0/(baseval*server.lfu_log_factor+1);
    if (r < p) counter++;
    return counter;
}

/* If the object decrement time is reached decrement the LFU counter but
 * do not update LFU fields of the object, we update the access time
 * and counter in an explicit way when the object is really accessed.
 * And we will times halve the counter according to the times of
 * elapsed time than server.lfu_decay_time.
 * Return the object frequency counter.
 *
 * This function is used in order to scan the dataset for the best object
 * to fit: as we check for the candidate, we incrementally decrement the
 * counter of the scanned objects if needed. */
//如果counter只是单向递增，那么在LDT周期内，一旦counter达到了255,就不会被淘汰了（很难被淘汰），反映到实际（一些过期的热点的key不能及时被删除）
//为了避免这个情况，redis对counter做衰减，
unsigned long LFUDecrAndReturn(robj *o) {
    unsigned long ldt = o->lru >> 8;//获得LDT时间戳
    unsigned long counter = o->lru & 255;//获得couter部分的值
    //根据lfu_decay_time中的配置的值来决定couter衰减的值
    unsigned long num_periods = server.lfu_decay_time ? LFUTimeElapsed(ldt) / server.lfu_decay_time : 0;
    //num_period是计算已经过去的时间段数（LDT过去的周期数）
    if (num_periods)
        counter = (num_periods > counter) ? 0 : counter - num_periods;
    return counter;
}

/* We don't want to count AOF buffers and slaves output buffers as
 * used memory: the eviction should use mostly data size, because
 * it can cause feedback-loop when we push DELs into them, putting
 * more and more DELs will make them bigger, if we count them, we
 * need to evict more keys, and then generate more DELs, maybe cause
 * massive eviction loop, even all keys are evicted.
 *
 * This function returns the sum of AOF and replication buffer. */
size_t freeMemoryGetNotCountedMemory(void) {
    size_t overhead = 0;

    /* Since all replicas and replication backlog share global replication
     * buffer, we think only the part of exceeding backlog size is the extra
     * separate consumption of replicas.
     *
     * Note that although the backlog is also initially incrementally grown
     * (pushing DELs consumes memory), it'll eventually stop growing and
     * remain constant in size, so even if its creation will cause some
     * eviction, it's capped, and also here to stay (no resonance effect)
     *
     * Note that, because we trim backlog incrementally in the background,
     * backlog size may exceeds our setting if slow replicas that reference
     * vast replication buffer blocks disconnect. To avoid massive eviction
     * loop, we don't count the delayed freed replication backlog into used
     * memory even if there are no replicas, i.e. we still regard this memory
     * as replicas'. */
    if ((long long)server.repl_buffer_mem > server.repl_backlog_size) {
        /* We use list structure to manage replication buffer blocks, so backlog
         * also occupies some extra memory, we can't know exact blocks numbers,
         * we only get approximate size according to per block size. */
        size_t extra_approx_size =
            (server.repl_backlog_size/PROTO_REPLY_CHUNK_BYTES + 1) *
            (sizeof(replBufBlock)+sizeof(listNode));
        size_t counted_mem = server.repl_backlog_size + extra_approx_size;
        if (server.repl_buffer_mem > counted_mem) {
            overhead += (server.repl_buffer_mem - counted_mem);
        }
    }

    if (server.aof_state != AOF_OFF) {
        overhead += sdsAllocSize(server.aof_buf);
    }
    return overhead;
}

/* Get the memory status from the point of view of the maxmemory directive:
 * if the memory used is under the maxmemory setting then C_OK is returned.
 * Otherwise, if we are over the memory limit, the function returns
 * C_ERR.
 *
 * The function may return additional info via reference, only if the
 * pointers to the respective arguments is not NULL. Certain fields are
 * populated only when C_ERR is returned:
 *
 *  'total'     total amount of bytes used.
 *              (Populated both for C_ERR and C_OK)
 *
 *  'logical'   the amount of memory used minus the slaves/AOF buffers.
 *              (Populated when C_ERR is returned)
 *
 *  'tofree'    the amount of memory that should be released
 *              in order to return back into the memory limits.
 *              (Populated when C_ERR is returned)
 *
 *  'level'     this usually ranges from 0 to 1, and reports the amount of
 *              memory currently used. May be > 1 if we are over the memory
 *              limit.
 *              (Populated both for C_ERR and C_OK)
 */
//total是记录redis已经分配的内存，第三个参数是待释放的内存空间
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level) {
    size_t mem_reported, mem_used, mem_tofree;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. */
    mem_reported = zmalloc_used_memory();//已经使用的内存空间
    if (total) *total = mem_reported;

    /* We may return ASAP if there is no need to compute the level. */
    if (!server.maxmemory) {
        if (level) *level = 0;
        return C_OK;
    }
    if (mem_reported <= server.maxmemory && !level) return C_OK;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    mem_used = mem_reported;//redis总共使用的内存空间
    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used-overhead : 0;

    /* Compute the ratio of memory usage. */
    if (level) *level = (float)mem_used / (float)server.maxmemory;

    if (mem_reported <= server.maxmemory) return C_OK;

    /* Check if we are still over the memory limit. */
    if (mem_used <= server.maxmemory) return C_OK;

    /* Compute how much memory we need to free. */
    //走到这里，使用的内存就超过了上限
    mem_tofree = mem_used - server.maxmemory;//计算需要释放的内存大小 

    if (logical) *logical = mem_used;
    if (tofree) *tofree = mem_tofree;

    return C_ERR;
}

/* Return 1 if used memory is more than maxmemory after allocating more memory,
 * return 0 if not. Redis may reject user's requests or evict some keys if used
 * memory exceeds maxmemory, especially, when we allocate huge memory at once. */
int overMaxmemoryAfterAlloc(size_t moremem) {
    if (!server.maxmemory) return  0; /* No limit. */

    /* Check quickly. */
    size_t mem_used = zmalloc_used_memory();
    if (mem_used + moremem <= server.maxmemory) return 0;

    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used - overhead : 0;
    return mem_used + moremem > server.maxmemory;
}

/* The evictionTimeProc is started when "maxmemory" has been breached and
 * could not immediately be resolved.  This will spin the event loop with short
 * eviction cycles until the "maxmemory" condition has resolved or there are no
 * more evictable items.  */
static int isEvictionProcRunning = 0;
//淘汰超时，会创建这个时间时间，在下次主线程处理时间事件的时候，继续进入对内存进行淘汰
static int evictionTimeProc(
        struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    if (performEvictions() == EVICT_RUNNING) return 0;  /* keep evicting */

    /* For EVICT_OK - things are good, no need to keep evicting.
     * For EVICT_FAIL - there is nothing left to evict.  */
    isEvictionProcRunning = 0;
    return AE_NOMORE;
}

void startEvictionTimeProc(void) {
    if (!isEvictionProcRunning) {
        isEvictionProcRunning = 1;
        aeCreateTimeEvent(server.el, 0,
                evictionTimeProc, NULL, NULL);//创建一个事件时间
    }
}

/* Check if it's safe to perform evictions.
 *   Returns 1 if evictions can be performed
 *   Returns 0 if eviction processing should be skipped
 */
static int isSafeToPerformEvictions(void) {
    /* - There must be no script in timeout condition.
     * - Nor we are loading data right now.  */
    if (isInsideYieldingLongCommand() || server.loading) return 0;

    /* By default replicas should ignore maxmemory
     * and just be masters exact copies. */
    if (server.masterhost && server.repl_slave_ignore_maxmemory) return 0;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    if (checkClientPauseTimeoutAndReturnIfPaused()) return 0;

    return 1;
}

/* Algorithm for converting tenacity (0-100) to a time limit.  */
//计算此次内存淘汰的超时时长
static unsigned long evictionTimeLimitUs() {
    //超时时长的计算和server.maxmemory_eviction_tenacit正相关
    serverAssert(server.maxmemory_eviction_tenacity >= 0);
    serverAssert(server.maxmemory_eviction_tenacity <= 100);

    if (server.maxmemory_eviction_tenacity <= 10) {
        /* A linear progression from 0..500us */
        //在10以内，比较保守的驱逐方式
        //耗时在5=0-500微妙
        return 50uL * server.maxmemory_eviction_tenacity;
    }

    if (server.maxmemory_eviction_tenacity < 100) {
        /* A 15% geometric progression, resulting in a limit of ~2 min at tenacity==99  */
        //这个是redis默认的配置，比较积极的经内存淘汰，接受内存使用情况和性能
        //耗时时长按照15%的比例进行增长，不再是正相关，几何计数的增长
        //最大值约2分钟
        return (unsigned long)(500.0 * pow(1.15, server.maxmemory_eviction_tenacity - 10.0));
    }
    //
    return ULONG_MAX;   /* No limit to eviction time */
}

/* Check that memory usage is within the current "maxmemory" limit.  If over
 * "maxmemory", attempt to free memory by evicting data (if it's safe to do so).
 *
 * It's possible for Redis to suddenly be significantly over the "maxmemory"
 * setting.  This can happen if there is a large allocation (like a hash table
 * resize) or even if the "maxmemory" setting is manually adjusted.  Because of
 * this, it's important to evict for a managed period of time - otherwise Redis
 * would become unresponsive while evicting.
 *
 * The goal of this function is to improve the memory situation - not to
 * immediately resolve it.  In the case that some items have been evicted but
 * the "maxmemory" limit has not been achieved, an aeTimeProc will be started
 * which will continue to evict items until memory limits are achieved or
 * nothing more is evictable.
 *
 * This should be called before execution of commands.  If EVICT_FAIL is
 * returned, commands which will result in increased memory usage should be
 * rejected.
 *
 * Returns:
 *   EVICT_OK       - memory is OK or it's not possible to perform evictions now 淘汰内存完了
 *   EVICT_RUNNING  - memory is over the limit, but eviction is still processing 经过此次淘汰，内存还没有降低maxmemory以下，但是执行淘汰的时间上限了，不能继续阻塞在这里，我们就会放一个时间时间，等到触发后继续淘汰key
 *   EVICT_FAIL     - memory is over the limit, and there's nothing to evict
 * */
//这个是内存淘汰的入口，当他无法淘汰任何key的时候，就会返回EVICT_FAIL，表示redis内存已经满了
int performEvictions(void) {
    /* Note, we don't goto update_metrics here because this check skips eviction
     * as if it wasn't triggered. it's a fake EVICT_OK. */
    //判断当前内存使用情况
    if (!isSafeToPerformEvictions()) return EVICT_OK;
    //可以执行内存淘汰
    int keys_freed = 0;
    size_t mem_reported, mem_tofree;//第一个是内存总共使用的大小，第二个是需要释放的内存空间大小
    long long mem_freed; /* May be negative */
    mstime_t latency, eviction_latency;
    long long delta;
    int slaves = listLength(server.slaves);
    int result = EVICT_FAIL;
    //等待释放的内存空间是在getMaxmemoryState中计算，他的返回值表示是否需要进行内存淘汰
    if (getMaxmemoryState(&mem_reported,NULL,&mem_tofree,NULL) == C_OK) {
        result = EVICT_OK;
        goto update_metrics;
    }

    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION) {
        result = EVICT_FAIL;  /* We need to free memory, but policy forbids. */
        goto update_metrics;
    }
    //计算此次内存淘汰的超时时长
    unsigned long eviction_time_limit_us = evictionTimeLimitUs();

    mem_freed = 0;//计算释放了多少内存

    latencyStartMonitor(latency);

    monotime evictionTimer;
    elapsedStart(&evictionTimer);//启动计时器，用于记录内存淘汰的总时长

    /* Unlike active-expire and blocked client, we can reach here from 'CONFIG SET maxmemory'
     * so we have to back-up and restore server.core_propagates. */
    int prev_core_propagates = server.core_propagates;
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;
    server.propagate_no_multi = 1;//在执行内存替换的时候，就将no_multi设置为1,来进行区分

    while (mem_freed < (long long)mem_tofree) {//进入循环判断是否释放了足够多的内存
        int j, k, i;
        static unsigned int next_db = 0;
        sds bestkey = NULL;
        int bestdbid;
        redisDb *db;
        dict *dict;
        dictEntry *de;
        //优先删除key
        //如果
        if (server.maxmemory_policy & (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU) ||
            server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL)
        {
            //在LRU和LFU策略下，使用到了EictionPoolLRU缓冲池
            struct evictionPoolEntry *pool = EvictionPoolLRU;
            //一次只会淘汰一个，如果内存还是不够，就继续进入再淘汰一次
            while (bestkey == NULL) {
                unsigned long total_keys = 0, keys;

                /* We don't want to make local-db choices when expiring keys,
                 * so to start populate the eviction pool sampling keys from
                 * every DB. */
                //更新待淘汰的候选键值对集合
                for (i = 0; i < server.dbnum; i++) {
                    db = server.db+i;//获得到他是第几号数据库
                    dict = (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) ?
                            db->dict : db->expires;//选择操作的数据库
                            //如果是需要allkey的策略，就需要在所有key的字典中查询，否则就只在过期的key中查询
                    if ((keys = dictSize(dict)) != 0) {
                        //如果里面存储的key数量不为0
                        //在里面采样填充evictPoolLRU逻辑在这个里面执行
                        evictionPoolPopulate(i, dict, db->dict, pool);//从每个数据库中采样需要淘汰的key
                        //现在已经填充完毕了
                        total_keys += keys;//计算整个系统的key数量
                    }
                }
                if (!total_keys) break; /* No keys to evict. */

                /* Go backward from best to worst element to evict. */
                //会根据piil池的尾部，选择bestkey进行淘汰
                //从最后一个元素开始进行遍历，选取bestkey进行淘汰
                //一次淘汰池中只会选择一个进行淘汰，之后会继续进入进行填充数据到淘汰池中
                for (k = EVPOOL_SIZE-1; k >= 0; k--) {
                    if (pool[k].key == NULL) continue;
                    bestdbid = pool[k].dbid;
                    //如果是allkey，就需要在全局查找到对应的entry，对应的有效数据
                    if (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) {
                        de = dictFind(server.db[bestdbid].dict,
                            pool[k].key);//根据当前的
                    } else {
                        //否则就，只需要在expires中查找到entry元素，里面的有效元素是过期时间
                        de = dictFind(server.db[bestdbid].expires,
                            pool[k].key);
                    }

                    /* Remove the entry from the pool. */
                    //选中了这个key，这个key就需要空出来，留待下一次使用
                    if (pool[k].key != pool[k].cached)
                        sdsfree(pool[k].key);//删除这个位置
                    pool[k].key = NULL;
                    pool[k].idle = 0;

                    /* If the key exists, is our pick. Otherwise it is
                     * a ghost and we need to try the next element. */
                    if (de) {
                        bestkey = dictGetKey(de);//获得到这个元素的key
                        break;
                    } else {
                        /* Ghost... Iterate again. */
                    }
                }
            }
        }

        /* volatile-random and allkeys-random policy */
        else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
                 server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
        {
            /* When evicting a random key, we try to evict a key for
             * each DB, so we use the static 'next_db' variable to
             * incrementally visit all DBs. */
            for (i = 0; i < server.dbnum; i++) {
                j = (++next_db) % server.dbnum;
                db = server.db+j;
                dict = (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ?
                        db->dict : db->expires;
                if (dictSize(dict) != 0) {
                    de = dictGetRandomKey(dict);
                    bestkey = dictGetKey(de);
                    bestdbid = j;
                    break;
                }
            }
        }
        //选中了需要的key
        /* Finally remove the selected key. */
        if (bestkey) {
            db = server.db+bestdbid;
            robj *keyobj = createStringObject(bestkey,sdslen(bestkey));//创建一个key对象
            /* We compute the amount of memory freed by db*Delete() alone.
             * It is possible that actually the memory needed to propagate
             * the DEL in AOF and replication link is greater than the one
             * we are freeing removing the key, but we can't account for
             * that otherwise we would never exit the loop.
             *
             * Same for CSC invalidation messages generated by signalModifiedKey.
             *
             * AOF and Output buffer memory will be freed eventually so
             * we only care about memory used by the key space. */
            delta = (long long) zmalloc_used_memory();
            latencyStartMonitor(eviction_latency);
            if (server.lazyfree_lazy_eviction)
                dbAsyncDelete(db,keyobj);//找到了对应的key，就进行异步的删除
            else
                dbSyncDelete(db,keyobj);//或者是进行同步的删除


            latencyEndMonitor(eviction_latency);
            latencyAddSampleIfNeeded("eviction-del",eviction_latency);
            delta -= (long long) zmalloc_used_memory();//delta里面存储的就是单次释放的内存
            mem_freed += delta;//对内存量进行增加
            server.stat_evictedkeys++;
            signalModifiedKey(NULL,db,keyobj);
            notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted",
                keyobj, db->id);
            propagateDeletion(db,keyobj,server.lazyfree_lazy_eviction);//在执行内存淘汰的时候，就会生成一个del命令在aof中，
            decrRefCount(keyobj);
            keys_freed++;
            //如果淘汰了16个key(因为缓冲池里面最多就能存储16个元素)，就会检查内存大小是否已经满足条件了，以及淘汰操作是否超时，如果满足条件
            //就停止淘汰，
            if (keys_freed % 16 == 0) {
                /* When the memory to free starts to be big enough, we may
                 * start spending so much time here that is impossible to
                 * deliver data to the replicas fast enough, so we force the
                 * transmission here inside the loop. */
                if (slaves) flushSlavesOutputBuffers();

                /* Normally our stop condition is the ability to release
                 * a fixed, pre-computed amount of memory. However when we
                 * are deleting objects in another thread, it's better to
                 * check, from time to time, if we already reached our target
                 * memory, since the "mem_freed" amount is computed only
                 * across the dbAsyncDelete() call, while the thread can
                 * release the memory all the time. */
                //如果开启了lazy-free，淘汰的key，就会进入到后台线程进行释放
                if (server.lazyfree_lazy_eviction) {
                    if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                        //每次删除16个的时候就进行一次检测，如果内存充足，就停止淘汰
                        break;
                    }
                }

                /* After some time, exit the loop early - even if memory limit
                 * hasn't been reached.  If we suddenly need to free a lot of
                 * memory, don't want to spend too much time here.  */
                //查看计时器至今过了多少时间
                if (elapsedUs(evictionTimer) > eviction_time_limit_us) {
                    // We still need to free memory - start eviction timer proc
                    startEvictionTimeProc();//如果发生了超时，就需要启动一个时间事件
                    break;
                }
            }
        } else {
            goto cant_free; /* nothing to free... */
        }
    }
    /* at this point, the memory is OK, or we have reached the time limit */
    result = (isEvictionProcRunning) ? EVICT_RUNNING : EVICT_OK;

cant_free:
    if (result == EVICT_FAIL) {
        /* At this point, we have run out of evictable items.  It's possible
         * that some items are being freed in the lazyfree thread.  Perform a
         * short wait here if such jobs exist, but don't wait long.  */
        //如果经历了上面的淘汰逻辑之后，淘汰了所有可能的key，但是内存仍然超过了maxmemory的值，
        mstime_t lazyfree_latency;
        latencyStartMonitor(lazyfree_latency);
        //就会查看BIO队列中，是否有等待后台线程删除，如果有的话，可以阻塞一些事件等待后台删除这些key
        while (bioPendingJobsOfType(BIO_LAZY_FREE) &&
              elapsedUs(evictionTimer) < eviction_time_limit_us) {
            if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {//查看年内存使用情况
                result = EVICT_OK;
                break;
            }
            //阻塞一些事件
            usleep(eviction_time_limit_us < 1000 ? eviction_time_limit_us : 1000);
        }
        latencyEndMonitor(lazyfree_latency);
        latencyAddSampleIfNeeded("eviction-lazyfree",lazyfree_latency);
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant */

    /* Propagate all DELs */
    propagatePendingCommands();

    server.core_propagates = prev_core_propagates;
    server.propagate_no_multi = 0;

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("eviction-cycle",latency);

update_metrics:
    if (result == EVICT_RUNNING || result == EVICT_FAIL) {
        if (server.stat_last_eviction_exceeded_time == 0)
            elapsedStart(&server.stat_last_eviction_exceeded_time);
    } else if (result == EVICT_OK) {
        if (server.stat_last_eviction_exceeded_time != 0) {
            server.stat_total_eviction_exceeded_time += elapsedUs(server.stat_last_eviction_exceeded_time);
            server.stat_last_eviction_exceeded_time = 0;
        }
    }
    return result;
}
