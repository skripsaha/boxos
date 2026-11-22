#ifndef BOX_READER_H
#define BOX_READER_H

/*
 * The one reader of a box (box_format.h) — the kernel's, BoxLib's and every
 * host tool's, one source built three times, so a box means the same thing to
 * all of them: as the kernel understood it, so did everyone else.
 *
 * Nothing in a box is taken on trust. Every offset and size is held to its
 * limit by subtraction, never by a sum that can wrap; every rule of the one
 * way a box is written is checked, not assumed; the first thing that is not
 * so refuses the box, and the verdict says what, and which part.
 *
 * Reading goes in the order the bytes are needed, so a loader reads no more
 * of the file than it must:
 *
 *   BoxReadHead     the first BOX_HEADER_BYTES: where the table and the names
 *                   are, and how much of the file's start (Head.PrefixBytes)
 *                   to read next
 *   BoxRead         that start — header, table, names: every part's place in
 *                   the file, its name, its rules, what the box must have
 *   BoxSealCompute  the seal over that same start, to hold to the header's
 *   then, per part read: its SHA-256 to its entry's (BoxSameHash), and the
 *   parts with a layout of their own read by their readers — BoxDoorsRead,
 *   BoxThreadRead, BoxFixupsCheck — the kernel's walk of the fixups going
 *   through the same BoxFixupNext the check went through.
 *   BoxGapsZero     for a reader holding the whole file: the alignment
 *                   between the parts is zeros.
 *
 * No allocation, no library, no global. Hashing is the includer's SHA-256,
 * handed in as a BoxHasher. Integer types come from the includer (see
 * box_format.h).
 */

#include "box_format.h"

typedef enum {
    BOX_OK = 0,
    BOX_NOT_BOX,
    BOX_TRUNCATED,
    BOX_HEADER_SIZE,
    BOX_NO_PARTS,
    BOX_TABLE_MISPLACED,
    BOX_NAMES_MISPLACED,
    BOX_FILE_SIZE,
    BOX_ENTRY_NOT_ZERO,
    BOX_FLAGS_UNKNOWN,
    BOX_NAME_MISPLACED,
    BOX_NAME_SPELLING,
    BOX_NAME_ORDER,
    BOX_NAME_NOT_STANDARD,
    BOX_NAMES_LEFT_OVER,
    BOX_PART_MISPLACED,
    BOX_PART_OUTSIDE,
    BOX_FILE_LEFT_OVER,
    BOX_PART_MEMORY,
    BOX_PART_PLACE,
    BOX_PART_FLAGS,
    BOX_PART_UNKNOWN_REQUIRED,
    BOX_LOADABLE_OVERLAP,
    BOX_NO_CODE,
    BOX_NO_DOORS,
    BOX_SEAL,
    BOX_PART_HASH,
    BOX_GAP_NOT_ZERO,
    BOX_DOORS_LAYOUT,
    BOX_DOOR_OUTSIDE_CODE,
    BOX_NO_START,
    BOX_THREAD_LAYOUT,
    BOX_FIXUPS_LAYOUT,
    BOX_FIXUP_ORDER,
    BOX_FIXUP_OUTSIDE,
    BOX_TEXT_LAYOUT,
    BOX_SIGNATURE_LAYOUT,
    BOX_VERDICT_COUNT
} BoxVerdict;

/* What BoxTable.Refused and BoxTable.Kinds hold where there is no part. */
#define BOX_NO_PART  0xFFFFFFFFu

typedef struct {
    uint32_t HeaderSize;
    uint32_t PartCount;
    uint64_t TableAt;
    uint64_t NamesAt;
    uint64_t NamesSize;
    uint64_t FileSize;
    uint64_t PrefixBytes;   /* where the names end: what BoxRead reads and the seal covers */
} BoxHead;

typedef struct {
    const uint8_t *Prefix;              /* the file's first Head.PrefixBytes */
    BoxHead        Head;
    uint32_t       Kinds[BOX_KIND_COUNT]; /* the part of each standard kind named bare */
    uint32_t       Refused;             /* the part a refusal is about */
    const uint8_t *RefusedName;         /* its name, once the name was found in its place; */
    uint16_t       RefusedNameLength;   /* NULL before that — a refusal can say whose */
} BoxTable;

typedef struct {
    uint32_t       Index;
    BoxKind        Kind;        /* BOX_KIND_UNKNOWN: a part to pass over */
    const uint8_t *Name;
    uint16_t       NameLength;
    const uint8_t *Hash;
    uint64_t       Offset;
    uint64_t       Size;
    uint64_t       Memory;
    uint64_t       Place;
    uint32_t       Flags;
} BoxPart;

/* The includer's SHA-256: Begin, then Add as often as there are pieces, then
 * End with the 32 bytes of the digest. */
typedef struct {
    void  *State;
    void (*Begin)(void *state);
    void (*Add)(void *state, const uint8_t *bytes, uint64_t count);
    void (*End)(void *state, uint8_t digest[BOX_HASH_BYTES]);
} BoxHasher;

/* ── bytes ───────────────────────────────────────────────────────────────── */

static inline uint16_t BoxU16(const uint8_t *at)
{
    return (uint16_t)(at[0] | (uint16_t)at[1] << 8);
}

