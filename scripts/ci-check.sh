#!/bin/sh

set -eu

printf '%s\n' '==> shell syntax and whitespace checks'
sh -n start-host.sh
sh -n lmux-start-target.sh
sh -n scripts/gen-version.sh
git diff --check

printf '%s\n' '==> host build and fast software tests'
make host
effective_version=$(sed -n '1p' build/generated/version.txt)
test "$(build/host/lmux --version)" = \
    "lmux $effective_version (LMUX Wire)"
test "$(build/host/lmuxd --version)" = \
    "lmuxd $effective_version (LMUX Wire)"
test "$(build/host/lmux-uart-dispatch --version)" = \
    "lmux-uart-dispatch $effective_version (LMUX Wire)"
test "$(build/host/lmux-mcp --version)" = \
    "lmux-mcp $effective_version (LMUX Wire)"
make test-unit
make test-carrier
make test-structured-dirty
make test-usb-regression

printf '%s\n' '==> ARM soft-float static build'
make target
make strip-target

for binary in \
    build/target/lmuxd \
    build/target/lmux \
    build/target/lmux-uart-dispatch
do
    grep -a -F -q "$effective_version" "$binary"
done

target_readelf="${CROSS_COMPILE:-arm-linux-gnueabi-}readelf"
for binary in \
    build/target/lmuxd \
    build/target/lmux \
    build/target/lmux-uart-dispatch
do
    description=$(file -b "$binary")
    case "$description" in
        *"ELF 32-bit LSB executable"*"ARM"*"statically linked"*"stripped"*)
            ;;
        *)
            printf 'unexpected target artifact: %s: %s\n' \
                "$binary" "$description" >&2
            exit 1
            ;;
    esac

    elf_header=$("$target_readelf" -h "$binary")
    case "$elf_header" in
        *"soft-float ABI"*)
            ;;
        *)
            printf 'target artifact is not marked soft-float: %s\n' \
                "$binary" >&2
            exit 1
            ;;
    esac
done

printf '%s\n' 'CI checks passed'
