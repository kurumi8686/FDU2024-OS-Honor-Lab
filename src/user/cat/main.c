#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char buf[512];

void simple_cat(int fd) {
    int len, buf_sz = sizeof(buf);
    while((len = read(fd, buf, buf_sz)) > 0) write(1, buf, len);
    if(len < 0) { printf("cat: read error\n"); exit(1); }
}

int main(int argc, char *argv[]) {
    /* (Final) TODO BEGIN */
    printf("cat start------------\n");
    int fd;
    if(argc <= 1) { simple_cat(0); exit(0); }
    for(int i = 1; i < argc; i++) {
        if ((fd = open(argv[i], 0)) < 0) {
            printf("cat: cannot open %s\n", argv[i]);
            exit(0);
        }
        simple_cat(fd);
        close(fd);
    }
    exit(0);
    /* (Final) TODO END */
}