#include <fcntl.h>

/* Since open() and close() are re-defined in nocache.c, it's not
 * possible to include <fcntl.h> there. So we do it here. */

int fadv_dontneed(int fd, off_t offset, off_t len)
{
        return posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED);
}
