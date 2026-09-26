# The compiler pin (oops-mesa#D013). Included by this repository's `Makefile` before anything
# is compiled; stops the build unless `CC`, and `TARGET_CC` when set, report clang major
# version $(OOPS_CLANG_MAJOR). Only the major version is checked: libc++ 21's headers need a
# clang at least as new. `-dumpversion` is used because `--version` carries a vendor prefix.

OOPS_CLANG_MAJOR := 21

# Goals that compile nothing work on a machine with no compiler.
OOPS_TOOLCHAIN_SKIP_GOALS := clean distclean help

ifeq ($(filter $(OOPS_TOOLCHAIN_SKIP_GOALS),$(MAKECMDGOALS)),)

# A compiler may carry a cache prefix, so the version is asked of the last word of the
# variable.
define oops_cc_major
$(firstword $(subst ., ,$(shell $(lastword $(1)) -dumpversion 2>/dev/null)))
endef

OOPS_CC_MAJOR := $(call oops_cc_major,$(CC))
ifneq ($(strip $(TARGET_CC)),)
OOPS_TARGET_CC_MAJOR := $(call oops_cc_major,$(TARGET_CC))
endif

ifneq ($(OOPS_CC_MAJOR),$(OOPS_CLANG_MAJOR))
$(error toolchain: this build is pinned to clang $(OOPS_CLANG_MAJOR) and `$(lastword $(CC))` \
reports major "$(OOPS_CC_MAJOR)" (empty means it is not runnable). \
The two runners that carry the pin are WSL `oops-builder` and the container \
`silkeh/clang:$(OOPS_CLANG_MAJOR)`; see oops-mesa#D013. Do not work around this by passing CC - the \
split between runners is the defect it was written for)
endif

ifneq ($(strip $(TARGET_CC)),)
ifneq ($(OOPS_TARGET_CC_MAJOR),$(OOPS_CLANG_MAJOR))
$(error toolchain: this build is pinned to clang $(OOPS_CLANG_MAJOR) and the target compiler \
`$(lastword $(TARGET_CC))` reports major "$(OOPS_TARGET_CC_MAJOR)" (empty means it is not \
runnable). See oops-mesa#D013)
endif
endif

endif

