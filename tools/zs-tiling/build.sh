#!/usr/bin/env bash
# Build tools/zs-tiling against the pinned Mesa's AddressLib (oops-mesa's submodule), run it, and
# write zs_tiling_gfx1013.txt next to this script. The same build as oops-mesa's
# tools/tiling-compare, which this is a sibling of: AddressLib's own source list and flags.
#
# Needs clang++ on PATH; the collection's WSL builder has it.
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OOPS_MESA=${OOPS_MESA:-$ROOT/../oops-mesa}
MESA=$OOPS_MESA/mesa
ADDR=$MESA/src/amd/addrlib
OUT=${OUT:-$ROOT/build/zs-tiling}
mkdir -p "$OUT"

if [ ! -f "$ADDR/src/addrinterface.cpp" ]; then
    echo "zs-tiling: no Mesa at $MESA (set OOPS_MESA, and init its submodule)" >&2
    exit 1
fi

SRCS="
$ADDR/src/addrinterface.cpp
$ADDR/src/core/addrelemlib.cpp
$ADDR/src/core/addrlib.cpp
$ADDR/src/core/addrlib1.cpp
$ADDR/src/core/addrlib2.cpp
$ADDR/src/core/addrlib3.cpp
$ADDR/src/core/addrobject.cpp
$ADDR/src/core/addrswizzler.cpp
$ADDR/src/core/coord.cpp
$ADDR/src/gfx9/gfx9addrlib.cpp
$ADDR/src/gfx10/gfx10addrlib.cpp
$ADDR/src/gfx11/gfx11addrlib.cpp
$ADDR/src/gfx12/gfx12addrlib.cpp
$ADDR/src/r800/ciaddrlib.cpp
$ADDR/src/r800/egbaddrlib.cpp
$ADDR/src/r800/siaddrlib.cpp
"
INCS="
-I$ADDR/inc
-I$ADDR/src
-I$ADDR/src/core
-I$ADDR/src/chip/gfx9
-I$ADDR/src/chip/gfx10
-I$ADDR/src/chip/gfx11
-I$ADDR/src/chip/gfx12
-I$ADDR/src/chip/r800
-I$MESA/src/amd/common
-I$MESA/src
-I$MESA/include
"
DEFS="-DADDR_FASTCALL= -DLITTLEENDIAN_CPU -DADDR_ALLOW_SIMD=1 -DDEBUG=0"
WARN="-Wno-unused-variable -Wno-unused-local-typedefs -Wno-unused-but-set-variable
      -Wno-self-assign -Wno-uninitialized -Wno-unused-private-field -Wno-missing-braces"

# shellcheck disable=SC2086
clang++ -std=c++17 -O1 $DEFS $WARN $INCS "$HERE/zs_tiling.cpp" $SRCS -o "$OUT/zs-tiling"
"$OUT/zs-tiling" > "$OUT/zs_tiling_gfx1013.txt"
if [ "${1:-}" = "--check" ]; then
    cmp -s "$OUT/zs_tiling_gfx1013.txt" "$HERE/zs_tiling_gfx1013.txt" ||
        { echo "zs-tiling: tracked output differs from a fresh run; run $0" >&2; exit 1; }
    echo "zs-tiling: tracked output matches the pinned Mesa"
else
    cp "$OUT/zs_tiling_gfx1013.txt" "$HERE/"
    cat "$HERE/zs_tiling_gfx1013.txt"
fi