static inline uint32_t BoxU32(const uint8_t *at)
{
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static inline uint64_t BoxU64(const uint8_t *at)
{
    return (uint64_t)BoxU32(at) | (uint64_t)BoxU32(at + 4) << 32;
}

static inline bool BoxSameHash(const uint8_t *a, const uint8_t *b)
{
    uint8_t differ = 0;
    for (uint32_t i = 0; i < BOX_HASH_BYTES; i++) differ |= (uint8_t)(a[i] ^ b[i]);
    return differ == 0;
}

/* `at` rounded up to `align` (a power of two), or false when that passes `limit`. */
static inline bool BoxAlignUp(uint64_t at, uint64_t align, uint64_t limit, uint64_t *out)
{
    uint64_t up = (align - (at & (align - 1))) & (align - 1);
    if (at > limit || up > limit - at) return false;
    *out = at + up;
    return true;
}

/* ── names ───────────────────────────────────────────────────────────────── *
 *
 * A part's name is a tag as BoxOS spells one, written the one way it reads
 * back: a key, then optionally ':' and a value; neither side empty or longer
 * than BOX_NAME_SIDE_MAX; no blank, comma, control byte or DEL anywhere, and no
 * "..." (a tag pattern, not a tag). The value may hold further ':'.
 */
static inline bool BoxNameSpelled(const uint8_t *name, uint64_t length)
{
    uint64_t colon = length;
    for (uint64_t i = 0; i < length; i++) {
        uint8_t c = name[i];
        if (c <= 0x20u || c == 0x7Fu || c == ',') return false;
        if (c == ':' && colon == length) colon = i;
        if (c == '.' && i + 2 < length && name[i + 1] == '.' && name[i + 2] == '.') return false;
    }
    if (colon == 0 || colon > BOX_NAME_SIDE_MAX) return false;
    if (colon == length) return true;
    return length - colon - 1 >= 1 && length - colon - 1 <= BOX_NAME_SIDE_MAX;
}

static inline int BoxSpanOrder(const uint8_t *a, uint64_t a_length, const uint8_t *b, uint64_t b_length)
{
    uint64_t common = a_length < b_length ? a_length : b_length;
    for (uint64_t i = 0; i < common; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return a_length == b_length ? 0 : (a_length < b_length ? -1 : 1);
}

static inline uint64_t BoxNameKeyLength(const uint8_t *name, uint64_t length)
{
    for (uint64_t i = 0; i < length; i++)
        if (name[i] == ':') return i;
    return length;
}

/* The order every list of tags in BoxOS keeps (TagSpellingCompare): by key,
 * a bare name before its key's names with a value, then by value; bytewise,
 * a shorter span before a longer one it begins. Zero only for one name. */
static inline int BoxNameOrder(const uint8_t *a, uint64_t a_length, const uint8_t *b, uint64_t b_length)
{
    uint64_t a_key = BoxNameKeyLength(a, a_length), b_key = BoxNameKeyLength(b, b_length);
    int      by_key = BoxSpanOrder(a, a_key, b, b_key);
    if (by_key != 0) return by_key;
    bool a_value = a_key < a_length, b_value = b_key < b_length;
    if (!a_value || !b_value) return (int)a_value - (int)b_value;
    return BoxSpanOrder(a + a_key + 1, a_length - a_key - 1, b + b_key + 1, b_length - b_key - 1);
}

static inline bool BoxSpanIs(const uint8_t *span, uint64_t length, const char *word)
{
    uint64_t i = 0;
    for (; i < length; i++)
        if (word[i] == '\0' || (uint8_t)word[i] != span[i]) return false;
    return word[i] == '\0';
}

/* The standard's word for each kind; the index is the BoxKind. */
static inline const char *BoxKindWord(BoxKind kind)
{
    static const char *const words[BOX_KIND_COUNT] = {
        "", "code", "const", "data", "fixups", "doors", "thread", "names", "signature",
        "needs", "hints", "life", "form", "settings", "recipe", "about", "icon", "works", "asks", "role"
    };
    return (unsigned)kind < BOX_KIND_COUNT ? words[kind] : "";
}

/* Whether a kind's name may say more after a ':' — the language of a part
 * written for people (about:ru), the size of an icon (icon:64). */
static inline bool BoxKindTakesValue(BoxKind kind)
{
    return kind == BOX_KIND_ABOUT || kind == BOX_KIND_FORM || kind == BOX_KIND_SETTINGS ||
           kind == BOX_KIND_ICON;
}

/* The parts a program cannot run without being understood: what is in its
 * memory, where it goes in, and what it needs of the processor. Each carries
 * BOX_FLAG_MUST_UNDERSTAND. */
static inline bool BoxKindRequired(BoxKind kind)
{
    return kind == BOX_KIND_CODE || kind == BOX_KIND_CONST || kind == BOX_KIND_DATA ||
           kind == BOX_KIND_FIXUPS || kind == BOX_KIND_DOORS || kind == BOX_KIND_THREAD ||
           kind == BOX_KIND_NEEDS;
}

/* The parts the kernel reads to start a program — those, and the hints it
 * prepares the start by. None is loaded at start by mark (each is used for
 * what it is) and none is compressed. */
static inline bool BoxKindStartsProgram(BoxKind kind)
{
    return BoxKindRequired(kind) || kind == BOX_KIND_HINTS;
}

static inline bool BoxKindLoadable(BoxKind kind)
{
    return kind == BOX_KIND_CODE || kind == BOX_KIND_CONST || kind == BOX_KIND_DATA;
}

/* The kind a spelled name names: BOX_KIND_UNKNOWN for a word the standard does
 * not have. *bare is whether it is the kind's own bare name. */
static inline BoxKind BoxKindOf(const uint8_t *name, uint64_t length, bool *bare)
{
    uint64_t key = BoxNameKeyLength(name, length);
    *bare = key == length;
    for (unsigned kind = 1; kind < BOX_KIND_COUNT; kind++)
        if (BoxSpanIs(name, key, BoxKindWord((BoxKind)kind))) return (BoxKind)kind;
    return BOX_KIND_UNKNOWN;
}

/* ── the header ──────────────────────────────────────────────────────────── */

/* The header, from the `available` bytes at the start of a file of
 * `file_size` bytes. */
static inline BoxVerdict BoxReadHead(const uint8_t *bytes, uint64_t available, uint64_t file_size,
                                     BoxHead *out)
{
    static const uint8_t magic[8] = { BOX_MAGIC_0, BOX_MAGIC_1, BOX_MAGIC_2, BOX_MAGIC_3,
                                      BOX_MAGIC_4, BOX_MAGIC_5, BOX_MAGIC_6, BOX_MAGIC_7 };
    if (available < 8) return BOX_NOT_BOX;
    for (unsigned i = 0; i < 8; i++)
        if (bytes[BOX_AT_MAGIC + i] != magic[i]) return BOX_NOT_BOX;
    if (available < BOX_HEADER_BYTES || file_size < BOX_HEADER_BYTES) return BOX_TRUNCATED;

    out->HeaderSize = BoxU32(bytes + BOX_AT_HEADER_SIZE);
    out->PartCount  = BoxU32(bytes + BOX_AT_PART_COUNT);
    out->TableAt    = BoxU64(bytes + BOX_AT_TABLE);
    out->NamesAt    = BoxU64(bytes + BOX_AT_NAMES);
    out->NamesSize  = BoxU64(bytes + BOX_AT_NAMES_SIZE);
    out->FileSize   = BoxU64(bytes + BOX_AT_FILE_SIZE);

    if (out->HeaderSize < BOX_HEADER_BYTES || out->HeaderSize % BOX_PART_ALIGN != 0)
        return BOX_HEADER_SIZE;
    if (out->FileSize != file_size) return BOX_FILE_SIZE;
    if (out->PartCount == 0) return BOX_NO_PARTS;
    if (out->TableAt != out->HeaderSize) return BOX_TABLE_MISPLACED;
    if (out->TableAt > file_size ||
        (uint64_t)out->PartCount * BOX_ENTRY_BYTES > file_size - out->TableAt)
        return BOX_TRUNCATED;
    if (out->NamesAt != out->TableAt + (uint64_t)out->PartCount * BOX_ENTRY_BYTES)
        return BOX_NAMES_MISPLACED;
    if (out->NamesSize > file_size - out->NamesAt) return BOX_TRUNCATED;
    out->PrefixBytes = out->NamesAt + out->NamesSize;
    return BOX_OK;
}

/* ── the table ───────────────────────────────────────────────────────────── */

static inline void BoxEntryRead(const BoxTable *table, uint32_t index, BoxPart *out)
{
    const uint8_t *entry = table->Prefix + table->Head.TableAt + (uint64_t)index * BOX_ENTRY_BYTES;
    bool           bare;

    out->Index      = index;
    out->Offset     = BoxU64(entry + BOX_ENTRY_OFFSET);
    out->Size       = BoxU64(entry + BOX_ENTRY_SIZE);
    out->Memory     = BoxU64(entry + BOX_ENTRY_MEMORY);
    out->Place      = BoxU64(entry + BOX_ENTRY_PLACE);
    out->Flags      = BoxU32(entry + BOX_ENTRY_FLAGS);
    out->NameLength = BoxU16(entry + BOX_ENTRY_NAME_LEN);
    out->Name       = table->Prefix + table->Head.NamesAt + BoxU32(entry + BOX_ENTRY_NAME_AT);
    out->Hash       = entry + BOX_ENTRY_HASH;
    out->Kind       = BoxKindOf(out->Name, out->NameLength, &bare);
}

/* The rules of a part of a kind this reader understands, by its kind. */
static inline BoxVerdict BoxKindRules(const BoxPart *part, bool required)
{
    if (part->Kind == BOX_KIND_DATA ? part->Memory < part->Size : part->Memory != part->Size)
        return BOX_PART_MEMORY;

    if (BoxKindLoadable(part->Kind)) {
        if (part->Place % BOX_LOADABLE_ALIGN != 0 || part->Place >= BOX_PLACE_LIMIT ||
            part->Memory > BOX_PLACE_LIMIT - part->Place)
            return BOX_PART_PLACE;
        if (part->Kind == BOX_KIND_CODE && part->Size == 0) return BOX_PART_MEMORY;
    } else if (part->Place != 0) {
        return BOX_PART_PLACE;
    }

    if (BoxKindRequired(part->Kind) && !required) return BOX_PART_FLAGS;
    if (BoxKindStartsProgram(part->Kind) &&
        (part->Flags & (BOX_FLAG_LOAD_AT_START | BOX_FLAG_COMPRESSED)) != 0)
        return BOX_PART_FLAGS;
    if (part->Kind == BOX_KIND_NAMES && (part->Flags & BOX_FLAG_LOAD_AT_START) != 0)
        return BOX_PART_FLAGS;
    return BOX_OK;
}

/* The rules of one part's entry, its name already held to its place. The
 * name's own kind decides what the name may say and what must be understood;
 * the rules of a kind's layout bind only a part this reader understands
 * (part->Kind). A part marked to be loaded at start is put into the program's memory
 * whatever its kind, one passed over too, and so as it is: its bytes exactly,
 * never compressed, on pages the kernel chooses. */
static inline BoxVerdict BoxPartRules(const BoxPart *part)
{
    bool    bare;
    BoxKind named    = BoxKindOf(part->Name, part->NameLength, &bare);
    bool    required = (part->Flags & BOX_FLAG_MUST_UNDERSTAND) != 0;

    if (named != BOX_KIND_UNKNOWN && !bare && !BoxKindTakesValue(named)) return BOX_NAME_NOT_STANDARD;
    if (named == BOX_KIND_UNKNOWN && required) return BOX_PART_UNKNOWN_REQUIRED;
    if (part->Kind != BOX_KIND_UNKNOWN) {
        BoxVerdict verdict = BoxKindRules(part, required);
        if (verdict != BOX_OK) return verdict;
    }

    if ((part->Flags & BOX_FLAG_LOAD_AT_START) == 0) return BOX_OK;
    if (part->Flags & BOX_FLAG_COMPRESSED) return BOX_PART_FLAGS;
    if (part->Memory != part->Size) return BOX_PART_MEMORY;
    if (part->Place != 0) return BOX_PART_PLACE;
    return BOX_OK;
}

/* Whether two loadable parts share a page — or, past the user half, could. */
static inline bool BoxLoadablesMeet(const BoxPart *a, const BoxPart *b)
{
    uint64_t a_end, b_end;
    if (!BoxAlignUp(a->Place + a->Memory, BOX_LOADABLE_ALIGN, BOX_PLACE_LIMIT, &a_end) ||
        !BoxAlignUp(b->Place + b->Memory, BOX_LOADABLE_ALIGN, BOX_PLACE_LIMIT, &b_end))
        return true;
    return a->Place < b_end && b->Place < a_end;
}

/*
 * The table and the names, from the file's first head->PrefixBytes bytes at
 * `prefix` (which must stay as they are for as long as `out` is used). On a
 * refusal out->Refused is the part it is about, or BOX_NO_PART.
 */
static inline BoxVerdict BoxRead(const uint8_t *prefix, uint64_t prefix_bytes, const BoxHead *head,
                                 BoxTable *out)
{
    out->Prefix            = prefix;
    out->Head              = *head;
    out->Refused           = BOX_NO_PART;
    out->RefusedName       = 0;
    out->RefusedNameLength = 0;
    for (unsigned kind = 0; kind < BOX_KIND_COUNT; kind++) out->Kinds[kind] = BOX_NO_PART;
    if (prefix_bytes < head->PrefixBytes) return BOX_TRUNCATED;

    uint64_t names_used = 0;
    uint64_t file_used  = head->PrefixBytes;
    BoxPart  previous   = { 0 };

    for (uint32_t i = 0; i < head->PartCount; i++) {
        const uint8_t *entry = prefix + head->TableAt + (uint64_t)i * BOX_ENTRY_BYTES;
        BoxPart        part;
        bool           bare;
        uint64_t       expected;

        out->Refused           = i;
        out->RefusedName       = 0;
        out->RefusedNameLength = 0;

        /* The names lie back to back in table order. */
        uint16_t name_length = BoxU16(entry + BOX_ENTRY_NAME_LEN);
        if (BoxU32(entry + BOX_ENTRY_NAME_AT) != names_used || name_length == 0 ||
            name_length > head->NamesSize - names_used)
            return BOX_NAME_MISPLACED;
        out->RefusedName       = prefix + head->NamesAt + names_used;
        out->RefusedNameLength = name_length;
        names_used += name_length;

        for (unsigned k = BOX_ENTRY_RESERVED; k < BOX_ENTRY_HASH; k++)
            if (entry[k] != 0) return BOX_ENTRY_NOT_ZERO;
        if ((BoxU32(entry + BOX_ENTRY_FLAGS) & ~BOX_FLAGS_KNOWN) != 0) return BOX_FLAGS_UNKNOWN;

        BoxEntryRead(out, i, &part);
        if (!BoxNameSpelled(part.Name, part.NameLength)) return BOX_NAME_SPELLING;
        if (i > 0 && BoxNameOrder(previous.Name, previous.NameLength, part.Name, part.NameLength) >= 0)
            return BOX_NAME_ORDER;

        /* Each part at the first offset its alignment allows after the last;
         * the alignment is the name's. */
        BoxKind  named = BoxKindOf(part.Name, part.NameLength, &bare);
        uint64_t align = BoxKindLoadable(named) ? BOX_LOADABLE_ALIGN : BOX_PART_ALIGN;
        if (!BoxAlignUp(file_used, align, head->FileSize, &expected)) return BOX_PART_OUTSIDE;
        if (part.Offset != expected) return BOX_PART_MISPLACED;
        if (part.Size > head->FileSize - part.Offset) return BOX_PART_OUTSIDE;
        file_used = part.Offset + part.Size;

        BoxVerdict rules = BoxPartRules(&part);
        if (rules != BOX_OK) return rules;

        if (part.Kind != BOX_KIND_UNKNOWN && bare) out->Kinds[part.Kind] = i;
        previous = part;
    }

    out->Refused           = BOX_NO_PART;
    out->RefusedName       = 0;
    out->RefusedNameLength = 0;
    if (names_used != head->NamesSize) return BOX_NAMES_LEFT_OVER;
    if (file_used != head->FileSize) return BOX_FILE_LEFT_OVER;
    if (out->Kinds[BOX_KIND_CODE] == BOX_NO_PART) return BOX_NO_CODE;
    if (out->Kinds[BOX_KIND_DOORS] == BOX_NO_PART) return BOX_NO_DOORS;

    static const BoxKind loadables[3] = { BOX_KIND_CODE, BOX_KIND_CONST, BOX_KIND_DATA };
    for (unsigned a = 0; a < 3; a++) {
        for (unsigned b = a + 1; b < 3; b++) {
            BoxPart first, second;
            if (out->Kinds[loadables[a]] == BOX_NO_PART || out->Kinds[loadables[b]] == BOX_NO_PART)
                continue;
            BoxEntryRead(out, out->Kinds[loadables[a]], &first);
            BoxEntryRead(out, out->Kinds[loadables[b]], &second);
            if (BoxLoadablesMeet(&first, &second)) {
                out->Refused           = second.Index;
                out->RefusedName       = second.Name;
                out->RefusedNameLength = second.NameLength;
                return BOX_LOADABLE_OVERLAP;
            }
        }
    }
    return BOX_OK;
}

/* ── the parts, once read ────────────────────────────────────────────────── */

static inline bool BoxPartAt(const BoxTable *table, uint32_t index, BoxPart *out)
{
    if (index >= table->Head.PartCount) return false;
    BoxEntryRead(table, index, out);
    return true;
}

/* The part of a standard kind named bare (code, doors, …), if the box has one. */
static inline bool BoxPartOfKind(const BoxTable *table, BoxKind kind, BoxPart *out)
{
    if ((unsigned)kind >= BOX_KIND_COUNT || table->Kinds[kind] == BOX_NO_PART) return false;
    return BoxPartAt(table, table->Kinds[kind], out);
}

/* The part named `length` bytes at `name`, found by halves. */
static inline bool BoxPartFind(const BoxTable *table, const uint8_t *name, uint64_t length, BoxPart *out)
{
    uint32_t low = 0, high = table->Head.PartCount;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        BoxPart  part;
        BoxEntryRead(table, middle, &part);
        int order = BoxNameOrder(part.Name, part.NameLength, name, length);
        if (order == 0) {
            *out = part;
            return true;
        }
        if (order < 0) low = middle + 1;
        else           high = middle;
    }
    return false;
}

/* ── the seal and the hashes ─────────────────────────────────────────────── */

static inline void BoxHashBytes(const BoxHasher *hasher, const uint8_t *bytes, uint64_t count,
                                uint8_t digest[BOX_HASH_BYTES])
{
    hasher->Begin(hasher->State);
    hasher->Add(hasher->State, bytes, count);
    hasher->End(hasher->State, digest);
}

/* The seal of the box whose first `prefix_bytes` bytes — up to the end of
 * its names — are at `prefix`: their SHA-256, the seal's own 32 bytes read as
 * zeros. */
static inline void BoxSealCompute(const uint8_t *prefix, uint64_t prefix_bytes, const BoxHasher *hasher,
                                  uint8_t seal[BOX_HASH_BYTES])
{
    static const uint8_t zeros[BOX_SEAL_BYTES] = { 0 };
    hasher->Begin(hasher->State);
    hasher->Add(hasher->State, prefix, BOX_AT_SEAL);
    hasher->Add(hasher->State, zeros, BOX_SEAL_BYTES);
    hasher->Add(hasher->State, prefix + BOX_AT_SEAL + BOX_SEAL_BYTES,
                prefix_bytes - BOX_AT_SEAL - BOX_SEAL_BYTES);
    hasher->End(hasher->State, seal);
}

/* Whether the seal in the header is the seal of what the table says. */
static inline bool BoxSealHolds(const BoxTable *table, const BoxHasher *hasher)
{
    uint8_t seal[BOX_HASH_BYTES];
    BoxSealCompute(table->Prefix, table->Head.PrefixBytes, hasher, seal);
    return BoxSameHash(seal, table->Prefix + BOX_AT_SEAL);
}

/* Whether `bytes` — a part's bytes as read from the file — are what its entry
 * says they are. */
static inline bool BoxPartHolds(const BoxPart *part, const uint8_t *bytes, const BoxHasher *hasher)
{
    uint8_t digest[BOX_HASH_BYTES];
    BoxHashBytes(hasher, bytes, part->Size, digest);
    return BoxSameHash(digest, part->Hash);
}

/* For a reader holding the whole file: every byte between the names and the
 * first part, and between one part and the next, is zero. */
static inline BoxVerdict BoxGapsZero(const uint8_t *file, BoxTable *table)
{
    uint64_t from = table->Head.PrefixBytes;
    for (uint32_t i = 0; i < table->Head.PartCount; i++) {
        BoxPart part;
        BoxEntryRead(table, i, &part);
        for (uint64_t at = from; at < part.Offset; at++) {
            if (file[at] != 0) {
                table->Refused           = i;
                table->RefusedName       = part.Name;
                table->RefusedNameLength = part.NameLength;
                return BOX_GAP_NOT_ZERO;
            }
        }
        from = part.Offset + part.Size;
    }
    return BOX_OK;
}

/* ── doors ───────────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *Bytes;
    uint64_t       Size;
    uint32_t       Count;
} BoxDoors;

static inline const uint8_t *BoxDoorEntry(const BoxDoors *doors, uint32_t index)
{
    return doors->Bytes + BOX_DOORS_HEAD_BYTES + (uint64_t)index * BOX_DOOR_BYTES;
}

/* The `doors` part's `size` bytes at `bytes`, every door held to the bytes of
 * the box's code; a box without a door `start` cannot be started. */
static inline BoxVerdict BoxDoorsRead(const uint8_t *bytes, uint64_t size, const BoxTable *table,
                                      BoxDoors *out)
{
    BoxPart code;
    if (!BoxPartOfKind(table, BOX_KIND_CODE, &code)) return BOX_NO_CODE;

    out->Bytes = bytes;
    out->Size  = size;
    out->Count = 0;
    if (size < BOX_DOORS_HEAD_BYTES || BoxU32(bytes + 4) != 0) return BOX_DOORS_LAYOUT;
    uint32_t count = BoxU32(bytes);
    if (count > (size - BOX_DOORS_HEAD_BYTES) / BOX_DOOR_BYTES) return BOX_DOORS_LAYOUT;
    out->Count = count;

    uint64_t names_at   = BOX_DOORS_HEAD_BYTES + (uint64_t)count * BOX_DOOR_BYTES;
    uint64_t names_used = 0;
    bool     start      = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry  = BoxDoorEntry(out, i);
        uint64_t       offset = BoxU64(entry);
        uint16_t       length = BoxU16(entry + 12);
        const uint8_t *name   = bytes + names_at + names_used;

        if (BoxU32(entry + 8) != names_used || BoxU16(entry + 14) != 0 || length == 0 ||
            length > size - names_at - names_used)
            return BOX_DOORS_LAYOUT;
        if (!BoxNameSpelled(name, length)) return BOX_DOORS_LAYOUT;
        if (i > 0) {
            const uint8_t *before = BoxDoorEntry(out, i - 1);
            if (BoxNameOrder(bytes + names_at + BoxU32(before + 8), BoxU16(before + 12), name,
                             length) >= 0)
                return BOX_DOORS_LAYOUT;
        }
        if (offset < code.Place || offset - code.Place >= code.Size) return BOX_DOOR_OUTSIDE_CODE;
        if (BoxSpanIs(name, length, BOX_DOOR_START)) start = true;
        names_used += length;
    }
    if (names_at + names_used != size) return BOX_DOORS_LAYOUT;
    return start ? BOX_OK : BOX_NO_START;
}

