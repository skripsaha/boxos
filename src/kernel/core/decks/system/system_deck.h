#ifndef SYSTEM_DECK_H
#define SYSTEM_DECK_H

#include "ktypes.h"
#include "boxos_decks.h"
#include "pocket.h"

#define SYSTEM_OP_PROC_SPAWN        0x01
#define SYSTEM_OP_PROC_KILL         0x02
#define SYSTEM_OP_PROC_INFO         0x03
#define SYSTEM_OP_CTX_USE           0x04
#define SYSTEM_OP_PROC_EXEC         0x06
#define SYSTEM_OP_BUF_ALLOC         0x10
#define SYSTEM_OP_BUF_FREE          0x11
#define SYSTEM_OP_BUF_RESIZE        0x12
#define SYSTEM_OP_DEFRAG_FILE       0x18
#define SYSTEM_OP_FRAG_SCORE        0x19
#define SYSTEM_OP_TAG_ADD           0x20
#define SYSTEM_OP_TAG_REMOVE        0x21
#define SYSTEM_OP_TAG_CHECK         0x22
#define SYSTEM_OP_ROUTE             0x40
#define SYSTEM_OP_ROUTE_TAG         0x41
#define SYSTEM_OP_LISTEN            0x42

int  system_deck_handler(Pocket* pocket);
bool system_security_gate(uint32_t pid, uint8_t deck_id, uint8_t opcode);
void system_deck_cleanup_process_buffers(uint32_t pid);

#endif // SYSTEM_DECK_H
