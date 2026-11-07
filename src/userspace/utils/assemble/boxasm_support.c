#include "boxasm_support.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

int BoxAsmBufferReserve(BoxAsmBuffer *Buffer, uint64_t Additional)
{
    uint64_t Needed;
    uint64_t Capacity;
    uint8_t *Fresh;

    if (!Buffer || Additional > UINT64_MAX - Buffer->Size)
        return -ERR_INVALID_ARGUMENT;
    Needed = Buffer->Size + Additional;
    if (Needed <= Buffer->Capacity)
        return 0;
    Capacity = Buffer->Capacity ? Buffer->Capacity : 256u;
    while (Capacity < Needed)
        Capacity = Capacity > UINT64_MAX / 2u ? Needed : Capacity * 2u;
    if (Capacity > SIZE_MAX)
        return -ERR_NO_MEMORY;
    Fresh = (uint8_t *)realloc(Buffer->Data, (size_t)Capacity);
    if (!Fresh)
        return -ERR_NO_MEMORY;
    Buffer->Data = Fresh;
    Buffer->Capacity = Capacity;
    return 0;
}

int BoxAsmBufferAppend(BoxAsmBuffer *Buffer, const void *Data, uint64_t Size)
{
    int Rc;

    if (!Buffer || (Size != 0 && !Data))
        return -ERR_INVALID_ARGUMENT;
    Rc = BoxAsmBufferReserve(Buffer, Size);
    if (Rc != 0)
        return Rc;
    if (Size != 0)
        memcpy(Buffer->Data + Buffer->Size, Data, (size_t)Size);
    Buffer->Size += Size;
    return 0;
}

int BoxAsmBufferAppendZeroes(BoxAsmBuffer *Buffer, uint64_t Size)
{
    int Rc;

    if (!Buffer)
        return -ERR_INVALID_ARGUMENT;
    Rc = BoxAsmBufferReserve(Buffer, Size);
    if (Rc != 0)
        return Rc;
    if (Size != 0)
        memset(Buffer->Data + Buffer->Size, 0, (size_t)Size);
    Buffer->Size += Size;
    return 0;
}

int BoxAsmBufferAlign(BoxAsmBuffer *Buffer, uint64_t Alignment)
{
    uint64_t Aligned;

    if (!Buffer)
        return -ERR_INVALID_ARGUMENT;
    Aligned = BoxAsmAlignUp(Buffer->Size, Alignment);
    if (Aligned == UINT64_MAX)
        return -ERR_INVALID_ARGUMENT;
    return BoxAsmBufferAppendZeroes(Buffer, Aligned - Buffer->Size);
}

void BoxAsmBufferRelease(BoxAsmBuffer *Buffer)
{
    if (!Buffer)
        return;
    free(Buffer->Data);
    memset(Buffer, 0, sizeof(*Buffer));
}

uint64_t BoxAsmAlignUp(uint64_t Value, uint64_t Alignment)
{
    uint64_t Mask;

    if (Alignment <= 1u)
        return Value;
    if ((Alignment & (Alignment - 1u)) != 0)
        return UINT64_MAX;
    Mask = Alignment - 1u;
    if (Value > UINT64_MAX - Mask)
        return UINT64_MAX;
    return (Value + Mask) & ~Mask;
}

int BoxAsmRangeValid(uint64_t Size, uint64_t Offset, uint64_t Length)
{
    return Offset <= Size && Length <= Size - Offset;
}

char *BoxAsmCopyText(const char *Text, size_t Length)
{
    char *Copy;

    if (!Text || Length == SIZE_MAX)
        return NULL;
    Copy = (char *)malloc(Length + 1u);
    if (!Copy)
        return NULL;
    memcpy(Copy, Text, Length);
    Copy[Length] = '\0';
    return Copy;
}

/* FNV-1a: names are short and the index only needs an even spread. */
static uint32_t BoxAsmNameHash(const char *Name)
{
    uint32_t Hash = 2166136261u;

    while (*Name != '\0') {
        Hash ^= (uint8_t)*Name++;
        Hash *= 16777619u;
    }
    return Hash;
}

static BoxAsmNameSlot *BoxAsmNameIndexSlot(const BoxAsmNameIndex *Index,
                                           const char *Name, uint32_t Hash)
{
    size_t Mask = Index->Capacity - 1u;
    size_t At = Hash & Mask;

    for (;;) {
        BoxAsmNameSlot *Slot = &Index->Slots[At];
        if (!Slot->Name || (Slot->Hash == Hash && strcmp(Slot->Name, Name) == 0))
            return Slot;
        At = (At + 1u) & Mask;
    }
}

