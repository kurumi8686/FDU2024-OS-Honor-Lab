#include <kernel/mem.h>
#include <kernel/sched.h>
#include <fs/pipe.h>
#include <common/string.h>
#include <kernel/printk.h>

void init_pipe(Pipe *pi) {
    /* (Final) TODO BEGIN */
    init_spinlock(&pi->lock);
    pi->readopen = 1; pi->writeopen = 1;
    pi->nread = 0; pi->nwrite = 0;
    init_sem(&pi->rlock, 0); init_sem(&pi->wlock, 0);
    /* (Final) TODO END */
}

void init_read_pipe(File *readp, Pipe *pipe) {
    /* (Final) TODO BEGIN */
    readp->type = FD_PIPE;
    readp->readable = 1;
    readp->writable = 0;
    readp->pipe = pipe;
    /* (Final) TODO END */
}

void init_write_pipe(File *writep, Pipe *pipe) {
    /* (Final) TODO BEGIN */
    writep->type = FD_PIPE;
    writep->readable = 0;
    writep->writable = 1;
    writep->pipe = pipe;
    /* (Final) TODO END */
}

int pipe_alloc(File **f0, File **f1) {
    /* (Final) TODO BEGIN */
    Pipe* p; p = NULL;
    *f0 = *f1 = 0;
    if ((*f0=file_alloc())==0||(*f1=file_alloc())==0) goto bad;
    if ((p=(Pipe*)kalloc(sizeof(Pipe)))==0) goto bad;
    init_pipe(p);
    init_read_pipe((*f0), p);
    init_write_pipe((*f1), p);
    return 0;
bad:
    if (p) kfree((char*)p);
    if (*f0) file_close(*f0);
    if (*f1) file_close(*f1);
    return -1;
    /* (Final) TODO END */
}

void pipe_close(Pipe *pi, int writable) {
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    if (writable) { pi->writeopen = 0; post_sem(&pi->rlock); } // 通知等待读取的进程
    else { pi->readopen=0; post_sem(&pi->wlock); } // 通知等待写入的进程
    release_spinlock(&pi->lock);
    // 如果读写端都已关闭，释放管道的内存
    if (pi->readopen==0 && pi->writeopen==0) kfree((void*) pi);
    /* (Final) TODO END */
}

int pipe_write(Pipe *pi, u64 addr, int n) {
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    for (int i = 0; i < n; i++) {
        while (pi->nwrite==pi->nread+PIPE_SIZE) {
            // 管道满了，等待
            if (pi->readopen==0 || thisproc()->killed){
                release_spinlock(&pi->lock);
                return -1; // 写失败
            }
            post_sem(&pi->rlock); // 通知可能有进程在读取
            release_spinlock(&pi->lock);
            unalertable_wait_sem(&pi->wlock); // 等待可写信号量
        }
        // 写入数据到管道缓冲区
        pi->data[pi->nwrite++ % PIPE_SIZE] = *((char *)addr + i);
    }
    post_sem(&pi->rlock);
    release_spinlock(&pi->lock);
    return n;
    /* (Final) TODO END */
}

int pipe_read(Pipe *pi, u64 addr, int n) {
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    while (pi->nread==pi->nwrite && pi->writeopen){
        // 管道为空且写端未关闭，等待
        if (thisproc()->killed) {
            release_spinlock(&pi->lock);
            return -1; // 读取失败
        }
        release_spinlock(&pi->lock);
        unalertable_wait_sem(&pi->rlock); // 等待可读信号量
    }
    int i;
    for (i = 0; i < n; i++){
        if (pi->nread == pi->nwrite) break; // 没有更多数据可读
        *((char *)addr + i) = pi->data[pi->nread++ % PIPE_SIZE];
    }
    post_sem(&pi->wlock); // 通知可能有进程等待写入
    release_spinlock(&pi->lock);
    return i;
    /* (Final) TODO END */
}