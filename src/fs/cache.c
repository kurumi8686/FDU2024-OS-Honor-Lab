#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

static const SuperBlock* sblock;
static const BlockDevice* device;

static SpinLock lock;
static SpinLock log_lock;
static SpinLock bitmap_lock;
static ListNode head;     // the list of all allocated in-memory block.
static LogHeader header;  // in-memory copy of log header block.
static usize block_num;   // 全局变量记录当前 bcache 中块的个数

struct {
    // bool committing;  // 日志是否正在提交？无需，end_op写回的时候大锁锁死算了
    // 当然可以每次log中blocks写回的时候都放锁，然后这committing其实就相当于锁，不允许sync
    int outstanding;  // 当前正在等待提交的操作数，也就是正在进行的操作的数量。
    Semaphore log_sem;
} log;

// read the content from disk.
static INLINE void device_read(Block* block) {
    device->read(block->block_no, block->data);
}
// write the content back to disk.
static INLINE void device_write(Block* block) {
    device->write(block->block_no, block->data);
}
// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8*)&header);
}
// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8*)&header);
}
// initialize a block struct.
static void init_block(Block* block) {
    block->block_no = 0;
    init_list_node(&block->node);
    block->acquired = false;
    block->pinned = false;
    init_sleeplock(&block->lock);
    block->valid = false;
    memset(block->data, 0, sizeof(block->data));
}

// 返回当前cache中块的个数（可以使用一个全局变量，但需要注意加减的原子性能否保证）。
static usize get_num_cached_blocks() {
    return block_num;
}

// 处理cache中的block数必须小于软上界
void manage_block_num() {
    ListNode* p = head.prev;
    while (block_num >= EVICTION_THRESHOLD) {
        ListNode* q = p->prev;
        if (p == &head) break;
        Block* current_block = container_of(p, Block, node);
        // 没有被任意进程（线程）acquire，未pin，则可以uncache掉
        // pin的块不能删（在做事，可能为dirty等等）
        if (!current_block->acquired && !current_block->pinned){
            _detach_from_list(p);
            block_num--;
            kfree(current_block);
        }
        p = q;
    }
}

/* 我们拥有一个链表记录了所有在cache的块，其软上限是EVICTION_THRESHOLD
 * 首先判断acquire的块在不在cache中，如果在应该如何操作？如果不在如何操作？
 * 如果当我们再插入一个块进入cache就要超过上界时，我们需要uncache一块？（如何选择这
 * 删除的一块？这一块还需要满足哪些条件？）使用device_read从设备读块的内容acquire
 * 返回时，需要保证调用者已获得了该block的锁。*/
static Block* cache_acquire(usize block_no) {
    acquire_spinlock(&lock);
    Block* acquired_block = 0;
    Block* current_block = 0;
    _for_in_list(p, &head) {
        if (p == &head) continue;
        current_block = container_of(p, Block, node);
        if (current_block->block_no == block_no){
            acquired_block = current_block;
            break;
        }
    }
    // acquire的块在cache中，更新为最近访问，直接取到返回
    if (acquired_block) {
        acquired_block->acquired = true;
        release_spinlock(&lock);
        if (!wait_sem(&acquired_block->lock)) PANIC();
        acquire_spinlock(&lock);
        _detach_from_list(&acquired_block->node);  // 将其拿出再插到队头
        _insert_into_list(&head, &acquired_block->node);  // 表示最近被访问过
        release_spinlock(&lock);
        return acquired_block;
    }
    // 如果在cache中没有找到acquire的块：要allocate空间搞个新块，block_no赋为传入值
    // 并且把这一块放到cache中，还要处理cache中block数量小于软上界。
    manage_block_num();
    acquired_block = kalloc(sizeof(Block));
    init_block(acquired_block);
    if (!wait_sem(&acquired_block->lock)) PANIC();
    acquired_block->block_no = block_no;
    acquired_block->acquired = true;
    acquired_block->valid = true;
    block_num++;
    release_spinlock(&lock);
    device_read(acquired_block);
    acquire_spinlock(&lock);
    _insert_into_list(&head, &acquired_block->node);
    release_spinlock(&lock);
    return acquired_block;
}

/* 如何表示这一块不再被acquire，处于可用状态？返回后，需要保证调用者释放了该block锁。
 * 提示：若希望一个信号量post时可以通知所有等待它的信号量，可以使用post_all_sem函数。
 */
static void cache_release(Block* block) {
    acquire_spinlock(&lock);
    block->acquired = false;
    post_sem(&block->lock);
    release_spinlock(&lock);
}

/* 当拥有特别多的操作时，我们需要等待（原因在于一个事务提交的log数量有上限），如何判断
 * 当前是否依然支持更多操作，判断条件是什么？（考虑log的数量限制）如果在进行checkpoint
 * 的过程中，我们是否可以继续begin_op? 上述情况我们都需要等待，如何实现？初始化ctx->rm
 * 为OP_MAX_NUM_BLOCKS，表示其剩余的可用操作数。
 */
