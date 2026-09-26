#!/usr/bin/env bash
#
# Compile a corpus of real shaders with oops-gl's own front end *and* its code generator, and
# histogram what each one refuses.
#
#   tools/shader-survey.sh <dir-or-file>...
#
# Point it at a port's upstream shaders (under `oops-apps/src/oops-titles`) to learn which GLSL
# features that port needs.
#
# The two columns are different questions. The first is the front end: does this shader
# compile. The second is the generator: can it become gfx1030 instructions, which decides
# whether a port runs on hardware rather than only on the software reference. A shader can pass
# the first and fail the second.
#
# `tools/shader-survey.c` infers each shader's stage from its source rather than its extension,
# and links each fragment shader with a vertex shader that declares its varyings.
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/oops-shader-survey"

# `--per-file` prints one line per shader instead of a histogram, which is what a caller that
# has an expected outcome per file needs - see `tools/shader-conformance/`.
PER_FILE=()
if [ "${1:-}" = "--per-file" ]; then
  PER_FILE=(--per-file)
  shift
fi

if [ "$#" -eq 0 ]; then
  echo "usage: $0 [--per-file] <dir-or-file>..." >&2
  exit 2
fi

# GLUT is the windowing layer rather than the GL core, and it pulls in time, keyboard and mouse.
mapfile -t SRCS < <(ls "$SDK"/src/gl/*.c | grep -v glut)

# The host configuration `oops-apps/common/app.mk` uses for its own host tests, so the survey
# measures the configuration the build uses.
clang -std=c11 -O1 -DOOPS_HOST_BUILD -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
      -I"$SDK/include" -I"$SDK/src/gl" -o "$OUT" "$HERE/shader-survey.c" \
      "${SRCS[@]}" \
      "$SDK/src/math/math.c" "$SDK/src/system/system.c" "$SDK/src/system/fs.c" \
      "$SDK/src/memory/heap.c" "$SDK/src/system/freestd.c" -lm

FILES=()
for arg in "$@"; do
  if [ -d "$arg" ]; then
    while IFS= read -r f; do FILES+=("$f"); done < <(find "$arg" \
      \( -name '*.frag' -o -name '*.vert' -o -name '*.glsl' -o -name '*.fs' -o -name '*.vs' \) \
      -type f | sort)
  else
    FILES+=("$arg")
  fi
done

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "no shader files found" >&2
  exit 1
fi

if [ "${#PER_FILE[@]}" -eq 0 ]; then
  echo "surveying ${#FILES[@]} shader files"
fi
"$OUT" "${PER_FILE[@]}" "${FILES[@]}"
