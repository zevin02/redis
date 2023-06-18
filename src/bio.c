/* Background I/O service for Redis.
 *
 * This file implements operations that we need to perform in the background.
 * Currently there is only a single operation, that is a background close(2)
 * system call. This is needed as when the process is the last owner of a
 * reference to a file closing it means unlinking it, and the deletion of the
 * file is slow, blocking the server.
 *
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put here Redis specific background tasks (for instance
 * it is not impossible that we'll need a non blocking FLUSHDB/FLUSHALL
 * implementation).
 *
 * DESIGN
 * ------
 *
 * The design is trivial, we have a structure representing a job to perform
 * and a different thread and job queue for every job type.
 * Every thread waits for new jobs in its queue, and process every job
 * sequentially.
 *
 * Jobs of the same type are guaranteed to be processed from the least
 * recently inserted to the most recently inserted (older jobs processed
 * first).
 *
 * Currently there is no way for the creator of the job to be notified about
 * the completion of the operation, this will only be added when/if needed.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

static pthread_t bio_threads[BIO_NUM_OPS];
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];//每个线程都会关联到一个锁，主线程可以暂停这个后台线程
static pthread_cond_t bio_newjob_cond[BIO_NUM_OPS];//存储每个bio后台线程关联的条件变量，用于通知bio后台线程有新的任务需要处理，消息等待和通知
static pthread_cond_t bio_step_cond[BIO_NUM_OPS];//
static list *bio_jobs[BIO_NUM_OPS];//bio_job的任务队列，每个线程中都有一个队列是任务
/* The following array is used to hold the number of pending jobs for every
 * OP type. This allows us to export the bioPendingJobsOfType() API that is
 * useful when the main thread wants to perform some operation that may involve
 * objects shared with the background thread. The main thread will just wait
 * that there are no longer jobs of this type to be executed before performing
 * the sensible operation. This data is also useful for reporting. */
static unsigned long long bio_pending[BIO_NUM_OPS];//存储每个后台线程待处理的任务个数

/* This structure represents a background Job. It is only used locally to this
 * file as the API does not expose the internals at all. */
typedef union bio_job {
    /* Job specific arguments.*/
    struct {
        int fd; /* Fd for file based background jobs 需要操作的文件描述符*/
        unsigned need_fsync:1; /* A flag to indicate that a fsync is required before
                                * the file is closed. 是否需要刷盘在文件关闭的时候*/
    } fd_args;

    struct {
        lazy_free_fn *free_fn; /* Function that will free the provided arguments 这些要处理的对象对应的要处理的回调函数*/
        void *free_args[]; /* List of arguments to be passed to the free function 这个就是要处理的任务的对象*/
    } free_args;
} bio_job;

void *bioProcessBackgroundJobs(void *arg);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
//这个是线程栈的要求的最小的大小
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/* Initialize the background system, spawning the thread. */
//初始化这些BIO线程
void bioInit(void) {
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;
    int j;

    /* Initialization of state vars and objects */
    for (j = 0; j < BIO_NUM_OPS; j++) {
        pthread_mutex_init(&bio_mutex[j],NULL);
        pthread_cond_init(&bio_newjob_cond[j],NULL);
        pthread_cond_init(&bio_step_cond[j],NULL);
        bio_jobs[j] = listCreate();//创建一个链表
        bio_pending[j] = 0;
    }

    /* Set the stack size as by default it may be small in some system */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr,&stacksize);//获得线程栈大小
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes 设置默认线程栈大小*/
    //在后续会对他更新到足够的大小
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;//确保栈大小足够大
    pthread_attr_setstacksize(&attr, stacksize);//重新设置线程栈的大小

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass the job ID the thread is
     * responsible of. */
    for (j = 0; j < BIO_NUM_OPS; j++) {
        void *arg = (void*)(unsigned long) j;
        if (pthread_create(&thread,&attr,bioProcessBackgroundJobs,arg) != 0) {
            serverLog(LL_WARNING,"Fatal: Can't initialize Background Jobs.");
            exit(1);
        }
        bio_threads[j] = thread;
    }
}

void bioSubmitJob(int type, bio_job *job) {
    pthread_mutex_lock(&bio_mutex[type]);//主线程把这个锁给锁上，避免其他线程误操作
    listAddNodeTail(bio_jobs[type],job);//把这个添加到任务队列中
    bio_pending[type]++;//待处理的数加1
    pthread_cond_signal(&bio_newjob_cond[type]);//同时唤醒该线程
    pthread_mutex_unlock(&bio_mutex[type]);
}
//创建一个异步的延迟释放任务,lazyfreeFreeObject这个是他的函数指针
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    va_list valist;//创建一个变量参数列表
    /* Allocate memory for the job structure and all required
     * arguments */
    //为任务结构体的所有参数分配内存空间
    bio_job *job = zmalloc(sizeof(*job) + sizeof(void *) * (arg_count));
    //设置任务结构体的释放函数
    job->free_args.free_fn = free_fn;
    //开始变量参数列表
    va_start(valist, arg_count);
    //遍历所有的参数
    for (int i = 0; i < arg_count; i++) {
        //从变量参数列表中获得参数值，将其存储到任务结构体的参数数组中
        job->free_args.free_args[i] = va_arg(valist, void *);
    }
    //结束变量参数列表
    va_end(valist);
    //提交任务到异步任务队列
    bioSubmitJob(BIO_LAZY_FREE, job);
}

