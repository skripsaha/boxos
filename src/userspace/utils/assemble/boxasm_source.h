#ifndef BOXASM_SOURCE_H
#define BOXASM_SOURCE_H

#include "boxasm_elf.h"

/* Assemble BoxASM source into an x86-64 ELF relocatable object: the program
 * must define `main` in .text and declare it global. FileName names the
 * source in diagnostics, which are written into Error as
 * "file:line:column: what is wrong", the line itself and a caret under the
 * place. The object is heap-owned by Out->Data. */
int BoxAsmSourceAssemble(const char *Source, size_t SourceSize, const char *FileName,
                         BoxAsmBlob *Out, char *Error, size_t ErrorCapacity);

#endif
