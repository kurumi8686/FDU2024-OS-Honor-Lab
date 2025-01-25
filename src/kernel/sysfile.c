//
// File-system system calls implementation.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <stddef.h>
#include "syscall.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/pt.h>

struct iovec {
    void *iov_base; /* Starting address. */
    usize iov_len; /* Number of bytes to transfer. */
};

/**
 * Get the file object by fd. Return null if the fd is invalid.
 */
static struct file *fd2file(int fd) {
    /* (Final) TODO BEGIN */
    if(fd < 0 || fd >= NOPENFILE) return NULL;
    File* ff = thisproc()->oftable.openfilelist[fd];
    if(ff == NULL) printk("sysfile.c--fd2file: file is null\n");
    return ff;
    /* (Final) TODO END */
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f) {
    /* (Final) TODO BEGIN */
    struct oftable* oft = &(thisproc()->oftable);
    for(int i = 0; i < NOPENFILE; i++) {
        if(oft->openfilelist[i] == NULL){
            oft->openfilelist[i] = f;
            return i;
        }
    }
    return -1;
    /* (Final) TODO END */
}

define_syscall(ioctl, int fd, u64 request) {
    // 0x5413 is TIOCGWINSZ (I/O Control to Get the WINdow SIZe, a magic request
    // to get the stdin terminal size) in our implementation. Just ignore it.
    ASSERT(request == 0x5413);
    // printk("ioctl called: fd=%d, request=0x%llx\n\n", fd, request);
    (void)fd;
    return 0;
}

// mmap
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define USER_SPACE_START 0x40000000
#define USER_SPACE_END   0x80000000
#define KMASK 0xffff000000000000

#define NVMA 16
#define VMA_START (USER_SPACE_END / 2)

struct vma {
    u64 start, end, length; // length == 0 means used
    u64 off;
    int prot;
    int flags;
    File *file;
    SpinLock lock;
};
struct vma vma_list[NVMA];
int vma_alloc_id() {
  for(int i = 0; i < NVMA; i++) {
      if(vma_list[i].length == 0) return i;
  }
  PANIC();
}
static int st;
static int ed;
void vma_init() {
    st = 0;
    ed = 0;
    memset(vma_list, 0, sizeof(vma_list));
}

void get_free_vm(struct pgdir* pd, u64 length, u64* begin, u64* end) {
    *begin = USER_SPACE_START;
    *end = USER_SPACE_END;
    printk("Initial free_begin: 0x%llx, free_end: 0x%llx\n", *begin, *end);
    _for_in_list(p, &pd->section_head) {
        auto sec = container_of(p, struct section, stnode);
        if (sec->begin >= KMASK || sec->end >= KMASK) continue;
        if (sec->begin == 0 && sec->end == 0) continue;
        printk("Checking section: [0x%llx - 0x%llx]\n", sec->begin, sec->end);
        if (sec->end > *begin) *begin = PAGE_ALIGN_UP(sec->end);
        if (sec->begin < *end) *end = PAGE_ALIGN_DOWN(sec->begin);
    }
    *begin = *end - length;
    printk("Final free_begin: 0x%llx, free_end: 0x%llx\n", *begin, *end);
}

