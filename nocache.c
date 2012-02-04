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
int (*_original_close)(int fd);

void init(void) __attribute__((constructor));
int open(const char *pathname, int flags, mode_t mode);
int close(int fd);

static void store_pageinfo(int fd);
static void free_unclaimed_pages(int fd);
extern int fadv_dontneed(int fd, off_t offset, off_t len);
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
static pthread_mutex_t lock;

void init(void)
{
    int i;
    _original_open = (int (*)(const char *, int, mode_t))
        dlsym(RTLD_NEXT, "open");
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
        pthread_mutex_lock(&lock);
        store_pageinfo(fd);
        pthread_mutex_unlock(&lock);
    }
    return fd;
}

int close(int fd)
{
    pthread_mutex_lock(&lock);
    free_unclaimed_pages(fd);
    pthread_mutex_unlock(&lock);
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
    for(i = 0; i < _MAX_FDS && fds[i].fd != -1; i++)
        ;
    if(i == _MAX_FDS)
        return; /* no space! */
    fds[i].fd = fd;

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

#if DEBUG
    fprintf(stderr, "cache stats: ");
    int j;
    for(j=0; i<pages; i++) {
        fprintf(stderr, "%c", (pageinfo[j] & 1) ? 'Y' : 'N');
    }
    fprintf(stderr, "\n");

    int j;
    for(j=0; j<pages; j++)
        if(!(pageinfo[j] & 1))
            break;
    if(j == pages)
        fprintf(stderr, "was fully in cache: %d: %d/%d\n", fd, j, pages);
    else
        fprintf(stderr, "was not fully in cache: %d: %d/%d\n", fd, j, pages);
#endif

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

    if(fd == -1)
        return;

    for(i = 0; i < _MAX_FDS; i++)
        if(fds[i].fd == fd)
            break;
    if(i == _MAX_FDS)
        return; /* not found */

    sync_if_writable(fd);

    for(j = 0; j < fds[i].nr_pages; j++) {
        if(!(fds[i].info[j] & 1)) {
            fadv_dontneed(fd, j*PAGESIZE, PAGESIZE);
        }
    }

    /* forget written contents that go beyond previous file size */
    fadv_dontneed(fd, fds[i].size, 0);

    if(fds[i].info)
        free(fds[i].info);
    fds[i].fd = -1;
}
