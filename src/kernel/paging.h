#pragma once
#include <aarch64/mmu.h>
#include <kernel/proc.h>

#define ST_FILE 1
#define ST_SWAP (1 << 1)
#define ST_RO (1 << 2)
#define ST_HEAP (1 << 3)
#define ST_TEXT (ST_FILE | ST_RO)
#define ST_DATA ST_FILE
#define ST_BSS ST_FILE

#define ST_USER_STACK (1 << 4)
#define ST_MMAP_SHARED (1 << 5)
#define ST_MMAP_PRIVATE (1 << 6)

struct section {
    u64 flags;
    u64 begin;
    u64 end;
    ListNode stnode;
    SleepLock sleeplock;
    /* The following fields are for the file-backed sections. */
    struct file *fp;
    u64 offset; // Offset in file
    u64 length; // Length of mapped content in file
    u64 prot;   // for mmap
};

int pgfault_handler(u64 iss);
void init_sections(ListNode *section_head);
void free_sections(struct pgdir *pd);
void copy_sections(ListNode *from_head, ListNode *to_head);
u64 sbrk(i64 size);
