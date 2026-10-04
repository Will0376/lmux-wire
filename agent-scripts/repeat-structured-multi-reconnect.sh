#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
count="${1:-1}"
cd "$root"

for i in $(seq 1 "$count"); do
  echo "structured-multi-reconnect run $i/$count"
  env LMUX_SUP_IT_ONLY_STRUCTURED_MULTI_RECONNECT=1 build/host/test_supervisor_integration
done
