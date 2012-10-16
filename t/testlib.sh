t() {
    NR=$(expr $NR + 1)
    if eval "$1"; then
        echo "ok $NR - $2"
    else
        echo "not okay $NR - $2"
    fi
}
