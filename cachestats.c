#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <error.h>
#include <stdio.h>

int exiterr(const char *s)
{
    perror(s);
    exit(-1);
}

int main(int argc, char *argv[])
{
    int i, j;
    int pages;
    int PAGESIZE;

    int quiet = 0;
    int verbose = 0;

    int fd;
    struct stat st;
    void *file = NULL;
    unsigned char *pageinfo = NULL;

    PAGESIZE = getpagesize();

    if(argc > 1) {
        if(!strcmp("-v", argv[1]))
            verbose = 1;
        else if(!strcmp("-q", argv[1]))
            quiet = 1;
    } else {
        fprintf(stderr, "usage: %s [-qv] <file> "
            "-- print out cache statistics\n", argv[0]);
        fprintf(stderr, "\t-v\tprint verbose cache map\n");
        fprintf(stderr, "\t-q\texit code tells if file is fully cached\n");
        exit(1);
    }

    if(quiet || verbose)
        argv++;

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
        printf("pages in cache: %d/%d (%.1f%%)  [filesize=%.1fK, "
                "pagesize=%dK]\n", 0, 0, 0.0,
                0.0, PAGESIZE / 1024);
        return EXIT_SUCCESS;
    }

    pages = (st.st_size + PAGESIZE - 1) / PAGESIZE;
    pageinfo = calloc(sizeof(*pageinfo), pages);
    if(!pageinfo)
        exiterr("calloc");

    file = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
    if(file == MAP_FAILED)
        exiterr("mmap");
    if(mincore(file, st.st_size, pageinfo) == -1)
        exiterr("mincore");

    i = j = 0;
    while(i < pages)
        if(pageinfo[i++] & 1)
            j++;

    if(quiet) {
        if(j == i)
            return EXIT_SUCCESS;
        return EXIT_FAILURE;
    }

    printf("pages in cache: %d/%d (%.1f%%)  [filesize=%.1fK, "
        "pagesize=%dK]\n", j, i, 100.0 * j / i,
        1.0 * st.st_size / 1024, PAGESIZE / 1024);

#define PAGES_PER_LINE 32
    if(verbose) {
        printf("\ncache map:\n");
        for(i = 0; i <= (pages - 1) / PAGES_PER_LINE; i++) {
            printf("%6d: |", i * PAGES_PER_LINE);
            for(j = 0; j < PAGES_PER_LINE; j++) {
                if(i * PAGES_PER_LINE + j == pages)
                    break;
                printf("%c|",
                    pageinfo[i * PAGES_PER_LINE + j] & 1 ? 'x' : ' ');
            }
            printf("\n");
        }
    }

    munmap(file, st.st_size);
    return EXIT_SUCCESS;
}
