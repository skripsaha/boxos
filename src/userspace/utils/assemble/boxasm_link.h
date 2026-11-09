#ifndef BOXASM_LINK_H
#define BOXASM_LINK_H

#include "boxasm_elf.h"

/* Also write .symtab and .strtab into an ELF image. The programs assembled
 * inside BoxOS do not carry them: the Nameplate already names their code at
 * runtime, and nothing on the machine reads a symbol table. A host check asks
 * for them so tools/nameplate can verify the table against the image's own
 * symbols. A box has no place for them. */
#define BOXASM_LINK_SYMBOLS 0x1u

/* An ELF image (ET_EXEC) at the code start instead of a box: what a program
 * named name.elf is made as. */
#define BOXASM_LINK_ELF 0x2u

/* Link the assembled ET_REL object with the entry object and the runtime
 * archive into one program for the BoxOS loader, laid out by the contract of
 * src/userspace/boxlib/user.ld: a box (src/include/box_format.h) linked at 0,
 * which the kernel places where it chooses — every word that holds an
 * address is one of its fixups, and a reference that would not hold at
 * another base is refused — or, with BOXASM_LINK_ELF, an image at the code
 * start. Every refusal is written into Error naming the object, section,
 * offset and symbol it concerns.
 *
 * Recipe, when it is not NULL and not empty, is the program's Recipe
 * (box/recipe.h): a box's part recipe, or in an ELF image .boxos.recipe, a
 * section after the segments that none maps — it changes no byte the loader
 * loads. */
int BoxAsmLink(const BoxAsmBlob *Entry,
               const BoxAsmBlob *Program,
               const BoxAsmBlob *RuntimeArchive,
               const BoxAsmBlob *Recipe,
               uint32_t Flags,
               BoxAsmBlob *Out,
               char *Error,
               size_t ErrorCapacity);

#endif
