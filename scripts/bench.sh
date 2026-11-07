#!/usr/bin/env bash
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
csyc="${CSYC:-$root/csyc}"
dir="$root/tests/bench"
runs="${RUNS:-3}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export CSY_JIT_CACHE_DIR="$work/cache"

now() { date +%s%N; }

best_ms() {
    local best=-1 i t0 t1 ms
    for i in $(seq "$runs"); do
        t0=$(now)
        "$@" >/dev/null 2>&1
        t1=$(now)
        ms=$(( (t1 - t0) / 1000000 ))
        if [ "$best" -lt 0 ] || [ "$ms" -lt "$best" ]; then best=$ms; fi
    done
    echo "$best"
}

prime() {
    "$csyc" "$1" >/dev/null 2>&1
    CSY_NATIVE_BACKEND=c "$csyc" "$1" >/dev/null 2>&1
    CSY_NATIVE_BACKEND=x64 "$csyc" "$1" >/dev/null 2>&1
}

printf '%-12s %8s %8s %8s %8s %8s\n' case vm c x64 auto cpp
for src in "$dir"/*.csy; do
    name="$(basename "$src" .csy)"
    prime "$src"
    vm=$(CSY_NO_NATIVE_JIT=1 best_ms "$csyc" "$src")
    c=$(CSY_NATIVE_BACKEND=c best_ms "$csyc" "$src")
    x=$(CSY_NATIVE_BACKEND=x64 best_ms "$csyc" "$src")
    a=$(best_ms "$csyc" "$src")
    cpp="-"
    if [ -f "$dir/cpp/$name.cpp" ]; then
        g++ -O2 -o "$work/$name" "$dir/cpp/$name.cpp" && cpp=$(best_ms "$work/$name")
    fi
    printf '%-12s %8s %8s %8s %8s %8s\n' "$name" "$vm" "$c" "$x" "$a" "$cpp"
done
