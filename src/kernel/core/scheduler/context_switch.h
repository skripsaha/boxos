#ifndef CONTEXT_SWITCH_H
#define CONTEXT_SWITCH_H

#include "process.h"
#include "idt.h"

void context_save_from_frame(process_t* proc, interrupt_frame_t* frame);
void context_restore_to_frame(process_t* proc, interrupt_frame_t* frame);

#endif