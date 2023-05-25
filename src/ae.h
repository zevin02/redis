/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
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

#ifndef __AE_H__
#define __AE_H__

#include "monotonic.h"

#define AE_OK 0
#define AE_ERR -1

//这个就是aeEentLoop结构体中，表示事件的一些特性
#define AE_NONE 0       /* No events registered. */
#define AE_READABLE 1   /* Fire when descriptor is readable.redis中统一使用AE_READABLE和AE_WRITABLE表示可读可写事件 */
#define AE_WRITABLE 2   /* Fire when descriptor is writable. */
#define AE_BARRIER 4    /* With WRITABLE, never fire the event if the
                           READABLE event already fired in the same event
                           loop iteration. Useful when you want to persist
                           things to disk before sending replies, and want
                           to do that in a group fashion. */

#define AE_FILE_EVENTS (1<<0)   //当前循环器处理网络事件
#define AE_TIME_EVENTS (1<<1)   //处理循环器处理时间事件
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT (1<<2)     //在事件没有发生的时候，不会进入阻塞状态
#define AE_CALL_BEFORE_SLEEP (1<<3)
#define AE_CALL_AFTER_SLEEP (1<<4)

#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
//函数指针
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
//抽象了一个网络事件,其中维护了监听的事件以及相应事件的函数指针，
typedef struct aeFileEvent {
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER),事件烟码，用来记录事件的发生 */
    aeFileProc *rfileProc;//发生可读事件，会调用rfileproc指向的函数来处理
    aeFileProc *wfileProc;//法身可写事件，同理
    void *clientData;//指向对应的客户端对象
} aeFileEvent;

/* Time event structure */
//抽象了一个时间事件
typedef struct aeTimeEvent {
    long long id; /* time event identifier. 唯一的标识，通过eventloop->timeeventnextid来计算*/
    monotime when;//时间事件触发的时间戳
    aeTimeProc *timeProc;//处理该时间事件的函数
    aeEventFinalizerProc *finalizerProc;//删除时间事件之前调用该函数
    void *clientData;//该时间时间关联的客户端
    struct aeTimeEvent *prev;   //前后指针
    struct aeTimeEvent *next;  
    int refcount; /* refcount to prevent timer events from being
  		   * freed in recursive time event calls.引用计数，当refcount为0的时候才能释放该时间事件 */
} aeTimeEvent;

/* A fired event */
//由于IO多路复用中实现的mask不同，epoll使用EPOLLIN，EPOLLOUT，kqueue使用EVFILT_READ,
//而redis中统一使用AE——READABLE，AE——WRITABLE来表示可读和可写事件
typedef struct aeFiredEvent {
    int fd;     //与事件相关联的文件描述符
    int mask;   //触发的事件
} aeFiredEvent;

/* State of an event based program */
//这个通常用于异步IO
//管理着一组文件描述符（socket，文件，管道等）

//并且在这些文件描述符中监听事件的发生，当事件发生的时候会调用响应的回调函数来执行
//eventloop是事件驱动的核心结构体

//Redis中事件分为网络读写事件和时间时间（定时触发的事件，判断key是否已经过期了）

//Redis中通过事件该结构体统一处理网络和时间事件


typedef struct aeEventLoop {
    int maxfd;   /* highest file descriptor currently registered 当前注册的fd的最大值*/
    int setsize; /* max number of file descriptors tracked 能够注册的文件描述法的最大值*/
    long long timeEventNextId;  //用于计算事件事件的唯一标识
    aeFileEvent *events; /* Registered events ，events中记录了一个已经注册的网络事件(全部需要监听的），长度为setsize大小*/
    aeFiredEvent *fired; /* Fired events fired是被触发的网络事件（最多能监听的个数时setsize个,epoll——wait后回迁移到这个结构体上来,后续只处理这个即可*/
    aeTimeEvent *timeEventHead; //指向时间事件链表的头节点
    int stop;   //用来进行结束循环的，为1表示停止
    void *apidata; /* This is used for polling API specific data ,redis平台会使用4个IO模型，epoll/evtport....，apidata对其进行封转，指向一个aeApiState实例中*/
    aeBeforeSleepProc *beforesleep;//redis主线程阻塞等待网络事件时，会调用beforesleep,就是在调用epoll_wait之前处理的
    aeBeforeSleepProc *aftersleep;//被唤醒后，会调用aftersleep
    int flags;      //该循环器的一些特性
} aeEventLoop;

/* Prototypes */
//setsize就是能够注册文件描述符的最大值
aeEventLoop *aeCreateEventLoop(int setsize);//创建该结构体，在服务器启动的时候
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
