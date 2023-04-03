/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
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

#include <stdint.h> // for UINTPTR_MAX

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

/* Node, quicklist, and Iterator are the only data structures used currently. */

/* quicklistNode is a 32 byte struct describing a listpack for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max lp bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, PLAIN=1 (a single item as char array), PACKED=2 (listpack with multiple items).
 * recompress: 1 bit, bool, true if node is temporary decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 10 bits, free for future use; pads out the remainder of 32 bits */

//quicklist类似于一个C++的list，双向循环链表

//一个quicklist的node
//由于quicklist节点的设计，所以在往对尾插入节点的时候是找到最后一个quuicklistnode，用quicklistnode的entry插入到listpack中
//quicklist使用双向链表和listpack为了空间和时间上的折中
//1.双向链表在首尾插入的时间复杂度为O（1），但是每个节点都会保持一个指向前后的指针，就会导致内存浪费，和内存碎片，而listpack是一段连续的内存空间，不需要指针，所以不会产生内存碎片
//同时每个元素根据自身的大小来编码，尽可能的减少每个元素占用的内存空间

//2.对于修改，插入，删除元素的时候，listpack的效率就比较低，需要进行一个内存拷贝，尤其在一块连续的空间很大的时候，依次内存拷贝会涉及到大量的数据
//redis为了同时使用这两个结构的优点，规避两者的缺点，就出现了quicklist的数据结构了，所以每个node中的listpack都是比较小的listpack（会有一定的阀值），避免大的listpack出现，而listpack中存了很多元素可以减少内存碎片出现

//quicklist这种双端链表的使用是频繁的对

typedef struct quicklistNode {
    struct quicklistNode *prev;//指向前一个quicklistnode节点
    struct quicklistNode *next;//指向后一个quicklistnode节点
    unsigned char *entry;//一个quicklist节点中存储一个listpack链表,也就是说多个元素存在一个quicklistnode中

    size_t sz;             /* entry size in bytes ,listpack占用多少字节*/
    unsigned int count : 16;     /* count of items in listpack listpack中的元素个数*/
    unsigned int encoding : 2;   /* RAW==1 or LZF==2 encoding表示是否使用压缩，2表示压缩（使用的是LZF压缩算法），1表示不压缩*/
    unsigned int container : 2;  /* PLAIN==1 or PACKED==2 有两个可选值，packed表示哪个指针指向一个listpack实例，多个元素打包在一起，而plain表示entry指向一个单个大的元素，所以entry指针不一定指向listpack*/
    unsigned int recompress : 1; /* was this node previous compressed? 进行一个临时的解压缩*/
    unsigned int attempted_compress : 1; /* node can't compress; too small */
    unsigned int dont_compress : 1; /* prevent compression of entry that will be used later */
    unsigned int extra : 9; /* more bits to steal for future usage */
} quicklistNode;

/* quicklistLZF is a 8+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->entry is compressed, node->entry points to a quicklistLZF */
//listpack被压缩值后，会用quicklistlzf来存储listpack压缩后的数据，
typedef struct quicklistLZF {
    size_t sz; /* LZF size in bytes*///压缩后的字节数
    char compressed[];//压缩后的具体数据
} quicklistLZF;

/* Bookmarks are padded with realloc at the end of of the quicklist struct.
 * They should only be used for very big lists if thousands of nodes were the
 * excess memory usage is negligible, and there's a real need to iterate on them
 * in portions.
 * When not used, they don't add any memory overhead, but when used and then
 * deleted, some overhead remains (to avoid resonance).
 * The number of bookmarks used should be kept to minimum since it also adds
 * overhead on node deletion (searching for a bookmark to update). */
typedef struct quicklistBookmark {
    quicklistNode *node;//对应的节点
    char *name;//对应的名字
} quicklistBookmark;

#if UINTPTR_MAX == 0xffffffff
/* 32-bit */
#   define QL_FILL_BITS 14
#   define QL_COMP_BITS 14
#   define QL_BM_BITS 4
#elif UINTPTR_MAX == 0xffffffffffffffff
/* 64-bit */
#   define QL_FILL_BITS 16
#   define QL_COMP_BITS 16
#   define QL_BM_BITS 4 /* we can encode more, but we rather limit the user
                           since they cause performance degradation. */
#else
#   error unknown arch bits count
#endif

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: 0 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor.
 * 'bookmarks are an optional feature that is used by realloc this struct,
 *      so that they don't consume memory when not used. */
