#ifndef LINKER_SYMBOLS_H
#define LINKER_SYMBOLS_H

#include "ktypes.h"

extern char _text_start[];
extern char _text_end[];
extern uintptr_t _kernel_phys_end;

extern uint8_t ap_trampoline_start;
extern uint8_t ap_trampoline_end;
extern uint8_t ap_trampoline_data;

extern uint8_t _binary_shell_stripped_elf_start[];
extern uint8_t _binary_shell_stripped_elf_end[];

#endif