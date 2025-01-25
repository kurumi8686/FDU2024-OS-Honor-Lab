#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>
#include <driver/clock.h>

extern void swtch(KernelContext* new_ctx, KernelContext** old_ctx);

extern bool panic_flag;
static SpinLock schedulerlock;  // 调度器锁
ListNode rq;  // runnable/running queue

static struct timer sched_timer[NCPU];  // lab3-时钟中断，调度时钟
void sched_timer_handler(struct timer* t) {
    t->elapse = 8;
    acquire_sched_lock();
    sched(RUNNABLE);
}

void init_sched(){
    // TODO: initialize the scheduler
    // 1. initialize the resources (e.g. locks, semaphores)
    init_list_node(&rq);
    init_spinlock(&schedulerlock);
    for (int i = 0; i < NCPU; i++) {
        sched_timer[i].triggered = true;
        sched_timer[i].elapse = 8;
        sched_timer[i].handler = &sched_timer_handler;
    }
    // 2. initialize the scheduler info of each CPU
    for (int i = 0; i < NCPU; i++) {
        Proc* p = kalloc(sizeof(Proc));
        p->idle = true;
        p->state = RUNNING;
        p->pid = -1;
        cpus[i].sched.thisproc = cpus[i].sched.idle = p;
        // idle进程是一个特殊的进程，也游离在进程树之外，所有永远不会进入rq，所以schinfo不用管，
        // 类似的，其它几个量也就不用初始化了，都和idle进程没什么关系。
    }
}

// TODO: return the current process
Proc *thisproc() { return cpus[cpuid()].sched.thisproc; }
// TODO: initialize your customized schinfo for every newly-created process
void init_schinfo(struct schinfo *p) { init_list_node(&p->rqnode); }
// TODO: acquire the sched_lock if need
void acquire_sched_lock() { acquire_spinlock(&schedulerlock); }
// TODO: release the sched_lock if need
void release_sched_lock() { release_spinlock(&schedulerlock); }

// 判断一个进程状态是否是zombie
bool is_zombie(Proc *p) {
    bool r;
    acquire_sched_lock();
    r = p->state == ZOMBIE;
    release_sched_lock();
    return r;
}
bool is_unused(Proc *p) {
    bool r;
    acquire_sched_lock();
    r = p->state == UNUSED;
    release_sched_lock();
    return r;
}


bool _activate_proc(Proc *p, bool onalert) {
    // TODO:(Lab5 new)
    // if the proc->state is RUNNING/RUNNABLE, do nothing and return false
    // if the proc->state is SLEEPING/UNUSED, set the process state to RUNNABLE, add it to the sched queue, and return true
    // if the proc->state is DEEPSLEEPING, do nothing if onalert or activate it if else, and return the corresponding value.
    acquire_sched_lock();
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    if (p->state==RUNNING || p->state==RUNNABLE || p->state==ZOMBIE ||
        (p->state==DEEPSLEEPING && onalert)) {
        release_sched_lock();
        return false;   // 什么都不做，已经在调度队列中了
    }
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    if (p->state==SLEEPING || p->state==UNUSED ||
        (p->state==DEEPSLEEPING && !onalert)) {
        p->state = RUNNABLE;
        _insert_into_list(&rq, &p->schinfo.rqnode);
    }
    release_sched_lock();
    return true;
}

static void update_this_state(enum procstate new_state) {
    // TODO: if you use template sched function, you should implement this routinue
    // update the state of current process to new_state, and modify the sched queue if necessary
    auto origin_state = thisproc()->state;
    thisproc()->state = new_state;
    if (thisproc()->pid == -1) return;
    if((origin_state==RUNNABLE || origin_state==RUNNING) && new_state!=RUNNING && new_state != RUNNABLE) {
        _detach_from_list(&thisproc()->schinfo.rqnode);
    }
    if (origin_state!=RUNNABLE && origin_state!=RUNNING && (new_state==RUNNABLE || new_state==RUNNING)) {
        _insert_into_list(&rq, &thisproc()->schinfo.rqnode);
    }
}

static Proc *pick_next() {
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process
    Proc *selected_proc = NULL;
    int min_load = 1000000;
    _for_in_list(p, &rq) {
        if(p == &rq) continue;
        auto proc = container_of(p, Proc, schinfo.rqnode);
        if(proc->state == RUNNABLE && proc->timeload < min_load) {
            selected_proc = proc;
            min_load = proc->timeload;
        }
    }
    if(selected_proc) {
        selected_proc->timeload++;
        return selected_proc;
    }

    return cpus[cpuid()].sched.idle;
}

static void update_this_proc(Proc *p) {
    // TODO: you should implement this routinue
    // update thisproc to the choosen process
    cpus[cpuid()].sched.thisproc = p;
    if (!sched_timer[cpuid()].triggered) {
        cancel_cpu_timer(&sched_timer[cpuid()]);
    }
    set_cpu_timer(&sched_timer[cpuid()]);
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state) {
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    // 如果当前进程带有killed标记，且new state不为zombie，则调度器直接返回，不做任何操作。
    if (this->killed && new_state!=ZOMBIE) { release_sched_lock(); return; }
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    // printk("sched at here\n");
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this) {
        attach_pgdir(&next->pgdir);
        swtch(next->kcontext, &this->kcontext); // bug at here -- bug fixed
        // printk("333");
    }
    release_sched_lock();
}

u64 proc_entry(void (*entry)(u64), u64 arg) {
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}
