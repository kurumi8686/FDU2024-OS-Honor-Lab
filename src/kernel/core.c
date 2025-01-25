#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>
#include <driver/virtio.h>
#include <common/string.h>
#include <kernel/proc.h>
#include <kernel/paging.h>
#include <kernel/mem.h>

extern char icode[], eicode[];
volatile bool panic_flag;
void trap_return();

NO_RETURN void idle_entry() {
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap {
            arch_wfi();
        }
    }
    set_cpu_off();
    arch_stop_cpu();
}

void kernel_entry() {
    init_filesystem();
    printk("------ kernel entry start ------\n");
    // pgfault_first_test();
    // pgfault_second_test();
    // lab4 todo:
    Buf buf;
    buf.block_no = 0;  // MBR is on the first block
    buf.flags = 0;  // 0 for read in io_test
    memset(buf.data, 0, BSIZE);  // Clear buffer
    if (virtio_blk_rw(&buf) != 0) {
        printk("read mbr wrong!\n");
        while(1) yield();
    }
    MBR *mbr = (MBR *)buf.data;
    PartitionEntry *partition = (PartitionEntry *)&mbr->partition_entries[1]; // Second partition
    u32 start_lba = partition->start_lba;
    u32 num_sectors = partition->num_sectors;
    printk("Partition 2-LBA Start: %d, Number of Sectors: %d\n", start_lba, num_sectors);

    /**
     * (Final) TODO BEGIN
     * Map init.S to user space and trap_return to run icode.
     */
    Proc *p = thisproc();
    p->cwd = inodes.root;
    struct section *sec = kalloc(sizeof(struct section));
    sec->flags = ST_TEXT;
    sec->begin = (u64)icode - PAGE_BASE((u64)icode);
    sec->end = sec->begin + (u64)eicode - (u64)icode;
    init_sleeplock(&(sec->sleeplock));
    _insert_into_list(&p->pgdir.section_head, &sec->stnode);
    u64 va = 0;
    for(u64 ka = PAGE_BASE((u64)icode); ka <= (u64)eicode; ka += PAGE_SIZE) {
        vmmap(&(p->pgdir), va, (void *)ka, PTE_USER_DATA | PTE_RO);
        va += PAGE_SIZE;
    }
    p->ucontext->elr = sec->begin;
    printk("------ kernel entry over ------\n");
    set_return_addr(trap_return);
    /* (Final) TODO END */
}

NO_INLINE NO_RETURN void _panic(const char *file, int line) {
    printk("=====%s:%d PANIC%lld!=====\n", file, line, cpuid());
    panic_flag = true;
    while(panic_flag) { continue; }
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}