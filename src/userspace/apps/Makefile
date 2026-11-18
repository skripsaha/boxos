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
# -fpie: a program is a box the kernel places where it chooses (box_format.h):
#        linked position-independent (LDFLAGS), its fixups the box's
# -mno-red-zone: Required for interrupt context compatibility
# -fno-ipa-ra:   Disable IPRA. Without it GCC parks live values in caller-
#                saved registers across calls. Any clobber (kernel
#                save/restore quirk, IRQ stack-trash, scheduler hiccup) then
#                yields a wild pointer that surfaces as a far-away page
#                fault inside boxlib (memcpy/ManifestBuilderAddOp). Same
#                fix the boxlib Makefile already carries — must be on every
#                userspace binary that links libbox.a.
CFLAGS = -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fpie -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
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
         -fno-stack-protector -fpie -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -g \
         -Werror=return-type \
         -I../boxcxx/include/std \
         -I../boxcxx/include \
         -I../painter/include \
         -I../boxlib/include \
         -I../../include

BOXLIB_DIR = ../boxlib
LDFLAGS    = -nostdlib -static -T $(BOXLIB_DIR)/user.ld -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,pack-relative-relocs
BOXLIB     = ../boxlib/libbox.a
BOXCXX     = ../boxcxx/libboxcxx.a
BOXLIB_START = ../build/boxlib_start.o
PAINTER    = ../painter/libpainter.a
TYPECASE   = ../typecase/libtypecase.a

.PHONY: all clean

all: proca.elf procb.elf testprogram.elf today.elf memtest.elf mtest.elf chain.elf decks.elf bench.elf touch_test.elf touch_stress.elf lifecycle.elf persist.elf write_stress.elf write_concurrent.elf write_observer.elf anchor_test.elf bay_test.elf brook_test.elf current_test.elf htest.elf cxxtest.elf strandtest.elf brookstrand.elf brookexec.elf currentexec.elf strandpark.elf childspin.elf print_stress.elf quietprint.elf exitpaths.elf quilltest.elf clock.elf pulse.elf calendar.elf bonsai.elf bigclock.elf snake.elf hacker.elf pipes.elf wordle.elf

# A file, not a phony: a phony prerequisite is never there, and every program
# that names it would be linked again at every make — and the volume laid
# down again, under whatever a run had made of it. The top-level Makefile
# builds Painter before this one runs; this rule makes it when it is missing.
$(PAINTER): $(BOXCXX) $(BOXLIB)
	@cd ../painter && $(MAKE)

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

today.elf: $(BOXLIB_START) today.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) today.o $(BOXCXX) $(BOXLIB) -lgcc)

testprogram.elf: $(BOXLIB_START) testprogram.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) testprogram.o $(BOXCXX) $(BOXLIB) -lgcc)

# Seven objects, one program: the red tests of the Quill write core, the ones
# only a throwaway image may be asked, the modes the power-cut stand drives
# (tools/powercut.sh), abba, the guard on the lock orders a growing chain
# crosses (logcheck abba), beside, a write among cabins that would keep the
# kernel to themselves (logcheck turns), the volume asked from many cabins at
# once (logcheck cores), away, writers queued behind a write when its medium
# leaves (logcheck replugwrite), and roomback, emptyfull and its halves either
# side of a reboot, a full volume (logcheck fullvolume), which together ran
# past the thousand lines one file may hold.
QUILLTEST_OBJS = quilltest.o quilltest_throwaway.o quilltest_powercut.o quilltest_locks.o \
                 quilltest_turns.o quilltest_cores.o quilltest_ground.o quilltest_away.o \
                 quilltest_room.o quilltest_book.o quilltest_escheat.o quilltest_dedup.o
quilltest.elf: $(BOXLIB_START) $(QUILLTEST_OBJS) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $(QUILLTEST_OBJS) $(BOXLIB))

# The cases that harm a volume past their own files exist only in a quilltest
# built for a throwaway image: the top Makefile's VOLUME_TESTS=on, which make
# hands down to this one. The marker's name carries the key — the top
# Makefile's build-key trick — so switching it names a file that does not
# exist, and every quilltest object compiles again. It lives in ../build with
# boxlib_start.o, which git does not see and `make clean` removes.
QUILLTEST_KEY_MARK = ../build/.quilltest.volume_tests.$(if $(filter on,$(VOLUME_TESTS)),on,off)
$(QUILLTEST_OBJS): CFLAGS += $(if $(filter on,$(VOLUME_TESTS)),-DCONFIG_VOLUME_TESTS=1,)
$(QUILLTEST_OBJS): $(QUILLTEST_KEY_MARK)
$(QUILLTEST_KEY_MARK):
	@mkdir -p $(dir $@)
	@rm -f ../build/.quilltest.volume_tests.*
	@touch $@

brookexec.elf: $(BOXLIB_START) brookexec.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) brookexec.o $(BOXCXX) $(BOXLIB) -lgcc)

currentexec.elf: $(BOXLIB_START) currentexec.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) currentexec.o $(BOXCXX) $(BOXLIB) -lgcc)

# clock and pulse are notices of the panel (panel.h), in text they measure.
clock.elf: $(BOXLIB_START) clock.o $(PAINTER) $(TYPECASE) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) clock.o $(PAINTER) $(TYPECASE) $(BOXCXX) $(BOXLIB) -lgcc)

pulse.elf: $(BOXLIB_START) pulse.o $(PAINTER) $(TYPECASE) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) pulse.o $(PAINTER) $(TYPECASE) $(BOXCXX) $(BOXLIB) -lgcc)

calendar.elf: $(BOXLIB_START) calendar.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) calendar.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

bonsai.elf: $(BOXLIB_START) bonsai/bonsai.o bonsai/tree.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) bonsai/bonsai.o bonsai/tree.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

bigclock.elf: $(BOXLIB_START) bigclock.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) bigclock.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

snake.elf: $(BOXLIB_START) snake.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) snake.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

hacker.elf: $(BOXLIB_START) hacker.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) hacker.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

pipes.elf: $(BOXLIB_START) pipes.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) pipes.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

wordle.elf: $(BOXLIB_START) wordle.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) wordle.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

# Phase203's child: the only witness to whether teardown runs on a given exit
# path is a process that took it. Stripped like cxxtest — it needs no debug
# info to die.
exitpaths.elf: $(BOXLIB_START) exitpaths.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) exitpaths.o $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# ---- C++ binaries (linked with the g++ driver) ----
# Link order matters: libboxcxx.a BEFORE libbox.a (boxcxx pulls malloc/printf
# from boxlib) and before any future libgcc so our runtime symbols always win.
cxxtest.elf: $(BOXLIB_START) cxxtest.o cxxtest_traits.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) cxxtest.o cxxtest_traits.o $(BOXCXX) $(BOXLIB) -lgcc)
	@# Drop .debug_* AND the symbol table. The debug info has to go because the
	@# correctly-rounded cmath suite bakes tens of thousands of MPFR-reference
	@# vectors, whose -g info bloats the ELF ~10x past the 16 MB TagFS image.
	@# .symtab used to be kept "for backtraces" — the Nameplate linked in above
	@# now carries that, in 3.7 MB where .symtab + .strtab took 6.9 MB, so this
	@# image comes out SMALLER than it was while gaining names at runtime. The
	@# offline answer nm used to give is `tools/nameplate list cxxtest.elf`.
	@x86_64-elf-objcopy --strip-all $@

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
	rm -f *.o *.d *.elf bonsai/*.o bonsai/*.d
