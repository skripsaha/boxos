# ============================================================================
# BoxOS Utilities Makefile
# ============================================================================
# Uses x86_64-elf-gcc cross-compiler on ALL platforms for consistency.
# ============================================================================

# Cross-compiler toolchain - SAME on all platforms
CC      = x86_64-elf-gcc
CXX     = x86_64-elf-g++

# Compiler flags
# -fno-stack-protector: No libc in userspace, canary support unavailable
# -fno-pic: Position-independent code not needed for static binary
# -mno-red-zone: Required for interrupt context compatibility
# -fno-ipa-ra:   Disable IPRA. Without it GCC parks live values in caller-
#                saved registers across calls. Any clobber (kernel
#                save/restore quirk, IRQ stack-trash, scheduler hiccup) then
#                yields a wild pointer that surfaces as a far-away page
#                fault inside boxlib (memcpy/ManifestBuilderAddOp). Same
#                fix the boxlib Makefile already carries — must be on every
#                userspace binary that links libbox.a.
CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static -fno-ipa-ra -fcf-protection=full \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -Wall -Wextra -g \
         -Werror=implicit-function-declaration \
         -Werror=incompatible-pointer-types \
         -Werror=return-type \
         -Werror=int-conversion \
         -I../boxlib/include \
         -I../../include

# C++ flags for the one utility that needs the zone database — mirrors the
# hardening above plus the C++ language set, exactly as apps/Makefile does.
# The machine's command set should be shaped by what a command is FOR, not by
# which language this Makefile happened to know, so the C++ shoulder lives here
# rather than the command living among the tests in apps/.
CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti \
         -fcoroutines \
         -fuse-cxa-atexit \
         -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static -fno-ipa-ra \
         -fcf-protection=full \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -g \
         -Werror=return-type \
         -I../boxcxx/include/std \
         -I../boxcxx/include \
         -I../boxlib/include \
         -I../../include

BOXLIB_DIR = ../boxlib
LDFLAGS    = -nostdlib -static -T $(BOXLIB_DIR)/user.ld
# C++ links force-pull tls_init.o out of libboxcxx.a (nothing references it by
# symbol — it self-registers via .init_array, the classic archive dead-pull
# trap).
CXXLDFLAGS = $(LDFLAGS) -Wl,--undefined=__boxcxx_tls_bootstrap
BOXLIB     = ../boxlib/libbox.a
BOXCXX     = ../boxcxx/libboxcxx.a
BOXLIB_START = ../build/boxlib_start.o

UTILS = help create show files tag untag name trash erase \
        me info say reboot bye defrag fsck ipc_test memtag hw \
        timezone logsave lastsaid rgbtest usetest luggagetest

.PHONY: all clean

all: $(addsuffix .elf,$(UTILS))

# After `all`: the fragment's first rule would otherwise become the default goal.
include ../nameplate.mk

%.elf: $(BOXLIB_START) %.o $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $*.o $(BOXLIB))

# The zone database is C++, so `timezone` is. An explicit rule, which overrides
# the pattern rule above: a different driver, and libboxcxx.a BEFORE libbox.a
# (boxcxx pulls malloc/printf out of boxlib, never the other way).
timezone.elf: $(BOXLIB_START) timezone.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) timezone.o $(BOXCXX) $(BOXLIB) -lgcc)
	@# Stripped like cxxtest: the Nameplate linked in above already carries the
	@# address-to-name table, so .symtab and .debug_* would be a second copy of
	@# what the image can already tell you at runtime — and this one costs half
	@# a megabyte of a 24 MB image.
	@x86_64-elf-objcopy --strip-all $@

# -MMD -MP: header dependencies, so a header edit rebuilds what read it.
# See boxcxx/Makefile for what their absence cost.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard *.d)

$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.d *.elf
