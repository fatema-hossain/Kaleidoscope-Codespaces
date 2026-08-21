#!/usr/bin/env bash
set -euo pipefail
BIN="$1"
OUT=$(printf '1 + 2 * 3\nprint(42)\ndef add(a b) a + b\nadd(10, 20)\nif 1 then 10 else 20\nif 0 then 10 else 20\n' | "$BIN" 2>&1)

echo "$OUT"
grep -q '=> 7' <<<"$OUT"
grep -q '42' <<<"$OUT"
grep -q '=> 30' <<<"$OUT"
grep -q '=> 10' <<<"$OUT"
echo 'All smoke tests passed.'
