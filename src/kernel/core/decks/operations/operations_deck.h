#ifndef OPERATIONS_DECK_H
#define OPERATIONS_DECK_H

#include "events.h"
#include "boxos_decks.h"

#define OPERATIONS_DECK_ID DECK_OPERATIONS

#define OP_BUF_MOVE     0x01  // Move block of bytes
#define OP_BUF_FILL     0x02  // Fill memory with byte (memset)
#define OP_BUF_XOR      0x03  // XOR operation
#define OP_BUF_HASH     0x04  // Compute ROL5 hash
#define OP_BUF_CMP      0x05  // Compare two regions
#define OP_BUF_FIND     0x06  // Find pattern
#define OP_BUF_PACK     0x07  // RLE compress
#define OP_BUF_UNPACK   0x08  // RLE decompress
#define OP_BIT_SWAP     0x09  // Endianness swap
#define OP_VAL_ADD      0x0A  // Increment/decrement value

int operations_deck_handler(Event* event);

#endif // OPERATIONS_DECK_H
