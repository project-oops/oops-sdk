#!/usr/bin/env bash
#
# Put every shader in this directory through oops-gl's front end and code generator, and check
# each one against the outcome its *name* declares.
#
#   tools/shader-conformance/check.sh
#
# **This is a gate, not a report.** `tools/shader-survey.sh` answers "what is the most common
# refusal across a corpus", which is the right question when pointing it at a port's shaders and
# deciding what to implement next. It is the wrong question here: every file below is written
# against the GLSL specification with a known expected outcome, and the only thing worth knowing
# is whether any of them departed from it. A histogram would print a number and pass.
#
# The naming is the expectation:
#
#   refused-compile-<what>.frag   the front end must refuse it
#   refused-gen-<what>.frag       it must compile and the generator must refuse it
#   <anything else>.vert          it must compile; the vertex stage runs on the CPU, so there is
#                                 no second gate and expecting one would expect a pass from a
#                                 gate that never ran
#   <anything else>.frag          it must compile *and* generate
#
# **The `refused-` files are the negative controls**, and they are what stops this suite from
# being one that cannot fail. A corpus of shaders that all pass says nothing about whether the
# harness ran: `refused-compile-switch.frag` uses a word GLSL 1.10 reserves, and if that one ever
# starts compiling then either the dialect changed or this script stopped looking.
#
# Two things this corpus found on the day it was written, neither reachable from any port's
# shaders - which is the whole argument for having it beside the port survey rather than instead
# of it:
#
#   - an array as a struct member (`struct S { float w[3]; }`), refused by the semantic stage for
#     indexing something that is neither a vector nor a matrix. GLSL 1.10 4.1.9 allows it.
#   - `p == q` on two structs, which GLSL 1.10 5.9 gives to every type but an array. The
#     generator refused it; the interpreter answered it by comparing the first component and
#     stopping, so two structs differing in any later member were equal.
#
# **Not yet part of `make checks`**, which is where it belongs - it exits non-zero and prints the
# file that departed, so it is already shaped like a gate. Adding it means editing the Makefile,
# which is not this session's to edit.
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

  # **Only lines that are a status and a file**, because the GL context this runs in logs to
  # stdout too. Matching on the status word rather than filtering the log out by its prefix means
  # a new log line cannot silently become a shader that passed.
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
      # **A vertex shader has one gate here, not two.** The vertex stage runs on the CPU, so
      # there is nothing for the code generator to refuse about one and compiling is the whole
      # of its answer. Expecting GENERATES of a `.vert` would be expecting a pass from a gate
      # that never ran.
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
