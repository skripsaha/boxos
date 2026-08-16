# ============================================================================
# Nameplate — link every userspace image with its own "address -> name" table.
#
# Included by apps/, shell/, display/ and utils/. What it gives them:
#
#   $(call NameplateLink,<driver>,<ldflags>,<objects>)
#
# which links TWICE. The table names function addresses, so it cannot be built
# before those addresses exist: pass 1 produces them, the tool turns them into
# a table, pass 2 links the table in. The section is placed after .text and
# .rodata (see boxlib/user.ld), so pass 2 moves .data and .bss but not a single
# function — and `nameplate verify` proves it on every build instead of
# trusting it, failing the build if any function moved.
#
# The format, and why an image carries its own names at all, are explained in
# src/include/nameplate_format.h.
# ============================================================================

NAMEPLATE_MK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
NAMEPLATE_TOOL   := $(NAMEPLATE_MK_DIR)../../tools/nameplate
NAMEPLATE_SRC    := $(NAMEPLATE_MK_DIR)../../tools/nameplate.c
NAMEPLATE_FORMAT := $(NAMEPLATE_MK_DIR)../include/nameplate_format.h

# Host compiler, as in the top-level Makefile — this tool runs on the build
# machine, not on BoxOS.
CC_HOST ?= gcc

# Built through a temporary and renamed: a parallel top-level build can enter
# apps/, shell/, display/ and utils/ at the same time, and four writers to one
# path is how you get a half-written binary that "works" until it doesn't.
$(NAMEPLATE_TOOL): $(NAMEPLATE_SRC) $(NAMEPLATE_FORMAT)
	@echo "Compiling Nameplate tool..."
	@$(CC_HOST) -O2 -Wall -Wextra -I$(NAMEPLATE_MK_DIR)../include -o $@.$$$$ $< && mv -f $@.$$$$ $@

# $(1) linker driver   $(2) link flags   $(3) objects and libraries
define NameplateLink
	$(1) $(2) -o $@.pass1 $(3)
	@$(NAMEPLATE_TOOL) build $@.pass1 $@.np.o
	$(1) $(2) -o $@ $(3) $@.np.o
	@$(NAMEPLATE_TOOL) verify $@
	@rm -f $@.pass1 $@.np.o
endef
