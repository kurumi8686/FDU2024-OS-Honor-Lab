#pragma once
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/rc.h>

#define PAGE_COUNT ((P2K(PHYSTOP) - PAGE_BASE((u64) & end)) / PAGE_SIZE - 1)
#define PAGE_INDEX(page_base) ((KSPACE((u64)page_base) - P2K(EXTMEM)) / PAGE_SIZE)

struct page {
    RefCount ref;
    SpinLock ref_lock;
};

void kinit();
u64 left_page_cnt();

WARN_RESULT void *kalloc_page();
void kfree_page(void *);

WARN_RESULT void *kalloc(unsigned long long);
void kfree(void *);

WARN_RESULT void *get_zero_page();
usize get_page_ref(u64);

