#include <fcntl.h>
#include <unistd.h>

/* Since open() and close() are re-defined in nocache.c, it's not
 * possible to include <fcntl.h> there. So we do it here. */

int fadv_dontneed(int fd, off_t offset, off_t len)
{
        return posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED);
}

void sync_if_writable(int fd)
{
    int r;
    if((r = fcntl(fd, F_GETFL)) == -1)
        return;
    if((r & O_ACCMODE) != O_RDONLY)
        fdatasync(fd);
}
