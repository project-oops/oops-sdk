#!/usr/bin/env bash
#
# Compile a corpus of real shaders with oops-gl's own front end *and* its code generator, and
# histogram what each one refuses.
#
#   tools/shader-survey.sh <dir-or-file>...
#
# It exists because "which GLSL features do the ports need" is a question with an answer, and
# guessing it wastes work. Run against the upstream trees under `oops-apps/src/oops-titles`, it
# found four real gaps in one evening - ES 1.00's `#version 100`, constant-expression array
# lengths, function overloading, and the sampler-set limit that is what actually stops SuperTux
# generating for the console.
#
# **The two columns are different questions.** The first is the front end: does this shader
# compile at all. The second is the generator: can it be turned into gfx1030 instructions, which
# is what decides whether a port runs on hardware rather than only on the software reference. A
# shader can pass the first and fail the second, and for the ports surveyed so far that is the
# common case - so a survey of only the front end is the more flattering measurement and the less
# useful one.
#
# Two harness mistakes are worth knowing about, because both reported a property of the harness
# as a finding about a port and `tools/shader-survey.c` carries the fix for each:
#
#   - taking the stage from the file extension. craft's vertex shaders are `*_vertex.glsl`, so
#     they were compiled as fragment shaders and `gl_Position` came back undeclared.
#   - pairing every fragment shader with one fixed vertex shader. A fragment `varying` has to be
#     declared by the vertex stage too, so three of craft's four would not link and the
#     generator was never reached.
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

# The host configuration `oops-apps/common/app.mk` uses for its own host tests, so that what this
# compiles with is what the build compiles with - a survey set up separately from the build can
# only tell you that some other configuration works. See AGENTS.md on that trap.
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
