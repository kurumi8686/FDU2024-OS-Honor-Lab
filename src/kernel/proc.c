#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <kernel/paging.h>

Proc root_proc;
void kernel_entry();
void proc_entry();
static SpinLock processlock;  // 进程锁
static int max_pid;  // 管理global的进程pid最大值（进程数量）

// init_kproc initializes the kernel process
// NOTE: should call after kinit
void init_kproc() { // TODO:
    // 1. init global resources (e.g. locks, semaphores)
    init_spinlock(&processlock);
    // 2. init the root_proc (finished)
    init_proc(&root_proc);
    root_proc.parent = &root_proc;  // 标识进程树的根，parent==self
    start_proc(&root_proc, kernel_entry, 123456);
}

void init_proc(Proc *p) { // TODO:
    // NOTE: be careful of concurrency
    acquire_spinlock(&processlock);
    // setup the Proc with kstack and pid allocated
    memset(p, 0, sizeof(Proc));
    p->killed = false;
    p->idle = false;
    p->exitcode = 0;
    p->state = UNUSED;
    p->pid = ++max_pid;
    p->parent = NULL;
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    p->kstack = kalloc_page();
    memset(p->kstack, 0, PAGE_SIZE);
    init_schinfo(&p->schinfo);
    init_pgdir(&p->pgdir);  // lab3_new_added
    p->kcontext=(KernelContext*)((u64)p->kstack+PAGE_SIZE-16-sizeof(KernelContext)-sizeof(UserContext));
    p->ucontext=(UserContext*)((u64)p->kstack+PAGE_SIZE-16-sizeof(UserContext));
    p->timeload = 0;
    init_oftable(&(p->oftable));
    release_spinlock(&processlock);
}

