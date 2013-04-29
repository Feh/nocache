#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* Since open() and close() are re-defined in nocache.c, it's not
 * possible to include <fcntl.h> there. So we do it here. */

int fadv_dontneed(int fd, off_t offset, off_t len, int n)
{
        int i, ret;
        for(i = 0, ret = 0; i < n && ret == 0; i++)
            ret = posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED);
        return ret;
}

int fadv_noreuse(int fd, off_t offset, off_t len)
{
        return posix_fadvise(fd, offset, len, POSIX_FADV_NOREUSE);
}

int valid_fd(int fd)
{
    /* will return 1 if fd is opened */
    return fcntl(fd, F_GETFL) != -1 || errno != EBADF;
}

void sync_if_writable(int fd)
{
    int r;
    if((r = fcntl(fd, F_GETFL)) == -1)
        return;
    if((r & O_ACCMODE) != O_RDONLY)
        fdatasync(fd);
}

int fcntl_dupfd(int fd, int arg)
{
    return fcntl(fd, F_DUPFD, arg);
}
