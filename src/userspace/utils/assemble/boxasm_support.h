#ifndef BOXASM_SUPPORT_H
#define BOXASM_SUPPORT_H

#include "box/defs.h"

/* A growable byte buffer; its holder owns Data and releases it. */
typedef struct {
    uint8_t *Data;
    uint64_t Size;
    uint64_t Capacity;
} BoxAsmBuffer;

int  BoxAsmBufferReserve(BoxAsmBuffer *Buffer, uint64_t Additional);
int  BoxAsmBufferAppend(BoxAsmBuffer *Buffer, const void *Data, uint64_t Size);
int  BoxAsmBufferAppendZeroes(BoxAsmBuffer *Buffer, uint64_t Size);
int  BoxAsmBufferAlign(BoxAsmBuffer *Buffer, uint64_t Alignment);
void BoxAsmBufferRelease(BoxAsmBuffer *Buffer);

/* Value rounded up to Alignment (0 and 1 mean none). UINT64_MAX when the
 * alignment is not a power of two or the rounded value does not fit. */
uint64_t BoxAsmAlignUp(uint64_t Value, uint64_t Alignment);

/* Whether Length bytes at Offset lie inside an object of Size bytes. */
int BoxAsmRangeValid(uint64_t Size, uint64_t Offset, uint64_t Length);

/* A heap copy of Length bytes of Text followed by a NUL, or NULL. */
char *BoxAsmCopyText(const char *Text, size_t Length);

/* A diagnostic built piece by piece into the caller's buffer. Every piece is
 * typed, so no format string can disagree with its arguments. A message that
 * does not fit ends in "..." and is never mistaken for the whole. A message
 * begun on a NULL buffer is accepted and discarded. */
typedef struct {
    char   *Text;
    size_t  Capacity;
    size_t  Length;
} BoxAsmMessage;

void BoxAsmMessageBegin(BoxAsmMessage *Message, char *Buffer, size_t Capacity);
void BoxAsmMessageText(BoxAsmMessage *Message, const char *Text);
void BoxAsmMessageName(BoxAsmMessage *Message, const char *Name);
void BoxAsmMessageDecimal(BoxAsmMessage *Message, int64_t Value);
void BoxAsmMessageHex(BoxAsmMessage *Message, uint64_t Value);

/* The whole buffer replaced by one sentence. */
void BoxAsmMessageSet(char *Buffer, size_t Capacity, const char *Text);

#endif
