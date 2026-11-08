# ============================================================================
# BoxOS Display Server Makefile
# ============================================================================
# Uses the BoxOS C++ userspace runtime and existing C boxlib primitives.
# ============================================================================

# Cross-compiler toolchain - SAME on all platforms
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
CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti -fcoroutines \
         -fuse-cxa-atexit -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -g -Werror=return-type \
         -I../boxlib/include \
         -I../boxcxx/include/std -I../boxcxx/include \
         -I../../include

BOXLIB_DIR   = ../boxlib
BOXCXX_DIR   = ../boxcxx
# The C++ link force-pulls tls_init.o out of libboxcxx.a: nothing references it
# by symbol (it registers itself through .init_array), so without this the
# daemon ran with FS base 0 and the first thread_local touch — the exception
# globals of a throw, __cxa_finalize on the way out — faulted at address 0.
LDFLAGS      = -nostdlib -static -T $(BOXLIB_DIR)/user.ld -Wl,--undefined=__boxcxx_tls_bootstrap
BOXLIB       = ../boxlib/libbox.a
BOXCXX       = ../boxcxx/libboxcxx.a
BOXLIB_START = ../build/boxlib_start.o

.PHONY: all clean

all: display.elf

# After `all`: the fragment's first rule would otherwise become the default goal.
include ../nameplate.mk

OBJS = display.o lanes.o layers.o console_model.o ring.o easel.o

display.elf: $(BOXLIB_START) $(OBJS) $(BOXLIB) $(BOXCXX) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
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