static void cache_begin_op(OpContext* ctx) {
    acquire_spinlock(&log_lock);
    ctx->rm = OP_MAX_NUM_BLOCKS;
    while (LOG_MAX_SIZE <= header.num_blocks + (log.outstanding+1) * OP_MAX_NUM_BLOCKS) {
        release_spinlock(&log_lock);
        if (!wait_sem(&log.log_sem)) PANIC();
        acquire_spinlock(&log_lock);
    }
    log.outstanding++;  // begin_op了，日志记录新增一个待处理的操作数
    release_spinlock(&log_lock);
}

/* 若ctx为NULL，直接使用device_write写入块
 * （在调用函数前，调用者必须保证已经通过acquire持有该块的锁）
 * 标记该块为脏块，在log_header中记录该块（如果该块已经被标记为脏块呢？）
 */
static void cache_sync(OpContext* ctx, Block* block) {
    if(ctx == 0) { device_write(block); return; }
    acquire_spinlock(&log_lock);
    block->pinned = true;  // 标记为脏块
    bool exist = false;
    for (usize i = 0; i < header.num_blocks; i++){
        if (block->block_no == header.block_no[i]) { exist = true; break; }
    }
    if(!exist) {
        header.block_no[header.num_blocks++] = block->block_no;  // 在log_header中记录
        if(ctx->rm <= 0 || header.num_blocks >= LOG_MAX_SIZE) { PANIC(); }  // 如果rm为0，任何sync都要panic
        ctx->rm--;
    }
    release_spinlock(&log_lock);
}

static void copy_block(usize from_block_no, usize to_block_no){
    Block* from = cache_acquire(from_block_no);
    Block* to = cache_acquire(to_block_no);
    for (int j = 0; j < BLOCK_SIZE; j++) to->data[j] = from->data[j];
    device_write(to);
    cache_release(from); cache_release(to);
}

/* 什么时候进行checkpoint写入操作？（checkpoint详细过程见3.4）
 * 注意：返回该函数时必须保证该事务内的所有写入全部完成。
 */
static void cache_end_op(OpContext* ctx) {
    ctx = ctx;
    acquire_spinlock(&log_lock);
    log.outstanding--;
    // 如果还有未完成的操作，不能end_op，直接返回
    if (log.outstanding > 0) {
        post_sem(&log.log_sem);
        release_spinlock(&log_lock);
        return;
    }
    for (usize i = 0; i < header.num_blocks; i++) {
        copy_block(header.block_no[i], sblock->log_start + i + 1);
    }
    write_header();
    for (usize i = 0; i < header.num_blocks; i++) {
        Block* current_block = cache_acquire(header.block_no[i]);
        release_spinlock(&log_lock);
        device_write(current_block);  // 写入块数据到设备
        acquire_spinlock(&log_lock);
        current_block->pinned = false;  // 持久化存储后标记为非脏块
        cache_release(current_block);
    }
    header.num_blocks = 0;
    write_header();
    post_all_sem(&log.log_sem);
    release_spinlock(&log_lock);
}

// initialize block cache.
void init_bcache(const SuperBlock* _sblock, const BlockDevice* _device) {
    sblock = _sblock;
    device = _device;
    block_num = 0;
    init_spinlock(&lock); init_spinlock(&bitmap_lock);
    init_sem(&log.log_sem,0); init_spinlock(&log_lock);
    log.outstanding = 0; init_list_node(&head);
    read_header();
    for (usize i = 0; i < header.num_blocks; i++){
        copy_block(sblock->log_start + i + 1, header.block_no[i]);
    }
    header.num_blocks=0;
    memset(header.block_no, 0, LOG_MAX_SIZE);
    write_header();
}

// hint: you can use `cache_acquire`/`cache_sync` to read/write blocks.
// 根据位图分配一个可用块，返回其块号，同时请注意：返回的块需要保证数据已经被memset，为干净块。
static usize cache_alloc(OpContext* ctx) {
    acquire_spinlock(&bitmap_lock);
    Block* bitmap_block = cache_acquire(sblock->bitmap_start);
    for(usize i = 0; i < sblock->num_blocks; i++){
        if (!bitmap_get((BitmapCell *)bitmap_block->data, i)) {  // 当前块未分配
            bitmap_set((BitmapCell *)bitmap_block->data, i);  // 使用bitmap_set标记块为已分配
            cache_sync(ctx, bitmap_block);  // 同步位图
            cache_release(bitmap_block);  // 释放位图块
            Block* allocated_block = cache_acquire(i);  // 获取分配的块
            memset(allocated_block->data, 0, BLOCK_SIZE);  // 清空块的数据
            cache_sync(ctx, allocated_block);  // 同步分配块
            cache_release(allocated_block);  // 释放分配的块
            release_spinlock(&bitmap_lock);  // 释放位图锁
            return i;  // 返回分配的块号
        }
    }
    cache_release(bitmap_block);
    release_spinlock(&bitmap_lock);
    PANIC();
}


// 根据位图恢复一个可用块，此时无需完全清除块的内容。
static void cache_free(OpContext* ctx, usize block_no) {
    acquire_spinlock(&bitmap_lock);
    Block* bitmap_block = cache_acquire(sblock->bitmap_start);
    bitmap_clear((BitmapCell *)bitmap_block->data, block_no);  // 调用clear标记该块已释放
    cache_sync(ctx, bitmap_block);
    cache_release(bitmap_block);
    release_spinlock(&bitmap_lock);
}


BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};