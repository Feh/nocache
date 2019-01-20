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
static void init_mutexes(void);
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


/* Info about a file descriptor 'fd' is stored in fds[fd]. Before accessing an
 * element, callers MUST both check fds_lock != NULL and then acquire
 * fds_lock[fd] while holding the fds_iter_lock. While any mutex in fds_lock is
 * held, fds_lock will not be freed and set to NULL. */
static int max_fds;
static struct file_pageinfo *fds;
static pthread_mutex_t *fds_lock;
static pthread_mutex_t fds_iter_lock;
static size_t PAGESIZE;

static char *env_nr_fadvise = "NOCACHE_NR_FADVISE";
static int nr_fadvise;

static char *env_debugfd = "NOCACHE_DEBUGFD";
int debugfd = -1;
FILE *debugfp;

static char *env_flushall = "NOCACHE_FLUSHALL";
static char flushall;

static char *env_max_fds = "NOCACHE_MAX_FDS";
static rlim_t max_fd_limit = 1 << 20;

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

    if((s = getenv(env_nr_fadvise)) != NULL)
        nr_fadvise = atoi(s);
    if(nr_fadvise <= 0)
        nr_fadvise = 1;

    if((s = getenv(env_flushall)) != NULL)
        flushall = atoi(s);
    if(flushall <= 0)
        flushall = 0;

    if((s = getenv(env_max_fds)) != NULL)
        max_fd_limit = atoll(s);

    getrlimit(RLIMIT_NOFILE, &rlim);
    max_fds = rlim.rlim_max;
    if(max_fds > max_fd_limit)
        max_fds = max_fd_limit;

    if(max_fds == 0)
        return;  /* There's nothing to do for us here. */

    init_mutexes();
    /* make sure to re-initialize mutex if forked */
    pthread_atfork(NULL, NULL, init_mutexes);

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

    PAGESIZE = getpagesize();
    pthread_mutex_lock(&fds_iter_lock);
    for(i = 0; i < max_fds; i++) {
        pthread_mutex_lock(&fds_lock[i]);
        fds[i].fd = -1;
        pthread_mutex_unlock(&fds_lock[i]);
    }
    pthread_mutex_unlock(&fds_iter_lock);
    init_debugging();
    handle_stdout();
}

static void init_mutexes(void)
{
    int i;
    pthread_mutex_init(&fds_iter_lock, NULL);
    pthread_mutex_lock(&fds_iter_lock);
    fds_lock = malloc(max_fds * sizeof(*fds_lock));
    assert(fds_lock != NULL);
    for(i = 0; i < max_fds; i++) {
        pthread_mutex_init(&fds_lock[i], NULL);
    }
    pthread_mutex_unlock(&fds_iter_lock);
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

    for(i = 0; i < max_fds; i++) {
        free_unclaimed_pages(i);
    }

    pthread_mutex_lock(&fds_iter_lock);
    if(fds_lock == NULL) {
        pthread_mutex_unlock(&fds_iter_lock);
        return;
    }
    for(i = 0; i < max_fds; i++) {
        pthread_mutex_lock(&fds_lock[i]);
    }
    /* We have acquired all locks. It is now safe to delete and free the list. */
    free(fds);
    fds = NULL;
    free(fds_lock);
    fds_lock = NULL;
    pthread_mutex_unlock(&fds_iter_lock);
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
    sigset_t mask, old_mask;

    if(fd >= max_fds - 1)
        return;

    /* We might know something about this fd already, so assume we have missed
     * it being closed. */
    free_unclaimed_pages(fd);

    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);

    pthread_mutex_lock(&fds_iter_lock);
    if(fds_lock == NULL) {
        pthread_mutex_unlock(&fds_iter_lock);
        return;
    }
    pthread_mutex_lock(&fds_lock[fd]);
    pthread_mutex_unlock(&fds_iter_lock);

    /* Hint we'll be using this file only once;
     * the Linux kernel will currently ignore this */
    fadv_noreuse(fd, 0, 0);

    fds[fd].fd = fd;
    if(flushall)
        goto out;

    if(!fd_get_pageinfo(fd, &fds[fd])) {
        fds[fd].fd = -1;
        goto out;
    }

    DEBUG("store_pageinfo(fd=%d): pages in cache: %zd/%zd (%.1f%%)  [filesize=%.1fK, "
            "pagesize=%dK]\n", fd, fds[fd].nr_pages_cached, fds[fd].nr_pages,
             fds[fd].nr_pages == 0 ? 0 : (100.0 * fds[fd].nr_pages_cached / fds[fd].nr_pages),
             1.0 * fds[fd].size / 1024, (int) PAGESIZE / 1024);

    out:
    pthread_mutex_unlock(&fds_lock[fd]);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    return;
}

static void free_unclaimed_pages(int fd)
{
    struct stat st;
    sigset_t mask, old_mask;

    if(fd == -1 || fd >= max_fds)
        return;

    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);

    pthread_mutex_lock(&fds_iter_lock);
    if(fds_lock == NULL) {
        pthread_mutex_unlock(&fds_iter_lock);
        return;
    }
    pthread_mutex_lock(&fds_lock[fd]);
    pthread_mutex_unlock(&fds_iter_lock);

    if(fds[fd].fd == -1)
        goto out;

    sync_if_writable(fd);

    if(flushall) {
        DEBUG("fadv_dontneed(fd=%d, from=0, len=0 [till end])\n", fd);
        fadv_dontneed(fd, 0, 0, nr_fadvise);
        fds[fd].fd = -1;
        goto out;
    }

    if(fstat(fd, &st) == -1)
        goto out;

    struct byterange *br;
    for(br = fds[fd].unmapped; br; br = br->next) {
        DEBUG("fadv_dontneed(fd=%d, from=%zd, len=%zd)\n", fd, br->pos, br->len);
        fadv_dontneed(fd, br->pos, br->len, nr_fadvise);
    }

    /* Has the file grown bigger? */
    if(st.st_size > fds[fd].size) {
        DEBUG("fadv_dontneed(fd=%d, from=%lld, len=0 [till new end, file has grown])\n",
              fd, (long long)fds[fd].size);
        fadv_dontneed(fd, fds[fd].size, 0, nr_fadvise);
    }

    free_br_list(&fds[fd].unmapped);
    fds[fd].fd = -1;

    out:
    pthread_mutex_unlock(&fds_lock[fd]);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
}

/* vim:set et sw=4 ts=4: */
