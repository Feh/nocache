#!/bin/sh
#set -x

NR=0

t() {
    NR=$(expr $NR + 1)
    if eval "$1"; then
        echo "ok $NR - $2"
    else
        echo "not okay $NR - $2"
    fi
}

if [ "$(basename "$(readlink -f .)")" != "t" ]; then
    cd t || exit 1;
fi

echo 1..4

t "echo test > testfile.$$ && ../cachestats -q testfile.$$" "file is cached"
t "while ../cachestats -q testfile.$$; do ../cachedel testfile.$$ && sleep 1; done" "file is not cached any more"
t "! ( env LD_PRELOAD=../nocache.so cat testfile.$$ >/dev/null && ../cachestats -q testfile.$$ )" "file is still not in cache"
t "! ( env LD_PRELOAD=../nocache.so cp testfile.$$ testfile.$$.2 && ../cachestats -q testfile.$$.2 )" "copy of file is not cached"

# clean up
rm -f testfile.$$ testfile.$$.2
