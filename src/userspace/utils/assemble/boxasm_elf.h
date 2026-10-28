#ifndef BOXASM_ELF_H
#define BOXASM_ELF_H

#include "boxasm_support.h"

/* ELF64 structures used by the in-BoxOS assembler and linker.  The target
 * toolchain is freestanding, so it cannot depend on a host <elf.h>. */
typedef struct __attribute__((packed)) {
    uint8_t  Ident[16];
    uint16_t Type;
    uint16_t Machine;
    uint32_t Version;
    uint64_t Entry;
    uint64_t ProgramHeaderOffset;
    uint64_t SectionHeaderOffset;
    uint32_t Flags;
    uint16_t HeaderSize;
    uint16_t ProgramHeaderSize;
    uint16_t ProgramHeaderCount;
    uint16_t SectionHeaderSize;
    uint16_t SectionHeaderCount;
    uint16_t SectionNameIndex;
} BoxAsmElfHeader;

typedef struct __attribute__((packed)) {
    uint32_t Name;
    uint32_t Type;
    uint64_t Flags;
    uint64_t Address;
    uint64_t Offset;
    uint64_t Size;
    uint32_t Link;
    uint32_t Info;
    uint64_t AddressAlign;
    uint64_t EntrySize;
} BoxAsmElfSectionHeader;

typedef struct __attribute__((packed)) {
    uint64_t Address;
    uint64_t Info;
    int64_t  Addend;
} BoxAsmElfRela;

typedef struct __attribute__((packed)) {
    uint32_t Name;
    uint8_t  Info;
    uint8_t  Other;
    uint16_t SectionIndex;
    uint64_t Value;
    uint64_t Size;
} BoxAsmElfSymbol;

typedef struct __attribute__((packed)) {
    uint32_t Type;
    uint32_t Flags;
    uint64_t Offset;
    uint64_t VirtualAddress;
    uint64_t PhysicalAddress;
    uint64_t FileSize;
    uint64_t MemorySize;
    uint64_t Align;
} BoxAsmElfProgramHeader;

typedef struct {
    const uint8_t *Data;
    uint64_t       Size;
    const char    *Name;
} BoxAsmBlob;

typedef struct {
    BoxAsmBlob                    Blob;
    const BoxAsmElfHeader        *Header;
    const BoxAsmElfSectionHeader *Sections;
    const char                   *SectionNames;
    uint64_t                      SectionNameBytes;
    const BoxAsmElfSymbol        *Symbols;
    uint32_t                      SymbolCount;
    const char                   *Strings;
    uint64_t                      StringBytes;
} BoxAsmElfObject;

/* ELF constants used by the bounded BoxASM v0 contract. */
#define BOXASM_ELF_MAGIC0 0x7Fu
#define BOXASM_ELF_MAGIC1 'E'
#define BOXASM_ELF_MAGIC2 'L'
#define BOXASM_ELF_MAGIC3 'F'
#define BOXASM_ELF_CLASS64 2u
#define BOXASM_ELF_LSB 1u
#define BOXASM_ELF_CURRENT 1u
#define BOXASM_ET_REL 1u
#define BOXASM_ET_EXEC 2u
#define BOXASM_EM_X86_64 62u

#define BOXASM_SHT_NULL 0u
#define BOXASM_SHT_PROGBITS 1u
#define BOXASM_SHT_SYMTAB 2u
#define BOXASM_SHT_STRTAB 3u
#define BOXASM_SHT_RELA 4u
#define BOXASM_SHT_NOBITS 8u

#define BOXASM_SHF_WRITE 0x1ull
#define BOXASM_SHF_ALLOC 0x2ull
#define BOXASM_SHF_EXECINSTR 0x4ull
#define BOXASM_SHF_TLS 0x400ull

#define BOXASM_SHN_UNDEF 0u
#define BOXASM_SHN_ABS 0xFFF1u
#define BOXASM_SHN_COMMON 0xFFF2u

#define BOXASM_STB_LOCAL 0u
#define BOXASM_STB_GLOBAL 1u
#define BOXASM_STB_WEAK 2u
#define BOXASM_STT_NOTYPE 0u
#define BOXASM_STT_OBJECT 1u
#define BOXASM_STT_FUNC 2u
#define BOXASM_STT_SECTION 3u
#define BOXASM_STT_FILE 4u
#define BOXASM_STT_TLS 6u

#define BOXASM_PT_LOAD 1u
#define BOXASM_PT_TLS 7u
#define BOXASM_PF_X 1u
#define BOXASM_PF_W 2u
#define BOXASM_PF_R 4u

#define BOXASM_R_X86_64_32 10u
#define BOXASM_R_X86_64_32S 11u
#define BOXASM_R_X86_64_64 1u
#define BOXASM_R_X86_64_PC32 2u
#define BOXASM_R_X86_64_PLT32 4u
#define BOXASM_R_X86_64_TPOFF32 23u

#define BOXASM_SYMBOL_BIND(Info) ((uint8_t)((Info) >> 4))
#define BOXASM_SYMBOL_TYPE(Info) ((uint8_t)((Info) & 0x0Fu))
#define BOXASM_SYMBOL_INFO(Bind, Type) \
    (uint8_t)((((uint8_t)(Bind)) << 4) | ((uint8_t)(Type) & 0x0Fu))
#define BOXASM_RELA_INFO(Symbol, Type) \
    ((((uint64_t)(Symbol)) << 32) | ((uint64_t)(Type) & 0xFFFFFFFFull))
#define BOXASM_RELA_SYMBOL(Info) ((uint32_t)((Info) >> 32))
#define BOXASM_RELA_TYPE(Info) ((uint32_t)(Info))

int BoxAsmElfOpen(const BoxAsmBlob *Blob, uint16_t ExpectedType,
                  BoxAsmElfObject *Out, char *Error, size_t ErrorCapacity);
const char *BoxAsmElfSectionName(const BoxAsmElfObject *Object, uint16_t Index);
const uint8_t *BoxAsmElfSectionData(const BoxAsmElfObject *Object,
                                    uint16_t Index);
const char *BoxAsmElfSymbolName(const BoxAsmElfObject *Object, uint32_t Index);

#endif