Proc *create_proc() {
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

void set_parent_to_this(Proc *proc) { // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    acquire_spinlock(&processlock);
    // NOTE: it's ensured that the old proc->parent = NULL
    ASSERT(proc->parent == NULL);
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    release_spinlock(&processlock);
}

int start_proc(Proc *p, void (*entry)(u64), u64 arg) { // TODO:
    // NOTE: be careful of concurrency
    acquire_spinlock(&processlock);
    // 1. set the parent to root_proc if NULL
    if(p->parent == NULL) {
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
    }
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    p->kcontext->lr = (u64)&proc_entry;
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = (u64)arg;
    // 3. activate the proc and return its pid
    activate_proc(p);
    release_spinlock(&processlock);
    return p->pid;
}

int wait(int *exitcode) { // TODO:
    // 1. return -1 if no children
    // init_list_node里面初始化prev和next都是自己，所以要是没有子进程，也就意味着next等于自己
    acquire_spinlock(&processlock);
    if(thisproc()->children.next == &thisproc()->children) { release_spinlock(&processlock); return -1; }
    // 2. wait for childexit
    // 等不到有子进程退出，直接返回-1。
    release_spinlock(&processlock);
    if(!wait_sem(&thisproc()->childexit)) { return -1; }
    // NOTE: be careful of concurrency
    acquire_spinlock(&processlock);
    acquire_sched_lock();
    // 3. if any child exits, clean it up and return its pid and exitcode
    Proc* zombienode = NULL;
    int zombieid = -1;
    _for_in_list(p, &thisproc()->children) {
        if(p == &thisproc()->children) continue;
        auto childproc = container_of(p, Proc, ptnode);
        if(childproc->state == ZOMBIE) { zombienode = childproc; break; }
    }
    if(zombienode) {
        *exitcode = zombienode->exitcode;  // 修改为这个僵尸进程的exitcode
        zombieid = zombienode->pid;  // 记录这个僵尸进程的pid，准备退出
        release_sched_lock();
        acquire_sched_lock();
        _detach_from_list(&zombienode->ptnode);  // 释放资源
        _detach_from_list(&zombienode->schinfo.rqnode);
        kfree_page(zombienode->kstack);
        kfree(zombienode);
    }
    release_sched_lock();
    release_spinlock(&processlock);
    return zombieid;
}

NO_RETURN void exit(int code) { // TODO:
    ASSERT(thisproc()!=&root_proc && thisproc()->pid!=-1);
    // NOTE: be careful of concurrency
    acquire_spinlock(&processlock);
    acquire_sched_lock();
    // 1. set the exitcode
    thisproc()->exitcode = code;
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    _for_in_list(p, &thisproc()->children) {
        if(p == &thisproc()->children) continue;
        auto childproc = container_of(p, Proc, ptnode);
        childproc->parent = &root_proc;  // 更新父进程为root进程
        if(childproc->state == ZOMBIE) {
            release_sched_lock();
            post_sem(&root_proc.childexit);
            acquire_sched_lock();
        }
    }
    if(!_empty_list(&thisproc()->children)) {
        _merge_list(&root_proc.children, thisproc()->children.next);
        _detach_from_list(&thisproc()->children);
    }

    // release files used by this proc
    for(int i = 0; i < NOPENFILE; ++i) {
        if(thisproc()->oftable.openfilelist[i] != NULL) {
            file_close(thisproc()->oftable.openfilelist[i]);
            thisproc()->oftable.openfilelist[i] = NULL;
        }
    }
/*
    // release current working dictionary
    OpContext ctx;
    bcache.begin_op(&ctx);
    inodes.put(&ctx, thisproc()->cwd);
    bcache.end_op(&ctx);
    thisproc()->cwd = NULL;
*/
    // 4. sched(ZOMBIE)
    release_sched_lock();
    release_spinlock(&processlock);
    acquire_spinlock(&processlock);
    acquire_sched_lock();
    free_pgdir(&thisproc()->pgdir);
    release_sched_lock();
    post_sem(&thisproc()->parent->childexit);
    acquire_sched_lock();
    release_spinlock(&processlock);
    sched(ZOMBIE);
    PANIC(); // prevent the warning of 'no_return function returns'
}

Proc* find_proc(int pid, Proc* current_proc) {
    if (current_proc->pid == pid && !is_unused(current_proc)) { return current_proc; }
    _for_in_list(p, &current_proc->children){
        if (p == &current_proc->children) continue;
        Proc* child_proc = container_of(p, Proc, ptnode);
        Proc* res_proc = find_proc(pid, child_proc);
        if(res_proc) return res_proc;
    }
    return NULL;
}

// 遍历进程树，搜索指定pid且状态不为unused的进程
int kill(int pid) { // TODO:
    acquire_spinlock(&processlock);
    Proc* kill_proc = find_proc(pid,&root_proc);
    release_spinlock(&processlock);
    // Set the killed flag of the proc to true and return 0.
    if (kill_proc) {
        acquire_spinlock(&processlock);
        kill_proc->killed = true;
        // activate_proc(kill_proc);
        alert_proc(kill_proc);  // _activate_proc(proc, true)
        release_spinlock(&processlock);
        return 0;
    }
    // Return -1 if the pid is invalid (proc not found).
    return -1;
}


/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();
int fork() {
    /**
     * (Final) TODO BEGIN
     * 1. Create a new child process.
     * 2. Copy the parent's memory space.
     * 3. Copy the parent's trapframe.
     * 4. Set the parent of the new proc to the parent of the parent.
     * 5. Set the state of the new proc to RUNNABLE.
     * 6. Activate the new proc and return its pid.
     */
    // printk("----- fork start -----\n");
    struct Proc* child_proc = create_proc();
    struct Proc* this_proc = thisproc();
    set_parent_to_this(child_proc);
    // copy all registers
    memmove(child_proc->ucontext, this_proc->ucontext, sizeof(UserContext));
    // fork return as child
    child_proc->ucontext->x[0] = 0;
    // use same working dictionary
    child_proc->cwd = inodes.share(this_proc->cwd);
    // copy oftable to use same files
    for(int i = 0; i < NOPENFILE; i++) {
        if(this_proc->oftable.openfilelist[i] != NULL) {
            child_proc->oftable.openfilelist[i] = file_dup(this_proc->oftable.openfilelist[i]);
        }
    }
    // copy pgdir
    PTEntriesPtr old_pte;
    _for_in_list(node, &(this_proc->pgdir.section_head)){
        if(node == &(this_proc->pgdir.section_head)) {
            break;
        }
        struct section* st = container_of(node, struct section, stnode);
        // fork without cow
        for(u64 va = PAGE_BASE(st->begin); va < st->end; va += PAGE_SIZE){
             old_pte = get_pte(&(this_proc->pgdir), va, false);
             if((old_pte == NULL) || !(*old_pte & PTE_VALID)) continue;
             void* new_page = kalloc_page();
             vmmap(&(child_proc->pgdir), va, new_page, PTE_FLAGS(*old_pte));
             copyout(&(child_proc->pgdir), (void*)va, (void*)P2K(PTE_ADDRESS(*old_pte)), PAGE_SIZE);
        }
    }
    copy_sections(&(this_proc->pgdir.section_head), &(child_proc->pgdir.section_head));

    // start proc
    start_proc(child_proc, trap_return, 0);
    // printk("------ fork end ------\n\n");
    return child_proc->pid;
    /* (Final) TODO END */
}