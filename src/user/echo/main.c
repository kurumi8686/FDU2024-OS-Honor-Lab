
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
    printf("\necho start----------------\n");
    if (argc == 1) {
        static char buf[512];
        char *s;
        while ((s = fgets(buf, 512, stdin)) != NULL)
            printf("%s", s);
        return 0;
    }

    int nflag;
    if (*++argv && !strcmp(*argv, "-n")) {
        ++argv;
        nflag = 1;
    } else {
        nflag = 0;
    }

    while (*argv) {
        fputs(*argv, stdout);
        if (*++argv)
            putchar(' ');
    }
    if (!nflag)
        putchar('\n');
    printf("echo end ------------\n\n");
    return 0;
}