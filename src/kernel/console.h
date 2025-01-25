#pragma once
#include "common/defines.h"
#include "fs/inode.h"

#define IBUF_SIZE 128
#define INPUT_BUF 128
#define BACKSPACE 127
#define C(x) ((x) - '@') // Control-x

struct console {
    SpinLock lock;
    Semaphore sem;
    char buf[IBUF_SIZE];
    usize read_idx;
    usize write_idx;
    usize edit_idx;
};

void console_init();
void console_intr(char c);
isize console_write(Inode *ip, char *buf, isize n);
isize console_read(Inode *ip, char *dst, isize n);