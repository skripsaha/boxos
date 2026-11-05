# Painter — the user-facing graphics family above boxcxx and boxlib.

CXX = x86_64-elf-g++
AR  = x86_64-elf-ar

CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti \
         -fcoroutines \
         -fuse-cxa-atexit \
         -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fno-pic -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -O2 \
         -Werror=return-type \
         -I include \
         -I ../boxcxx/include/std \
         -I ../boxcxx/include \
         -I ../boxlib/include \
         -I ../../include

SRCS = src/tui/frame.cpp src/tui/stage.cpp src/tui/surface.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: libpainter.a

libpainter.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) libpainter.a

.PHONY: all clean
