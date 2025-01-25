#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <common/spinlock.h>
#include <common/rc.h>
#include <kernel/proc.h>
#include <kernel/console.h>
#include <sys/stat.h>
#include <kernel/sched.h>
#include <assert.h>

/**
    @brief the private reference to the super block.
    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.
    @see init_inodes
 */
static const SuperBlock* sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache* cache;

/**
    @brief global lock for inode layer.
    Use it to protect anything you need.
    e.g. the list of allocated blocks, ref counts, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory inodes.
    We use a linked list to manage all allocated inodes.
    You can implement your own data structure if you want better performance.
    @see Inode
 */
static ListNode head;

// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no) {
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry* get_entry(Block* block, usize inode_no) {
    return ((InodeEntry*)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32* get_addrs(Block* block) {
    return ((IndirectBlock*)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock* _sblock, const BlockCache* _cache) {
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;
    if (ROOT_INODE_NO < sblock->num_inodes)
        inodes.root = inodes.get(ROOT_INODE_NO);
    else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode* inode) {
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0; // 0在一些函数中用于表示「没有 Inode」的意思。
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext* ctx, InodeType type) {
    ASSERT(type != INODE_INVALID);
    // TODO
    acquire_spinlock(&lock);
    for (usize inode_no = 1; inode_no < sblock->num_inodes; inode_no++){
        Block* current_block = cache->acquire(to_block_no(inode_no));
        InodeEntry* current_entry = get_entry(current_block, inode_no);
        if (current_entry->type == INODE_INVALID){
            memset(current_entry, 0, sizeof(InodeEntry));
            current_entry->type = type;
            cache->sync(ctx, current_block);
            cache->release(current_block);
            release_spinlock(&lock);
            return inode_no;
        }
        cache->release(current_block);
    }
    release_spinlock(&lock);
    PANIC();
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    unalertable_wait_sem(&inode->lock);
}

// see `inode.h`.
static void inode_unlock(Inode* inode) {
    ASSERT(inode->rc.count > 0);
    // TODO
    post_sem(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext* ctx, Inode* inode, bool do_write) {
    // TODO
    if(inode->valid && do_write) {  // write the content of `inode` to disk
        Block* current_block = cache->acquire(to_block_no(inode->inode_no));
        InodeEntry* current_entry = get_entry(current_block, inode->inode_no);
        memcpy(current_entry, &inode->entry, sizeof(InodeEntry));
        cache->sync(ctx, current_block);
        cache->release(current_block);
    }
    else if(!inode->valid && !do_write) {  // read the content of `inode` from disk
        Block* current_block = cache->acquire(to_block_no(inode->inode_no));
        InodeEntry* current_entry = get_entry(current_block, inode->inode_no);
        memcpy(&inode->entry, current_entry, sizeof(InodeEntry));
        inode->valid=1;
        cache->release(current_block);
    }
    else if(inode->valid && !do_write) { return; }  // do nothing
    else if(!inode->valid && do_write) { PANIC(); }
}

// see `inode.h`.
// 获得一个 inode（依然在inode中维护一个链表）
static Inode* inode_get(usize inode_no) {
    if(inode_no == 0) return NULL;
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    // TODO
    _for_in_list(p, &head){
        if (p == &head) continue;
        auto current_inode = container_of(p, Inode, node);
        if (current_inode->inode_no == inode_no) {
            increment_rc(&current_inode->rc);
            release_spinlock(&lock);
            return current_inode;
        }
    }
    Inode* new_inode = kalloc(sizeof(Inode));
    init_inode(new_inode);
    new_inode->inode_no = inode_no;
    increment_rc(&new_inode->rc);
    inode_lock(new_inode);
    inode_sync(NULL, new_inode, false);
    inode_unlock(new_inode);
    _insert_into_list(&head, &new_inode->node);
    return new_inode;
}

// see `inode.h`.
// 清空 inode 的内容（使文件变成长度为 0 的空文件）
static void inode_clear(OpContext* ctx, Inode* inode) {
    // TODO
    acquire_spinlock(&lock);
    if (inode->entry.indirect){  // 清空间接块的内容
        Block* indirect_addr_block = cache->acquire(inode->entry.indirect);
        u32* addrs = get_addrs(indirect_addr_block);
        cache->release(indirect_addr_block);
        for (usize i = 0; i < INODE_NUM_INDIRECT; i++){
            if (addrs[i]) cache->free(ctx, addrs[i]);
        }
        cache->free(ctx, inode->entry.indirect);
        inode->entry.indirect = 0;
    }

    for (usize i = 0; i < INODE_NUM_DIRECT; i++){  // 情况块的内容
        if (inode->entry.addrs[i]) {
            cache->free(ctx, inode->entry.addrs[i]);
            inode->entry.addrs[i] = 0;
        }
    }
    inode->entry.num_bytes=0;
    inode_sync(ctx, inode, true);
    release_spinlock(&lock);
}

// see `inode.h`.
// 让 inode 的引用计数加一
static Inode* inode_share(Inode* inode) {
    // TODO
    increment_rc(&inode->rc);
    return inode;  // may just return inode
}

// see `inode.h`.
// 释放 inode，判断是否清空inode内容
static void inode_put(OpContext* ctx, Inode* inode) {
    // TODO
    decrement_rc(&inode->rc);
    // if no one needs the inode any more
    if(inode->rc.count == 0 && inode->entry.num_links == 0) {
        inode->entry.type = INODE_INVALID;
        inode_clear(ctx, inode);
        _detach_from_list(&inode->node);
        kfree(inode);  // dont forget to kfree
    }
}

/**
    @brief get which block is the offset of the inode in.

    e.g. `inode_map(ctx, my_inode, 1234, &modified)` will return the block_no
    of the block that contains the 1234th byte of the file represented by `my_inode`.

    If a block has not been allocated for that byte, `inode_map` will
    allocate a new block and update `my_inode`, at which time, `modified`
    will be set to true.

    HOWEVER, if `ctx == NULL`, `inode_map` will NOT try to allocate any new block,
    and when it finds that the block has not been allocated, it will return 0.

    @param[out] modified true if some new block is allocated and `inode`
    has been changed.

    @return usize the block number of that block, or 0 if `ctx == NULL` and
    the required block has not been allocated.

    @note the caller must hold the lock of `inode`.
 */

// 获取inode在offset位置的块号，同时若ctx!=NULL且offset超过当前文件大小，
// 会allocate新的一块并标记modified为true
static usize inode_map(OpContext* ctx, Inode* inode, usize offset, bool* modified) {
    *modified = false;
    // Check if the offset falls within the direct blocks
    if (offset < INODE_NUM_DIRECT) {
        if (inode->entry.addrs[offset] == 0) { // Block not allocated
            if (!ctx) return 0; // If ctx == NULL, return 0 without allocating
            inode->entry.addrs[offset] = cache->alloc(ctx);
            inode_sync(ctx, inode, true);
            *modified = true;
        }
        return inode->entry.addrs[offset];
    }
    // offset >= INODE_NUM_DIRECT，落在间接块区域
    usize indirect_index = offset - INODE_NUM_DIRECT;
    if (inode->entry.indirect == 0) {  // no indirect place
        inode->entry.indirect = cache->alloc(ctx);
    }
    Block* indirect_addr_block = cache->acquire(inode->entry.indirect);
    u32* addrs = get_addrs(indirect_addr_block);
    cache->release(indirect_addr_block);
    if (addrs[indirect_index] == 0) {  // Block not allocated
        if (!ctx) return 0;
        addrs[indirect_index] = cache->alloc(ctx);
        inode_sync(ctx, inode, true);
        *modified = true;
    }
    return addrs[indirect_index];
}


// see `inode.h`.
// 将 inode 的 offset 处的 len 字节读入 buf
static usize inode_read(Inode* inode, u8* dest, usize offset, usize count) {
    InodeEntry* entry = &inode->entry;
    if (inode->entry.type == INODE_DEVICE) {
        ASSERT(inode->entry.major == 1);
        return console_read(inode, (char*)dest, count);
    }
    if (count + offset > entry->num_bytes) count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= entry->num_bytes);
    ASSERT(offset <= end);
    bool modified = false;
    for(usize have_read = 0,sz = 0;have_read < count;have_read+=sz) {
        Block* block = cache->acquire(inode_map(NULL,inode,offset / BLOCK_SIZE,&modified));
        if(count - have_read < BLOCK_SIZE - offset % BLOCK_SIZE) sz = count - have_read;
        else sz = BLOCK_SIZE - offset % BLOCK_SIZE;
        memmove(dest, block->data + offset % BLOCK_SIZE, sz);
        // for(usize i=0;i<sz;i++) printk("%c", block->data[i]);
        cache->release(block);
        dest += sz; offset += sz;
    }
    ASSERT(modified == false);
    return count;
}

// see `inode.h`.
// 将长度为 len 的 buf 写入 inode 的 offset 处
static usize inode_write(OpContext* ctx, Inode* inode, u8* src, usize offset, usize count) {
    InodeEntry* entry = &inode->entry;
    if (inode->entry.type == INODE_DEVICE) {
        // if(inode->entry.major != 1) return NULL;
        // printk("inode->entry.major = %d\n", inode->entry.major);
        ASSERT(inode->entry.major == 1);
        return console_write(inode, (char*)src, count);
    }
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);
    // TODO
    // Update file size if the write extends beyond current file size
    if (end > entry->num_bytes) {
        entry->num_bytes = end;
        inode_sync(ctx, inode, true);
    }
    usize bytes_written = 0;  // Total bytes written
    // Iterate through blocks and write data
    while (offset < end) {
        usize block_index = offset / BLOCK_SIZE;  // Block number
        usize block_offset = offset % BLOCK_SIZE;  // offset within the block
        usize bytes_to_write = MIN(BLOCK_SIZE - block_offset, end - offset);
        bool modified = false;
        usize block_no = inode_map(ctx, inode, block_index, &modified);
        ASSERT(block_no != 0);
        Block* block = cache->acquire(block_no);
        // Write the data to the block
        memcpy(block->data + block_offset, src, bytes_to_write);
        cache->sync(ctx, block);
        cache->release(block);
        src += bytes_to_write;
        offset += bytes_to_write;
        bytes_written += bytes_to_write;
    }
    return bytes_written;
}


// 目录操作
// see `inode.h`.
// 从给定的目录 inode 中查找名为 name 的文件，并返回其对应的 inode 编号，
// 同时通过 index 参数返回它在目录中的偏移位置。如果没有找到文件，则返回 0。
static usize inode_lookup(Inode* inode, const char* name, usize* index) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);
    // TODO
    DirEntry dir_entry;
    usize dsize = sizeof(DirEntry);
    for (usize i = 0; i < entry->num_bytes; i += dsize) {  // Iterate over directory entries
        inode_read(inode, (u8*)&dir_entry, i, dsize); // Read directory entry
        if (strncmp(dir_entry.name, name, FILE_NAME_MAX_LENGTH) == 0 &&
            dir_entry.inode_no != 0) { // Found the target entry
            if(index) *index = i;
            return dir_entry.inode_no;
        }
    }
    return 0;
}

