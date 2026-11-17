# Painter — the user-facing graphics family above boxcxx and boxlib.

CXX = x86_64-elf-g++
AR  = x86_64-elf-ar

CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti \
         -fcoroutines \
         -fuse-cxa-atexit \
         -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fpie -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -O2 \
         -Werror=return-type \
         -I include \
         -I ../typecase/include \
         -I ../boxcxx/include/std \
         -I ../boxcxx/include \
         -I ../boxlib/include \
         -I ../../include

SRCS = src/core/color.cpp src/core/pixel.cpp src/core/transform.cpp src/core/path.cpp src/core/gradient.cpp \
       src/core/picture.cpp src/core/fill.cpp src/core/shape_room.cpp src/core/paint.cpp src/core/blur.cpp \
       src/core/mask.cpp src/core/shadow.cpp src/core/canvas.cpp src/core/cpu_path.cpp src/core/lanes_plain.cpp \
       src/core/lanes_sse2.cpp src/core/lanes_avx2.cpp src/core/text_draw.cpp src/core/system_fonts.cpp \
       src/tui/frame.cpp src/tui/stage.cpp src/tui/layer_canvas.cpp src/tui/surface.cpp src/tui/console_surface.cpp src/tui/keyboard.cpp \
       src/tui/display_client.cpp src/tui/text.cpp src/tui/window.cpp \
       src/tui/mosaic.cpp src/tui/fine_canvas.cpp
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
