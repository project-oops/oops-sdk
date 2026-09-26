#!/usr/bin/env bash
#
# Put every shader in this directory through oops-gl's front end and code generator, and check
# each one against the outcome its *name* declares.
#
#   tools/shader-conformance/check.sh
#
# A gate, not a report: every file here is written against the GLSL specification with a known
# expected outcome, and any departure fails. `tools/shader-survey.sh` is the histogram for a
# port's shaders.
#
# The naming is the expectation:
#
#   refused-compile-<what>.frag   the front end must refuse it
#   refused-gen-<what>.frag       it must compile and the generator must refuse it
#   <anything else>.vert          it must compile; the vertex stage runs on the CPU, so there is
#                                 no generator gate
#   <anything else>.frag          it must compile and generate
#
# The `refused-` files are the negative controls: they fail if the harness stops looking.
# `refused-compile-switch.frag` uses a word GLSL 1.10 reserves, so if it ever compiles either
# the dialect changed or this script is broken.
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$(cd "$HERE/../.." && pwd)"

fails=0
checked=0

while IFS= read -r line; do
  status="${line%% *}"
  rest="${line#* }"
  file="${rest%%:*}"
  name="$(basename "$file")"

  # Only lines that are a status and a file: the GL context logs to stdout too, and matching
  # the status word means a new log line cannot become a shader that passed.
  case "$status" in
    GENERATES|COMPILES|GEN-FAIL|COMP-FAIL|LINK-FAIL|UNREADABLE|NOT-A-UNIT) ;;
    *) continue ;;
  esac
  checked=$((checked + 1))

  case "$name" in
    refused-compile-*)
      want="COMP-FAIL" ;;
    refused-gen-*)
      want="GEN-FAIL" ;;
    *.vert)
      # The vertex stage runs on the CPU, so compiling is a vertex shader's whole answer.
      want="COMPILES" ;;
    *)
      want="GENERATES" ;;
  esac

  if [ "$status" != "$want" ]; then
    echo "FAIL $name: wanted $want, got $line" >&2
    fails=$((fails + 1))
  fi
done < <("$SDK/tools/shader-survey.sh" --per-file "$HERE")

if [ "$checked" -eq 0 ]; then
  echo "conformance: no shaders were checked, which is a broken harness and not a pass" >&2
  exit 1
fi

if [ "$fails" -ne 0 ]; then
  echo "conformance: $fails of $checked shaders did not match the outcome their name declares" >&2
  exit 1
fi

echo "conformance: $checked shaders, each with the outcome its name declares"
