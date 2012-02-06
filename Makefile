default: all
all: cachestats cachedel nocache.so

cachestats: cachestats.c
	gcc -Wall -o cachestats cachestats.c
cachedel: cachedel.c
	gcc -Wall -o cachedel cachedel.c
nocache.o: nocache.c
	gcc -Wall -fPIC -c -o nocache.o nocache.c
fcntl_helpers.o: fcntl_helpers.c
	gcc -Wall -fPIC -c -o fcntl_helpers.o fcntl_helpers.c
nocache.so: nocache.o fcntl_helpers.o
	gcc -Wall -shared -Wl,-soname,nocache.so -ldl -o nocache.so nocache.o fcntl_helpers.o

install: all
	install -m 0644 nocache.so /usr/local/lib
	install -m 0755 nocache.global /usr/local/bin/nocache

clean:
	rm -f cachestats cachedel fcntl_helpers.o nocache.o nocache.so
