#!/usr/bin/env bash
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
csyc="${CSYC:-$root/csyc}"
cases="${1:-$root/tests/cases}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export CSY_JIT_CACHE_DIR="$work/cache"
modes=(vm c x64 auto)
fail=0
total=0

run_mode() {
    local mode="$1" src="$2" out="$3"
    case "$mode" in
        vm) env CSY_NO_NATIVE_JIT=1 "$csyc" "$src" >"$out.out" 2>"$out.err" ;;
        c) env CSY_NATIVE_BACKEND=c "$csyc" "$src" >"$out.out" 2>"$out.err" ;;
        x64) env CSY_NATIVE_BACKEND=x64 "$csyc" "$src" >"$out.out" 2>"$out.err" ;;
        auto) "$csyc" "$src" >"$out.out" 2>"$out.err" ;;
    esac
    echo $? >"$out.rc"
    grep -v '^\[csy\]' "$out.err" | sed -E 's/^ *(.*\.csy):[0-9]+:[0-9]+/\1/' >"$out.errn"
}

for src in "$cases"/*.csy; do
    name="$(basename "$src" .csy)"
    total=$((total + 1))
    for m in "${modes[@]}"; do run_mode "$m" "$src" "$work/$name.$m"; done
    bad=""
    for m in c x64 auto; do
        cmp -s "$work/$name.vm.out" "$work/$name.$m.out" || bad="$bad $m:stdout"
        cmp -s "$work/$name.vm.rc" "$work/$name.$m.rc" || bad="$bad $m:status"
        cmp -s "$work/$name.vm.errn" "$work/$name.$m.errn" || bad="$bad $m:stderr"
    done
    expected="$cases/$name.expected"
    if [ -f "$expected" ]; then
        for m in "${modes[@]}"; do
            cmp -s "$expected" "$work/$name.$m.out" || bad="$bad $m:expected"
        done
    fi
    if [ -z "$bad" ]; then
        printf 'ok    %s\n' "$name"
    else
        printf 'DIFF  %s (%s )\n' "$name" "${bad# }"
        fail=$((fail + 1))
    fi
done

printf '%d case(s), %d differing\n' "$total" "$fail"
[ "$fail" -eq 0 ]
