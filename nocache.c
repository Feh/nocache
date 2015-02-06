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
#include <signal.h>

#include "pageinfo.h"
#include "fcntl_helpers.h"

static void init(void) __attribute__((constructor));
static void destroy(void) __attribute__((destructor));
static void init_mutex(void);
static void init_debugging(void);
static void handle_stdout(void);

static void store_pageinfo(int fd);
static void free_unclaimed_pages(int fd);

int open(const char *pathname, int flags, mode_t mode);
int open64(const char *pathname, int flags, mode_t mode);
int creat(const char *pathname, int flags, mode_t mode);
int creat64(const char *pathname, int flags, mode_t mode);
int openat(int dirfd, const char *pathname, int flags, mode_t mode);
int openat64(int dirfd, const char *pathname, int flags, mode_t mode);
int __openat_2(int dirfd, const char *pathname, int flags, mode_t mode)
    __attribute__ ((alias ("openat")));
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int close(int fd);
FILE *fopen(const char *path, const char *mode);
FILE *fopen64(const char *path, const char *mode);
int fclose(FILE *fp);

int (*_original_open)(const char *pathname, int flags, mode_t mode);
int (*_original_open64)(const char *pathname, int flags, mode_t mode);
int (*_original_creat)(const char *pathname, int flags, mode_t mode);
int (*_original_creat64)(const char *pathname, int flags, mode_t mode);
int (*_original_openat)(int dirfd, const char *pathname, int flags, mode_t mode);
int (*_original_openat64)(int dirfd, const char *pathname, int flags, mode_t mode);
int (*_original_dup)(int fd);
int (*_original_dup2)(int newfd, int oldfd);
int (*_original_close)(int fd);
FILE *(*_original_fopen)(const char *path, const char *mode);
FILE *(*_original_fopen64)(const char *path, const char *mode);
int (*_original_fclose)(FILE *fp);


static int max_fds;
static struct file_pageinfo *fds;
static size_t PAGESIZE;
static pthread_mutex_t lock; /* protects access to fds[] */

static char *env_nr_fadvise = "NOCACHE_NR_FADVISE";
static int nr_fadvise;

static char *env_debugfd = "NOCACHE_DEBUGFD";
int debugfd = -1;
FILE *debugfp;

#define DEBUG(...) \
    do { \
        if(debugfp != NULL) { \
            fprintf(debugfp, "[nocache] DEBUG: " __VA_ARGS__); \
        } \
    } while(0)

static void init(void)
{
    int i;
    char *s;
    char *error;
    struct rlimit rlim;

    getrlimit(RLIMIT_NOFILE, &rlim);
    max_fds = rlim.rlim_max;
    fds = malloc(max_fds * sizeof(*fds));

    assert(fds != NULL);

    _original_open = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "open");
    _original_open64 = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "open64");
    _original_creat = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "creat");
    _original_creat64 = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "creat64");
    _original_openat = (int (*)(int, const char *, int, mode_t)) dlsym(RTLD_NEXT, "openat");
    _original_openat64 = (int (*)(int, const char *, int, mode_t)) dlsym(RTLD_NEXT, "openat64");
    _original_dup = (int (*)(int)) dlsym(RTLD_NEXT, "dup");
    _original_dup2 = (int (*)(int, int)) dlsym(RTLD_NEXT, "dup2");
    _original_close = (int (*)(int)) dlsym(RTLD_NEXT, "close");
    _original_fopen = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
    _original_fopen64 = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen64");
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
    init_debugging();
    handle_stdout();
}

static void init_mutex(void)
{
    pthread_mutex_init(&lock, NULL);
    /* make sure to re-initialize mutex if forked */
    pthread_atfork(NULL, NULL, init_mutex);
}

static void init_debugging(void)
{
    char *s = getenv(env_debugfd);
    if(!s)
        return;
    debugfd = atoi(s);
    debugfp = fdopen(debugfd, "a");
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
    free(fds);
}

int open(const char *pathname, int flags, mode_t mode)
{
    int fd;

    if(!_original_open)
        _original_open = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "open");
    assert(_original_open != NULL);

    if((fd = _original_open(pathname, flags, mode)) != -1) {
        DEBUG("open(pathname=%s, flags=0x%x, mode=0%o) = %d\n",
            pathname, flags, mode, fd);
        store_pageinfo(fd);
    }
    return fd;
}

