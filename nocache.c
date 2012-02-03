#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <stdio.h>

int (*_original_open)(const char *pathname, int flags, mode_t mode);
int (*_original_close)(int fd);

void init(void) __attribute__((constructor));
int open(const char *pathname, int flags, mode_t mode);
int close(int fd);

void init(void)
{
    _original_open = (int (*)(const char *, int, mode_t))
        dlsym(RTLD_NEXT, "open");
    _original_close = (int (*)(int)) dlsym(RTLD_NEXT, "close");
}

int open(const char *pathname, int flags, mode_t mode)
{
    fprintf(stderr, "I intercepted the open() call!\n");
    return _original_open(pathname, flags, mode);
}

int close(int fd)
{
    fprintf(stderr, "I intercepted the close() call!\n");
    return _original_close(fd);
}