define_syscall(mmap, void *addr, int length, int prot, int flags, int fd, int offset) {
    /* (Final) TODO BEGIN */
    if(prot == PROT_NONE || prot & PROT_EXEC || fd < 0 || fd >= NOPENFILE || length <= 0) return -1;
    length = PAGE_ALIGN_UP(length);
    auto st = (struct section*)kalloc(sizeof(struct section));
    memset(st, 0, sizeof(struct section));
    st->flags = (flags & MAP_SHARED) ? ST_MMAP_SHARED : ST_MMAP_PRIVATE;
    st->offset = offset; st->prot = prot;
    auto cp = thisproc();
    auto f = fd2file(fd);
    if(!f) { kfree(st); return -1; }
    if((prot & PROT_WRITE) && !f->writable && flags != MAP_PRIVATE) { kfree(st); return -1; }
    acquire_spinlock(&cp->pgdir.lock);
    ASSERT(addr == 0); // 只有自动分配空间的情况
    u64 free_begin = 0, free_end = 0;
    get_free_vm(&cp->pgdir, length, &free_begin, &free_end);
    if(free_end == free_begin) {
        printk("can not find a space\n");
        kfree(st); release_spinlock(&cp->pgdir.lock); return -1;
    }
    if (free_begin % PAGE_SIZE != 0) {
        printk("addr not aligned\n");
        kfree(st); release_spinlock(&cp->pgdir.lock); return -1;
    }
    st->begin = free_begin; st->end = free_end;
    // f->readable = 1; f->writable = 1;
    f->off = offset; // 重置file的offset！！否则同一个fd，off他一直在累增
    st->fp = file_dup(f);
    _insert_into_list(&cp->pgdir.section_head, &st->stnode);
    // 读取文件内容到这个地址
    file_read(f, (void *)st->begin, length);
    release_spinlock(&cp->pgdir.lock);
    printk("Allocated region: [0x%llx - 0x%llx]\n", st->begin, st->end);
    return st->begin;
    /* (Final) TODO END */
}

void free_section_pages(struct pgdir* pd, struct section* sec) {
    for(auto i = PAGE_BASE(sec->begin); i < sec->end; i+= PAGE_SIZE) {
        auto pte = get_pte(pd, i, false);
        if(pte && (*pte & PTE_VALID)) {
            if((sec->flags == ST_MMAP_PRIVATE || sec->flags == ST_MMAP_SHARED)
                && !(*pte & PTE_RO) && get_page_ref(P2K(PTE_ADDRESS(*pte))) == 1){
                if(sec->fp->type == FD_INODE) {
                    u64 this_begin = MAX(i, sec->begin);
                    u64 this_end = MIN(i + PAGE_SIZE, sec->end);
                    sec->fp->off = sec->offset + this_begin - sec->begin;
                    file_write(sec->fp, (char*)P2K(PTE_ADDRESS(*pte)), this_end - this_begin);
                }
                else PANIC();
            }
            kfree_page((void*)P2K(PTE_ADDRESS(*pte)));
            *pte = 0; // 确保清理页表项，防止重复映射
        }
    }
}

define_syscall(munmap, void *addr, size_t length) {
    /* (Final) TODO BEGIN */
    auto cp = thisproc();
    acquire_spinlock(&cp->pgdir.lock);
    _for_in_list(p, &cp->pgdir.section_head){
        if(p != &cp->pgdir.section_head) {
            auto st = container_of(p, struct section, stnode);
            if((u64)addr == st->begin) {
                ASSERT(st->flags == ST_MMAP_PRIVATE || st->flags == ST_MMAP_SHARED);
                if(length >= st->end - st->begin) {
                    for (size_t i = 0; i < length; i++) ((char*)addr)[i] = 0;
                    free_section_pages(&cp->pgdir, st);
                    _detach_from_list(p); file_close(st->fp); kfree(st);
                }
                else {
                    auto end = st->begin + length;
                    for(auto i = PAGE_BASE(st->begin); i < end; i += PAGE_SIZE){
                        auto pte = get_pte(&cp->pgdir, i, false);
                        if(st->fp->type == FD_INODE){
                            u64 this_begin = MAX(i, st->begin);
                            u64 this_end = MIN(i + PAGE_SIZE, end);
                            st->fp->off = st->offset + this_begin - st->begin;
                            file_write(st->fp, (char*)P2K(PTE_ADDRESS(*pte)), MIN((u64)PAGE_SIZE, this_end - this_begin));
                        }
                        else PANIC();
                        kfree_page((void*)P2K(PTE_ADDRESS(*pte)));
                        *pte = 0;
                    }
                    st->begin = end;
                }
                printk("Freed region: [0x%llx - 0x%llx]\n", st->begin, st->end);
                break;
            }
        }
    }
    release_spinlock(&cp->pgdir.lock);
    return 0;
    /* (Final) TODO END */
}

