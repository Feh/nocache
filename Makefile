default:
	gcc -Wall -fPIC -c -o nocache.o nocache.c
	gcc -Wall -fPIC -c -o fadv_dontneed.o fadv_dontneed.c
	gcc -Wall -shared -Wl,-soname,nocache.so.1 -ldl -o nocache.so nocache.o fadv_dontneed.o
