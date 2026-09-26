#!/usr/bin/env bash
# Build tools/rx-check and run its self-test. Given an obSCEne log of the colour-target layout
# arms, write the verdict beside this script:
#
#   tools/rx-check/build.sh <obscene>/reports/hardware/<sweep>-eboot.obs.log
#
# Needs a C compiler on PATH; the collection's WSL builder has clang.
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$ROOT/build/rx-check}
mkdir -p "$OUT"
${CC:-clang} -std=c11 -Wall -Wextra -Werror -O1 -I"$ROOT/include" -o "$OUT/rx_check" "$HERE/rx_check.c"
"$OUT/rx_check" --self-test
if [ $# -ge 1 ]; then
    # The 128 x 128 arms against their linear control: which CB_COLOR0_ATTRIB3 draws the
    # display's layout inside a block.
    "$OUT/rx_check" --log "$1" | tee "$HERE/rx_check_7e21.txt"
    # The 256 x 256 arm has no linear control, so the shape decides the order of the blocks.
    # A log without that arm skips this step.
    if grep -q 'multiblock-256' "$1" 2>/dev/null; then
        "$OUT/rx_check" --log "$1" --extent 256x256 --arm multiblock-256=0x08c6c000 \
            --shape multiblock-256 | tee "$HERE/rx_check_4b19.txt"
    fi
fi
