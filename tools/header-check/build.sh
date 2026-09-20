#!/usr/bin/env bash
# header-check: compile this SDK's GL headers alone, and again beside a real `GL/glext.h`.
#
#   bash tools/header-check/build.sh
#
# Why this exists rather than a unit test: the mistakes it catches are not wrong *values*, they
# are declarations and macros that only conflict when two headers meet. A test that includes only
# our own header cannot see them, and the app that did see them found them by failing to build -
# see tools/header-check/header_check.c.
#
# The second compile needs a sibling `oops-mesa` checkout with Mesa's headers in it. Without one
# it is skipped, loudly.
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
COLLECTION=$(cd "$ROOT/.." && pwd)
OUT=${OUT:-$ROOT/build/header-check}
mkdir -p "$OUT"

WARN="-std=c11 -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes"

# 1. The headers on their own, under the flags a freestanding port compiles with.
# shellcheck disable=SC2086
${CC:-clang} $WARN \
    -target x86_64-unknown-freebsd -ffreestanding -fno-builtin -nostdlib -fPIC \
    -fno-stack-protector -fvisibility=hidden \
    -I"$ROOT/include" -I"$ROOT/include/libc" -I"$ROOT" -DOOPS_TARGET=3 \
    -c "$HERE/header_check.c" -o "$OUT/header_check.o"
echo "header-check: passed - the SDK's GL headers compile on their own"

# 2. The same translation unit with Mesa's `glext.h` after ours, which is the order a hosted
#    title includes them in. Every extension name is then a redefinition, which is the case that
#    broke both oops-mesa probes on 2026-09-20.
GLEXT_DIR=""
for candidate in "$COLLECTION/oops-mesa/mesa/include" "$COLLECTION/oops-mesa/include"; do
    if [ -f "$candidate/GL/glext.h" ]; then
        GLEXT_DIR="$candidate"
        break
    fi
done

if [ -z "$GLEXT_DIR" ]; then
    echo "header-check: SKIPPED the glext.h half - no sibling oops-mesa checkout with GL/glext.h."
    echo "  That is the half that catches a macro or a declaration which only conflicts when"
    echo "  this SDK's headers meet a real one, so a change to include/GL/gl.h is unchecked"
    echo "  against that until this runs somewhere it is present."
    exit 0
fi

# shellcheck disable=SC2086
${CC:-clang} $WARN -DHEADER_CHECK_WITH_GLEXT \
    -target x86_64-unknown-freebsd -ffreestanding -fno-builtin -nostdlib -fPIC \
    -fno-stack-protector -fvisibility=hidden \
    -I"$ROOT/include" -I"$ROOT/include/libc" -I"$ROOT" -I"$GLEXT_DIR" -DOOPS_TARGET=3 \
    -c "$HERE/header_check.c" -o "$OUT/header_check_glext.o"
echo "header-check: passed - and beside $(basename "$(dirname "$GLEXT_DIR")")'s GL/glext.h"
