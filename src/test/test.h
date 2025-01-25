#pragma once
#include <common/defines.h>

#define RAND_MAX 32768

void kalloc_test();
void rbtree_test();
void proc_test();
void vm_test();
void user_proc_test();
void io_test();
unsigned rand();
void srand(unsigned seed);
void pgfault_first_test();
void pgfault_second_test();

// syscall
u64 syscall_myreport(u64 id);