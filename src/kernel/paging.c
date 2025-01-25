#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>

void read_page_from_disk(void* ka, u32 bno) {
    for(u32 i=0;i<8;++i){
        Block* block = bcache.acquire(bno+i);
        memmove((ka+i*BLOCK_SIZE),block->data,BLOCK_SIZE);
        bcache.release(block);
    }
}

// Free 8 continuous disk blocks
void swap_in(struct pgdir* pd, struct section* st){
	ASSERT(st->flags & ST_SWAP);
	unalertable_acquire_sleeplock(&(st->sleeplock));
	for(u64 addr = st->begin; addr<st->end; addr+=PAGE_SIZE){
		PTEntriesPtr entry_ptr = get_pte(pd, addr, false);
		if(entry_ptr == NULL || (*entry_ptr) == 0) continue;
		u32 bno = (*entry_ptr)>>12;
		void* ka = kalloc_page();
		for (int i=0;i<8;i++) block_device.read(bno+i,(u8*)ka+i*512);
		vmmap(pd, addr, ka, PTE_USER_DATA);
	}
	release_sleeplock(&(st->sleeplock));
	st->flags &= ~ST_SWAP;
}

void init_sections(ListNode *section_head) {
    /* (Final) TODO BEGIN */
	struct section *st = kalloc(sizeof(struct section));
	st->begin = 0x0; st->end = 0x0; st->flags = 0;
	st->flags |= ST_HEAP;
	init_sleeplock(&(st->sleeplock));
	_insert_into_list(section_head, &(st->stnode));
    /* (Final) TODO END */
}

// 释放与页目录（pgdir）相关的所有段及其分配的内存资源，确保内存不会泄漏。
void free_sections(struct pgdir *pd) {
    /* (Final) TODO BEGIN */
    ListNode* node = pd->section_head.next;
	struct section* st = NULL;
	// 从页目录的段链表头开始遍历所有段。
	while(node) {
		if(node == &(pd->section_head)) break;
		st = container_of(node, struct section, stnode);
		// 如果段被标记为已交换（ST_SWAP），需要先将其从交换设备中调入内存。
		if(st->flags & ST_SWAP) swap_in(pd, st);
		// 遍历段的地址范围，逐页释放内存
		for(u64 i = PAGE_BASE(st->begin); i < st->end; i += PAGE_SIZE){
			PTEntriesPtr pte_p = get_pte(pd, i, false);
			if(pte_p && (*pte_p & PTE_VALID)){
				kfree_page((void*)P2K(PTE_ADDRESS(*pte_p)));
			}
		}
		if(st->fp != NULL) file_close(st->fp);
		node = node->next;
		_detach_from_list(&(st->stnode));
		kfree((void*)st);
	}
    /* (Final) TODO END */
}

u64 sbrk(i64 size) {
    /**
     * (Final) TODO BEGIN
     * Increase the heap size of current process by `size`.
     * If `size` is negative, decrease heap size. `size` must be a multiple of PAGE_SIZE.
     * Return the previous heap_end.
     */
    printk("in sbrk\n");
	struct Proc* p = thisproc();
	struct section* st = NULL;
	_for_in_list(node, &(p->pgdir.section_head)){
		if(node == &(p->pgdir.section_head)) break;
		st = container_of(node, struct section, stnode);
		if(st->flags & ST_HEAP) break;
	}
	ASSERT(st!=NULL);
	u64 ret = st->end;
	if(size >= 0) st->end += size*PAGE_SIZE;
	else {
		ASSERT((u64)(-size)*PAGE_SIZE <= (st->end-st->begin));
		st->end += size*PAGE_SIZE;
		if(st->flags & ST_SWAP) swap_in(&(p->pgdir), st);
		for(int i = 0; i < (-size); i++) {
			PTEntriesPtr entry_ptr = get_pte(&(p->pgdir), st->end+i*PAGE_SIZE, false);
			if(entry_ptr != NULL && ((*entry_ptr) & PTE_VALID)){
				void* ka = (void*)P2K(PTE_ADDRESS(*entry_ptr));
				kfree_page(ka);
				*(entry_ptr) = 0;
			}
		}
	}
	arch_tlbi_vmalle1is();
	return ret;
    /* (Final) TODO END */
}

int pgfault_handler(u64 iss) {
    // printk("pagefault handle start\n");
    Proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    u64 addr = arch_get_far(); // Attempting to access this address caused the page fault
    /**
     * (Final) TODO BEGIN
     * 1. Find the section struct which contains the faulting address `addr`.
     * 2. Check section flags to determine page fault type.
     * 3. Handle the page fault accordingly.
     * 4. Return to user code or kill the process.
     */
    struct section* st = NULL;
	_for_in_list(node, &(pd->section_head)) { // 找到包含 addr 的段。
		if(node == &(pd->section_head)) break;
		st = container_of(node, struct section, stnode);
		if(addr >= st->begin && addr < st->end) break;
	}
	ASSERT(st != NULL);
	printk("addr is: 0x%x, st_begin is: 0x%x, st_end is: 0x%x\n", (int)addr,(int)st->begin,(int)st->end);
	ASSERT(addr >= st->begin && addr < st->end);
	// 如果段被标记为 ST_SWAP（表示已被交换出到磁盘），调用 swap_in 将段从磁盘调入内存。
	if(st->flags & ST_SWAP) swap_in(pd, st);
	// 调用 get_pte 获取虚拟地址 addr 的页表项指针。如果页表项不存在且 true 参数允许创建，会创建一个新的页表项。
	PTEntriesPtr ptentry_ptr = get_pte(pd, addr, true);
	if(*ptentry_ptr == 0){ // lazy allocation
		void* new_page = kalloc_page();
		vmmap(pd, addr, new_page, PTE_USER_DATA);
	}
	else if(PTE_FLAGS(*ptentry_ptr) & PTE_RO){ // copy on write
		void* new_page = kalloc_page();
		void* old_page = (void*)P2K(PTE_ADDRESS(*ptentry_ptr));
		memcpy(new_page, old_page, PAGE_SIZE);
		kfree_page(old_page);
		vmmap(pd, addr, new_page, PTE_USER_DATA);
	}
	if(!(PTE_FLAGS(*ptentry_ptr) & PTE_VALID)) PANIC();
	arch_tlbi_vmalle1is();
	iss = iss;
	printk("pagefault handle done\n");
	return 0;
    /* (Final) TODO END */
}

// 从一个段链表复制所有段到另一个段链表中（确保段的信息在分页系统间可以复用）
void copy_sections(ListNode *from_head, ListNode *to_head) {
    /* (Final) TODO BEGIN */
	_for_in_list(node, from_head) {
		if(node == from_head) break;
		struct section* st = container_of(node, struct section, stnode);
		struct section* new_st = kalloc(sizeof(struct section));
		memmove(new_st, st, sizeof(struct section));
		if(st->fp != NULL) new_st->fp = file_dup(st->fp);
		_insert_into_list(to_head, &(new_st->stnode));
	}
    /* (Final) TODO END */
}
