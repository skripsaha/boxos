#ifndef OPERATIONS_DECK_H
#define OPERATIONS_DECK_H

#include "ktypes.h"
#include "boxos_decks.h"
#include "error.h"

#define OPERATIONS_DECK_ID DECK_OPERATIONS

/* Manifest opcodes (used by operations_ops.c handlers + boxlib wrappers). */
#define OP_BUF_MOVE     0x01
#define OP_BUF_FILL     0x02
#define OP_BUF_XOR      0x03
#define OP_BUF_HASH     0x04
#define OP_BUF_CMP      0x05
#define OP_BUF_FIND     0x06
#define OP_BUF_PACK     0x07
#define OP_BUF_UNPACK   0x08
#define OP_BIT_SWAP     0x09
#define OP_VAL_ADD      0x0A

error_t OperationsDeckRegister(void);

#endif /* OPERATIONS_DECK_H */
