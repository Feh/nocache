#!/bin/sh

NR=0

. ./testlib.sh

echo 1..5

t "echo test > testfile.$$ && ../cachestats -q testfile.$$" "file is cached"
t "while ../cachestats -q testfile.$$; do ../cachedel testfile.$$ && sleep 1; done" "file is not cached any more"
t "env NOCACHE_MAX_FDS=3 LD_PRELOAD=../nocache.so cat testfile.$$ >/dev/null && ../cachestats -q testfile.$$" "file is in cache because it has an FD >= 3"
t "while ../cachestats -q testfile.$$; do ../cachedel testfile.$$ && sleep 1; done" "file is not cached any more"
t "env NOCACHE_MAX_FDS=4 LD_PRELOAD=../nocache.so cat testfile.$$ >/dev/null && ! ../cachestats -q testfile.$$" "file is not in cache because it has an FD < 4"

# clean up
rm -f testfile.$$
