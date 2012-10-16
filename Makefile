default: all
all: cachestats cachedel nocache.so
.PHONY: test

GCC = gcc $(CFLAGS)
%.c: Makefile
cachestats: cachestats.c
	$(GCC) -Wall -o cachestats cachestats.c
cachedel: cachedel.c
	$(GCC) -Wall -o cachedel cachedel.c
nocache.o: nocache.c
	$(GCC) -Wall -fPIC -c -o nocache.o nocache.c
fcntl_helpers.o: fcntl_helpers.c
	$(GCC) -Wall -fPIC -c -o fcntl_helpers.o fcntl_helpers.c
nocache.so: nocache.o fcntl_helpers.o
	$(GCC) -Wall -pthread -shared -Wl,-soname,nocache.so -o nocache.so nocache.o fcntl_helpers.o -ldl

install: all
	install -m 0644 nocache.so /usr/local/lib
	install -m 0755 nocache.global /usr/local/bin/nocache

test:
	prove -v t

clean:
	rm -f cachestats cachedel fcntl_helpers.o nocache.o nocache.so
