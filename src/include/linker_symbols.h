#ifndef LINKER_SYMBOLS_H
#define LINKER_SYMBOLS_H

#include "ktypes.h"

// Linker script section boundaries
extern char _text_start[];
extern char _text_end[];
extern uintptr_t _kernel_phys_end;

// AP trampoline (assembled, linked as binary blob)
extern uint8_t ap_trampoline_start;
extern uint8_t ap_trampoline_end;
extern uint8_t ap_trampoline_data;

#endif // LINKER_SYMBOLS_H
