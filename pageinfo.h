struct byterange {
    size_t pos, len;
    struct byterange *next;
};

struct file_pageinfo {
    int fd;
    off_t size;
    size_t nr_pages;
    size_t nr_pages_cached;
    struct byterange *unmapped;
};

struct file_pageinfo *fd_get_pageinfo(int fd, struct file_pageinfo *pi);
void free_br_list(struct byterange **br);
