#include <aarch64/trap.h>
#include <aarch64/intrinsic.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <driver/interrupt.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <kernel/paging.h>

#define SPSR_EL1_DAIF_MASK 0xF

void trap_global_handler(UserContext *context) {
    thisproc()->ucontext = context;
    u64 esr = arch_get_esr();
    u64 ec = esr >> ESR_EC_SHIFT;
    u64 iss = esr & ESR_ISS_MASK;
    u64 ir = esr & ESR_IR_MASK;
    arch_reset_esr();
    switch (ec) {
    case ESR_EC_UNKNOWN: {
        if (ir)
            PANIC();
        else {
            interrupt_global_handler();
        }
    } break;
    case ESR_EC_SVC64: {
        syscall_entry(context);
    } break;
    case ESR_EC_IABORT_EL0:
    case ESR_EC_IABORT_EL1:
    case ESR_EC_DABORT_EL0:
    case ESR_EC_DABORT_EL1: {
        pgfault_handler(iss);
    } break;
    default: {
        printk("Unknwon exception %llu\n", esr);
        PANIC();
    }
    }
    
    // Lab4: stop killed process while returning to user space
    // exception link register (ELR_EL1)
    // 进程的返回地址会被保存到 ELR_EL1 寄存器中
    // 当处理完异常或系统调用后，内核会从 ELR_EL1 中读取该地址，决定返回到哪个地址执行下一条指令。
    // 如果该地址指向用户态地址空间，则表明进程即将返回用户态。
    if (thisproc()->killed == true && ((context->elr) & 0xffff000000000000) == 0) exit(-1);

}

NO_RETURN void trap_error_handler(u64 type) {
    printk("Unknown trap type %llu\n", type);
    PANIC();
}