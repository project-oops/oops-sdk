# D003 - Source inclusion over static archives for freestanding consumers


**Status:** decided
**Date:** 2026-09-04

The SDK initially provided both source lists (`OOPS_SDK_C_SRCS`) and a pre-built static archive
(`liboops.a`). Consumers were linking against `liboops.a`. That straddled two distribution models
and introduced two severe technical hazards:

1. **Freestanding flag-matching.** On a freestanding target without a hosted C runtime, the SDK
   and its consumers must strictly agree on codegen flags (`-target x86_64-unknown-freebsd`,
   `-ffreestanding`, `-fno-builtin`, `-fno-stack-protector`, `-fPIC`, `-mno-sse`, optimization
   levels). A `.a` freezes these flags at SDK build time. If a consumer adjusts codegen flags
   (for instance, to target a specific kernel ABI or firmware revision), a pre-built archive
   becomes silently stale, resulting in an ABI or codegen mismatch that links cleanly but crashes
   or misbehaves at runtime. Source inclusion makes the consumer's flags authoritative across the
   entire translation unit graph.

2. **Weak-symbol and member-selection hazards.** Platform entry points across `oops-sdk` are
   declared with `__attribute__((weak))` and guarded by null checks (e.g. dynamic backend dispatch
   between AGC and GNM). Traditional static archives (`ar`) pull an archive member only when it
   resolves an undefined strong symbol. Weak references or symbols reached indirectly through
   function pointers can be dropped during archive member selection, causing runtime null pointer
   dereferences on hardware. Source inclusion compiles each translation unit unconditionally,
   completely eliminating archive member-dropping.

Link-time dead-code elimination is not lost: consumers compile with `-ffunction-sections` and
`-fdata-sections` and link with `-Wl,--gc-sections`, ensuring unused SDK functions are pruned
from final payloads and eboots.

`oops-sdk.mk` therefore defines only `OOPS_SDK_INCLUDE` and `OOPS_SDK_C_SRCS`. The `Makefile`'s
`all` target continues to build `liboops.a` solely as a `-Werror` compile check gate for
`./bin/oops-sdk check`, not for consumer distribution.

