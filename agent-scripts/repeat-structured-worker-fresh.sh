#!/bin/sh
set -eu

count="${1:-30}"
i=1
while [ "$i" -le "$count" ]; do
    echo "structured-worker-fresh run $i/$count"
    env LMUX_SUP_IT_ONLY_STRUCTURED_WORKER_FRESH=1 build/host/test_supervisor_integration
    i=$((i + 1))
done
