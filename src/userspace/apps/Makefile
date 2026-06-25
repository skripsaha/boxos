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

all: proca.elf procb.elf today.elf memtest.elf mtest.elf chain.elf decks.elf bench.elf touch_test.elf touch_stress.elf lifecycle.elf persist.elf write_stress.elf write_concurrent.elf write_observer.elf cow_test.elf anchor_test.elf bay_test.elf brook_test.elf current_test.elf htest.elf cxxtest.elf strandtest.elf brookstrand.elf brookexec.elf currentexec.elf strandpark.elf

# ---- ELF binaries (stored in TagFS) ----
proca.elf: $(BOXLIB_START) proca.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) proca.o $(BOXLIB)

strandtest.elf: $(BOXLIB_START) strandtest.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) strandtest.o $(BOXLIB)

brookstrand.elf: $(BOXLIB_START) brookstrand.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) brookstrand.o $(BOXLIB)

strandpark.elf: $(BOXLIB_START) strandpark.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) strandpark.o $(BOXLIB)

brookexec.elf: $(BOXLIB_START) brookexec.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld
	$(CXX) $(CXXLDFLAGS) -o $@ $(BOXLIB_START) brookexec.o $(BOXCXX) $(BOXLIB) -lgcc

currentexec.elf: $(BOXLIB_START) currentexec.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld
	$(CXX) $(CXXLDFLAGS) -o $@ $(BOXLIB_START) currentexec.o $(BOXCXX) $(BOXLIB) -lgcc

procb.elf: $(BOXLIB_START) procb.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) procb.o $(BOXLIB)

today.elf: $(BOXLIB_START) today.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) today.o $(BOXLIB)

memtest.elf: $(BOXLIB_START) memtest.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) memtest.o $(BOXLIB)

mtest.elf: $(BOXLIB_START) mtest.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) mtest.o $(BOXLIB)

chain.elf: $(BOXLIB_START) chain.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) chain.o $(BOXLIB)

decks.elf: $(BOXLIB_START) decks.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) decks.o $(BOXLIB)

bench.elf: $(BOXLIB_START) bench.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) bench.o $(BOXLIB)

touch_test.elf: $(BOXLIB_START) touch_test.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) touch_test.o $(BOXLIB)

touch_stress.elf: $(BOXLIB_START) touch_stress.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) touch_stress.o $(BOXLIB)

lifecycle.elf: $(BOXLIB_START) lifecycle.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) lifecycle.o $(BOXLIB)

persist.elf: $(BOXLIB_START) persist.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) persist.o $(BOXLIB)

write_stress.elf: $(BOXLIB_START) write_stress.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) write_stress.o $(BOXLIB)

write_concurrent.elf: $(BOXLIB_START) write_concurrent.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) write_concurrent.o $(BOXLIB)

write_observer.elf: $(BOXLIB_START) write_observer.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) write_observer.o $(BOXLIB)

cow_test.elf: $(BOXLIB_START) cow_test.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) cow_test.o $(BOXLIB)

anchor_test.elf: $(BOXLIB_START) anchor_test.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) anchor_test.o $(BOXLIB)

bay_test.elf: $(BOXLIB_START) bay_test.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) bay_test.o $(BOXLIB)

brook_test.elf: $(BOXLIB_START) brook_test.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) brook_test.o $(BOXLIB)

current_test.elf: $(BOXLIB_START) current_test.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) current_test.o $(BOXLIB)

htest.elf: $(BOXLIB_START) htest.o $(BOXLIB)
	$(CC) $(LDFLAGS) -o $@ $(BOXLIB_START) htest.o $(BOXLIB)

# ---- C++ binaries (linked with the g++ driver) ----
# Link order matters: libboxcxx.a BEFORE libbox.a (boxcxx pulls
# _malloc_impl/printf from boxlib) and before any future libgcc so our
# runtime symbols always win.
cxxtest.elf: $(BOXLIB_START) cxxtest.o cxxtest_traits.o $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld
	$(CXX) $(CXXLDFLAGS) -o $@ $(BOXLIB_START) cxxtest.o cxxtest_traits.o $(BOXCXX) $(BOXLIB) -lgcc
	@align=$$(x86_64-elf-readelf -lW $@ | awk '$$1=="TLS"{print $$NF}'); \
	if [ -n "$$align" ] && [ $$((align)) -gt 64 ]; then \
		echo "ERROR: $@ PT_TLS p_align=$$align > 64 — boxcxx tls_init layout contract broken (alignas(>64) thread_local?)"; \
		exit 1; \
	fi

# ---- object files ----
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- boxlib_start entry (shared with shell) ----
$(BOXLIB_START): $(BOXLIB_DIR)/boxlib_start.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

clean:
	rm -f *.o *.elf