/* Door `index` of doors BoxDoorsRead accepted: its name and its offset from
 * the image base. False past the last. */
static inline bool BoxDoorAt(const BoxDoors *doors, uint32_t index, const uint8_t **name,
                             uint16_t *length, uint64_t *offset)
{
    if (index >= doors->Count) return false;
    const uint8_t *entry = BoxDoorEntry(doors, index);
    *name   = doors->Bytes + BOX_DOORS_HEAD_BYTES + (uint64_t)doors->Count * BOX_DOOR_BYTES +
              BoxU32(entry + 8);
    *length = BoxU16(entry + 12);
    *offset = BoxU64(entry);
    return true;
}

/* The door named `name` of doors BoxDoorsRead accepted: its offset from the
 * image base. */
static inline bool BoxDoorFind(const BoxDoors *doors, const char *name, uint64_t *offset)
{
    const uint8_t *door;
    uint16_t       length;
    uint64_t       at;
    for (uint32_t i = 0; BoxDoorAt(doors, i, &door, &length, &at); i++) {
        if (BoxSpanIs(door, length, name)) {
            *offset = at;
            return true;
        }
    }
    return false;
}

/* ── thread ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t       Align;
    uint64_t       InitBytes;
    uint64_t       TotalBytes;
    const uint8_t *Init;
} BoxThread;

static inline BoxVerdict BoxThreadRead(const uint8_t *bytes, uint64_t size, BoxThread *out)
{
    if (size < BOX_THREAD_HEAD_BYTES || BoxU64(bytes + 24) != 0) return BOX_THREAD_LAYOUT;
    out->Align      = BoxU64(bytes);
    out->InitBytes  = BoxU64(bytes + 8);
    out->TotalBytes = BoxU64(bytes + 16);
    out->Init       = bytes + BOX_THREAD_HEAD_BYTES;
    if (out->Align == 0 || (out->Align & (out->Align - 1)) != 0 || out->Align > BOX_THREAD_ALIGN_MAX)
        return BOX_THREAD_LAYOUT;
    if (out->InitBytes != size - BOX_THREAD_HEAD_BYTES || out->TotalBytes < out->InitBytes ||
        out->TotalBytes > BOX_PLACE_LIMIT)
        return BOX_THREAD_LAYOUT;
    return BOX_OK;
}

/* ── fixups ──────────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *Words;
    uint64_t       Count;
    uint64_t       At;       /* the next word */
    uint64_t       Where;    /* the place a bitmap starts at */
    uint64_t       Bits;     /* what is left of the bitmap being walked */
    uint64_t       BitAt;    /* the place of its lowest bit */
} BoxFixupWalk;

