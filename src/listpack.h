/* Listpack -- A lists of strings serialization format
 *
 * This file implements the specification you can find at:
 *
 *  https://github.com/antirez/listpack
 *
 * Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __LISTPACK_H
#define __LISTPACK_H

#include <stdlib.h>
#include <stdint.h>

#define LP_INTBUF_SIZE 21 /* 20 digits of -2^63 + 1 null term = 21. */

/* lpInsert() where argument possible values: */
#define LP_BEFORE 0
#define LP_AFTER 1
#define LP_REPLACE 2

/* Each entry in the listpack is either a string or an integer. */
typedef struct {
    /* When string is used, it is provided with the length (slen). */
    unsigned char *sval;
    uint32_t slen;
    /* When integer is used, 'sval' is NULL, and lval holds the value. */
    long long lval;
} listpackEntry;

/* 

    listpack和ziplist的区别就是listpack的不需要存储前一个元素的字节大小，而是将自己元素的字节大小存储在元素最后backlen字段中
    如果想要进行前向遍历，就需要往后退一个字节，退到前一个元素的backlen的字段中，就能够通过计算前一个元素的大小，就能够将指针指向前一个元素了
    listpack



    listpack的结构
    <totalsize><elementnum><eleme1><eleme2><eleme3><eleme4><end>
    * totalsize：表示listpack这个链表的总字节数字
    * elementnum：表示listpack中存储了多少个元素
    * elem：里面就是每个元素


    entry的三个元素
    <encoding><data><backlen>
    1. encoding：可变长度，里面记录着编码类型和长度，如果是整形的话，encoding的后面几位直接存储的就是data的值
              如果是字符串的话，encoding里面会记录着data的字节大小，encoding字段后面会跟着data
    2. data：如果是字符串的话，就有data字段，里面存储的就是字符串，如果是整形的话，就没有data字段
    3. backlen：可变长度，存储的是element中encoding+data字段的大小，不包含backlen本身
            backlen字段最多占用5个字节，这个字段主要支持前向遍历
*/




unsigned char *lpNew(size_t capacity);
void lpFree(unsigned char *lp);
unsigned char* lpShrinkToFit(unsigned char *lp);
unsigned char *lpInsertString(unsigned char *lp, unsigned char *s, uint32_t slen,
                              unsigned char *p, int where, unsigned char **newp);
unsigned char *lpInsertInteger(unsigned char *lp, long long lval,
                               unsigned char *p, int where, unsigned char **newp);
unsigned char *lpPrepend(unsigned char *lp, unsigned char *s, uint32_t slen);
unsigned char *lpPrependInteger(unsigned char *lp, long long lval);
unsigned char *lpAppend(unsigned char *lp, unsigned char *s, uint32_t slen);
unsigned char *lpAppendInteger(unsigned char *lp, long long lval);
unsigned char *lpReplace(unsigned char *lp, unsigned char **p, unsigned char *s, uint32_t slen);
unsigned char *lpReplaceInteger(unsigned char *lp, unsigned char **p, long long lval);
unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp);
unsigned char *lpDeleteRangeWithEntry(unsigned char *lp, unsigned char **p, unsigned long num);
unsigned char *lpDeleteRange(unsigned char *lp, long index, unsigned long num);
unsigned char *lpMerge(unsigned char **first, unsigned char **second);
unsigned long lpLength(unsigned char *lp);
unsigned char *lpGet(unsigned char *p, int64_t *count, unsigned char *intbuf);
unsigned char *lpGetValue(unsigned char *p, unsigned int *slen, long long *lval);
unsigned char *lpFind(unsigned char *lp, unsigned char *p, unsigned char *s, uint32_t slen, unsigned int skip);
unsigned char *lpFirst(unsigned char *lp);//获得element的第一个元素的指针
unsigned char *lpLast(unsigned char *lp);//获得最后一个节点的位置
unsigned char *lpNext(unsigned char *lp, unsigned char *p);
unsigned char *lpPrev(unsigned char *lp, unsigned char *p);
size_t lpBytes(unsigned char *lp);
unsigned char *lpSeek(unsigned char *lp, long index);
typedef int (*listpackValidateEntryCB)(unsigned char *p, unsigned int head_count, void *userdata);
int lpValidateIntegrity(unsigned char *lp, size_t size, int deep,
                        listpackValidateEntryCB entry_cb, void *cb_userdata);
unsigned char *lpValidateFirst(unsigned char *lp);
int lpValidateNext(unsigned char *lp, unsigned char **pp, size_t lpbytes);
unsigned int lpCompare(unsigned char *p, unsigned char *s, uint32_t slen);
void lpRandomPair(unsigned char *lp, unsigned long total_count, listpackEntry *key, listpackEntry *val);
void lpRandomPairs(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals);
unsigned int lpRandomPairsUnique(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals);
int lpSafeToAdd(unsigned char* lp, size_t add);
void lpRepr(unsigned char *lp);

#ifdef REDIS_TEST
int listpackTest(int argc, char *argv[], int flags);
#endif

#endif
