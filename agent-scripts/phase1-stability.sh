#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

agent-scripts/repeat-private-exec.sh 10
agent-scripts/repeat-structured-control.sh 10
agent-scripts/repeat-structured-reconnect.sh 5
agent-scripts/repeat-structured-worker.sh 5
agent-scripts/repeat-supervisor.sh 3