static inline void BoxFixupBegin(const uint8_t *bytes, uint64_t size, BoxFixupWalk *walk)
{
    walk->Words = bytes;
    walk->Count = size / 8;
    walk->At    = 0;
    walk->Where = 0;
    walk->Bits  = 0;
    walk->BitAt = 0;
}

/* The next fixup's offset from the image base. Only over fixups
 * BoxFixupsCheck accepted is it a place in the image. */
static inline bool BoxFixupNext(BoxFixupWalk *walk, uint64_t *offset)
{
    for (;;) {
        while (walk->Bits != 0) {
            bool     set   = (walk->Bits & 1u) != 0;
            uint64_t place = walk->BitAt;
            walk->Bits >>= 1;
            walk->BitAt += 8;
            if (set) {
                *offset = place;
                return true;
            }
        }
        if (walk->At == walk->Count) return false;

        uint64_t word = BoxU64(walk->Words + walk->At * 8);
        walk->At++;
        if ((word & 1u) == 0) {
            *offset      = word;
            walk->Where = word + 8;
            return true;
        }
        walk->Bits   = word >> 1;
        walk->BitAt  = walk->Where;
        walk->Where += (uint64_t)BOX_FIXUP_BITS * 8;
    }
}

/* Whether the eight bytes at `offset` are bytes a loadable part has in the file. */
static inline bool BoxFixupInImage(const BoxTable *table, uint64_t offset)
{
    static const BoxKind loadables[3] = { BOX_KIND_CODE, BOX_KIND_CONST, BOX_KIND_DATA };
    for (unsigned i = 0; i < 3; i++) {
        BoxPart part;
        if (!BoxPartOfKind(table, loadables[i], &part)) continue;
        if (offset >= part.Place && part.Size >= 8 && offset - part.Place <= part.Size - 8)
            return true;
    }
    return false;
}

