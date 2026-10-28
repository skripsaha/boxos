#include "boxasm_link_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* FNV-1a: names are short and the table only needs an even spread. */
static uint32_t BoxAsmNameHash(const char *Name)
{
    uint32_t Hash = 2166136261u;

    while (*Name != '\0') {
        Hash ^= (uint8_t)*Name++;
        Hash *= 16777619u;
    }
    return Hash;
}

/* The slot holding Name, or the empty slot where it belongs. The table is
 * never full, so the probe always ends. */
static BoxAsmGlobal *BoxAsmGlobalSlot(const BoxAsmGlobalMap *Map,
                                      const char *Name, uint32_t Hash)
{
    size_t Mask = Map->Capacity - 1u;
    size_t At = Hash & Mask;

    for (;;) {
        BoxAsmGlobal *Slot = &Map->Slots[At];
        if (!Slot->Name || (Slot->Hash == Hash && strcmp(Slot->Name, Name) == 0))
            return Slot;
        At = (At + 1u) & Mask;
    }
}

static int BoxAsmGlobalGrow(BoxAsmGlobalMap *Map)
{
    size_t Capacity = Map->Capacity ? Map->Capacity * 2u : 256u;
    BoxAsmGlobal *Old = Map->Slots;
    size_t OldCapacity = Map->Capacity;
    size_t Index;

    if (Capacity < Map->Capacity || Capacity > SIZE_MAX / sizeof(BoxAsmGlobal))
        return -ERR_NO_MEMORY;
    Map->Slots = (BoxAsmGlobal *)malloc(Capacity * sizeof(BoxAsmGlobal));
    if (!Map->Slots) {
        Map->Slots = Old;
        return -ERR_NO_MEMORY;
    }
    memset(Map->Slots, 0, Capacity * sizeof(BoxAsmGlobal));
    Map->Capacity = Capacity;
    for (Index = 0; Index < OldCapacity; Index++)
        if (Old[Index].Name)
            *BoxAsmGlobalSlot(Map, Old[Index].Name, Old[Index].Hash) = Old[Index];
    free(Old);
    return 0;
}

/* The slot for Name, made when absent; *Fresh says which. */
static int BoxAsmGlobalClaim(BoxAsmGlobalMap *Map, const char *Name,
                             BoxAsmGlobal **Out, uint8_t *Fresh)
{
    uint32_t Hash = BoxAsmNameHash(Name);
    BoxAsmGlobal *Slot;

    if ((Map->Count + 1u) * 4u > Map->Capacity * 3u &&
        BoxAsmGlobalGrow(Map) != 0)
        return -ERR_NO_MEMORY;
    Slot = BoxAsmGlobalSlot(Map, Name, Hash);
    *Fresh = Slot->Name == NULL;
    if (*Fresh) {
        memset(Slot, 0, sizeof(*Slot));
        Slot->Name = Name;
        Slot->Hash = Hash;
        Map->Count++;
    }
    *Out = Slot;
    return 0;
}

const BoxAsmGlobal *BoxAsmGlobalFind(const BoxAsmGlobalMap *Map, const char *Name)
{
    const BoxAsmGlobal *Slot;

    if (Map->Capacity == 0 || !Name)
        return NULL;
    Slot = BoxAsmGlobalSlot(Map, Name, BoxAsmNameHash(Name));
    return Slot->Name ? Slot : NULL;
}

void BoxAsmGlobalRelease(BoxAsmGlobalMap *Map)
{
    free(Map->Slots);
    memset(Map, 0, sizeof(*Map));
}

static int BoxAsmSymbolIsDefinition(const BoxAsmElfObject *Object, uint32_t Index)
{
    const BoxAsmElfSymbol *Symbol = &Object->Symbols[Index];
    uint8_t Bind = BOXASM_SYMBOL_BIND(Symbol->Info);
    const char *Name = BoxAsmElfSymbolName(Object, Index);

    return (Bind == BOXASM_STB_GLOBAL || Bind == BOXASM_STB_WEAK) &&
           Symbol->SectionIndex != BOXASM_SHN_UNDEF && Name && Name[0] != '\0';
}

int BoxAsmSymbolPrevails(const BoxAsmLinker *Link, uint32_t ObjectIndex,
                         uint32_t SymbolIndex)
{
    const BoxAsmElfObject *Object = &Link->Objects.Items[ObjectIndex].Elf;
    const BoxAsmGlobal *Global;

    if (BOXASM_SYMBOL_BIND(Object->Symbols[SymbolIndex].Info) == BOXASM_STB_LOCAL)
        return 1;
    Global = BoxAsmGlobalFind(&Link->Globals,
                              BoxAsmElfSymbolName(Object, SymbolIndex));
    return Global && Global->Object == ObjectIndex && Global->Symbol == SymbolIndex;
}

static int BoxAsmRefuseDefinition(BoxAsmLinker *Link, const char *Name,
                                  const char *Why, const BoxAsmLinkObject *First,
                                  const BoxAsmLinkObject *Second)
{
    BoxAsmMessage Message;

    BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
    BoxAsmMessageName(&Message, Name);
    BoxAsmMessageText(&Message, Why);
    BoxAsmMessageText(&Message, First->Name);
    if (Second) {
        BoxAsmMessageText(&Message, " and in ");
        BoxAsmMessageText(&Message, Second->Name);
    }
    return -ERR_INVALID_ARGUMENT;
}

