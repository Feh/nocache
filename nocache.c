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
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <assert.h>

#include "fcntl_helpers.h"

static void init(void) __attribute__((constructor));
static void destroy(void) __attribute__((destructor));
static void init_mutex(void);
static void handle_stdout(void);

static void store_pageinfo(int fd);
static void free_unclaimed_pages(int fd);

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
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int close(int fd);
FILE *fopen(const char *path, const char *mode);
FILE *fopen64(const char *path, const char *mode)
    __attribute__ ((alias ("fopen")));
int fclose(FILE *fp);

int (*_original_open)(const char *pathname, int flags, mode_t mode);
int (*_original_creat)(const char *pathname, int flags, mode_t mode);
int (*_original_openat)(int dirfd, const char *pathname, int flags, mode_t mode);
int (*_original_dup)(int fd);
int (*_original_dup2)(int newfd, int oldfd);
int (*_original_close)(int fd);
FILE *(*_original_fopen)(const char *path, const char *mode);
int (*_original_fclose)(FILE *fp);


struct fadv_info {
    int fd;
    off_t size;
    unsigned int nr_pages;
    unsigned char *info;
};
static int max_fds;
static struct fadv_info *fds;
static size_t PAGESIZE;
static pthread_mutex_t lock; /* protects access to fds[] */

static char *env_nr_fadvise = "NOCACHE_NR_FADVISE";
static int nr_fadvise;

static void init(void)
{
    int i;
    char *s;
    char *error;
    struct rlimit rlim;
    
    getrlimit(RLIMIT_NOFILE, &rlim);
    max_fds=(int) rlim.rlim_max;
    
    fds=(struct fadv_info *) malloc(max_fds * sizeof(struct fadv_info));
    
    assert(fds != NULL);
    
    _original_open = (int (*)(const char *, int, mode_t))
        dlsym(RTLD_NEXT, "open");
    _original_creat = (int (*)(const char *, int, mode_t))
        dlsym(RTLD_NEXT, "creat");
    _original_openat = (int (*)(int, const char *, int, mode_t))
        dlsym(RTLD_NEXT, "openat");
    _original_dup = (int (*)(int)) dlsym(RTLD_NEXT, "dup");
    _original_dup2 = (int (*)(int, int)) dlsym(RTLD_NEXT, "dup2");
    _original_close = (int (*)(int)) dlsym(RTLD_NEXT, "close");
    _original_fopen = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
    _original_fclose = (int (*)(FILE *)) dlsym(RTLD_NEXT, "fclose");

    if ((error = dlerror()) != NULL)  {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }

    if((s = getenv(env_nr_fadvise)) != NULL)
        nr_fadvise = atoi(s);
    if(nr_fadvise <= 0)
        nr_fadvise = 1;

    PAGESIZE = getpagesize();
    for(i = 0; i < max_fds; i++)
        fds[i].fd = -1;
    init_mutex();
    handle_stdout();
}

static void init_mutex(void)
{
    pthread_mutex_init(&lock, NULL);
    /* make sure to re-initialize mutex if forked */
    pthread_atfork(NULL, NULL, init_mutex);
}

/* duplicate stdout if it is a regular file. We will use this later to
 * fadvise(DONTNEED) on it, although the real stdout was already
 * closed. This makes "nocache tar cfz" work properly. */
static void handle_stdout(void)
{
    int fd;
    struct stat st;

    if(fstat(STDOUT_FILENO, &st) == -1 || !S_ISREG(st.st_mode))
        return;

    fd = fcntl_dupfd(STDOUT_FILENO, 23);
    if(fd == -1)
        return;
    store_pageinfo(fd);
}

/* try to advise fds that were not manually closed */
static void destroy(void)
{
    int i;
    pthread_mutex_lock(&lock);
    for(i = 0; i < max_fds; i++) {
        if(fds[i].fd == -1)
            continue; /* slot is empty */
        if(!valid_fd(fds[i].fd))
            continue;
        pthread_mutex_unlock(&lock);
        free_unclaimed_pages(fds[i].fd);
        pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
}

int open(const char *pathname, int flags, mode_t mode)
{
    int fd;
    if((fd = _original_open(pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int creat(const char *pathname, int flags, mode_t mode)
{
    int fd;
    if((fd = _original_creat(pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int openat(int dirfd, const char *pathname, int flags, mode_t mode)
{
    int fd;
    if((fd = _original_openat(dirfd, pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int dup(int oldfd)
{
    int fd;
    if((fd = _original_dup(oldfd)) != -1)
        store_pageinfo(fd);
    return fd;
}

int dup2(int oldfd, int newfd)
{
    int ret;

    /* if newfd is already opened, the kernel will close it directly
     * once dup2 is invoked. So now is the last chance to mark the
     * pages as "DONTNEED" */
    if(valid_fd(newfd))
        free_unclaimed_pages(newfd);

    if((ret = _original_dup2(oldfd, newfd)) != -1)
        store_pageinfo(newfd);
    return ret;
}

int close(int fd)
{
    free_unclaimed_pages(fd);
    return _original_close(fd);
}

FILE *fopen(const char *path, const char *mode)
{
    int fd;
    FILE *fp = NULL;

    if(!_original_fopen)
       _original_fopen = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");

    if(_original_fopen) {
        if((fp = _original_fopen(path, mode)) != NULL)
            if((fd = fileno(fp)) != -1)
                store_pageinfo(fd);
    }

    return fp;
}

int fclose(FILE *fp)
{
    if(!_original_fclose)
        _original_fclose = (int (*)(FILE *)) dlsym(RTLD_NEXT, "fclose");

    if(_original_fclose) {
       free_unclaimed_pages(fileno(fp));
       return _original_fclose(fp);
    }

    errno = EFAULT;
    return EOF;
}

static void store_pageinfo(int fd)
{
    int i;
    int pages;
    struct stat st;
    void *file = NULL;
    unsigned char *pageinfo = NULL;

    if(fstat(fd, &st) == -1 || !S_ISREG(st.st_mode))
        return;

    /* Hint we'll be using this file only once;
     * the Linux kernel will currently ignore this */
    fadv_noreuse(fd, 0, 0);

    /* check if there's space to store the info */
    pthread_mutex_lock(&lock);
    for(i = 0; i < max_fds && fds[i].fd != -1; i++)
        ;
    if(i == max_fds) {
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
    for(i = 0; i < max_fds; i++)
        if(fds[i].fd == fd)
            break;
    pthread_mutex_unlock(&lock);
    if(i == max_fds)
        return; /* not found */

    sync_if_writable(fd);

    start = j = 0;
    while(j < fds[i].nr_pages) {
        if(fds[i].info[j] & 1) {
            if(start < j)
                fadv_dontneed(fd, start*PAGESIZE, (j - start) * PAGESIZE, nr_fadvise);
            start = j + 1;
        }
        j++;
    }

    /* forget written contents that go beyond previous file size */
    fadv_dontneed(fd, start < j ? start*PAGESIZE : fds[i].size, 0, nr_fadvise);

    free(fds[i].info);
    fds[i].fd = -1;
}
