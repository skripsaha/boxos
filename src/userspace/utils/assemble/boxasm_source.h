#ifndef BOXASM_SOURCE_H
#define BOXASM_SOURCE_H

#include "boxasm_elf.h"

/* Assemble the deliberately small, complete BoxASM v0 source language:
 *
 *   section .text
 *   global main
 *   main:
 *       box.say "Hello from BoxOS"
 *       box.exit 0
 *
 * Unknown syntax is an error.  The result owns its bytes and must be released
 * with BoxAsmBufferRelease(). */
int BoxAsmSourceAssemble(const char *Source, size_t SourceSize,
                         BoxAsmBlob *Out, char *Error, size_t ErrorCapacity);

#endif