/* The `fixups` part's `size` bytes at `bytes`: whole words, the first a
 * place, and the places they decode to rising, each a whole word in the bytes
 * a loadable part has in the file. */
static inline BoxVerdict BoxFixupsCheck(const uint8_t *bytes, uint64_t size, const BoxTable *table)
{
    if (size % 8 != 0) return BOX_FIXUPS_LAYOUT;
    if (size > 0 && (BoxU64(bytes) & 1u) != 0) return BOX_FIXUPS_LAYOUT;

    BoxFixupWalk walk;
    uint64_t     next = 0, offset;
    BoxFixupBegin(bytes, size, &walk);
    while (BoxFixupNext(&walk, &offset)) {
        if (offset < next) return BOX_FIXUP_ORDER;
        if (offset % 8 != 0 || offset >= BOX_PLACE_LIMIT || !BoxFixupInImage(table, offset))
            return BOX_FIXUP_OUTSIDE;
        next = offset + 8;
    }
    return BOX_OK;
}

/* ── parts written as text ───────────────────────────────────────────────── *
 *
 * needs, hints, life, form and the rest are written for people and programs
 * alike: lines ended by '\n' (the last one's may be left off), a line's words
 * between blanks, no byte below a blank but the newline and no DEL. The first
 * line with words says the kind alone, "needs"; after it, a line is a fact. A reader passes over a line it does not know and reads one it
 * knows strictly.
 */

