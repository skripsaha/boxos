#ifndef CONTEXT_SWITCH_H
#define CONTEXT_SWITCH_H

#include "process.h"
#include "idt.h"

/* The scheduler dispatches via the IRQ frame exclusively. The plain
 * context_save/restore/switch helpers were never called and have been
 * removed; *_from_frame / *_to_frame are the single source of truth. */
void context_save_from_frame(process_t* proc, interrupt_frame_t* frame);
void context_restore_to_frame(process_t* proc, interrupt_frame_t* frame);

#endif