define_syscall(dup, int fd) {
    //printk("sys_dup called: fd=%d\n", fd);
    struct file *f = fd2file(fd);
    if (!f) {
        //printk("sys_dup: invalid file descriptor %d\n\n", fd);
        return -1;
    }
    int new_fd = fdalloc(f);
    if (new_fd < 0) {
        //printk("sys_dup: fail to allocate new fd for fd=%d\n\n", fd);
        return -1;
    }
    file_dup(f); // 增加文件引用计数
    //printk("sys_dup: duplicated fd=%d to new_fd=%d\n\n", fd, new_fd);
    return new_fd;
}

// 63
define_syscall(read, int fd, char *buffer, int size) {
    // printk("[syscall_read] fd: %d, buffer: %p, size: %d\n", fd, buffer, size);
    struct file *f = fd2file(fd);
    if (!f) {
        // printk("[syscall_read] Error: Invalid file descriptor (fd: %d)\n\n", fd);
        return -1;
    }
    if (size <= 0) {
        // printk("[syscall_read] Error: Invalid size (size: %d)\n\n", size);
        return -1;
    }
    if (!user_writeable(buffer, size)) {
        // printk("[syscall_read] Error: Buffer not writable or invalid (buffer: %p, size: %d)\n\n", buffer, size);
        return -1;
    }
    int bytes_read = file_read(f, buffer, size);
    // printk("[syscall_read] Successfully read %d bytes from fd: %d\n\n", bytes_read, fd);
    return bytes_read;
}

define_syscall(write, int fd, char *buffer, int size) {
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size)) return -1;
    return file_write(f, buffer, size);
}

// id == 66
define_syscall(writev, int fd, struct iovec *iov, int iovcnt) {
    // printk("writev called: fd=%d, iovcnt=%d\n", fd, iovcnt);
    struct file *f = fd2file(fd);
    if (!f) {
        //printk("writev: invalid fd or no file structure found for fd=%d\n\n", fd);
        return -1;
    }
    struct iovec *p;
    if (iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt)) {
        //printk("writev: invalid iov or iovcnt=%d\n", iovcnt);
        return -1;
    }
    // printk("writev: file path is %s\n", f->path);
    usize tot = 0;
    for (p = iov; p < iov + iovcnt; p++) {
        if (!user_readable(p->iov_base, p->iov_len)) {
            //printk("writev: invalid user buffer at base=%p, len=%llu\n", p->iov_base, p->iov_len);
            return -1;
        }
        // printk("writev: writing %llu bytes from %p to fd=%d\n", p->iov_len, p->iov_base, fd);
        tot += file_write(f, p->iov_base, p->iov_len);
    }
    //printk("writev: successfully written %llu bytes to fd=%d\n\n", tot, fd);
    return tot;
}


define_syscall(close, int fd) {
    /* (Final) TODO BEGIN */
    if(fd < 0 || fd >= NOPENFILE) return -1;
    File* f = thisproc()->oftable.openfilelist[fd];
    thisproc()->oftable.openfilelist[fd] = NULL;
    file_close(f);
    return 0;
    /* (Final) TODO END */
}

define_syscall(fstat, int fd, struct stat *st) {
    struct file *f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st))) return -1;
    return file_stat(f, st);
}

define_syscall(newfstatat, int dirfd, const char *path, struct stat *st, int flags) {
    if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st))) return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_fstatat: dirfd unimplemented\n"); return -1;
    }
    if (flags != 0) {
        printk("sys_fstatat: flags unimplemented\n"); return -1;
    }
    Inode *ip;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = namei(path, &ctx)) == 0) {
        bcache.end_op(&ctx); return -1;
    }
    inodes.lock(ip);
    stati(ip, st);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

static int isdirempty(Inode *dp) {
    usize off;
    DirEntry de;
    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de)) PANIC();
        if (de.inode_no != 0) return 0;
    }
    return 1;
}

