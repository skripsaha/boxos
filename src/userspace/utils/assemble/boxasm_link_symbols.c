#include "boxasm_link_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* The entry for Name, made when absent; *Fresh says which. */
static int BoxAsmGlobalClaim(BoxAsmGlobalMap *Map, const char *Name,
                             BoxAsmGlobal **Out, uint8_t *Fresh)
{
    uint32_t *Position;

    if (BoxAsmNameIndexClaim(&Map->Index, Name, &Position, Fresh) != 0)
        return -ERR_NO_MEMORY;
    if (*Fresh) {
        if (Map->Count == Map->Capacity) {
            size_t Capacity = Map->Capacity ? Map->Capacity * 2u : 256u;
            BoxAsmGlobal *Grown;

            if (Capacity > UINT32_MAX || Capacity > SIZE_MAX / sizeof(*Grown))
                return -ERR_NO_MEMORY;
            Grown = (BoxAsmGlobal *)realloc(Map->Items, Capacity * sizeof(*Grown));
            if (!Grown)
                return -ERR_NO_MEMORY;
            Map->Items = Grown;
            Map->Capacity = Capacity;
        }
        *Position = (uint32_t)Map->Count;
        memset(&Map->Items[Map->Count], 0, sizeof(BoxAsmGlobal));
        Map->Items[Map->Count++].Name = Name;
    }
    *Out = &Map->Items[*Position];
    return 0;
}

const BoxAsmGlobal *BoxAsmGlobalFind(const BoxAsmGlobalMap *Map, const char *Name)
{
    uint32_t Position;

    return BoxAsmNameIndexFind(&Map->Index, Name, &Position) ? &Map->Items[Position] : NULL;
}

void BoxAsmGlobalRelease(BoxAsmGlobalMap *Map)
{
    BoxAsmNameIndexRelease(&Map->Index);
    free(Map->Items);
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
