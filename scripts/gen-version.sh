#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd -P) || exit 1
ROOT_DIR=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd -P) || exit 1

cd "$ROOT_DIR" || exit 1

base=$(sed -n '1p' VERSION | tr -d ' \t\r\n')
case "$base" in
'')
echo "empty VERSION" >&2
exit 1
;;
esac

numfile=.build-number
old=0

if [ -f "$numfile" ]; then
old=$(sed -n '1p' "$numfile" | tr -d ' \t\r\n')
fi

case "$old" in
''|*[!0-9]*)
old=0
;;
esac

new=$((old + 1))
version="$base+build.$new"

mkdir -p build/generated

printf '%s\n' "$new" >"$numfile.tmp"
mv "$numfile.tmp" "$numfile"

cat >build/generated/version.h.tmp <<EOF
#ifndef LMUX_GENERATED_VERSION_H
#define LMUX_GENERATED_VERSION_H

#define LMUX_BUILD_VERSION "$version"
#define LMUX_BUILD_BASE_VERSION "$base"
#define LMUX_BUILD_NUMBER $new

#endif
EOF

mv build/generated/version.h.tmp build/generated/version.h
printf '%s\n' "$version" >build/generated/version.txt
