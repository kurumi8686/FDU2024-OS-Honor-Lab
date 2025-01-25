#pragma once
#include <kernel/proc.h>
#include <common/rbtree.h>
#define NCPU 4

struct sched {
    // TODO: customize your sched info
    Proc* thisproc;  // cpu当前正在运行的进程
    Proc* idle;  // 每个cpu都有一个idle进程（记录main函数的上下文自然演化而来）
};

struct cpu {
    bool online;
    struct rb_root_ timer;
    struct sched sched;
};

extern struct cpu cpus[NCPU];

struct timer {
    bool triggered;
    int elapse;  // 倒计时值，表示在计时器被触发之前剩余的时间。
    // 当elapse为0时，计时器会被触发，并调用对应的回调函数handler
    u64 _key;
    struct rb_node_ _node;
    void (*handler)(struct timer *);
    u64 data;
};

void init_clock_handler();

void set_cpu_on();
void set_cpu_off();

void set_cpu_timer(struct timer *timer);
void cancel_cpu_timer(struct timer *timer);