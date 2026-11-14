#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds mp3toxwm with the system C compiler. Needs C23 support: gcc 13+ or clang 19+.
set -e
cd "$(dirname "$0")"

CC_BIN="${CC:-}"
if [ -z "$CC_BIN" ]; then
    for c in cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then CC_BIN="$c"; break; fi
    done
fi
if [ -z "$CC_BIN" ]; then
    echo "No C compiler found. Install gcc 13+ or clang 19+." >&2
    exit 1
fi

# gcc 13 knows C23 as c2x.
STD=""
for s in c23 c2x; do
    if echo 'int main(void){constexpr int x = 1; int *p = nullptr; return x - 1 + (p != nullptr);}' \
        | "$CC_BIN" -std=$s -x c -o /dev/null - 2>/dev/null; then STD=$s; break; fi
done
if [ -z "$STD" ]; then
    echo "$CC_BIN does not support C23. Install gcc 13+ or clang 19+, or set CC." >&2
    exit 1
fi

mkdir -p build
"$CC_BIN" -std=$STD -O2 -Wall -Wextra -o build/mp3toxwm src/*.c -lm
echo "Built build/mp3toxwm"
