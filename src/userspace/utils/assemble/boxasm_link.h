#ifndef BOXASM_LINK_H
#define BOXASM_LINK_H

#include "boxasm_elf.h"

/* Link the assembled ET_REL object with the existing entry object and the
 * embedded BoxLib archive.  The result is one ET_EXEC image suitable for the
 * current BoxOS loader. */
int BoxAsmLink(const BoxAsmBlob *Entry,
               const BoxAsmBlob *Program,
               const BoxAsmBlob *RuntimeArchive,
               BoxAsmBlob *Out,
               char *Error,
               size_t ErrorCapacity);

#endif
