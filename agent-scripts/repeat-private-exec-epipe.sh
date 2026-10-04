#!/bin/sh
set -eu

count="${1:-50}"
i=1
while [ "$i" -le "$count" ]; do
    echo "private-exec-epipe run $i/$count"
    env LMUX_SUP_IT_ONLY_PRIVATE_EXEC_EPIPE=1 build/host/test_supervisor_integration
    i=$((i + 1))
done
