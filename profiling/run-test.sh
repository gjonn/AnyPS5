#!/usr/bin/env bash
# usage: profiling/run-test.sh <log-name> <test-exe-name> [args...]
set -u
root=/c/repos/AnyPS5-pr639
name=$1; shift
exe=$(find "$root/build" -name "$1.exe" -path '*tests*' | head -1); shift
export PATH="$root/build/core/libs/libs/unpatched:/c/tools/mingw64/bin:$root/build/core/libs/libs:$PATH"
log="$root/profiling/test-$name.txt"
start=$(date +%s)
timeout 1200 "$exe" "$@" > "$log" 2>&1
code=$?
echo "exit $code after $(( $(date +%s) - start )) s" >> "$log"
echo "== $name: exit $code ($(( $(date +%s) - start )) s)"
tail -4 "$log"
