#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

define_syscall(gettid) { return thisproc()->pid; }

define_syscall(set_tid_address, int *tidptr) {
    // printk("set_tid_address called: pid=%d\n\n", thisproc()->pid);
    (void)tidptr; return thisproc()->pid;
}

define_syscall(sigprocmask) {
    // printk("sigprocmask, nothing\n\n");
    return 0;
}
define_syscall(rt_sigprocmask) {
    // printk("sigprocmask, nothing\n\n");
    return 0;
}

define_syscall(myyield) { yield(); return 0; }
define_syscall(yield) { yield(); return 0; }

define_syscall(pstat) { return (u64)left_page_cnt(); }

define_syscall(sbrk, i64 size) { return sbrk(size); }

define_syscall(clone, int flag, void *childstk) {
    // printk("sys_clone called: flag=%d, childstk=%p\n", flag, childstk);
    if (flag != 17) {
        // printk("sys_clone: flags other than SIGCHLD are not supported.\n");
        return -1;
    }
    (void)childstk;
    // printk("sys_clone: calling fork to create a new child process...\n");
    return fork();
}

define_syscall(myexit, int n) { exit(n); }

define_syscall(exit, int n) { exit(n); }

define_syscall(exit_group, int n) { exit(n); }

int execve(const char *path, char *const argv[], char *const envp[]);
define_syscall(execve, const char *p, void *argv, void *envp) {
    if (!user_strlen(p, 256)) { printk("what???\n\n"); return -1; }
    return execve(p, argv, envp);
}

define_syscall(wait4, int pid, int *wstatus, int options, void *rusage) {
    // printk("sys_wait4 called: pid=%d, options=0x%x, wstatus=0x%p, rusage=0x%p\n",
    //        pid, options, wstatus, rusage);
    if (pid != -1 || wstatus != 0 || options != 0 || rusage != 0) {
        printk("sys_wait4: unimplemented. pid %d, wstatus 0x%p, options 0x%x, "
               "rusage 0x%p\n",
               pid, wstatus, options, rusage);
        while (1) {}
        return -1;
    }
    int code;
    // printk("sys_wait4 end...\n\n");
    return wait(&code);
}