static int BoxAsmNameIndexGrow(BoxAsmNameIndex *Index)
{
    size_t Capacity = Index->Capacity ? Index->Capacity * 2u : 256u;
    BoxAsmNameSlot *Old = Index->Slots;
    size_t OldCapacity = Index->Capacity;
    size_t At;

    if (Capacity < Index->Capacity || Capacity > SIZE_MAX / sizeof(BoxAsmNameSlot))
        return -ERR_NO_MEMORY;
    Index->Slots = (BoxAsmNameSlot *)malloc(Capacity * sizeof(BoxAsmNameSlot));
    if (!Index->Slots) {
        Index->Slots = Old;
        return -ERR_NO_MEMORY;
    }
    memset(Index->Slots, 0, Capacity * sizeof(BoxAsmNameSlot));
    Index->Capacity = Capacity;
    for (At = 0; At < OldCapacity; At++)
        if (Old[At].Name)
            *BoxAsmNameIndexSlot(Index, Old[At].Name, Old[At].Hash) = Old[At];
    free(Old);
    return 0;
}

int BoxAsmNameIndexClaim(BoxAsmNameIndex *Index, const char *Name,
                         uint32_t **Value, uint8_t *Fresh)
{
    uint32_t Hash = BoxAsmNameHash(Name);
    BoxAsmNameSlot *Slot;

    if ((Index->Count + 1u) * 4u > Index->Capacity * 3u &&
        BoxAsmNameIndexGrow(Index) != 0)
        return -ERR_NO_MEMORY;
    Slot = BoxAsmNameIndexSlot(Index, Name, Hash);
    *Fresh = Slot->Name == NULL;
    if (*Fresh) {
        Slot->Name = Name;
        Slot->Hash = Hash;
        Slot->Value = 0;
        Index->Count++;
    }
    *Value = &Slot->Value;
    return 0;
}

int BoxAsmNameIndexFind(const BoxAsmNameIndex *Index, const char *Name,
                        uint32_t *Value)
{
    const BoxAsmNameSlot *Slot;

    if (Index->Capacity == 0 || !Name)
        return 0;
    Slot = BoxAsmNameIndexSlot(Index, Name, BoxAsmNameHash(Name));
    if (!Slot->Name)
        return 0;
    *Value = Slot->Value;
    return 1;
}

void BoxAsmNameIndexRelease(BoxAsmNameIndex *Index)
{
    free(Index->Slots);
    memset(Index, 0, sizeof(*Index));
}

void BoxAsmMessageBegin(BoxAsmMessage *Message, char *Buffer, size_t Capacity)
{
    Message->Text = Capacity != 0 ? Buffer : NULL;
    Message->Capacity = Buffer ? Capacity : 0;
    Message->Length = 0;
    if (Message->Text)
        Message->Text[0] = '\0';
}

static void BoxAsmMessagePut(BoxAsmMessage *Message, const char *Text,
                             size_t Length)
{
    size_t Room;

    if (!Message->Text)
        return;
    Room = Message->Capacity - 1u - Message->Length;
    if (Length <= Room) {
        memcpy(Message->Text + Message->Length, Text, Length);
        Message->Length += Length;
        Message->Text[Message->Length] = '\0';
        return;
    }
    memcpy(Message->Text + Message->Length, Text, Room);
    Message->Length = Message->Capacity - 1u;
    Message->Text[Message->Length] = '\0';
    if (Message->Length >= 3u)
        memcpy(Message->Text + Message->Length - 3u, "...", 3u);
}

void BoxAsmMessageText(BoxAsmMessage *Message, const char *Text)
{
    if (Text)
        BoxAsmMessagePut(Message, Text, strlen(Text));
}

void BoxAsmMessageName(BoxAsmMessage *Message, const char *Name)
{
    BoxAsmMessagePut(Message, "'", 1u);
    BoxAsmMessageText(Message, Name ? Name : "(unnamed)");
    BoxAsmMessagePut(Message, "'", 1u);
}

void BoxAsmMessageSpan(BoxAsmMessage *Message, const char *Text, size_t Length)
{
    BoxAsmMessagePut(Message, Text, Length);
}

void BoxAsmMessageDecimal(BoxAsmMessage *Message, int64_t Value)
{
    char Digits[20];
    size_t Count = 0;
    uint64_t Magnitude = Value < 0 ? (uint64_t)0 - (uint64_t)Value
                                   : (uint64_t)Value;

    do {
        Digits[Count++] = (char)('0' + Magnitude % 10u);
        Magnitude /= 10u;
    } while (Magnitude != 0);
    if (Value < 0)
        BoxAsmMessagePut(Message, "-", 1u);
    while (Count != 0)
        BoxAsmMessagePut(Message, &Digits[--Count], 1u);
}

void BoxAsmMessageHex(BoxAsmMessage *Message, uint64_t Value)
{
    static const char Alphabet[] = "0123456789abcdef";
    char Digits[16];
    size_t Count = 0;

    do {
        Digits[Count++] = Alphabet[Value & 0xFu];
        Value >>= 4;
    } while (Value != 0);
    BoxAsmMessagePut(Message, "0x", 2u);
    while (Count != 0)
        BoxAsmMessagePut(Message, &Digits[--Count], 1u);
}

void BoxAsmMessageSet(char *Buffer, size_t Capacity, const char *Text)
{
    BoxAsmMessage Message;

    BoxAsmMessageBegin(&Message, Buffer, Capacity);
    BoxAsmMessageText(&Message, Text);
}