typedef struct {
    const uint8_t *Bytes;
    uint64_t       Size;
    uint64_t       At;      /* where the next line starts */
} BoxText;

/* The next line that has words, from text->At on. */
static inline bool BoxTextLine(BoxText *text, const uint8_t **line, uint64_t *length)
{
    while (text->At < text->Size) {
        uint64_t start = text->At, end = start;
        while (end < text->Size && text->Bytes[end] != '\n') end++;
        text->At = end < text->Size ? end + 1 : end;
        for (uint64_t i = start; i < end; i++) {
            if (text->Bytes[i] != ' ') {
                *line   = text->Bytes + start;
                *length = end - start;
                return true;
            }
        }
    }
    return false;
}

/* The next word of a line, from *at on. */
static inline bool BoxTextWord(const uint8_t *line, uint64_t length, uint64_t *at, const uint8_t **word,
                               uint64_t *word_length)
{
    while (*at < length && line[*at] == ' ') (*at)++;
    if (*at == length) return false;
    uint64_t start = *at;
    while (*at < length && line[*at] != ' ') (*at)++;
    *word        = line + start;
    *word_length = *at - start;
    return true;
}

/* How many words a line has. */
static inline uint64_t BoxTextWordCount(const uint8_t *line, uint64_t length)
{
    const uint8_t *word;
    uint64_t       at = 0, word_length, count = 0;
    while (BoxTextWord(line, length, &at, &word, &word_length)) count++;
    return count;
}

/* The `size` bytes at `bytes`, a part of kind `kind` written as text, opened
 * past its first line, which must be "<kind>". */
static inline BoxVerdict BoxTextBegin(const uint8_t *bytes, uint64_t size, BoxKind kind, BoxText *out)
{
    for (uint64_t i = 0; i < size; i++)
        if ((bytes[i] < 0x20u && bytes[i] != '\n') || bytes[i] == 0x7Fu) return BOX_TEXT_LAYOUT;

    out->Bytes = bytes;
    out->Size  = size;
    out->At    = 0;

    const uint8_t *line, *word = NULL;
    uint64_t       length, at = 0, word_length = 0;
    if (!BoxTextLine(out, &line, &length) || BoxTextWordCount(line, length) != 1) return BOX_TEXT_LAYOUT;
    BoxTextWord(line, length, &at, &word, &word_length);
    if (!BoxSpanIs(word, word_length, BoxKindWord(kind))) return BOX_TEXT_LAYOUT;
    return BOX_OK;
}

/* A size as a text part writes one: a number — digits, with no leading zero
 * but in "0" itself — and its unit, B, KiB, MiB or GiB. False for anything
 * else, and for a size past 2^64. */
