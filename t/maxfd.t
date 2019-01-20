#!/bin/sh

NR=0

. ./testlib.sh

echo 1..3

t "echo test > testfile.$$ && ../cachestats -q testfile.$$" "file is cached"
t "while ../cachestats -q testfile.$$; do ../cachedel testfile.$$ && sleep 1; done" "file is not cached any more"
t "env NOCACHE_MAX_FDS=1 LD_PRELOAD=../nocache.so cat testfile.$$ >/dev/null && ../cachestats -q testfile.$$" "file is in cache because it has an FD > 1"

# clean up
rm -f testfile.$$
