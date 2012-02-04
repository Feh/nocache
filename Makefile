default:
	gcc -Wall -fPIC -c -o nocache.o nocache.c
	gcc -Wall -fPIC -c -o fcntl_helpers.o fcntl_helpers.c
	gcc -Wall -shared -Wl,-soname,nocache.so.1 -ldl -o nocache.so nocache.o fcntl_helpers.o
