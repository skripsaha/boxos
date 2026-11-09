#ifndef BOXASM_LINK_INTERNAL_H
#define BOXASM_LINK_INTERNAL_H

#include "boxasm_link.h"

#define BOXASM_USER_BASE 0xC000ull   /* where an ELF image is linked: the code start */
#define BOXASM_PAGE      0x1000ull

/* The output sections of src/userspace/boxlib/user.ld, in address order.
 * An output section's header index in the image equals its slot. */
typedef enum {
    BOXASM_SLOT_NONE = 0,
    BOXASM_SLOT_TEXT,
    BOXASM_SLOT_RODATA,
    BOXASM_SLOT_EH_FRAME,
    BOXASM_SLOT_EXCEPT_TABLE,
    BOXASM_SLOT_NAMEPLATE,
    BOXASM_SLOT_FORM,
    BOXASM_SLOT_DATA,
    BOXASM_SLOT_INIT_ARRAY,
    BOXASM_SLOT_FINI_ARRAY,
    BOXASM_SLOT_TDATA,
    BOXASM_SLOT_TBSS,
    BOXASM_SLOT_BSS,
    BOXASM_SLOT_COUNT
} BoxAsmSlot;

typedef struct {
    const char  *Name;
    uint32_t     Type;
    uint64_t     Flags;
    uint64_t     Address;
    uint64_t     Size;
    uint64_t     FileOffset;
    uint64_t     Align;
    BoxAsmBuffer Data;
} BoxAsmOutputSection;

/* The segments of user.ld's PHDRS, each the run of output sections with one
 * set of rights, beginning on a page of its own. */
typedef enum {
    BOXASM_SEGMENT_CODE = 0,    /* .text: R+X */
    BOXASM_SEGMENT_CONST,       /* .rodata to .boxos.form — in a box to .gcc_except_table: R */
    BOXASM_SEGMENT_DATA,        /* .data to .bss: R+W */
    BOXASM_SEGMENT_COUNT
} BoxAsmSegmentKind;

typedef struct {
    uint8_t  First;             /* its first and last slot */
    uint8_t  Last;
    uint32_t Rights;            /* BOXASM_PF_* */
    uint64_t Address;
    uint64_t FileBytes;         /* to the end of its last section with bytes */
    uint64_t Memory;            /* to the end of its last section; 0: the image has none of it */
} BoxAsmSegment;

typedef struct {
    BoxAsmElfObject Elf;
    char           *Name;          /* "hello.asm", "libbox.a(print.o)"; owned */
    uint8_t         Selected;
    uint16_t       *SectionSlot;   /* per input section; BOXASM_SLOT_NONE = not emitted */
    uint64_t       *SectionOffset; /* per input section, inside its output section */
} BoxAsmLinkObject;

typedef struct {
    BoxAsmLinkObject *Items;
    size_t            Count;
    size_t            Capacity;
} BoxAsmObjectList;

/* A global name and the definition that prevails for it. Name points into the
 * defining object's string table. */
typedef struct {
    const char *Name;
    uint32_t    Object;
    uint32_t    Symbol;
    uint8_t     Strong;
} BoxAsmGlobal;

typedef struct {
    BoxAsmNameIndex Index;      /* name -> position in Items */
    BoxAsmGlobal   *Items;
    size_t          Count;
    size_t          Capacity;
} BoxAsmGlobalMap;

/* The places, from the base, of the words a box's kernel adds the base to. */
typedef struct {
    uint64_t *Places;
    size_t    Count;
    size_t    Capacity;
} BoxAsmFixups;

typedef struct {
    BoxAsmOutputSection Sections[BOXASM_SLOT_COUNT];
    uint8_t      Box;            /* a box: linked at 0, its addresses fixed up wherever it goes,
                                    the Nameplate and the Form parts of their own */
    uint64_t     Base;           /* 0 for a box, BOXASM_USER_BASE for an ELF image */
    BoxAsmFixups Fixups;
    uint64_t     TlsAddress;
    uint64_t     TlsMemorySize;
    uint64_t     TlsAlign;
    uint64_t     ThreadPointer;  /* variant II: the aligned end of the TLS block */
    uint64_t     ProgramEnd;
    uint64_t     Entry;
    BoxAsmBuffer Symbols;        /* .symtab; empty unless BOXASM_LINK_SYMBOLS */
    BoxAsmBuffer SymbolNames;    /* .strtab */
    uint32_t     FirstGlobal;
} BoxAsmLinkImage;

typedef struct {
    BoxAsmObjectList Objects;
    BoxAsmGlobalMap  Globals;
    BoxAsmLinkImage  Image;
    const BoxAsmBlob *Recipe;          /* written as .boxos.recipe; NULL for none */
    const char      *ArchiveName;
    char            *Error;
    size_t           ErrorCapacity;
} BoxAsmLinker;

/* A symbol followed to its prevailing definition. Object is NULL for a name
 * the linker itself defines and for a weak name nobody defines. Moves: the
 * address is a place in the image, which moves with its base — not an
 * absolute value or the zero of a weak name nobody defines. */
typedef struct {
    uint64_t                Address;
    const BoxAsmLinkObject *Object;
    const BoxAsmElfSymbol  *Symbol;
    uint8_t                 Moves;
} BoxAsmResolved;

/* boxasm_link_symbols.c */
const BoxAsmGlobal *BoxAsmGlobalFind(const BoxAsmGlobalMap *Map, const char *Name);
void BoxAsmGlobalRelease(BoxAsmGlobalMap *Map);
int  BoxAsmLinkSelect(BoxAsmLinker *Link);
int  BoxAsmSymbolPrevails(const BoxAsmLinker *Link, uint32_t ObjectIndex,
                          uint32_t SymbolIndex);

/* boxasm_link.c */
int  BoxAsmSymbolAddress(const BoxAsmLinker *Link, const BoxAsmLinkObject *Object,
                         const BoxAsmElfSymbol *Symbol, uint64_t *Out);
/* The segment as the layout made it, and its bytes: FileBytes of them, its
 * sections' with zeros between, written at Into. */
void BoxAsmSegmentOf(const BoxAsmLinkImage *Image, BoxAsmSegmentKind Kind, BoxAsmSegment *Out);
void BoxAsmSegmentCopy(const BoxAsmLinkImage *Image, const BoxAsmSegment *Segment, uint8_t *Into);

/* boxasm_link_names.c */
int BoxAsmBuildNameplate(BoxAsmLinker *Link);
int BoxAsmBuildSymbolTable(BoxAsmLinker *Link);

/* boxasm_link_elf.c */
int BoxAsmWriteElf(BoxAsmLinker *Link, BoxAsmBlob *Out);

/* boxasm_link_box.c */
int  BoxAsmFixupAdd(BoxAsmFixups *Fixups, uint64_t Place);
void BoxAsmFixupsRelease(BoxAsmFixups *Fixups);
int  BoxAsmWriteBox(BoxAsmLinker *Link, BoxAsmBlob *Out);

#endif
