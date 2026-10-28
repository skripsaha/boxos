#ifndef BOXASM_LINK_INTERNAL_H
#define BOXASM_LINK_INTERNAL_H

#include "boxasm_elf.h"

typedef struct {
    const char *Name;
    uint32_t Type;
    uint64_t Flags;
    uint64_t Address;
    uint64_t Size;
    uint64_t FileOffset;
    uint64_t Align;
    BoxAsmBuffer Data;
    uint8_t Present;
    uint8_t NoBits;
} BoxAsmOutputSection;

typedef struct {
    BoxAsmOutputSection Sections[12];
    uint64_t TlsAddress;
    uint64_t TlsFileOffset;
    uint64_t TlsFileSize;
    uint64_t TlsMemorySize;
    uint64_t TlsAlign;
    uint64_t ProgramEnd;
    uint64_t Entry;
} BoxAsmLinkImage;

uint64_t BoxAsmLinkAlign(uint64_t Value, uint64_t Alignment);

int BoxAsmWriteOutput(BoxAsmLinkImage *Image, BoxAsmBlob *Out,
                      char *Error, size_t ErrorCapacity);

#endif
