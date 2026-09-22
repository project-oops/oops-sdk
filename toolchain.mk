# The compiler pin, enforced rather than described.
#
# Included by this repository's `Makefile` before anything is compiled. It exists because the
# pin used to be ambient: build files said bare `CC := clang`, and which compiler that was
# depended on where you were standing. Under WSL `oops-builder` it was clang 21; under Docker
# `silkeh/clang:18` it was clang 18. Two runners, one spelling, two compilers, and nothing
# anywhere said which was correct. (oops-mesa#D013)
#
# A pin that is a sentence in a document is not a pin. This file is the same statement in a
# form the build cannot proceed past.
#
# # Compiler variables
#
# oops-sdk builds host self-tests AND a freestanding target archive, so it may have two
# compiler variables (`CC` for host and `TARGET_CC` for target) like oops-apps, or a single
# compiler (`CC`) like obscene.
#
# NOTE: This file was written without sight of `oops-sdk/Makefile` (which is access-restricted).
# Therefore, `CC` is guarded unconditionally, and the second compiler variable (`TARGET_CC`)
# is checked only if set, skipping cleanly if unset so the include works in either configuration.
#
# # What is checked, and what deliberately is not
#
# The **major** version, because that is what the pin is about: libc++ 21's headers use
# compiler-internal constructs that a clang older than itself does not implement, which is the
# whole reason the collection moved (oops-mesa#D013, oops-mesa#D006). A patch-level difference
# between two runners is not that problem and refusing it would make the guard a nuisance
# nobody keeps.
#
# `-dumpversion` is the spelling every clang answers identically. The first line of
# `--version` carries a vendor prefix that differs between the Debian container
# ("Debian clang version 21.1.8") and Ubuntu's apt build ("Ubuntu clang version 21.1.8"),
# so parsing that would compare packaging rather than compilers.
#
# # Why the error and not a warning
#
# CONVENTIONS section 3: a wrong answer costs more than no answer. A build that silently used
# the other compiler is exactly the failure this is here to end, and it is invisible in the
# output - the objects compile, the link succeeds, and the difference shows up as a C++
# standard library that will not build or a fixture that churns.

OOPS_CLANG_MAJOR := 21

# `clean` and `help` must work on a machine with no compiler at all: a guard that stops you
# tidying up is one people route around, and a routed-around guard is not a guard. Every goal
# that actually compiles something is checked.
OOPS_TOOLCHAIN_SKIP_GOALS := clean distclean help

ifeq ($(filter $(OOPS_TOOLCHAIN_SKIP_GOALS),$(MAKECMDGOALS)),)

# A compiler may carry a cache prefix, so the version is asked of the last word rather than of
# the whole variable. Both are asked in one pass and reported together: finding out about the
# second one only after fixing the first is the kind of guard that gets a reputation.
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

