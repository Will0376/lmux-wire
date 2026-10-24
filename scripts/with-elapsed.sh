#!/bin/sh

if [ "$#" -lt 2 ]; then
    echo "usage: $0 LABEL COMMAND [ARG...]" >&2
    exit 2
fi

label=$1
shift
start=$(date +%s) || exit 2

"$@"
status=$?

end=$(date +%s) || exit 2
elapsed=$((end - start))
minutes=$((elapsed / 60))
seconds=$((elapsed % 60))

if [ "$status" -eq 0 ]; then
    printf '%s: PASS (elapsed %dm %ds; %ds)\n' \
        "$label" "$minutes" "$seconds" "$elapsed"
else
    printf '%s: FAIL exit=%d (elapsed %dm %ds; %ds)\n' \
        "$label" "$status" "$minutes" "$seconds" "$elapsed" >&2
fi

exit "$status"
