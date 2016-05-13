#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "pageinfo.h"

extern FILE *debugfp;
#define DEBUG(...) \
    do { \
        if(debugfp != NULL) { \
            fprintf(debugfp, "[nocache] DEBUG: " __VA_ARGS__); \
        } \
    } while(0)

static int insert_into_br_list(struct file_pageinfo *pi,
    struct byterange **brtail, size_t pos, size_t len);

struct file_pageinfo *fd_get_pageinfo(int fd, struct file_pageinfo *pi)
{
    int PAGESIZE;
    void *file;
    struct byterange *br = NULL; /* tail of our interval list */
    struct stat st;
    unsigned char *page_vec = NULL;

    PAGESIZE = getpagesize();

    if(pi->fd != fd) {
        DEBUG("fd_get_pageinfo BUG, pi->fd != fd\n");
        return NULL;
    }
    pi->fd = fd;
    pi->unmapped = NULL;

    if(fstat(fd, &st) == -1 || !S_ISREG(st.st_mode))
        return NULL;
    pi->size = st.st_size;
    pi->nr_pages = (st.st_size + PAGESIZE - 1) / PAGESIZE;
    DEBUG("fd_get_pageinfo(fd=%d): st.st_size=%lld, nr_pages=%lld\n",
          fd, (long long)st.st_size, (long long)pi->nr_pages);

    /* If size is 0, mmap() will fail. We'll keep the fd stored, anyway, to
     * make sure the newly written pages will be freed on close(). */
    if(pi->size == 0)
        return pi;

    /* If mmap() fails, we will probably have a file in write-only or
     * append-only mode. In this mode the caller will not be able to
     * bring in new pages anyway, but we'll record the current size */
    file = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
    if(file == MAP_FAILED) {
        DEBUG("fd_get_pageinfo(fd=%d): mmap failed (don't worry), errno:%d, %s\n",
                fd, errno, strerror(errno));
        return pi;
    }

    page_vec = calloc(sizeof(*page_vec), pi->nr_pages);
    if(!page_vec) {
        DEBUG("calloc failed: size=%zd on fd=%d\n", pi->nr_pages, fd);
        goto cleanup;
    }

    if(mincore(file, pi->size, page_vec) == -1)
        goto cleanup;

    munmap(file, st.st_size);
    file = NULL;

    /* compute (byte) intervals that are *not* in the file system
     * cache, since we will want to free those on close() */
    pi->nr_pages_cached = pi->nr_pages;
    size_t i, start = 0;
    for(i = 0; i < pi->nr_pages; i++) {
        if(!(page_vec[i] & 1))
            continue;
        if(start < i) {
            insert_into_br_list(pi, &br, start * PAGESIZE,
                (i - start) * PAGESIZE);
            pi->nr_pages_cached -= i - start;
        }
        start = i + 1;
    }
    /* Leftover interval: clear until end of file */
    if(start < pi->nr_pages) {
        insert_into_br_list(pi, &br, start * PAGESIZE,
                pi->size - start * PAGESIZE);
        pi->nr_pages_cached -= pi->nr_pages - start;
    }

    free(page_vec);

    return pi;

cleanup:
    if(file)
        munmap(file, st.st_size);
    free(page_vec);
    return NULL;
}

static int insert_into_br_list(struct file_pageinfo *pi,
    struct byterange **brtail, size_t pos, size_t len)
{
    struct byterange *tmp;
    tmp = malloc(sizeof(*tmp));
    if(!tmp)
        return 0;

    tmp->pos = pos;
    tmp->len = len;
    tmp->next = NULL;

    if(pi->unmapped == NULL) {
        pi->unmapped = tmp;
    } else if(*brtail != NULL) {
        (*brtail)->next = tmp;
    }
    *brtail = tmp;
    return 1;
}

void free_br_list(struct byterange **br)
{
    struct byterange *tmp;
    while(*br != NULL) {
        tmp = *br;
        (*br) = tmp->next;
        free(tmp);
    }
    *br = NULL;
}

/* vim:set et sw=4 ts=4: */
