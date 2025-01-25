#pragma once
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/rbtree.h>
#include <common/spinlock.h>
#include <kernel/pt.h>
#include <fs/file.h>
#include <fs/inode.h>

// 进程在操作系统中串连成一个树状结构
enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, DEEPSLEEPING, ZOMBIE };

typedef struct UserContext {
    // TODO: customize your trap frame
    u64 q0[2];
    u64 spsr; u64 elr; u64 sp; u64 tpidr0;
    u64 x[32]; // 对应trap.S中
} UserContext;

// 考虑到aarch64对栈指针的16bytes对齐限制，kcontext最好是16的倍数（包是的）
typedef struct KernelContext {
    // TODO: customize your context
    u64 lr; u64 x0; u64 x1;
    u64 x[11]; // x[19]~x[29] 对应swtch.S中
    // 算是占位空间，如果预留空间不足，会栈溢出
} KernelContext;

// embeded data for procs
struct schinfo {
    // TODO: customize your sched info
    ListNode rqnode;  // all the runnable/running processes
};

typedef struct Proc {
    bool killed;  // 下一个实验中会引入 killed 的概念
    bool idle;  // 标记进程是否为 idle 进程。
    int pid;  // 进程唯一标记pid，不重复
    int exitcode;  // 进程退出时设置，将回由其父进程。
    enum procstate state;  // 当前进程所处状态
    Semaphore childexit;  // 进程退出的信号量，用于提示子进程退出
    // 还要唤醒 SLEEPING 状态的父进程以回收子进程。
    ListNode children;  // 子进程列表
    ListNode ptnode;  // 进程作为子进程时，自己串在链表上的节点。
    struct Proc *parent;  // 指向父进程指针
    struct schinfo schinfo;  // 调度信息
    struct pgdir pgdir;  // 存储进程的用户态内存空间的相关信息
    void *kstack;  // 内核程序运行时使用的栈
    UserContext *ucontext;  // 用户态上下文，用于保存用户态的寄存器信息，也称作 trap frame
    KernelContext *kcontext;  // 内核态上下文，用于保存内核态的寄存器信息。
    int timeload;  // 时间负载，衡量当前进程的cpu占用率
    struct oftable oftable;
    Inode *cwd;
    struct vma *vma;
} Proc;

void init_kproc();
void init_proc(Proc *);

// 每个节点可以通过 create_proc 创建子进程
WARN_RESULT Proc *create_proc();
// 启动子进程
int start_proc(Proc *, void (*entry)(u64), u64 arg);
// 当子进程 exit 退出后，父进程会负责管理并回收子进程的资源
NO_RETURN void exit(int code);
// 并且获取子进程退出的信息 exitcode
WARN_RESULT int wait(int *exitcode);
WARN_RESULT int kill(int pid);
WARN_RESULT int fork();

void set_parent_to_this(struct Proc*);


