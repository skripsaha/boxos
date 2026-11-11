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
         -fno-stack-protector -fpie -static -fno-ipa-ra -fcf-protection=full -fstack-clash-protection \
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
PAINTER    = ../painter/libpainter.a
BOXLIB_START = ../build/boxlib_start.o
# The assembler is several parts, kept in its own directory.
ASSEMBLE_DIR  = assemble
ASSEMBLE_OBJS = $(patsubst %.c,%.o,$(wildcard $(ASSEMBLE_DIR)/*.c))
ASSEMBLE_RUNTIME_BLOB = $(ASSEMBLE_DIR)/runtime_blob.o
ASSEMBLE_ENTRY_BLOB = $(ASSEMBLE_DIR)/entry_blob.o

UTILS = create show files tag untag name trash delete assemble \
        me info say reboot bye defrag escheat fsck ipc_test memtag hw \
        timezone logsave lastsaid usetest faretest luggagetest rollcall handset headcount \
        playtime lavalamp finish taskboard hexer stringview \
        calculate difference copy typer flame checksum passport aquarium \
        nyancat fireworks 2048 tetris todo tagmap mandelbrot qr minesweeper eyes donut slime tumbleweed crab \
        edit seek retag read inside settings scratchpad garden gardener

.PHONY: all clean

# The Painter archive has its own target, but utilities must still default to
# the complete utility set when this Makefile is invoked recursively.
.DEFAULT_GOAL := all

# A file, not a phony: a phony prerequisite is never there, and every program
# that names it would be linked again at every make — and the volume laid
# down again, under whatever a run had made of it. The top-level Makefile
# builds Painter before this one runs; this rule makes it when it is missing.
$(PAINTER): $(BOXCXX) $(BOXLIB)
	@cd ../painter && $(MAKE)

all: $(addsuffix .elf,$(UTILS))

# After `all`: the fragment's first rule would otherwise become the default goal.
include ../nameplate.mk

%.elf: $(BOXLIB_START) %.o $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $*.o $(BOXLIB))

# inside is its one source shared with tools/inside (inside/inside_show.c) and
# the utility's own main.
INSIDE_OBJS = inside.o inside/inside_show.o

inside.elf: $(BOXLIB_START) $(INSIDE_OBJS) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) $(INSIDE_OBJS) $(BOXLIB))

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

# ‼ THE NEWEST UTILITIES ARE STRIPPED, AND THE REASON IS THE VOLUME.
#
# The medium holds 24 MB and the image was already using all but 946 KB of it.
# Four of them unstripped came to 530 KB, which left 400 KB — and the C++
# conformance suite, which CREATES files as it tests <fstream>, then failed
# seventy checks on a volume with nowhere to put them. Measured: UEFI STRICT 4c
# went from no cxxtest failures at all to seventy, every one of them in a phase
# that writes.
#
# What is dropped is .symtab and .debug_*, and dropping them costs nothing that
# can be read from the running machine: the Nameplate linked in above already
# carries the address-to-name table a crash needs, in a section the loader maps
# (`tools/nameplate list` read all 362 names out of a stripped program).
# It must come AFTER the link, because `nameplate verify` reads the symbol
# table this deletes. cxxtest, timezone and exitpaths are stripped for the same
# reason and were the precedent.
finish.elf: $(BOXLIB_START) finish.o $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CC),$(LDFLAGS),$(BOXLIB_START) finish.o $(BOXLIB))
	@x86_64-elf-objcopy --strip-all $@

playtime.elf: $(BOXLIB_START) playtime.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) playtime.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

lavalamp.elf: $(BOXLIB_START) lavalamp.o $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) lavalamp.o $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# The zone database is C++, so `timezone` is. An explicit rule, which overrides
# the pattern rule above: a different driver, and libboxcxx.a BEFORE libbox.a
# (boxcxx pulls malloc/printf out of boxlib, never the other way).
timezone.elf: $(BOXLIB_START) timezone.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) timezone.o $(BOXCXX) $(BOXLIB) -lgcc)
	@# Stripped like cxxtest: the Nameplate linked in above already carries the
	@# address-to-name table, so .symtab and .debug_* would be a second copy of
	@# what the image can already tell you at runtime — and this one costs half
	@# a megabyte of a 24 MB image.
	@x86_64-elf-objcopy --strip-all $@

# The board is C++ in parts of its own directory, each named for what it is.
TASKBOARD_DIR  = taskboard
TASKBOARD_OBJS = $(addprefix $(TASKBOARD_DIR)/,taskboard.o board.o paint.o graph.o look.o machine.o programs.o \
                                               history.o journal.o)

taskboard.elf: $(BOXLIB_START) $(TASKBOARD_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(TASKBOARD_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# The scratchpad is C++ in parts of its own directory, each named for what it is.
SCRATCHPAD_DIR  = scratchpad
SCRATCHPAD_OBJS = $(addprefix $(SCRATCHPAD_DIR)/,scratchpad.o pad.o paint.o look.o notes.o text.o)

scratchpad.elf: $(BOXLIB_START) $(SCRATCHPAD_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(SCRATCHPAD_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)

# The garden is C++ in parts of its own directory, each named for what it is;
# gardener, its keeper in the background, is the file and the Touch it shares
# with it (record.o) and its own part. Both keep the plants' file with keep.o.
GARDEN_DIR     = garden
GARDEN_SHARED  = $(addprefix $(GARDEN_DIR)/,record.o) keep.o
GARDEN_OBJS    = $(addprefix $(GARDEN_DIR)/,garden.o view.o paint.o growth.o look.o sky.o flora.o air.o) $(GARDEN_SHARED)
GARDENER_OBJS  = $(GARDEN_DIR)/gardener.o $(GARDEN_SHARED)

# The parts are named from the utilities' root: "keep.h".
$(sort $(GARDEN_OBJS) $(GARDENER_OBJS)): CXXFLAGS += -I.

garden.elf: $(BOXLIB_START) $(GARDEN_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(GARDEN_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

gardener.elf: $(BOXLIB_START) $(GARDENER_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(GARDENER_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# hexer and stringview are C++ in parts: each names its own objects, links
# libboxcxx.a before libbox.a, and is stripped like taskboard.
HEXER_OBJS      = hexer.o hexer_bytes.o hexer_paint.o hexer_list.o
STRINGVIEW_OBJS = stringview.o stringview_sift.o stringview_paint.o

hexer.elf: $(BOXLIB_START) $(HEXER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(HEXER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

stringview.elf: $(BOXLIB_START) $(STRINGVIEW_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(STRINGVIEW_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# calculate, difference, copy, typer, flame, checksum, passport, aquarium, nyancat,
# fireworks, 2048, tetris, todo, tagmap, mandelbrot, qr, minesweeper and eyes are C++, the larger ones in
# parts named after their program; linked and stripped as hexer and stringview.
CALCULATE_OBJS  = calculate.o calculate_integer.o calculate_fraction.o calculate_math.o \
                  calculate_show.o calculate_read.o calculate_parse.o
DIFFERENCE_OBJS = difference.o difference_myers.o difference_text.o difference_print.o
COPY_OBJS       = copy.o
SEEK_OBJS       = seek.o
RETAG_OBJS      = retag.o
TYPER_OBJS      = typer.o typer_passages.o typer_text.o typer_attempt.o typer_record.o \
                  typer_store.o typer_screen.o typer_paint.o typer_results.o
FLAME_OBJS      = flame.o flame_sim.o flame_palette.o flame_font.o flame_paint.o flame_menu.o
CHECKSUM_OBJS   = checksum.o checksum_small.o checksum_sha2.o checksum_sha3.o checksum_kinds.o
PASSPORT_OBJS   = passport.o keep.o
AQUARIUM_OBJS   = aquarium.o aquarium_kinds.o aquarium_scene.o aquarium_life.o aquarium_paint.o
# A small file of its own on the volume, shared by the programs after aquarium
# (keep.h).
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
DONUT_OBJS      = donut.o
SLIME_OBJS      = slime.o
TUMBLEWEED_OBJS = tumbleweed.o tumbleweed_world.o tumbleweed_scene.o tumbleweed_paint.o
CRAB_OBJS      = crab.o
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
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(CALCULATE_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

difference.elf: $(BOXLIB_START) $(DIFFERENCE_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(DIFFERENCE_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

copy.elf: $(BOXLIB_START) $(COPY_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(COPY_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

seek.elf: $(BOXLIB_START) $(SEEK_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(SEEK_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

retag.elf: $(BOXLIB_START) $(RETAG_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(RETAG_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

typer.elf: $(BOXLIB_START) $(TYPER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(TYPER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

flame.elf: $(BOXLIB_START) $(FLAME_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(FLAME_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

checksum.elf: $(BOXLIB_START) $(CHECKSUM_OBJS) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(CHECKSUM_OBJS) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

aquarium.elf: $(BOXLIB_START) $(AQUARIUM_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(AQUARIUM_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

nyancat.elf: $(BOXLIB_START) $(NYANCAT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(NYANCAT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

fireworks.elf: $(BOXLIB_START) $(FIREWORKS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(FIREWORKS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

2048.elf: $(BOXLIB_START) $(2048_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(2048_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

tetris.elf: $(BOXLIB_START) $(TETRIS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(TETRIS_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

todo.elf: $(BOXLIB_START) $(TODO_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(TODO_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

tagmap.elf: $(BOXLIB_START) $(TAGMAP_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(TAGMAP_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

mandelbrot.elf: $(BOXLIB_START) $(MANDELBROT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(MANDELBROT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

qr.elf: $(BOXLIB_START) $(QR_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(QR_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

minesweeper.elf: $(BOXLIB_START) $(MINESWEEPER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(MINESWEEPER_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

eyes.elf: $(BOXLIB_START) $(EYES_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(EYES_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

donut.elf: $(BOXLIB_START) $(DONUT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(DONUT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

slime.elf: $(BOXLIB_START) $(SLIME_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(SLIME_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

tumbleweed.elf: $(BOXLIB_START) $(TUMBLEWEED_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(TUMBLEWEED_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

crab.elf: $(BOXLIB_START) $(CRAB_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(CRAB_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

passport.elf: $(BOXLIB_START) $(PASSPORT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(PASSPORT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# What edit and read share, so the two are one family: their look, their bars,
# the help of their keys, a field, the file chooser and the file's card, the
# volume and the clipboard. Linked into each; included as "kin/...".
KIN_DIR  = kin
KIN_OBJS = $(addprefix $(KIN_DIR)/,look.o bars.o keys.o field.o clipboard.o volume.o picker.o \
                                   suggest.o card.o)

# The editor is C++ in parts of its own directory, each named for what it is;
# linked and stripped as hexer.
EDIT_DIR  = edit
EDIT_OBJS = $(addprefix $(EDIT_DIR)/,edit.o editor.o tab.o text.o history.o keymap.o search.o clips.o paint.o) \
            $(KIN_OBJS)

edit.elf: $(BOXLIB_START) $(EDIT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(EDIT_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# The reader likewise, in a directory of its own.
READ_DIR  = read
READ_OBJS = $(addprefix $(READ_DIR)/,read.o reader.o tab.o page.o keymap.o search.o lists.o marks.o book.o \
                                     layout.o) \
            $(KIN_OBJS)

read.elf: $(BOXLIB_START) $(READ_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(READ_OBJS) $(PAINTER) $(BOXCXX) $(BOXLIB) -lgcc)
	@x86_64-elf-objcopy --strip-all $@

# The family's parts are named from the utilities' root: "kin/look.h".
$(sort $(EDIT_OBJS) $(READ_OBJS)): CXXFLAGS += -I.

# -MMD -MP: header dependencies, so a header edit rebuilds what read it.
# See boxcxx/Makefile for what their absence cost.
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard *.d $(ASSEMBLE_DIR)/*.d $(KIN_DIR)/*.d $(EDIT_DIR)/*.d $(READ_DIR)/*.d $(TASKBOARD_DIR)/*.d $(SCRATCHPAD_DIR)/*.d $(GARDEN_DIR)/*.d)

$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.d *.elf inside/*.o inside/*.d $(ASSEMBLE_DIR)/*.o $(ASSEMBLE_DIR)/*.d $(KIN_DIR)/*.o $(KIN_DIR)/*.d $(EDIT_DIR)/*.o $(EDIT_DIR)/*.d $(READ_DIR)/*.o $(READ_DIR)/*.d \
	      $(TASKBOARD_DIR)/*.o $(TASKBOARD_DIR)/*.d $(SCRATCHPAD_DIR)/*.o $(SCRATCHPAD_DIR)/*.d $(GARDEN_DIR)/*.o $(GARDEN_DIR)/*.d