static inline bool BoxTextSize(const uint8_t *number, uint64_t number_length, const uint8_t *unit,
                               uint64_t unit_length, uint64_t *bytes)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB" };
    unsigned shift = 0;
    bool     known = false;
    for (unsigned i = 0; i < sizeof(units) / sizeof(units[0]) && !known; i++) {
        known = BoxSpanIs(unit, unit_length, units[i]);
        shift = 10u * i;
    }
    if (!known || number_length == 0 || (number_length > 1 && number[0] == '0')) return false;

    uint64_t value = 0;
    for (uint64_t i = 0; i < number_length; i++) {
        if (number[i] < '0' || number[i] > '9') return false;
        uint64_t digit = (uint64_t)(number[i] - '0');
        if (value > (~(uint64_t)0 - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    if (value > (~(uint64_t)0 >> shift)) return false;
    *bytes = value << shift;
    return true;
}

/* ── life ────────────────────────────────────────────────────────────────── *
 *
 * How a program lives, a word a line after "life":
 *
 *   task     it does its work and ends — what a program without the part does;
 *            whoever starts it waits for its end
 *   stays    it lives on by itself: whoever starts it does not wait
 *   serves   it serves others for as long as the machine runs
 *   rise     when it falls, the system raises it again
 *
 * One of the first three at most, and rise at most once. A line of a word this
 * reader does not know is passed over, and *unknown says the first: a newer
 * word an older system lets be, and that the toolchain, which writes the part,
 * refuses.
 */

typedef enum {
    BOX_LIFE_TASK = 0,
    BOX_LIFE_STAYS,
    BOX_LIFE_SERVES
} BoxLifeKind;

typedef struct {
    uint8_t Kind;   /* BoxLifeKind */
    uint8_t Rise;
} BoxLife;

/* The words of a `life` part. *unknown (when asked for) is the number of the
 * first line it passed over, counted from 1 with the head; 0 when none. */
static inline BoxVerdict BoxLifeRead(const uint8_t *bytes, uint64_t size, BoxLife *out, uint64_t *unknown)
{
    static const char *const kinds[] = { "task", "stays", "serves" };
    BoxText    text;
    BoxVerdict verdict = BoxTextBegin(bytes, size, BOX_KIND_LIFE, &text);
    out->Kind = BOX_LIFE_TASK;
    out->Rise = 0;
    if (unknown) *unknown = 0;
    if (verdict != BOX_OK) return verdict;

    bool           said_kind = false;
    const uint8_t *line, *word = NULL;
    uint64_t       length, word_length = 0;
    while (BoxTextLine(&text, &line, &length)) {
        uint64_t at = 0;
        BoxTextWord(line, length, &at, &word, &word_length);
        int  kind = -1;
        bool rise = BoxSpanIs(word, word_length, "rise");
        for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
            if (BoxSpanIs(word, word_length, kinds[i])) kind = (int)i;
        if (kind < 0 && !rise) {
            if (unknown && *unknown == 0) {
                uint64_t number = 1;
                for (const uint8_t *at_byte = bytes; at_byte < line; at_byte++) number += *at_byte == '\n';
                *unknown = number;
            }
            continue;
        }
        if (BoxTextWordCount(line, length) != 1 || (rise ? out->Rise : said_kind)) return BOX_TEXT_LAYOUT;
        if (rise) out->Rise = 1;
        else {
            said_kind = true;
            out->Kind = (uint8_t)kind;
        }
    }
    return BOX_OK;
}

/* ── asks ────────────────────────────────────────────────────────────────── *
 *
 * What a program asks to be allowed, a line each after "asks":
 *
 *   ask utility [why]      to start other programs, to read the machine's log,
 *                          to flush a disk and to defragment a file
 *   ask network [why]      to use the network
 *   ask see-tags [why]     to see the person's tags — the labels the person
 *                          finds files by — on files and in questions
 *   ask change-tags [why]  to put the person's tags on and take them off, and
 *                          a program's own off a file as the person's hands;
 *                          seeing them with it
 *
 * Every program has tags of its own without asking: its own words for its own
 * files, which no other program meets (tagfs.h, TagOwner). The person's are
 * the person's, and a program that has not asked for them neither sees nor
 * changes one.
 *
 * The why is words for the person and is not read. What a program does not ask
 * it is not given: a box with no `asks` part may use storage and the other
 * things every program may, and nothing more. `system` is never asked: it is
 * the role of a file on the volume (TAGFS_FILE_ROLE_SYSTEM), which a box does
 * not get by asking — it says what it is in its `role` part (below), and only
 * whoever lays the volume reads that. A line of an ask this reader does not
 * know is passed over and is not granted; *unknown says the first, as
 * BoxLifeRead's does.
 */

typedef struct {
    uint8_t Utility;
    uint8_t Network;
    uint8_t SeeTags;
    uint8_t ChangeTags;
} BoxAsks;

static inline BoxVerdict BoxAsksRead(const uint8_t *bytes, uint64_t size, BoxAsks *out, uint64_t *unknown)
{
    BoxText    text;
    BoxVerdict verdict = BoxTextBegin(bytes, size, BOX_KIND_ASKS, &text);
    out->Utility    = 0;
    out->Network    = 0;
    out->SeeTags    = 0;
    out->ChangeTags = 0;
    if (unknown) *unknown = 0;
    if (verdict != BOX_OK) return verdict;

    const uint8_t *line, *word = NULL;
    uint64_t       length, word_length = 0;
    while (BoxTextLine(&text, &line, &length)) {
        uint64_t at = 0;
        if (!BoxTextWord(line, length, &at, &word, &word_length) || !BoxSpanIs(word, word_length, "ask"))
            return BOX_TEXT_LAYOUT;
        if (!BoxTextWord(line, length, &at, &word, &word_length)) return BOX_TEXT_LAYOUT;
        uint8_t *asked = BoxSpanIs(word, word_length, "utility")       ? &out->Utility
                       : BoxSpanIs(word, word_length, "network")       ? &out->Network
                       : BoxSpanIs(word, word_length, "see-tags")      ? &out->SeeTags
                       : BoxSpanIs(word, word_length, "change-tags")   ? &out->ChangeTags : NULL;
        if (!asked) {
            if (unknown && *unknown == 0) {
                uint64_t number = 1;
                for (const uint8_t *at_byte = bytes; at_byte < line; at_byte++) number += *at_byte == '\n';
                *unknown = number;
            }
            continue;
        }
        if (*asked) return BOX_TEXT_LAYOUT;
        *asked = 1;
    }
    return BOX_OK;
}

/* ── role ────────────────────────────────────────────────────────────────── *
 *
 * What the program is to the machine it is laid on, said by the box's maker
 * in one line after "role":
 *
 *   system    a program the machine runs on: the display, the hub, the
 *             programs that start, stop and mend it
 *   boot      a program a loader may take to start the machine with, after
 *             the kernel
 *
 * It is said for the one who lays a volume — the image builder, and one day
 * an installer that asks the person — and is read by nobody at runtime: a
 * cabin is given nothing for it. A volume's maker that trusts it puts the file
 * in the role in its record (TAGFS_FILE_ROLE_*); a box that carries the part
 * and is copied onto a volume by a person is no more trusted than one that
 * does not, and no tag of a file ever said it. A word this reader does not
 * know is passed over — the file is ordinary — and *unknown says its line; a
 * box with no `role` part is ordinary, and ordinary is never less safe than a
 * role.
 */

typedef enum {
    BOX_ROLE_ORDINARY = 0,
    BOX_ROLE_SYSTEM,
    BOX_ROLE_BOOT
} BoxRole;

static inline BoxVerdict BoxRoleRead(const uint8_t *bytes, uint64_t size, BoxRole *out, uint64_t *unknown)
{
    BoxText    text;
    BoxVerdict verdict = BoxTextBegin(bytes, size, BOX_KIND_ROLE, &text);
    *out = BOX_ROLE_ORDINARY;
    if (unknown) *unknown = 0;
    if (verdict != BOX_OK) return verdict;

    const uint8_t *line, *word = NULL;
    uint64_t       length, word_length = 0, at = 0;
    if (!BoxTextLine(&text, &line, &length) || BoxTextWordCount(line, length) != 1) return BOX_TEXT_LAYOUT;
    BoxTextWord(line, length, &at, &word, &word_length);
    if (BoxSpanIs(word, word_length, "system")) {
        *out = BOX_ROLE_SYSTEM;
    } else if (BoxSpanIs(word, word_length, "boot")) {
        *out = BOX_ROLE_BOOT;
    } else if (unknown) {
        uint64_t number = 1;
        for (const uint8_t *at_byte = bytes; at_byte < line; at_byte++) number += *at_byte == '\n';
        *unknown = number;
    }
    if (BoxTextLine(&text, &line, &length)) {
        *out = BOX_ROLE_ORDINARY;
        return BOX_TEXT_LAYOUT;
    }
    return BOX_OK;
}

/* ── signature ───────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t        How;             /* BOX_SIGN_RSA2048_SHA256, or one this reader cannot check */
    const uint8_t *Name;            /* the program's name as its author gives it */
    uint8_t        NameLength;
    const uint8_t *Key;             /* the author's public modulus, big-endian */
    uint16_t       KeyBytes;
    const uint8_t *Signature;       /* big-endian */
    uint16_t       SignatureBytes;
} BoxSignature;

/* The `signature` part's `size` bytes at `bytes`: BOX_OK, or
 * BOX_SIGNATURE_LAYOUT. A how this reader cannot check is read no further than
 * its first byte (How alone is set) and is BOX_OK: a box signed another way is,
 * to this system, a box that carries no signature. */
static inline BoxVerdict BoxSignatureRead(const uint8_t *bytes, uint64_t size, BoxSignature *out)
{
    *out = (BoxSignature){ 0 };
    if (size < BOX_SIGN_HEAD_BYTES) return BOX_SIGNATURE_LAYOUT;
    out->How = bytes[0];
    if (out->How != BOX_SIGN_RSA2048_SHA256) return BOX_OK;

    uint8_t  name = bytes[1];
    uint16_t key  = BoxU16(bytes + 2), sig = BoxU16(bytes + 4);
    if (name == 0 || name > BOX_SIGN_NAME_MAX || key != BOX_SIGN_RSA2048_BYTES || sig != BOX_SIGN_RSA2048_BYTES ||
        BoxU16(bytes + 6) != 0 || size != (uint64_t)BOX_SIGN_HEAD_BYTES + name + key + sig ||
        !BoxNameSpelled(bytes + BOX_SIGN_HEAD_BYTES, name))
        return BOX_SIGNATURE_LAYOUT;
    out->Name           = bytes + BOX_SIGN_HEAD_BYTES;
    out->NameLength     = name;
    out->Key            = out->Name + name;
    out->KeyBytes       = key;
    out->Signature      = out->Key + key;
    out->SignatureBytes = sig;
    return BOX_OK;
}

/* The seal a signature is over: the box's, made with the `signature` part's
 * own hash in the table read as zeros beside the seal's own 32 bytes — the
 * hash of bytes the signature is part of cannot be in what it signs. */
static inline void BoxSignedSeal(const BoxTable *table, const BoxPart *signature, const BoxHasher *hasher,
                                 uint8_t seal[BOX_HASH_BYTES])
{
    static const uint8_t zeros[BOX_HASH_BYTES] = { 0 };
    const uint8_t *prefix  = table->Prefix;
    uint64_t       seal_to = BOX_AT_SEAL + BOX_SEAL_BYTES;
    uint64_t       hash_at = table->Head.TableAt + (uint64_t)signature->Index * BOX_ENTRY_BYTES + BOX_ENTRY_HASH;
    hasher->Begin(hasher->State);
    hasher->Add(hasher->State, prefix, BOX_AT_SEAL);
    hasher->Add(hasher->State, zeros, BOX_SEAL_BYTES);
    hasher->Add(hasher->State, prefix + seal_to, hash_at - seal_to);
    hasher->Add(hasher->State, zeros, BOX_HASH_BYTES);
    hasher->Add(hasher->State, prefix + hash_at + BOX_HASH_BYTES,
                table->Head.PrefixBytes - hash_at - BOX_HASH_BYTES);
    hasher->End(hasher->State, seal);
}

/* What the signature is over: the SHA-256 of the signed seal followed by the
 * program's name — the digest the author's key signs and a checker verifies
 * (box_format.h). */
static inline void BoxSignedDigest(const BoxTable *table, const BoxPart *part, const BoxSignature *signature,
                                   const BoxHasher *hasher, uint8_t digest[BOX_HASH_BYTES])
{
    uint8_t seal[BOX_HASH_BYTES];
    BoxSignedSeal(table, part, hasher, seal);
    hasher->Begin(hasher->State);
    hasher->Add(hasher->State, seal, BOX_HASH_BYTES);
    hasher->Add(hasher->State, signature->Name, signature->NameLength);
    hasher->End(hasher->State, digest);
}

/* ── words for a refusal ─────────────────────────────────────────────────── */

/* The reason a verdict refuses, said so it follows "is not started: "; a
 * verdict about one part is followed by the caller's "(part NAME)". */
static inline const char *BoxVerdictText(BoxVerdict verdict)
{
    switch (verdict) {
    case BOX_OK:                    return "nothing is wrong with it";
    case BOX_NOT_BOX:               return "it does not begin as a box";
    case BOX_TRUNCATED:             return "it ends before what its header says it holds";
    case BOX_HEADER_SIZE:           return "its header says a length a header cannot have";
    case BOX_NO_PARTS:              return "it holds no parts";
    case BOX_TABLE_MISPLACED:       return "its table does not start where its header ends";
    case BOX_NAMES_MISPLACED:       return "its names do not start where its table ends";
    case BOX_FILE_SIZE:             return "its header says another length than the file has";
    case BOX_ENTRY_NOT_ZERO:        return "a part's entry is not zero where it must be";
    case BOX_FLAGS_UNKNOWN:         return "a part carries a mark this system does not know";
    case BOX_NAME_MISPLACED:        return "a part's name is not where the names say it is";
    case BOX_NAME_SPELLING:         return "a part's name is not spelled as a tag";
    case BOX_NAME_ORDER:            return "a part's name is out of order, or twice in the box";
    case BOX_NAME_NOT_STANDARD:     return "a part's name says more than its kind allows";
    case BOX_NAMES_LEFT_OVER:       return "its names hold bytes no part is named by";
    case BOX_PART_MISPLACED:        return "a part does not start where the one way of writing a box puts it";
    case BOX_PART_OUTSIDE:          return "a part runs past the end of the file";
    case BOX_FILE_LEFT_OVER:        return "the file goes on after its last part";
    case BOX_PART_MEMORY:           return "a part's size in memory does not fit its kind";
    case BOX_PART_PLACE:            return "a part's place in the image does not fit its kind";
    case BOX_PART_FLAGS:            return "a part's marks do not fit its kind";
    case BOX_PART_UNKNOWN_REQUIRED: return "it needs a part this system does not know";
    case BOX_LOADABLE_OVERLAP:      return "two parts of its image share a page";
    case BOX_NO_CODE:               return "it has no code";
    case BOX_NO_DOORS:              return "it has no doors";
    case BOX_SEAL:                  return "it is damaged: its seal differs";
    case BOX_PART_HASH:             return "a part is damaged: its hash differs";
    case BOX_GAP_NOT_ZERO:          return "the bytes between its parts are not zeros";
    case BOX_DOORS_LAYOUT:          return "its doors are not laid out as doors are";
    case BOX_DOOR_OUTSIDE_CODE:     return "a door leads outside its code";
    case BOX_NO_START:              return "it has no door named start";
    case BOX_THREAD_LAYOUT:         return "its thread template is not laid out as one is";
    case BOX_FIXUPS_LAYOUT:         return "its fixups are not laid out as fixups are";
    case BOX_FIXUP_ORDER:           return "its fixups do not rise";
    case BOX_FIXUP_OUTSIDE:         return "a fixup lies outside the bytes of its image";
    case BOX_TEXT_LAYOUT:           return "a part written as text is not written as such a part is";
    case BOX_SIGNATURE_LAYOUT:      return "its signature is not laid out as a signature is";
    case BOX_VERDICT_COUNT:         break;
    }
    return "it was refused for a reason this system has no words for";
}

#endif /* BOX_READER_H */
