# Typecase — characters to glyphs to pictures: TrueType fonts read and drawn,
# and the chain of sources a screen asks. Standard C++ only: the same sources
# build here, against boxcxx, and on the host for tools/typecase_stand.sh.

CXX = x86_64-elf-g++
AR  = x86_64-elf-ar

CXXFLAGS = -std=gnu++23 -nostdinc++ -fexceptions -frtti \
         -fuse-cxa-atexit \
         -fasynchronous-unwind-tables \
         -nostdlib -ffreestanding -fno-builtin -mno-red-zone \
         -fno-stack-protector -fpie -static -fno-ipa-ra \
         -fcf-protection=full -fstack-clash-protection \
         -fno-delete-null-pointer-checks -fno-strict-aliasing -fwrapv \
         -Wall -Wextra -O2 \
         -Werror=return-type \
         -I include \
         -I ../boxcxx/include/std \
         -I ../boxcxx/include \
         -I ../boxlib/include \
         -I ../../include

SRCS = src/font.cpp src/character_map.cpp src/glyph_reader.cpp src/rasterizer.cpp src/size.cpp \
       src/font_source.cpp src/bitmap_source.cpp src/geometry_source.cpp src/box_lines.cpp src/wedges.cpp src/code_box.cpp \
       src/chain.cpp src/glyph_cache.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: libtypecase.a

libtypecase.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) libtypecase.a

.PHONY: all clean
