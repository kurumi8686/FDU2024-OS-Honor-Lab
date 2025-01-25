#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    /* (Final) TODO BEGIN */
    printf("\nmkdir start--------------\n");
    if(argc < 2) { printf("Usage: mkdir your_file_name\n"); exit(1); }
    for(int i = 1; i < argc; i++) {
        if(mkdir(argv[i], 0) < 0) {
            printf("mkdir: %s failed to create\n", argv[i]);
            break;
        }
    }
    printf("mkdir end -------------\n\n");
    exit(0);
    /* (Final) TODO END */
}