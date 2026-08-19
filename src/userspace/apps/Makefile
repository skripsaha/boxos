# ============================================================================
# BoxOS Apps Makefile - builds proca, procb, today, memtest
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
         -fno-stack-protector -fno-pic -static -fno-ipa-ra \
         -fcf-protection=full \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -Wall -Wextra -g \
         -Werror=implicit-function-declaration \
         -Werror=incompatible-pointer-types \
         -Werror=return-type \
         -Werror=int-conversion \
         -I../boxlib/include \
         -I../../include

# C++ flags — mirror CFLAGS hardening, plus the C++ language set.
# -nostdinc++          : boxcxx provides ALL std headers (toolchain has none)
# -fasynchronous-unwind-tables : DWARF CFI for the boxcxx unwinder
# C-only -Werror=* diagnostics dropped (implicit-function-declaration,
# incompatible-pointer-types, int-conversion are C-language options).
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
# C++ links force-pull tls_init.o out of libboxcxx.a (nothing references
# it by symbol — it self-registers via .init_array, the classic archive
# dead-pull trap).
CXXLDFLAGS = $(LDFLAGS) -Wl,--undefined=__boxcxx_tls_bootstrap
BOXLIB     = ../boxlib/libbox.a
BOXCXX     = ../boxcxx/libboxcxx.a
BOXLIB_START = ../build/boxlib_start.o

.PHONY: all clean

all: proca.elf procb.elf today.elf memtest.elf mtest.elf chain.elf decks.elf bench.elf touch_test.elf touch_stress.elf lifecycle.elf persist.elf write_stress.elf write_concurrent.elf write_observer.elf cow_test.elf anchor_test.elf bay_test.elf brook_test.elf current_test.elf htest.elf cxxtest.elf strandtest.elf brookstrand.elf brookexec.elf currentexec.elf strandpark.elf childspin.elf print_stress.elf exitpaths.elf

# Included AFTER `all`, never before: the fragment's first rule is the tool,
# and whatever rule make reads first becomes the default goal — an include at
# the top quietly turns `make` into "build the tool and stop".
include ../nameplate.mk

# ---- ELF binaries (stored in TagFS) ----
# Twenty-five apps that link the same way: one object, boxlib, done. They were
# twenty-five identical rules until every image started carrying a Nameplate,
# which made each of them five lines instead of one — the same pattern rule
# utils/Makefile has always used says it once. The C++ binaries below need a
# different driver and extra objects, so they keep explicit rules (which
# override this one).
%.elf: $(BOXLIB_START) %.o $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $*.o $(BOXLIB))

brookexec.elf: $(BOXLIB_START) brookexec.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) brookexec.o $(BOXCXX) $(BOXLIB) -lgcc)

currentexec.elf: $(BOXLIB_START) currentexec.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) currentexec.o $(BOXCXX) $(BOXLIB) -lgcc)

# Phase203's child: the only witness to whether teardown runs on a given exit
# path is a process that took it. Stripped like cxxtest — it needs no debug
# info to die.
exitpaths.elf: $(BOXLIB_START) exitpaths.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) exitpaths.o $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# ---- C++ binaries (linked with the g++ driver) ----
# Link order matters: libboxcxx.a BEFORE libbox.a (boxcxx pulls malloc/printf
# from boxlib) and before any future libgcc so our runtime symbols always win.
cxxtest.elf: $(BOXLIB_START) cxxtest.o cxxtest_traits.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) cxxtest.o cxxtest_traits.o $(BOXCXX) $(BOXLIB) -lgcc)
	@# Drop .debug_* AND the symbol table. The debug info has to go because the
	@# correctly-rounded cmath suite bakes tens of thousands of MPFR-reference
	@# vectors, whose -g info bloats the ELF ~10x past the 16 MB TagFS image.
	@# .symtab used to be kept "for backtraces" — the Nameplate linked in above
	@# now carries that, in 3.7 MB where .symtab + .strtab took 6.9 MB, so this
	@# image comes out SMALLER than it was while gaining names at runtime. The
	@# offline answer nm used to give is `tools/nameplate list cxxtest.elf`.
	@x86_64-elf-objcopy --strip-all $@
	@align=$$(x86_64-elf-readelf -lW $@ | awk '$$1=="TLS"{print $$NF}'); \
	if [ -n "$$align" ] && [ $$((align)) -gt 64 ]; then \
		echo "ERROR: $@ PT_TLS p_align=$$align > 64 — boxcxx tls_init layout contract broken (alignas(>64) thread_local?)"; \
		exit 1; \
	fi

# ---- object files ----
# -MMD -MP: header dependencies. Their absence is what let a boxcxx header
# change leave cxxtest.o stale, so a mutation could sit in a header while the
# test ran on the previous image and reported green. See boxcxx/Makefile.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard *.d) $(wildcard commands/*.d)

# ---- boxlib_start entry (shared with shell) ----
$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.d *.elf
