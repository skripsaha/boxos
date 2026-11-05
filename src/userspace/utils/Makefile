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
         -fno-stack-protector -fno-pic -static -fno-ipa-ra -fcf-protection=full -fstack-clash-protection \
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
LDFLAGS    = -nostdlib -static -T $(BOXLIB_DIR)/user.ld
# C++ links force-pull tls_init.o out of libboxcxx.a (nothing references it by
# symbol — it self-registers via .init_array, the classic archive dead-pull
# trap).
CXXLDFLAGS = $(LDFLAGS) -Wl,--undefined=__boxcxx_tls_bootstrap
BOXLIB     = ../boxlib/libbox.a
BOXCXX     = ../boxcxx/libboxcxx.a
PAINTER    = ../painter/libpainter.a
BOXLIB_START = ../build/boxlib_start.o
# The assembler is several parts, kept in its own directory.
ASSEMBLE_DIR  = assemble
ASSEMBLE_OBJS = $(patsubst %.c,%.o,$(wildcard $(ASSEMBLE_DIR)/*.c))
ASSEMBLE_RUNTIME_BLOB = $(ASSEMBLE_DIR)/runtime_blob.o
ASSEMBLE_ENTRY_BLOB = $(ASSEMBLE_DIR)/entry_blob.o

UTILS = help create show files tag untag name trash erase assemble \
        me info say reboot bye defrag fsck ipc_test memtag hw \
        timezone logsave lastsaid rgbtest usetest faretest luggagetest rollcall handset headcount \
        draft playtime lavalamp finish taskcenter hexer stringview \
        calculate difference copy typer flame checksum passport aquarium \
        nyancat fireworks 2048 tetris todo tagmap mandelbrot qr minesweeper eyes

.PHONY: all clean

# The Painter archive has its own target, but utilities must still default to
# the complete utility set when this Makefile is invoked recursively.
.DEFAULT_GOAL := all

.PHONY: $(PAINTER)
$(PAINTER): $(BOXCXX) $(BOXLIB)
	@cd ../painter && $(MAKE)

all: $(addsuffix .elf,$(UTILS))

# After `all`: the fragment's first rule would otherwise become the default goal.
include ../nameplate.mk

%.elf: $(BOXLIB_START) %.o $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $*.o $(BOXLIB))

assemble.elf: $(BOXLIB_START) $(ASSEMBLE_OBJS) $(ASSEMBLE_RUNTIME_BLOB) \
		$(ASSEMBLE_ENTRY_BLOB) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $(ASSEMBLE_OBJS) \
		$(ASSEMBLE_RUNTIME_BLOB) $(ASSEMBLE_ENTRY_BLOB) $(BOXLIB))

$(ASSEMBLE_RUNTIME_BLOB): $(BOXLIB)
	@x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents $< $@

$(ASSEMBLE_ENTRY_BLOB): $(BOXLIB_START)
	@x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents $< $@

# draft is four parts — the book, the glass, the volume, and what decides —
# and the pattern rule above links exactly one object, so it names its own.
draft.elf: $(BOXLIB_START) draft.o draft_book.o draft_paint.o draft_file.o $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) draft.o draft_book.o draft_paint.o draft_file.o $(BOXLIB))
	@x86_64-elf-objcopy --strip-all $@

# ‼ THE FOUR NEWEST UTILITIES ARE STRIPPED, AND THE REASON IS THE VOLUME.
#
# The medium holds 24 MB and the image was already using all but 946 KB of it.
# These four unstripped came to 530 KB, which left 400 KB — and the C++
# conformance suite, which CREATES files as it tests <fstream>, then failed
# seventy checks on a volume with nowhere to put them. Measured: UEFI STRICT 4c
# went from no cxxtest failures at all to seventy, every one of them in a phase
# that writes.
#
# What is dropped is .symtab and .debug_*, and dropping them costs nothing that
# can be read from the running machine: the Nameplate linked in above already
# carries the address-to-name table a crash needs, in a section the loader maps
# (`tools/nameplate list` still reads all 362 names out of a stripped draft).
# It must come AFTER the link, because `nameplate verify` reads the symbol
# table this deletes. cxxtest, timezone and exitpaths are stripped for the same
# reason and were the precedent.
$(addsuffix .elf,playtime lavalamp finish): %.elf: $(BOXLIB_START) %.o $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $*.o $(BOXLIB))
	@x86_64-elf-objcopy --strip-all $@

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

taskcenter.elf: $(BOXLIB_START) taskcenter.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) taskcenter.o $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# hexer and stringview are C++ in parts, as draft is in C: each names its own
# objects, links libboxcxx.a before libbox.a, and is stripped like taskcenter.
HEXER_OBJS      = hexer.o hexer_bytes.o hexer_paint.o hexer_list.o
STRINGVIEW_OBJS = stringview.o stringview_sift.o stringview_paint.o

hexer.elf: $(BOXLIB_START) $(HEXER_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(HEXER_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

stringview.elf: $(BOXLIB_START) $(STRINGVIEW_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(STRINGVIEW_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# calculate, difference, copy, typer, flame, checksum, passport, aquarium, nyancat,
# fireworks, 2048, tetris, todo, tagmap, mandelbrot, qr, minesweeper and eyes are C++, the larger ones in
# parts named after their program; linked and stripped as hexer and stringview.
CALCULATE_OBJS  = calculate.o calculate_integer.o calculate_fraction.o calculate_math.o \
                  calculate_show.o calculate_read.o calculate_parse.o
DIFFERENCE_OBJS = difference.o difference_myers.o difference_text.o difference_print.o
COPY_OBJS       = copy.o
TYPER_OBJS      = typer.o typer_passages.o typer_text.o typer_attempt.o typer_record.o \
                  typer_store.o typer_screen.o typer_paint.o typer_results.o
FLAME_OBJS      = flame.o flame_sim.o flame_palette.o flame_font.o flame_paint.o flame_menu.o
CHECKSUM_OBJS   = checksum.o checksum_small.o checksum_sha2.o checksum_sha3.o checksum_kinds.o
PASSPORT_OBJS   = passport.o keep.o
AQUARIUM_OBJS   = aquarium.o aquarium_kinds.o aquarium_scene.o aquarium_life.o aquarium_paint.o
# The glass a program owns and its own small file on the volume, shared by
# the programs after aquarium (stage.h, keep.h).
NYANCAT_OBJS    = nyancat.o
FIREWORKS_OBJS  = fireworks.o
2048_OBJS       = 2048.o keep.o
TETRIS_OBJS     = tetris.o tetris_play.o keep.o
TODO_OBJS       = todo.o todo_list.o todo_view.o keep.o
TAGMAP_OBJS     = tagmap.o tagmap_tiles.o keep.o
MANDELBROT_OBJS = mandelbrot.o mandelbrot_paint.o
QR_OBJS         = qr.o qr_code.o
MINESWEEPER_OBJS = minesweeper.o minesweeper_field.o keep.o
EYES_OBJS       = eyes.o
# The utilities are built without optimisation, for their debug information;
# a hash is the one place in them where that costs — every byte of a file
# goes through its rounds — so these three are built as BoxLib is, -O2.
checksum_small.o checksum_sha2.o checksum_sha3.o: CXXFLAGS += -O2
# The aquarium's picture is worked out pixel by pixel, every pixel of every
# frame, so it is built -O2 for the same reason.
aquarium_paint.o: CXXFLAGS += -O2
# So are the night sky's sparks: every one of thousands, every frame.
fireworks.o: CXXFLAGS += -O2
# And a fractal is nothing but that: every pixel followed up to thousands of steps.
mandelbrot_paint.o: CXXFLAGS += -O2

calculate.elf: $(BOXLIB_START) $(CALCULATE_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(CALCULATE_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

difference.elf: $(BOXLIB_START) $(DIFFERENCE_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(DIFFERENCE_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

copy.elf: $(BOXLIB_START) $(COPY_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(COPY_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

typer.elf: $(BOXLIB_START) $(TYPER_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(TYPER_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

flame.elf: $(BOXLIB_START) $(FLAME_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(FLAME_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

checksum.elf: $(BOXLIB_START) $(CHECKSUM_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(CHECKSUM_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

aquarium.elf: $(BOXLIB_START) $(AQUARIUM_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(AQUARIUM_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

nyancat.elf: $(BOXLIB_START) $(NYANCAT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(NYANCAT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

fireworks.elf: $(BOXLIB_START) $(FIREWORKS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(FIREWORKS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

2048.elf: $(BOXLIB_START) $(2048_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(2048_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

tetris.elf: $(BOXLIB_START) $(TETRIS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(TETRIS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

todo.elf: $(BOXLIB_START) $(TODO_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(TODO_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

tagmap.elf: $(BOXLIB_START) $(TAGMAP_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(TAGMAP_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

mandelbrot.elf: $(BOXLIB_START) $(MANDELBROT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(MANDELBROT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

qr.elf: $(BOXLIB_START) $(QR_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(QR_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

minesweeper.elf: $(BOXLIB_START) $(MINESWEEPER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(MINESWEEPER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

eyes.elf: $(BOXLIB_START) $(EYES_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(EYES_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

passport.elf: $(BOXLIB_START) $(PASSPORT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(CXXLDFLAGS),$(BOXLIB_START) $(PASSPORT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# -MMD -MP: header dependencies, so a header edit rebuilds what read it.
# See boxcxx/Makefile for what their absence cost.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard *.d $(ASSEMBLE_DIR)/*.d)

$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.d *.elf $(ASSEMBLE_DIR)/*.o $(ASSEMBLE_DIR)/*.d