// see `inode.h`.
// 向目录增加一项 {inode_no, name}
static usize inode_insert(OpContext* ctx, Inode* inode, const char* name, usize inode_no) {
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);
    if(inode_lookup(inode,name,NULL) != 0) return -1;
    u32 sz = sizeof(DirEntry); u32 i = 0;
    DirEntry dir_entry;
    for(i = 0; i < inode->entry.num_bytes; i += sz) {
        inode_read(inode,(u8*)(&dir_entry), i, sz);
        if(dir_entry.inode_no == 0) break;
    }
    strncpy(dir_entry.name, name, FILE_NAME_MAX_LENGTH);
    dir_entry.inode_no = inode_no;
    inode_write(ctx,inode,(u8*)(&dir_entry),i,sz);
    return i;
}

// see `inode.h`.
// 从目录中删除第 index 项
static void inode_remove(OpContext* ctx, Inode* inode, usize index) {
    // TODO
    InodeEntry* entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);
    usize dsize = sizeof(DirEntry);
    ASSERT(index < entry->num_bytes);
    DirEntry dir_entry;
    inode_read(inode, (u8*)&dir_entry, index, dsize);  // 读取指定目录项
    if (dir_entry.inode_no == 0) return;  // is not used before, do nothing
    usize last = entry->num_bytes - dsize;  // 最后一项dir_entry
    inode_read(inode, (u8*)&dir_entry, last, dsize);
    inode_write(ctx, inode, (u8*)&dir_entry, index, dsize);  // 最后一项写到当前空缺
    memset(&dir_entry, 0, dsize);  // 清理最后一个位置
    entry->num_bytes -= dsize;
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};