typedef struct quicklist {
    quicklistNode *head;//链表的头指针
    quicklistNode *tail;//链表的尾指针
    unsigned long count;        /* total count of all entries in all listpacks，quicklist中的元素总数，listpack中存储的元素个数总和 */
    unsigned long len;          /* number of quicklistNodes quicklistnode的个数*/
    signed int fill : QL_FILL_BITS;       /* fill factor for individual nodes 存放list-max-listpack-size参数，大于0,代表listpack可以存放的节点个数*/
    unsigned int compress : QL_COMP_BITS; /* depth of end nodes not to compress;0=off 用来存放list-compress-depth参数*/
    unsigned int bookmark_count: QL_BM_BITS;//记录quicklist中的quicklistnode的个数
    quicklistBookmark bookmarks[];//这是一个柔性数组，为某个quicklistnode添加自定义名称，这样就能实现随机访问quicklist的效果
} quicklist;

//quicklist的迭代器，用来屏蔽quicklist复杂的底层实现
typedef struct quicklistIter {
    quicklist *quicklist;//指向当前的quicklist实例
    quicklistNode *current;//指向迭代到的quicklistnode节点
    unsigned char *zi; /* points to the current element in the corresponding listpack*/
    long offset; /* offset in current listpack 当前的entry在listpack中的偏移量，也就是第几个元素（从0开始），offset=-1说明当前没有定位到任何的quicklistnode上，或者定位到的元素中已经没有元素了。*/
    int direction;//当前quicklist的迭代方向，start_head:正向迭代，start_tail反向迭代
} quicklistIter;

//这个和listpack中的zlentry很相似，quicklistentry不会真实存在一个quicklist中，只是用来存放quicklist中元素的值，以及这个元素的位置信息

typedef struct quicklistEntry {
    const quicklist *quicklist;//所属的list实例
    quicklistNode *node;//所属的quicknode节点
    unsigned char *zi;//对应的element（在listpack中）
    unsigned char *value;//如果对应的值为字符串，用这个指针纪律
    long long longval;//如果为字符串，记录字符串的长度
    size_t sz;//如果为整形，记录整数的大小
    int offset;//在listpack中的第几个元素
} quicklistEntry;

#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1
#define QUICKLIST_NODE_ENCODING_LZF 2

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist node container formats */
#define QUICKLIST_NODE_CONTAINER_PLAIN 1
#define QUICKLIST_NODE_CONTAINER_PACKED 2

#define QL_NODE_IS_PLAIN(node) ((node)->container == QUICKLIST_NODE_CONTAINER_PLAIN)

#define quicklistNodeIsCompressed(node)                                        \
    ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
quicklist *quicklistCreate(void);
quicklist *quicklistNew(int fill, int compress);
void quicklistSetCompressDepth(quicklist *quicklist, int depth);
void quicklistSetFill(quicklist *quicklist, int fill);
void quicklistSetOptions(quicklist *quicklist, int fill, int depth);
void quicklistRelease(quicklist *quicklist);
int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz);
int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz);
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where);
void quicklistAppendListpack(quicklist *quicklist, unsigned char *zl);
void quicklistAppendPlainNode(quicklist *quicklist, unsigned char *data, size_t sz);
void quicklistInsertAfter(quicklistIter *iter, quicklistEntry *entry,
                          void *value, const size_t sz);
void quicklistInsertBefore(quicklistIter *iter, quicklistEntry *entry,
                           void *value, const size_t sz);
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry);
void quicklistReplaceEntry(quicklistIter *iter, quicklistEntry *entry,
                           void *data, size_t sz);
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            const size_t sz);
int quicklistDelRange(quicklist *quicklist, const long start, const long stop);
quicklistIter *quicklistGetIterator(quicklist *quicklist, int direction);
quicklistIter *quicklistGetIteratorAtIdx(quicklist *quicklist,
                                         int direction, const long long idx);
quicklistIter *quicklistGetIteratorEntryAtIdx(quicklist *quicklist, const long long index,
                                              quicklistEntry *entry);
int quicklistNext(quicklistIter *iter, quicklistEntry *entry);
void quicklistSetDirection(quicklistIter *iter, int direction);
void quicklistReleaseIterator(quicklistIter *iter);
quicklist *quicklistDup(quicklist *orig);
void quicklistRotate(quicklist *quicklist);
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       size_t *sz, long long *sval,
                       void *(*saver)(unsigned char *data, size_t sz));
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 size_t *sz, long long *slong);
unsigned long quicklistCount(const quicklist *ql);
int quicklistCompare(quicklistEntry *entry, unsigned char *p2, const size_t p2_len);
size_t quicklistGetLzf(const quicklistNode *node, void **data);
void quicklistRepr(unsigned char *ql, int full);

/* bookmarks */
int quicklistBookmarkCreate(quicklist **ql_ref, const char *name, quicklistNode *node);
int quicklistBookmarkDelete(quicklist *ql, const char *name);
quicklistNode *quicklistBookmarkFind(quicklist *ql, const char *name);
void quicklistBookmarksClear(quicklist *ql);
int quicklistisSetPackedThreshold(size_t sz);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[], int flags);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
