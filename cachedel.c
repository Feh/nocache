#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>

int exiterr(const char *s)
{
    perror(s);
    exit(-1);
}

int main(int argc, char *argv[])
{
    int fd;
    struct stat st;

    if(argc != 2) {
        fprintf(stderr, "usage: %s <file> "
            "-- call fadvise(DONTNEED) on file", argv[0]);
        exit(1);
    }

    fd = open(argv[1], O_RDONLY);
    if(fd == -1)
        exiterr("open");

    if(fstat(fd, &st) == -1)
        exiterr("fstat");
    if(!S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s: S_ISREG: not a regular file", argv[1]);
        return EXIT_FAILURE;
    }
    if(st.st_size == 0) {
        fprintf(stderr, "%s: file size is 0!\n", argv[1]);
        return EXIT_FAILURE;
    }

    if(posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == -1)
        exiterr("posix_fadvise");

    return EXIT_SUCCESS;
}