define_syscall(unlinkat, int fd, const char *path, int flag) {
    ASSERT(fd == AT_FDCWD && flag == 0);
    Inode *ip, *dp;
    DirEntry de;
    char name[FILE_NAME_MAX_LENGTH];
    usize off;
    if (!user_strlen(path, 256)) return -1;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((dp = nameiparent(path, name, &ctx)) == 0) {
        bcache.end_op(&ctx); return -1;
    }
    inodes.lock(dp);

    // Cannot unlink "." or "..".
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
        strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
        goto bad;

    usize inumber = inodes.lookup(dp, name, &off);
    if (inumber == 0)
        goto bad;
    ip = inodes.get(inumber);
    inodes.lock(ip);

    if (ip->entry.num_links < 1)
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (inodes.write(&ctx, dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY) {
        dp->entry.num_links--;
        inodes.sync(&ctx, dp, true);
    }
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    ip->entry.num_links--;
    inodes.sync(&ctx, ip, true);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;

bad:
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    bcache.end_op(&ctx);
    return -1;
}


/**
    @brief create an inode at `path` with `type`.
    If the inode exists, just return it.
    If `type` is directory, you should also create "." and ".." entries and link them with the new inode.
    @note BE careful of handling error! You should clean up ALL the resources
    you allocated and free ALL acquired locks when error occurs. e.g. if you
    allocate a new inode "/my/dir", but failed to create ".", you should free the
    inode "/my/dir" before return.
    @see `nameiparent` will find the parent directory of `path`.
    @return Inode* the created inode, or NULL if failed.
 */
Inode *create(const char *path, short type, short major, short minor, OpContext *ctx) {
    /* (Final) TODO BEGIN */
    char filename[FILE_NAME_MAX_LENGTH] = {0};
    Inode* parent_dir = nameiparent(path, filename, ctx);
    Inode* current_inode = NULL;
    if(parent_dir == NULL) return NULL;
    inodes.lock(parent_dir);
    usize idx = 0;
    u32 inode_no = inodes.lookup(parent_dir, filename, &idx);
    // if inode of name do exist
    if(inode_no != 0){
        current_inode = inodes.get(inode_no);
        inodes.lock(current_inode);
    }
    else{ // not exist
        current_inode = inodes.get(inodes.alloc(ctx, type));
        if(current_inode == NULL) return NULL;
        inodes.lock(current_inode);
        current_inode->entry.major = major;
        current_inode->entry.minor = minor;
        current_inode->entry.num_links = 1;
        inodes.sync(ctx, current_inode, true);
        // handle "." and ".." , insert "." and ".." as entry in cur_inode
        if(type == INODE_DIRECTORY){
            usize ret1 = inodes.insert(ctx, current_inode, ".", current_inode->inode_no);
            usize ret2 = inodes.insert(ctx, current_inode, "..", parent_dir->inode_no);
            ASSERT(ret1 != (usize)-1 && ret2 != (usize)-1);
            parent_dir->entry.num_links += 1;
            inodes.sync(ctx, parent_dir, true);
        }
        // put the new alloc inode in parent inode's dir
        usize ret = inodes.insert(ctx, parent_dir, filename, current_inode->inode_no);
        ASSERT(ret != (usize)-1);
    }
    inodes.unlock(parent_dir);
    inodes.put(ctx, parent_dir);
    return current_inode;
    /* (Final) TODO END */
}

define_syscall(openat, int dirfd, const char *path, int omode) {
    //printk("sys_openat called: dirfd=%d, path=%s, omode=0x%x\n", dirfd, path, omode);
    int fd; struct file *f; Inode *ip;
    if (!user_strlen(path, 256)) {
        //printk("sys_openat: invalid path\n");
        return -1;
    }
    if (dirfd != AT_FDCWD) {
        //printk("sys_openat: dirfd unimplemented\n");
        return -1;
    }
    OpContext ctx; bcache.begin_op(&ctx);
    if (omode & O_CREAT) {
        // FIXME: Support acl mode.
        //printk("sys_openat: creating file at path=%s\n", path);
        ip = create(path, INODE_REGULAR, 0, 0, &ctx);
        if (ip == 0) {
            printk("sys_openat: failed to create file\n");
            bcache.end_op(&ctx); return -1;
        }
    } else { // 打开已有文件
        if ((ip = namei(path, &ctx)) == 0) {
            //printk("sys_openat: file not found at path=%s\n\n", path);
            bcache.end_op(&ctx); return -1;
        }
        inodes.lock(ip);
    }
    if ((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
        //printk("sys_openat: failed to allocate file or fd\n");
        if (f) { file_close(f); }
        inodes.unlock(ip); inodes.put(&ctx, ip); bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip); bcache.end_op(&ctx);
    f->type = FD_INODE; f->ip = ip; f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    //printk("sys_openat: opened file path=%s, fd=%d  done\n\n", path, fd);
    return fd;
}

define_syscall(mkdirat, int dirfd, const char *path, int mode) {
    Inode *ip;
    if (!user_strlen(path, 256)) return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mkdirat: dirfd unimplemented\n"); return -1;
    }
    if (mode != 0) {
        printk("sys_mkdirat: mode unimplemented\n"); return -1;
    }
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
        bcache.end_op(&ctx); return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(mknodat, int dirfd, const char *path, mode_t mode, dev_t dev) {
    //printk("sys_mknodat called: dirfd=%d, path=%s, mode=0x%x\n", dirfd, path, mode);
    Inode *ip; mode = mode;
    if (!user_strlen(path, 256)) {
        printk("sys_mknodat: invalid path length\n\n"); return -1;
    }
    if (dirfd != AT_FDCWD) {  // 仅支持 AT_FDCWD
        printk("sys_mknodat: dirfd unimplemented\n\n"); return -1;
    }
    unsigned int ma = major(dev);
    if (strncmp(path, "console", 7) == 0) ma = 1;
    unsigned int mi = minor(dev);
    printk("mknodat: path '%s', major:minor %u:%u\n", path, ma, mi);
    OpContext ctx;
    bcache.begin_op(&ctx);
    // 创建设备文件
    printk("sys_mknodat: attempting to create device file at path=%s\n", path);
    if ((ip = create(path, INODE_DEVICE, (short)ma, (short)mi, &ctx)) == 0) {
        printk("sys_mknodat: failed to create device file\n\n");
        bcache.end_op(&ctx); return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    //printk("sys_mknodat: successfully created device file at path=%s\n\n", path);
    return 0;
}

define_syscall(chdir, const char *path) {
    /**
     * (Final) TODO BEGIN
     * Change the cwd (current working dictionary) of current process to 'path'.
     * You may need to do some validations.
     */
    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode* ip = namei(path, &ctx); // 解析路径并获取对应的 inode
    if (ip == NULL) { // 解析出的inode不存在，返回 -1 表示失败。
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.lock(ip); // 检查inode是否为目录类型。如果不是，解锁并释放资源，返回 -1。
    if(ip->entry.type != INODE_DIRECTORY){
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    // change cwd
    Proc* cur_proc = thisproc();
    inodes.put(&ctx, cur_proc->cwd);
    bcache.end_op(&ctx); // 将 ip 更新为新的 cwd，并结束磁盘操作事务
    cur_proc->cwd = ip;
    return 0;
    /* (Final) TODO END */
}

define_syscall(pipe2, int pipefd[2], int flags) {
    /* (Final) TODO BEGIN */
    File* f0 = NULL;
    File* f1 = NULL;
    // alloc pipe
    if(pipe_alloc(&f0, &f1) == -1) return -1;
    int fd0 = fdalloc(f0);
    int fd1 = fdalloc(f1);
    // validation with close unneed file
    if(fd0 == -1 || fd1 == -1){
        if(fd0 != -1){
            sys_close(fd0);
        }
        if(fd1 != -1){
            sys_close(fd1);
        }
        return -1;
    }
    pipefd[0] = fd0;
    pipefd[1] = fd1;
    return flags & 0;
    /* (Final) TODO END */
}