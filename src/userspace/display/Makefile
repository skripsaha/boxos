# ============================================================================
# BoxOS Display Server Makefile
# ============================================================================
# Uses the BoxOS C++ userspace runtime and existing C boxlib primitives.
# ============================================================================

# Cross-compiler toolchain - SAME on all platforms
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
CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti -fcoroutines \
         -fuse-cxa-atexit -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fpie -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -g -Werror=return-type \
         -I../boxlib/include \
         -I../boxcxx/include/std -I../boxcxx/include \
         -I../typecase/include \
         -I../painter/include \
         -I../../include \
         -I$(IMPRINT_DIR)

BOXLIB_DIR   = ../boxlib
# Where the build wrote the imprint's data (the top Makefile says it).
IMPRINT_DIR ?= ../../../build/imprint_data
BOXCXX_DIR   = ../boxcxx
LDFLAGS      = -nostdlib -static -T $(BOXLIB_DIR)/user.ld -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,pack-relative-relocs
BOXLIB       = ../boxlib/libbox.a
BOXCXX       = ../boxcxx/libboxcxx.a
TYPECASE     = ../typecase/libtypecase.a
PAINTER      = ../painter/libpainter.a
BOXLIB_START = ../build/boxlib_start.o

# `make MEASURE=on` — a display that clocks where its time goes and says it in
# the kernel's log (measure.h). Keyed by a mark, so switching it rebuilds what
# it changes.
ifeq ($(MEASURE),on)
CXXFLAGS += -DBOX_MEASURE
endif
MEASURE_MARK = .measure.$(if $(filter on,$(MEASURE)),on,off)

.PHONY: all clean

all: display.elf

# After `all`: the fragment's first rule would otherwise become the default goal.
include ../nameplate.mk

OBJS = display.o lanes.o front.o console_model.o cell_grid.o cells_draw.o frame.o mirror.o ring.o easel.o effects.o effects_warp.o effects_ink.o \
       typesetter.o scene.o areas.o compositor.o animator.o input.o item_tree.o item_draw.o item_reception.o measure.o

display.elf: $(BOXLIB_START) $(OBJS) $(PAINTER) $(TYPECASE) $(BOXLIB) $(BOXCXX) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(OBJS) $(PAINTER) $(TYPECASE) $(BOXCXX) $(BOXLIB) -lgcc)

# An effect works out every place of the glass every frame, the cells every
# pixel of a cell they draw, the mirror every pixel it moves, the easel every
# pixel it puts on the glass, the scene every pixel of the part of a frame that
# changed, and the areas every pixel of a canvas page a program gives: built
# -O2, as the utilities' per-pixel parts are.
effects.o effects_warp.o effects_ink.o easel.o scene.o areas.o cells_draw.o cell_grid.o frame.o mirror.o: CXXFLAGS += -O2

# -MMD -MP: header dependencies, so a header edit rebuilds what read it.
# See boxcxx/Makefile for what their absence cost.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard *.d)

$(OBJS): $(MEASURE_MARK)
$(MEASURE_MARK):
	rm -f .measure.*
	touch $@

$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.d *.elf .measure.*
