#ifndef BOXASM_LINK_H
#define BOXASM_LINK_H

#include "boxasm_elf.h"

/* Also write .symtab and .strtab into the image. The images assembled inside
 * BoxOS do not carry them: the Nameplate already names their code at runtime,
 * and nothing on the machine reads a symbol table. A host check asks for them
 * so tools/nameplate can verify the table against the image's own symbols. */
#define BOXASM_LINK_SYMBOLS 0x1u

/* Link the assembled ET_REL object with the entry object and the runtime
 * archive into one ET_EXEC image for the BoxOS loader, laid out by the
 * contract of src/userspace/boxlib/user.ld. Every refusal is written into
 * Error naming the object, section, offset and symbol it concerns.
 *
 * Recipe, when it is not NULL and not empty, is the program's Recipe
 * (box/recipe.h): its bytes go into .boxos.recipe, a section after both
 * segments that no segment maps — it changes no byte the loader loads. */
int BoxAsmLink(const BoxAsmBlob *Entry,
               const BoxAsmBlob *Program,
               const BoxAsmBlob *RuntimeArchive,
               const BoxAsmBlob *Recipe,
               uint32_t Flags,
               BoxAsmBlob *Out,
               char *Error,
               size_t ErrorCapacity);

#endif