/**
    @brief read the next path element from `path` into `name`.
    @param[out] name next path element.
    @return const char* a pointer offseted in `path`, without leading `/`. If no
    name to remove, return NULL.
    @example
    skipelem("a/bb/c", name) = "bb/c", setting name = "a",
    skipelem("///a//bb", name) = "bb", setting name = "a",
    skipelem("a", name) = "", setting name = "a",
    skipelem("", name) = skipelem("////", name) = NULL, not setting name.
 */
static const char* skipelem(const char* path, char* name) {
    const char* s;
    int len;
    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
    @brief look up and return the inode for `path`.
    If `nameiparent`, return the inode for the parent and copy the final
    path element into `name`.
    @param path a relative or absolute path. If `path` is relative, it is
    relative to the current working directory of the process.
    @param[out] name the final path element if `nameiparent` is true.
    @return Inode* the inode for `path` (or its parent if `nameiparent` is true),
    or NULL if such inode does not exist.
    @example
    namex("/a/b", false, name) = inode of b,
    namex("/a/b", true, name) = inode of a, setting name = "b",
    namex("/", true, name) = NULL (because "/" has no parent!)
 */
static Inode* namex(const char* path, bool nameiparent, char* name, OpContext* ctx) {
    /* (Final) TODO BEGIN */
    Inode *ans, *next;
    if (*path=='/') ans = inode_get(ROOT_INODE_NO);
    else ans = inode_share(thisproc()->cwd);

    while ((path = skipelem(path, name)) != 0) {
        inode_lock(ans);
        if (ans->entry.type != INODE_DIRECTORY) {
            inode_unlock(ans);
            inode_put(ctx, ans);
            return NULL;
        }
        if (nameiparent && *path=='\0') {
            inode_unlock(ans);
            return ans;
        }
        next = inode_get(inode_lookup(ans, name, 0));
        if (next==NULL) {
            inode_unlock(ans);
            inode_put(ctx, ans);
            return NULL;
        }
        inode_unlock(ans);
        inode_put(ctx, ans);
        ans = next;
    }

    if (nameiparent) { inode_put(ctx, ans); return NULL; }
    return ans;
    /* (Final) TODO END */
}

Inode* namei(const char* path, OpContext* ctx) {
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode* nameiparent(const char* path, char* name, OpContext* ctx) {
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.
    @note the caller must hold the lock of `ip`.
 */
void stati(Inode* ip, struct stat* st) {
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
        case INODE_REGULAR:
            st->st_mode = S_IFREG;
            break;
        case INODE_DIRECTORY:
            st->st_mode = S_IFDIR;
            break;
        case INODE_DEVICE:
            st->st_mode = 0;
            break;
        default:
            PANIC();
    }
}