#!/bin/sh

NR=0

. ./testlib.sh

echo 1..1

t "ls / >/dev/null" "ls works"