/* Every global and weak definition of a selected object enters the table. A
 * strong definition outranks a weak one; two strong ones are refused, as are
 * COMMON symbols, which this linker does not allocate. */
static int BoxAsmDefineObject(BoxAsmLinker *Link, uint32_t ObjectIndex)
{
    BoxAsmLinkObject *Object = &Link->Objects.Items[ObjectIndex];
    uint32_t Index;

    for (Index = 0; Index < Object->Elf.SymbolCount; Index++) {
        const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[Index];
        const char *Name;
        uint8_t Strong;
        uint8_t Fresh;
        BoxAsmGlobal *Slot;

        if (!BoxAsmSymbolIsDefinition(&Object->Elf, Index))
            continue;
        Name = BoxAsmElfSymbolName(&Object->Elf, Index);
        if (Symbol->SectionIndex == BOXASM_SHN_COMMON)
            return BoxAsmRefuseDefinition(Link, Name,
                " is a COMMON symbol, which this linker does not allocate, in ",
                Object, NULL);
        Strong = BOXASM_SYMBOL_BIND(Symbol->Info) == BOXASM_STB_GLOBAL;
        if (BoxAsmGlobalClaim(&Link->Globals, Name, &Slot, &Fresh) != 0)
            return -ERR_NO_MEMORY;
        if (!Fresh && Slot->Strong && Strong)
            return BoxAsmRefuseDefinition(Link, Name, " is defined twice: in ",
                                          &Link->Objects.Items[Slot->Object],
                                          Object);
        if (Fresh || (Strong && !Slot->Strong)) {
            Slot->Name = Name;
            Slot->Object = ObjectIndex;
            Slot->Symbol = Index;
            Slot->Strong = Strong;
        }
    }
    return 0;
}

/* For each name an unselected archive member defines, the first member that
 * defines it — the member an undefined reference pulls in. */
static int BoxAsmIndexArchive(const BoxAsmLinker *Link, BoxAsmGlobalMap *Archive)
{
    size_t ObjectIndex;

    for (ObjectIndex = 0; ObjectIndex < Link->Objects.Count; ObjectIndex++) {
        const BoxAsmLinkObject *Object = &Link->Objects.Items[ObjectIndex];
        uint32_t Index;

        if (Object->Selected)
            continue;
        for (Index = 0; Index < Object->Elf.SymbolCount; Index++) {
            BoxAsmGlobal *Slot;
            uint8_t Fresh;

            if (!BoxAsmSymbolIsDefinition(&Object->Elf, Index))
                continue;
            if (BoxAsmGlobalClaim(Archive, BoxAsmElfSymbolName(&Object->Elf, Index),
                                  &Slot, &Fresh) != 0)
                return -ERR_NO_MEMORY;
            if (Fresh) {
                Slot->Object = (uint32_t)ObjectIndex;
                Slot->Symbol = Index;
            }
        }
    }
    return 0;
}

/* The objects given directly are linked whole. An archive member is linked
 * when it defines a name a linked object needs with a strong reference and no
 * linked object defines — the static-archive rule GNU ld keeps: a weak
 * reference pulls nothing, and a weak definition already linked satisfies. */
int BoxAsmLinkSelect(BoxAsmLinker *Link)
{
    BoxAsmGlobalMap Archive = { 0 };
    uint32_t *Queue;
    size_t Head = 0;
    size_t Tail = 0;
    size_t Index;
    int Rc;

    if (Link->Objects.Count > UINT32_MAX)
        return -ERR_INVALID_ARGUMENT;
    Queue = (uint32_t *)malloc((Link->Objects.Count ? Link->Objects.Count : 1u) *
                               sizeof(uint32_t));
    if (!Queue)
        return -ERR_NO_MEMORY;
    Rc = BoxAsmIndexArchive(Link, &Archive);
    for (Index = 0; Rc == 0 && Index < Link->Objects.Count; Index++) {
        if (!Link->Objects.Items[Index].Selected)
            continue;
        Rc = BoxAsmDefineObject(Link, (uint32_t)Index);
        Queue[Tail++] = (uint32_t)Index;
    }
    while (Rc == 0 && Head < Tail) {
        const BoxAsmLinkObject *Object = &Link->Objects.Items[Queue[Head++]];
        uint32_t Symbol;

        for (Symbol = 0; Rc == 0 && Symbol < Object->Elf.SymbolCount; Symbol++) {
            const BoxAsmElfSymbol *Entry = &Object->Elf.Symbols[Symbol];
            const char *Name = BoxAsmElfSymbolName(&Object->Elf, Symbol);
            const BoxAsmGlobal *Member;

            if (Entry->SectionIndex != BOXASM_SHN_UNDEF ||
                BOXASM_SYMBOL_BIND(Entry->Info) != BOXASM_STB_GLOBAL ||
                !Name || Name[0] == '\0' ||
                BoxAsmGlobalFind(&Link->Globals, Name))
                continue;
            Member = BoxAsmGlobalFind(&Archive, Name);
            if (!Member || Link->Objects.Items[Member->Object].Selected)
                continue;
            Link->Objects.Items[Member->Object].Selected = 1;
            Rc = BoxAsmDefineObject(Link, Member->Object);
            Queue[Tail++] = Member->Object;
        }
    }
    BoxAsmGlobalRelease(&Archive);
    free(Queue);
    return Rc;
}
