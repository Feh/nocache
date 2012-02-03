#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

int (*_original_open)(const char *pathname, int flags, mode_t mode);
int (*_original_close)(int fd);

void init(void) __attribute__((constructor));
int open(const char *pathname, int flags, mode_t mode);
int close(int fd);
static void store_pageinfo(int fd);
static void free_pages(int fd);
extern int fadv_dontneed(int fd, off_t offset, off_t len);

#define _MAX_FDS 1024

struct fadv_info {
    int fd;
    unsigned int nr_pages;
    void *info;
};
static struct fadv_info fds[_MAX_FDS];
static size_t PAGESIZE;

void init(void)
{
    _original_open = (int (*)(const char *, int, mode_t))
        dlsym(RTLD_NEXT, "open");
    _original_close = (int (*)(int)) dlsym(RTLD_NEXT, "close");
    PAGESIZE = sysconf(_SC_PAGESIZE);
}

int open(const char *pathname, int flags, mode_t mode)
{
    int fd;
    if((fd = _original_open(pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int close(int fd)
{
    free_pages(fd);
    return _original_close(fd);
}

static void store_pageinfo(int fd)
{
    int i;
    int pages;
    struct stat st;
    void *file;
    unsigned char *pageinfo;

    /* check if there's space to store the info */
    for(i = 0; i < _MAX_FDS && fds[i].fd; i++)
        ;
    if(i == _MAX_FDS)
        return; /* no space! */
    fds[i].fd = fd;

    if(fstat(fd, &st) == -1)
        return;

    pages = fds[i].nr_pages = (st.st_size + PAGESIZE - 1) / PAGESIZE;
    pageinfo = calloc(sizeof(*pageinfo), pages);
    if(!pageinfo)
        return;

    file = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
    if(file == MAP_FAILED)
        return;
    if(mincore(file, st.st_size, pageinfo) == -1)
        return;

    fds[i].info = pageinfo;

#if DEBUG
    fprintf(stderr, "cache stats: ");
    int j;
    for(j=0; i<pages; i++) {
        fprintf(stderr, "%c", (pageinfo[j] & 1) ? 'Y' : 'N');
    }
    fprintf(stderr, "\n");
#endif

    munmap(file, st.st_size);
}

static void free_pages(int fd)
{
    int i, j;

    for(i = 0; i < _MAX_FDS; i++)
        if(fds[i].fd == fd)
            break;
    if(i == _MAX_FDS)
        return; /* not found */

    for(j = 0; j < fds[i].nr_pages; j++)
        if(!(((unsigned char *)fds[i].info)[j] & 1))
            fadv_dontneed(fd, j*PAGESIZE, PAGESIZE);
}
