#include "file.h"
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/sem.h>
#include <fs/inode.h>
#include <common/list.h>
#include <kernel/mem.h>
#include <fs/pipe.h>
#include <kernel/printk.h>
#include <common/string.h>

// the global file table.
static struct ftable ftable;

void init_ftable() {
    // TODO: initialize your ftable.
    init_spinlock(&ftable.ftable_lock);
}

void init_oftable(struct oftable *oftable) {
    // TODO: initialize your oftable for a new process.
    oftable->of_num = 0;
    memset(oftable, 0, sizeof(struct oftable));
}

/* Allocate a file structure. */
struct file* file_alloc() {
    /* (Final) TODO BEGIN */
    acquire_spinlock(&ftable.ftable_lock);
    for (int i=0;i<NFILE;i++){
        if (ftable.filelist[i].ref==0){
            ftable.filelist[i].ref=1;
            release_spinlock(&ftable.ftable_lock);
            return &(ftable.filelist[i]);
        }
    }
    release_spinlock(&ftable.ftable_lock);
    /* (Final) TODO END */
    return 0;
}

/* Increment ref count for file f. */
struct file* file_dup(struct file* f) {
    /* (Final) TODO BEGIN */
    acquire_spinlock(&ftable.ftable_lock);
    f->ref++;
    release_spinlock(&ftable.ftable_lock);
    /* (Final) TODO END */
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file* f) {
    /* (Final) TODO BEGIN */
    if(f->type == FD_NONE) return;
    acquire_spinlock(&ftable.ftable_lock);
    f->ref--;
    if(f->ref > 0){
        release_spinlock(&ftable.ftable_lock);
        return;
    }
    struct file now = *f;
    f->ref = 0;
    f->type = FD_NONE;
    release_spinlock(&ftable.ftable_lock);
    if (now.type==FD_PIPE) pipe_close(now.pipe, now.writable);
    else if (now.type==FD_INODE){
        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.put(&ctx,now.ip);
        bcache.end_op(&ctx);
    }
    /* (Final) TODO END */
}

/* Get metadata about file f. */
int file_stat(struct file* f, struct stat* st) {
    /* (Final) TODO BEGIN */
    if (f->type == FD_INODE){
        inodes.lock(f->ip);
        stati(f->ip, st);
        inodes.unlock(f->ip);
        return 0;
    }
    /* (Final) TODO END */
    return -1;
}

/* Read from file f. */
isize file_read(struct file* f, char* addr, isize n) {
    /* (Final) TODO BEGIN */
    if (f->readable==0) return -1;
    if (f->type==FD_PIPE) return pipe_read(f->pipe, (u64)addr, n);
    else if (f->type==FD_INODE) {
        inodes.lock(f->ip);
        usize nread = inodes.read(f->ip, (u8*)addr, f->off, (usize)n);
        //printk("f off is %lld\n", f->off);
        f->off += nread;
        inodes.unlock(f->ip);
        //printk("fileread ok %lld\n", nread);
        return nread;
    }
    else PANIC();
    /* (Final) TODO END */
    return 0;
}

/* Write to file f. */
isize file_write(struct file* f, char* addr, isize n) {
    /* (Final) TODO BEGIN */
    if (f->writable==0) return -1;
    if (f->type==FD_PIPE) return pipe_write(f->pipe, (u64)addr, n);
    else if (f->type==FD_INODE) {
        // 2 blocks for each write block
        // 1 block for inode
        // 1 block for map
        // 2 blocks for IndirectBlock
        usize max_valid_write_n = INODE_MAX_BYTES - f->off;
        n = MIN((usize)n, max_valid_write_n);
        usize nwrite = 0;
        while(nwrite < (usize)n){
            usize op_n = MIN((usize)(n-nwrite), (usize)MAX_OP_WRITE_N);
            OpContext ctx;
            bcache.begin_op(&ctx);
            inodes.lock(f->ip);
            usize w = inodes.write(&ctx, f->ip, (u8*)(addr + nwrite), f->off, op_n);
            f->off += w;
            inodes.unlock(f->ip);
            bcache.end_op(&ctx);
            nwrite += w;
        }
        return nwrite;
    }
    else PANIC();
    /* (Final) TODO END */
    return 0;
}