# ============================================================================
# boxcxx Makefile — BoxOS C++ runtime + standard library (libboxcxx.a)
# ============================================================================
# Own implementation: std:: headers under include/std (served via
# -nostdinc++), Itanium C++ ABI runtime, box:: native extensions under
# include/box/cxx. Mirrors boxlib's real-HW hardening posture.
# ============================================================================

CXX = x86_64-elf-g++
AR  = x86_64-elf-ar
ASM = nasm

# -fasynchronous-unwind-tables: every boxcxx frame carries DWARF CFI so
#   the own unwinder (src/unwind/) can walk through library code.
# -fuse-cxa-atexit: bare-metal g++ defaults to atexit()-based static-dtor
# registration (boxlib has no atexit); we provide __cxa_atexit instead.
CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti \
         -fcoroutines \
         -fuse-cxa-atexit \
         -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static -fno-ipa-ra \
         -fcf-protection=full \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -O2 \
         -Werror=return-type \
         -I include/std -I include \
         -I ../boxlib/include -I ../../include

SRCS = src/runtime/cxa_runtime.cpp \
       src/runtime/csignal.cpp \
       src/runtime/cassert.cpp \
       src/runtime/cstdlib.cpp \
       src/runtime/new_delete.cpp \
       src/runtime/cxxabi_typeinfo.cpp \
       src/runtime/dynamic_cast.cpp \
       src/runtime/cxa_exception.cpp \
       src/runtime/personality.cpp \
       src/runtime/atomic_support.cpp \
       src/runtime/mutex.cpp \
       src/runtime/stdexcept.cpp \
       src/runtime/system_error.cpp \
       src/runtime/ios.cpp \
       src/runtime/iostream.cpp \
       src/runtime/syncstream.cpp \
       src/runtime/par_engine.cpp \
       src/runtime/box_error.cpp \
       src/runtime/vocab_exceptions.cpp \
       src/runtime/memory_resource.cpp \
       src/runtime/charconv_float.cpp \
       src/runtime/format.cpp \
       src/runtime/string_io.cpp \
       src/runtime/demangle.cpp \
       src/tls/tls_init.cpp \
       src/tls/tls_strand.cpp \
       src/unwind/dwarf_cfi.cpp \
       src/unwind/unwind_level1.cpp
OBJS = $(SRCS:.cpp=.o) src/unwind/unwind_context.o

all: libboxcxx.a

libboxcxx.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.asm
	$(ASM) -f elf64 $< -o $@

clean:
	rm -f $(OBJS) libboxcxx.a

.PHONY: all clean
