#ifndef _FCNTL_HELPERS_H
#define _FCNTL_HELPERS_H
extern int fadv_dontneed(int fd, off_t offset, off_t len, int n);
extern int fadv_noreuse(int fd, off_t offset, off_t len);
extern int valid_fd(int fd);
extern void sync_if_writable(int fd);
extern int fcntl_dupfd(int fd, int arg);
#endif
