#!/usr/bin/env bash
# libc-check: link a translation unit that calls every name <libc/*.h> declares, and fail if the
# result leaves any of them undefined.
#
#   bash tools/libc-check/build.sh
#
# Why this exists rather than a unit test: a payload link passes `--unresolved-symbols=ignore-all`
# so the platform can resolve its own `sce*` imports at load, which means a missing `sqrtf` links
# silently and faults on the console. Nothing but the symbol table can tell you, and nothing but
# a call can put the name in it.
#
# Needs the target clang; the collection's WSL builder has it.
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$ROOT/build/libc-check}
mkdir -p "$OUT"

${CC:-clang} -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion \
    -Wstrict-prototypes -Wmissing-prototypes \
    -target x86_64-unknown-freebsd -ffreestanding -fno-builtin -nostdlib -fPIC \
    -fno-stack-protector -fvisibility=hidden \
    -I"$ROOT/include" -I"$ROOT/include/libc" -I"$ROOT" -DOOPS_TARGET=3 \
    -c "$HERE/libc_check.c" -o "$OUT/libc_check.o"

${CC:-clang} -std=c11 -Wall -Wextra -Werror \
    -target x86_64-unknown-freebsd -ffreestanding -fno-builtin -nostdlib -fPIC \
    -fno-stack-protector -fvisibility=hidden \
    -I"$ROOT/include" -I"$ROOT/include/libc" -I"$ROOT" -DOOPS_TARGET=3 \
    -c "$ROOT/src/system/libc.c" -o "$OUT/libc.o"

# The two objects and what they stand on. Linked with the same flags a payload uses, so that the
# symbol table this inspects is the one a payload would have.
${CC:-clang} -target x86_64-unknown-freebsd -ffreestanding -nostdlib -fPIC \
    -fuse-ld=lld -shared -Wl,-Bsymbolic -Wl,--unresolved-symbols=ignore-all \
    -o "$OUT/libc_check.elf" \
    "$OUT/libc_check.o" "$OUT/libc.o" \
    "$ROOT/src/math/math.c" "$ROOT/src/memory/heap.c" "$ROOT/src/memory/memory.c" \
    "$ROOT/src/system/freestd.c" "$ROOT/src/system/scanf.c" \
    "$ROOT/src/system/syscall.c" "$ROOT/src/system/system.c" \
    "$ROOT/src/system/fs.c" "$ROOT/src/system/krw.c" "$ROOT/src/system/sysmodule.c" \
    "$ROOT/src/time/time.c" \
    -I"$ROOT/include" -I"$ROOT/include/libc" -I"$ROOT" -DOOPS_TARGET=3

# Anything undefined that is not a platform import is a name a port would not find.
undefined_names() {
    nm -u "$1" | sed 's/^[[:space:]]*//;s/^w //;s/^U //' \
        | grep -vE '^sce[A-Z]|^sysctlbyname$|^__error$|^_sigaction$|^$' || true
}

# **Does this tool notice?** A checker that cannot fail says nothing when it passes, and this one
# passes by finding no undefined names - which is also what it would report if the inspection
# were broken. So a unit calling a function that exists nowhere is linked the same way, and its
# name has to come back.
cat > "$OUT/negative.c" <<'EOF'
void libc_check_absent_function(void);
void libc_check_negative(void);
void libc_check_negative(void) { libc_check_absent_function(); }
EOF
${CC:-clang} -target x86_64-unknown-freebsd -ffreestanding -nostdlib -fPIC \
    -fuse-ld=lld -shared -Wl,-Bsymbolic -Wl,--unresolved-symbols=ignore-all \
    -o "$OUT/negative.elf" "$OUT/negative.c"
if ! undefined_names "$OUT/negative.elf" | grep -q '^libc_check_absent_function$'; then
    echo "libc-check: FAILED its own self-test - a missing name was not reported, so a pass" >&2
    echo "           from this tool would mean nothing" >&2
    exit 1
fi

missing=$(undefined_names "$OUT/libc_check.elf")
if [ -n "$missing" ]; then
    echo "libc-check: FAILED - these are declared and not defined:" >&2
    echo "$missing" >&2
    exit 1
fi
echo "libc-check: self-test passed (a missing name is reported)"
echo "libc-check: passed - every declared name is defined in the link"
