# ============================================================================
# BoxOS Hub Makefile
# ============================================================================
# The hub is the place where everything meets: the line typed, the programs on
# the volume, the Use Context. C++ over the BoxOS runtime (boxcxx) and boxlib.
# ============================================================================

CXX     = x86_64-elf-g++

# -fpie: a program is a box the kernel places where it chooses (box_format.h):
#        linked position-independent (LDFLAGS), its fixups the box's
# -mno-red-zone: Required for interrupt context compatibility
# -fno-ipa-ra:   Disable IPRA — the same reason the boxlib Makefile gives;
#                must be on every userspace binary that links libbox.a.
CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti -fcoroutines \
         -fuse-cxa-atexit -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fpie -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -g -Werror=return-type \
         -I../boxlib/include \
         -I../boxcxx/include/std -I../boxcxx/include \
         -I../../include

BOXLIB_DIR   = ../boxlib
LDFLAGS      = -nostdlib -static -T $(BOXLIB_DIR)/user.ld -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,pack-relative-relocs
BOXLIB       = ../boxlib/libbox.a
BOXCXX       = ../boxcxx/libboxcxx.a
BOXLIB_START = ../build/boxlib_start.o

.PHONY: all clean

all: hub.elf

# After `all`: the fragment's first rule would otherwise become the default goal.
include ../nameplate.mk

OBJS = main.o session.o line.o look.o prompt.o context.o history.o words.o typing.o \
       text.o tags.o volume.o programs.o asking.o them.o jobs.o news.o fill.o launch.o \
       remake.o itinerary.o nearest.o view.o whisper.o builtins.o help.o use.o said.o

hub.elf: $(BOXLIB_START) $(OBJS) $(BOXLIB) $(BOXCXX) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(OBJS) $(BOXCXX) $(BOXLIB) -lgcc)

# -MMD -MP: header dependencies, so a header edit rebuilds what read it.
# See boxcxx/Makefile for what their absence cost.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard *.d)

$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.d *.elf
