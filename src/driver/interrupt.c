#include <aarch64/intrinsic.h>
#include <driver/base.h>
#include <driver/interrupt.h>
#include <driver/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <driver/gicv3.h>

static InterruptHandler int_handler[NUM_IRQ_TYPES];

static void default_handler(u32 intid) {
    printk("[Error CPU %lld]: Interrupt %d not implemented.", cpuid(), intid);
    PANIC();
}

void init_interrupt() {
    for (usize i = 0; i < NUM_IRQ_TYPES; i++) {
        int_handler[i] = default_handler;
    }
    // device_put_u32(GPU_INT_ROUTE, GPU_IRQ2CORE(0)); // add
}

void set_interrupt_handler(InterruptType type, InterruptHandler handler) {
    // device_put_u32(ENABLE_IRQS_1 + 4 * (type / 32), 1u << (type % 32)); // add
    int_handler[type] = handler;
}

void interrupt_global_handler() {
    u32 iar = gic_iar();
    u32 intid = iar & 0x3ff;

    if (intid == 1023) {
        printk("[Warning]: Spurious Interrupt.\n");
        return;
    }

    gic_eoi(iar);

    if (int_handler[intid])
        int_handler[intid](intid);
}