int open64(const char *pathname, int flags, mode_t mode)
{
    int fd;

    if(!_original_open64)
        _original_open64 = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "open64");
    assert(_original_open64 != NULL);

    DEBUG("open64(pathname=%s, flags=0x%x, mode=0%o)\n", pathname, flags, mode);

    if((fd = _original_open64(pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int creat(const char *pathname, int flags, mode_t mode)
{
    int fd;

    if(!_original_creat)
        _original_creat = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "creat");
    assert(_original_creat != NULL);

    DEBUG("creat(pathname=%s, flags=0x%x, mode=0%o)\n", pathname, flags, mode);

    if((fd = _original_creat(pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int creat64(const char *pathname, int flags, mode_t mode)
{
    int fd;

    if(!_original_creat64)
        _original_creat64 = (int (*)(const char *, int, mode_t)) dlsym(RTLD_NEXT, "creat64");
    assert(_original_creat64 != NULL);

    DEBUG("creat64(pathname=%s, flags=0x%x, mode=0%o)\n", pathname, flags, mode);

    if((fd = _original_creat64(pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int openat(int dirfd, const char *pathname, int flags, mode_t mode)
{
    int fd;

    if(!_original_openat)
        _original_openat = (int (*)(int, const char *, int, mode_t)) dlsym(RTLD_NEXT, "openat");
    assert(_original_openat != NULL);

    DEBUG("openat(dirfd=%d, pathname=%s, flags=0x%x, mode=0%o)\n", dirfd, pathname, flags, mode);

    if((fd = _original_openat(dirfd, pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int openat64(int dirfd, const char *pathname, int flags, mode_t mode)
{
    int fd;

    if(!_original_openat64)
        _original_openat64 = (int (*)(int, const char *, int, mode_t)) dlsym(RTLD_NEXT, "openat64");
    assert(_original_openat64 != NULL);

    DEBUG("openat64(dirfd=%d, pathname=%s, flags=0x%x, mode=0%o)\n", dirfd, pathname, flags, mode);

    if((fd = _original_openat64(dirfd, pathname, flags, mode)) != -1)
        store_pageinfo(fd);
    return fd;
}

int dup(int oldfd)
{
    int fd;

    if(!_original_dup)
        _original_dup = (int (*)(int)) dlsym(RTLD_NEXT, "dup");
    assert(_original_dup != NULL);

    DEBUG("dup(oldfd=%d)\n", oldfd);

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

    if(!_original_dup2)
        _original_dup2 = (int (*)(int, int)) dlsym(RTLD_NEXT, "dup2");
    assert(_original_dup2 != NULL);

    DEBUG("dup2(oldfd=%d, newfd=%d)\n", oldfd, newfd);

    if((ret = _original_dup2(oldfd, newfd)) != -1)
        store_pageinfo(newfd);
    return ret;
}

int close(int fd)
{
    if(!_original_close)
        _original_close = (int (*)(int)) dlsym(RTLD_NEXT, "close");
    assert(_original_close != NULL);

    free_unclaimed_pages(fd);

    DEBUG("close(%d)\n", fd);
    return _original_close(fd);
}

FILE *fopen(const char *path, const char *mode)
{
    int fd;
    FILE *fp = NULL;

    if(!_original_fopen)
       _original_fopen = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen");
    assert(_original_fopen != NULL);

    DEBUG("fopen(path=%s, mode=%s)\n", path, mode);

    if((fp = _original_fopen(path, mode)) != NULL)
        if((fd = fileno(fp)) != -1)
            store_pageinfo(fd);

    return fp;
}

FILE *fopen64(const char *path, const char *mode)
{
    int fd;
    FILE *fp;
    fp = NULL;

    if(!_original_fopen64)
        _original_fopen64 = (FILE *(*)(const char *, const char *)) dlsym(RTLD_NEXT, "fopen64");
    assert(_original_fopen64 != NULL);

    DEBUG("fopen64(path=%s, mode=%s)\n", path, mode);

    if((fp = _original_fopen64(path, mode)) != NULL)
        if((fd = fileno(fp)) != -1)
            store_pageinfo(fd);

    return fp;
}

int fclose(FILE *fp)
{
    if(!_original_fclose)
        _original_fclose = (int (*)(FILE *)) dlsym(RTLD_NEXT, "fclose");
    assert(_original_fclose != NULL);

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
    sigset_t mask, old_mask;

    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);

    /* Hint we'll be using this file only once;
     * the Linux kernel will currently ignore this */
    fadv_noreuse(fd, 0, 0);

    /* check if there's space to store the info */
    pthread_mutex_lock(&lock);
    for(i = 0; i < max_fds && fds[i].fd != -1; i++)
        ;
    if(i == max_fds) {
        pthread_mutex_unlock(&lock);
        goto restoresigset; /* no space! */
    }
    fds[i].fd = fd;
    pthread_mutex_unlock(&lock);

    if(!fd_get_pageinfo(fd, &fds[i]))
        goto cleanup;

    DEBUG("store_pageinfo(fd=%d): pages in cache: %zd/%zd (%.1f%%)  [filesize=%.1fK, "
            "pagesize=%dK]\n", fd, fds[i].nr_pages_cached, fds[i].nr_pages,
             fds[i].nr_pages == 0 ? 0 : (100.0 * fds[i].nr_pages_cached / fds[i].nr_pages),
             1.0 * fds[i].size / 1024, (int) PAGESIZE / 1024);
    goto restoresigset;

    cleanup:
    fds[i].fd = -1;

    restoresigset:
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    return;
}

static void free_unclaimed_pages(int fd)
{
    int i;
    struct stat st;
    sigset_t mask, old_mask;

    if(fd == -1)
        return;

    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);

    pthread_mutex_lock(&lock);
    for(i = 0; i < max_fds; i++)
        if(fds[i].fd == fd)
            break;
    pthread_mutex_unlock(&lock);
    if(i == max_fds)
        goto restoresigset; /* not found */

    sync_if_writable(fd);

    if(fstat(fd, &st) == -1)
        goto restoresigset;

    struct byterange *br;
    for(br = fds[i].unmapped; br; br = br->next) {
        DEBUG("fadv_dontneed(fd=%d, from=%zd, len=%zd)\n", fd, br->pos, br->len);
        fadv_dontneed(fd, br->pos, br->len, nr_fadvise);
    }

    /* Has the file grown bigger? */
    if(st.st_size > fds[i].size) {
        DEBUG("fadv_dontneed(fd=%d, from=%zd, len=0 [till new end, file has grown])\n",
                fd, fds[i].size);
        fadv_dontneed(fd, fds[i].size, 0, nr_fadvise);
    }

    free_br_list(&fds[i].unmapped);
    fds[i].fd = -1;

    restoresigset:
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
}
