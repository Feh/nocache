#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int exiterr(const char *s)
{
    perror(s);
    exit(-1);
}

int main(int argc, char *argv[])
{
    int i, n = 1;
    int fd;
    char *fn;
    struct stat st;

    if(argc == 4 && !strcmp("-n", argv[1])) {
            n = atoi(argv[2]);
            fn = argv[3];
    } else if(argc != 2) {
        fprintf(stderr, "usage: %s [-n <n>] <file> "
            "-- call fadvise(DONTNEED) <n> times on file\n", argv[0]);
        exit(1);
    } else {
        fn = argv[1];
    }

    fd = open(fn, O_RDONLY);
    if(fd == -1)
        exiterr("open");

    if(fstat(fd, &st) == -1)
        exiterr("fstat");
    if(!S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s: S_ISREG: not a regular file", fn);
        return EXIT_FAILURE;
    }
    if(st.st_size == 0) {
        fprintf(stderr, "%s: file size is 0!\n", fn);
        return EXIT_FAILURE;
    }

    for(i = 0; i < n; i++)
        if(posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == -1)
            exiterr("posix_fadvise");

    return EXIT_SUCCESS;
}
