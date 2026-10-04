#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
count="${1:-3}"
cd "$root"

for i in $(seq 1 "$count"); do
  echo "supervisor integration run $i/$count"
  build/host/test_supervisor_integration
done