void bioCreateCloseJob(int fd, int need_fsync) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.need_fsync = need_fsync;

    bioSubmitJob(BIO_CLOSE_FILE, job);
}

void bioCreateFsyncJob(int fd) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;

    bioSubmitJob(BIO_AOF_FSYNC, job);
}


//BIO后台线程与IO线程很像，但是IO线程是通过自旋的方式来等待新的读写任务，而bio是通过条件变量方式来进行处理
//因为IO任务发生的频率很频繁，使用自旋可以减少线程切换的成本


//这里因为3个线程的都是走这个函数
void *bioProcessBackgroundJobs(void *arg) {
    bio_job *job;
    unsigned long type = (unsigned long) arg;
    sigset_t sigset;

    /* Check that the type is within the right interval. */
    if (type >= BIO_NUM_OPS) {
        serverLog(LL_WARNING,
            "Warning: bio thread started with wrong type %lu",type);
        return NULL;
    }

    switch (type) {
    case BIO_CLOSE_FILE:
        redis_set_thread_title("bio_close_file");//关闭文件,设置当前线程的名字
        break;
    case BIO_AOF_FSYNC:
        redis_set_thread_title("bio_aof_fsync");
        break;
    case BIO_LAZY_FREE:
        redis_set_thread_title("bio_lazy_free");
        break;
    }

    redisSetCpuAffinity(server.bio_cpulist);

    makeThreadKillable();

    pthread_mutex_lock(&bio_mutex[type]);//加锁
    /* Block SIGALRM so we are sure that only the main thread will
     * receive the watchdog signal. */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))//该线程把SIGALARM给阻塞掉了，确保只有主线程能够收到这个信号(检查网络延迟之类的)
        serverLog(LL_WARNING,
            "Warning: can't mask SIGALRM in bio.c thread: %s", strerror(errno));
    //循环处理任务
    //上面的代码3个线程各自都只会执行一次
    //执行完了，就到下面的循环中了
    while(1) {
        listNode *ln;//需要处理的任务的节点

        /* The loop always starts with the lock hold. */
        if (listLength(bio_jobs[type]) == 0) {//如果没有任务需要处理，就需要进入休眠等待状态
            pthread_cond_wait(&bio_newjob_cond[type],&bio_mutex[type]);
            continue;//因为这里continue，所以会重新到while循环中判断一次
        }
        //到这里，就需要正常处理任务了
        /* Pop the job from the queue. */
        ln = listFirst(bio_jobs[type]);
        job = ln->value;//获得任务
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
        pthread_mutex_unlock(&bio_mutex[type]);//这个是为了允许其他线程可以同步访问后台系统处理任务

        /* Process the job accordingly to its type. */
        if (type == BIO_CLOSE_FILE) {
            if (job->fd_args.need_fsync) {//如果需要刷盘
            //这个底层会调用fdatasync来进行刷盘，fdatasync的速度比fsync要快
            //fdatasync只会等待文件的数据写入，而不会等待文件的元数据
                redis_fsync(job->fd_args.fd);
            }
            close(job->fd_args.fd);//关闭该文件
        } else if (type == BIO_AOF_FSYNC) {
            /* The fd may be closed by main thread and reused for another
             * socket, pipe, or file. We just ignore these errno because
             * aof fsync did not really fail. */
            if (redis_fsync(job->fd_args.fd) == -1 &&
                errno != EBADF && errno != EINVAL)
            {
                int last_status;
                atomicGet(server.aof_bio_fsync_status,last_status);
                atomicSet(server.aof_bio_fsync_status,C_ERR);
                atomicSet(server.aof_bio_fsync_errno,errno);
                if (last_status == C_OK) {
                    serverLog(LL_WARNING,
                        "Fail to fsync the AOF file: %s",strerror(errno));
                }
            } else {
                atomicSet(server.aof_bio_fsync_status,C_OK);
            }
        } else if (type == BIO_LAZY_FREE) {
            job->free_args.free_fn(job->free_args.free_args);//走到lazyfree中
        } else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);//回收任务实例本身

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex[type]);//重新上锁
        listDelNode(bio_jobs[type],ln);//该节点已经处理完成就进行删除
        bio_pending[type]--;//处理的事件数减少

        /* Unblock threads blocked on bioWaitStepOfType() if any. */
        pthread_cond_broadcast(&bio_step_cond[type]);
    }
}

/* Return the number of pending jobs of the specified type. */
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* If there are pending jobs for the specified type, the function blocks
 * and waits that the next job was processed. Otherwise the function
 * does not block and returns ASAP.
 *
 * The function returns the number of jobs still to process of the
 * requested type.
 *
 * This function is useful when from another thread, we want to wait
 * a bio.c thread to do more work in a blocking way.
 */
unsigned long long bioWaitStepOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    if (val != 0) {
        pthread_cond_wait(&bio_step_cond[type],&bio_mutex[type]);
        val = bio_pending[type];
    }
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
void bioKillThreads(void) {
    int err, j;

    for (j = 0; j < BIO_NUM_OPS; j++) {
        if (bio_threads[j] == pthread_self()) continue;
        if (bio_threads[j] && pthread_cancel(bio_threads[j]) == 0) {
            if ((err = pthread_join(bio_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d can not be joined: %s",
                        j, strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d terminated",j);
            }
        }
    }
}
