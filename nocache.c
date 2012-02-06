#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#include <pthread.h>

int (*_original_open)(const char *pathname, int flags, mode_t mode);
int (*_original_creat)(const char *pathname, int flags, mode_t mode);
int (*_original_openat)(int dirfd, const char *pathname, int flags, mode_t mode);
int (*_original_close)(int fd);

void init(void) __attribute__((constructor));
int open(const char *pathname, int flags, mode_t mode);
int open64(const char *pathname, int flags, mode_t mode)
    __attribute__(( alias("open")));
int creat(const char *pathname, int flags, mode_t mode);
int creat64(const char *pathname, int flags, mode_t mode)
    __attribute__(( alias("creat")));
int openat(int dirfd, const char *pathname, int flags, mode_t mode);
int openat64(int dirfd, const char *pathname, int flags, mode_t mode)
    __attribute__ ((alias ("openat")));
int __openat_2(int dirfd, const char *pathname, int flags, mode_t mode)
    __attribute__ ((alias ("openat")));
int close(int fd);

static void store_pageinfo(int fd);
static void free_unclaimed_pages(int fd);
extern int fadv_dontneed(int fd, off_t offset, off_t len);
extern int fadv_noreuse(int fd, off_t offset, off_t len);
extern void sync_if_writable(int fd);

#define _MAX_FDS 1024

struct fadv_info {
    int fd;
    off_t size;
    unsigned int nr_pages;
    unsigned char *info;
};
static struct fadv_info fds[_MAX_FDS];
static size_t PAGESIZE;
static pthread_mutex_t lock; /* protects access to fds[] */

void init(void)
{
    int i;
    _original_open = (int (*)(const char *, int, mode_t))
        dlsym(RTLD_NEXT, "open");
    _original_creat = (int (*)(const char *, int, mode_t))
        dlsym(RTLD_NEXT, "creat");
    _original_openat = (int (*)(int, const char *, int, mode_t))
        dlsym(RTLD_NEXT, "openat");
    _original_close = (int (*)(int)) dlsym(RTLD_NEXT, "close");
    pthread_mutex_init(&lock, NULL);
    PAGESIZE = getpagesize();
    for(i = 0; i < _MAX_FDS; i++)
        fds[i].fd = -1;
}

int open(const char *pathname, int flags, mode_t mode)
{
    int fd;
    if((fd = _original_open(pathname, flags, mode)) != -1) {
        store_pageinfo(fd);
        fadv_noreuse(fd, 0, 0);
    }
    return fd;
}

int creat(const char *pathname, int flags, mode_t mode)
{
    int fd;
    if((fd = _original_creat(pathname, flags, mode)) != -1) {
        store_pageinfo(fd);
        fadv_noreuse(fd, 0, 0);
    }
    return fd;
}

int openat(int dirfd, const char *pathname, int flags, mode_t mode)
{
    int fd;
    if((fd = _original_openat(dirfd, pathname, flags, mode)) != -1) {
        store_pageinfo(fd);
        fadv_noreuse(fd, 0, 0);
    }
    return fd;
}

int close(int fd)
{
    free_unclaimed_pages(fd);
    return _original_close(fd);
}

static void store_pageinfo(int fd)
{
    int i;
    int pages;
    struct stat st;
    void *file = NULL;
    unsigned char *pageinfo = NULL;

    if(fstat(fd, &st) == -1)
        return;
    if(!S_ISREG(st.st_mode))
        return;

    /* check if there's space to store the info */
    pthread_mutex_lock(&lock);
    for(i = 0; i < _MAX_FDS && fds[i].fd != -1; i++)
        ;
    if(i == _MAX_FDS) {
        pthread_mutex_unlock(&lock);
        return; /* no space! */
    }
    fds[i].fd = fd;
    pthread_mutex_unlock(&lock);

    /* If size is 0, mmap() will fail. We'll keep the fd stored, anyway, to
     * make sure the newly written pages will be freed (so no cleanup!). */
    if(st.st_size == 0) {
        fds[i].size = 0;
        fds[i].nr_pages = 0;
        fds[i].info = NULL;
        return;
    }

    fds[i].size = st.st_size;
    pages = fds[i].nr_pages = (st.st_size + PAGESIZE - 1) / PAGESIZE;
    pageinfo = calloc(sizeof(*pageinfo), pages);
    if(!pageinfo)
        goto cleanup;

    file = mmap(NULL, st.st_size, PROT_NONE, MAP_SHARED, fd, 0);
    if(file == MAP_FAILED)
        goto cleanup;
    if(mincore(file, st.st_size, pageinfo) == -1)
        goto cleanup;
    fds[i].info = pageinfo;

    munmap(file, st.st_size);
    return;

    cleanup:
    fds[i].fd = -1;
    if(pageinfo)
        free(pageinfo);
    if(file)
        munmap(file, st.st_size);
}

static void free_unclaimed_pages(int fd)
{
    int i, j;
    int start;

    if(fd == -1)
        return;

    pthread_mutex_lock(&lock);
    for(i = 0; i < _MAX_FDS; i++)
        if(fds[i].fd == fd)
            break;
    pthread_mutex_unlock(&lock);
    if(i == _MAX_FDS)
        return; /* not found */

    sync_if_writable(fd);

    start = j = 0;
    while(j < fds[i].nr_pages) {
        if(fds[i].info[j] & 1) {
            if(start < j)
                fadv_dontneed(fd, start*PAGESIZE, (j - start) * PAGESIZE);
            start = j + 1;
        }
        j++;
    }

    /* forget written contents that go beyond previous file size */
    fadv_dontneed(fd, start < j ? start*PAGESIZE : fds[i].size, 0);

    free(fds[i].info);
    fds[i].fd = -1;
}
