#include <kernel/console.h>
#include <aarch64/intrinsic.h>
#include <kernel/sched.h>
#include <driver/uart.h>
#include <common/string.h>
#include <kernel/printk.h>

/*
// for tab
// 示例命令列表
const char *commands[] = {"ls", "cat", "echo", "mkdir", "rm", "touch", "exit", NULL};
// 自动补全函数
char *autocomplete(const char *input, size_t length) {
    static char completion[128];
    size_t i;
    // 清空 completion
    for (i = 0; i < sizeof(completion); i++) completion[i] = '\0';
    // 1. 检查是否是路径
    const char *last_slash = NULL;
    for (i = strlen(input); i > 0; --i) {
        if (input[i - 1] == '/') {
            last_slash = input + i;
            break;
        }
    }
    if (last_slash) {
        // 这是一个路径补全
        char directory[128];
        size_t dir_length = last_slash - input;
        strncpy(directory, input, dir_length);
        // 这里用一个自定义方式模拟打开目录并遍历
        // 注意：这里只是示范，实际操作应该从你自己的文件系统实现中读取文件列表
        const char *dummy_files[] = {"file1.txt", "file2.txt", "dir1", NULL};  // 模拟文件和目录
        for (i = 0; dummy_files[i] != NULL; i++) {
            if (strncmp(dummy_files[i], last_slash, strlen(last_slash)) == 0) {
                strncpy(completion, directory, sizeof(completion) - 1);
                strncpy(completion + strlen(directory), dummy_files[i], sizeof(completion) - strlen(directory) - 1);
                return completion;
            }
        }
    }
    // 2. 检查是否是命令
    const char *commands[] = {"ls", "cd", "pwd", "exit", NULL};  // 模拟命令列表
    for (i = 0; commands[i] != NULL; i++) {
        if (strncmp(commands[i], input, length) == 0) {
            strncpy(completion, commands[i], sizeof(completion) - 1);
            return completion;
        }
    }
    // 没有找到匹配项
    return NULL;
}
// for 历史记录
int history_index = 0, history_count = 0;
void clear_current_line() { return; }
void load_history_line(int history_index, char* buf, usize* edit_idx) {
    history_index = history_index;
    buf = buf;
    edit_idx = edit_idx;
    return;
}
// for 复制粘贴
char clipboard[100];
*/

struct console cons;

void console_init() {
    /* (Final) TODO BEGIN */
    init_spinlock(&(cons.lock));
    init_sem(&(cons.sem), 0);
    /* (Final) TODO END */
}

/**
 * console_write - write to uart from the console buffer.
 * @ip: the pointer to the inode
 * @buf: the buffer
 * @n: number of bytes to write
 */
isize console_write(Inode *ip, char *buf, isize n) {
    /* (Final) TODO BEGIN */
    inodes.unlock(ip);
    acquire_spinlock(&(cons.lock));
    for(int i = 0; i < n; i++) uart_put_char(buf[i] % INPUT_BUF);
    release_spinlock(&(cons.lock));
    inodes.lock(ip);
    return n;
    /* (Final) TODO END */
}

/**
 * console_read - read to the destination from the buffer
 * @ip: the pointer to the inode
 * @dst: the destination
 * @n: number of bytes to read
 */
isize console_read(Inode *ip, char *dst, isize n) {
    /* (Final) TODO BEGIN */
    isize target = n;
    isize r = 0;
    inodes.unlock(ip);
    acquire_spinlock(&(cons.lock));
    while(n > 0) {
        if(cons.read_idx == cons.write_idx) {
            release_spinlock(&(cons.lock));
            if(_wait_sem(&(cons.sem), true) == 0) return -1;
            acquire_spinlock(&(cons.lock));
        }
        cons.read_idx = (cons.read_idx + 1) % INPUT_BUF;
        char c = cons.buf[cons.read_idx];
        if(c == C('D')) {
            if (n < target) cons.read_idx--;
            break;
        }
        *(dst++) = c; r++; n--;
        if(c == '\n') break;
    }
    release_spinlock(&(cons.lock));
    inodes.lock(ip);
    return r;
    /* (Final) TODO END */
}

void console_intr(char c) {
    /* (Final) TODO BEGIN */
    acquire_spinlock(&(cons.lock));
    while(c != 0xff){
        if(c == '\r') c = '\n';
        if(c == BACKSPACE) {
            if(cons.edit_idx != cons.write_idx){
                cons.edit_idx = (cons.edit_idx-1) % INPUT_BUF;
                uart_put_char('\b'); uart_put_char(' '); uart_put_char('\b');
            }
        }
        else if(c == C('U')) {
            while(cons.edit_idx != cons.write_idx && cons.buf[(cons.edit_idx - 1) % INPUT_BUF] != '\n'){
                cons.edit_idx = (cons.edit_idx - 1) % INPUT_BUF;
                uart_put_char('\b'); uart_put_char(' '); uart_put_char('\b');
            }
        }
        else if(c == C('D')){
            if((cons.edit_idx + 1) % INPUT_BUF == cons.read_idx) continue;
            cons.edit_idx = (cons.edit_idx + 1) % INPUT_BUF;
            cons.buf[cons.edit_idx] = c;
            uart_put_char(c);
            cons.write_idx = cons.edit_idx;
            post_sem(&(cons.sem));
        }
        else if(c == C('C')) {
            uart_put_char('^'); uart_put_char('C');
            int pid = thisproc()->pid;
            if(!(pid == -1 || pid == 0 || pid == 1)) {
                ASSERT(kill(thisproc()->pid)!=-1);
            }
        }
        else{ // normal char
            if((cons.edit_idx + 1) % INPUT_BUF == cons.read_idx) continue;
            cons.edit_idx = (cons.edit_idx + 1) % INPUT_BUF;
            cons.buf[cons.edit_idx] = c;
            uart_put_char(c);
            if(c == '\n' || (cons.edit_idx + 1) % INPUT_BUF == cons.read_idx) {
                cons.write_idx = cons.edit_idx;
                post_sem(&(cons.sem));
            }
        }
        break;
    }
    release_spinlock(&(cons.lock));
    /* (Final) TODO END */
}