#include "spine_read.h"
#include "book_unit.h"

/*
 * The Spine's slots, read and judged (spine_read.h). A slot's stream is read
 * by the one codec every reader and writer of a slot keeps (volume_tally.h);
 * what is judged here is the slot's shape — which records, in which order,
 * how many of each — the rule volume-read's tally.py SlotParse keeps too.
 */

/* A reason the slot gives that the volume may only be read, kept to be said
 * if the mount stands on it. False with no memory to keep it. */
static bool SpineSlotGlass(SpineSlot* s, const char* why)
{
    char** more = kmalloc(sizeof(char*) * (s->nglass + 1u));
    char*  text = kmalloc(strlen(why) + 1u);
    if (!more || !text) {
        kfree(more);
        kfree(text);
        return false;
    }
    if (s->nglass)
        memcpy(more, s->glass, sizeof(char*) * s->nglass);
    strcpy(text, why);
    more[s->nglass++] = text;
    kfree(s->glass);
    s->glass = more;
    return true;
}

/* The bytes of a known record's body this reader knows — what is past them
 * was written by a kernel that knows more. */
static uint32_t SpineKnownBytes(const SpineSlot* s, uint64_t kind, const uint8_t* b, uint32_t len)
{
    switch (kind) {
    case SPINE_HEAD:
        return TallyVarintBytes(s->head.tally_seq) + TallyVarintBytes(s->head.stitch_seq) + 4u +
               TallyVarintBytes(s->head.made_ns);
    case SPINE_ROOT: {
        const SpineRoot* r = &s->root[s->roots - 1u];
        uint32_t n = TallyVarintBytes(r->tree) + TallyVarintBytes(r->flags) + TallyVarintBytes(r->level) +
                     TallyVarintBytes(r->items);
        BookPtr ptr;
        return n + BookPtrGet(b + n, len - n, &ptr);
    }
    case SPINE_RIBBON:
        return TallyVarintBytes(s->ribbon.at) + TallyVarintBytes(s->ribbon.quire_block) +
               TallyVarintBytes(s->ribbon.quire_blocks) + TallyVarintBytes(s->ribbon.next_block) +
               TallyVarintBytes(s->ribbon.next_blocks);
    case SPINE_COUNTS:
        return TallyVarintBytes(s->counts.next_fid) + TallyVarintBytes(s->counts.next_tag) +
               TallyVarintBytes(s->counts.next_bookmark);
    default:
        return TallyVarintBytes(s->flags);
    }
}

/* One known record's body into the slot: TALLY_BODY_* as the codec reads it. */
static int SpineRecordRead(SpineSlot* s, uint64_t kind, const uint8_t* b, uint32_t len)
{
    switch (kind) {
    case SPINE_HEAD:   return SpineHeadGet(b, len, &s->head);
    case SPINE_ROOT:   return SpineRootGet(b, len, &s->root[s->roots++]);
    case SPINE_RIBBON: return SpineRibbonGet(b, len, &s->ribbon);
    case SPINE_COUNTS: return SpineCountsGet(b, len, &s->counts);
    default:           return SpineFlagsGet(b, len, &s->flags);
    }
}

/*
 * One record of the slot's stream, the `nth`: OK, or ERR_TAGFS_CORRUPTED with
 * why the slot is torn, or ERR_NO_MEMORY. `seen` holds a bit for each kind
 * met. SpineHead first; a second SpineHead, SpineRibbon or SpineCounts, or a
 * second root of one tree, is no slot a writer writes; SpineFlags may come
 * again, the last one standing.
 */
static error_t SpineRecord(SpineSlot* s, uint32_t nth, uint64_t tag, const uint8_t* b, uint32_t len,
                           uint32_t* seen)
{
    char     words[160];
    uint64_t kind = tag >> 1;

    if (nth == 0 && kind != SPINE_HEAD) {
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: its first record is of kind %llu, "
                  "not SpineHead", (unsigned long long)kind);
        return ERR_TAGFS_CORRUPTED;
    }
    if (kind < SPINE_HEAD || kind > SPINE_FLAGS) {
        if (tag & 1u)
            return OK;
        ksnprintf(words, sizeof(words), "slot %u carries a record of kind %llu this reader does not "
                  "know and may not step over — written by a kernel that knows more", s->index,
                  (unsigned long long)kind);
        return SpineSlotGlass(s, words) ? OK : ERR_NO_MEMORY;
    }
    if (kind != SPINE_FLAGS && kind != SPINE_ROOT && (*seen & (1u << kind))) {
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: it carries a second record of "
                  "kind %llu", (unsigned long long)kind);
        return ERR_TAGFS_CORRUPTED;
    }
    *seen |= 1u << kind;
    int read = SpineRecordRead(s, kind, b, len);
    if (read == TALLY_BODY_BROKEN) {
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: its record of kind %llu does not "
                  "read", (unsigned long long)kind);
        return ERR_TAGFS_CORRUPTED;
    }
    for (uint32_t r = 0; kind == SPINE_ROOT && r + 1u < s->roots; r++) {
        if (s->root[r].tree == s->root[s->roots - 1u].tree) {
            ksnprintf(s->why, sizeof(s->why), "its stream does not parse: it names a root of tree %llu "
                      "twice", (unsigned long long)s->root[r].tree);
            return ERR_TAGFS_CORRUPTED;
        }
    }
    if (read == TALLY_BODY_LONGER) {
        ksnprintf(words, sizeof(words), "slot %u's record of kind %llu is %u byte(s) longer than this "
                  "reader knows — written by a kernel that knows more", s->index,
                  (unsigned long long)kind, len - SpineKnownBytes(s, kind, b, len));
        if (!SpineSlotGlass(s, words))
            return ERR_NO_MEMORY;
    }
    return OK;
}

/* The roots a slot's stream carries, counted before they are read: as many
 * SpineRoot records as the walk meets. */
static uint32_t SpineRootsNamed(const uint8_t* stream, uint32_t room)
{
    uint32_t at = 0, body, len, n = 0;
    uint64_t tag;
    while (BookEntryNext(stream, room, &at, &tag, &body, &len) == 1)
        n += (tag >> 1) == SPINE_ROOT;
    return n;
}

/* The slot's stream, record by record, and the leads of its sectors held to
 * where its records begin. */
static error_t SpineParse(SpineSlot* s, const BookUnit* u)
{
    uint32_t  room   = u->count * BOOK_PAYLOAD_BYTES, at = 0, body, len, seen = 0, nth = 0;
    uint32_t* starts = kmalloc(sizeof(uint32_t) * (room / 2u + 1u));   /* a record is 2 bytes at least */
    uint32_t  roots  = SpineRootsNamed(u->stream, room);
    error_t   rc     = OK;
    uint64_t  tag;
    int       step;

    if (!starts || (roots && !(s->root = kmalloc(sizeof(SpineRoot) * roots)))) {
        kfree(starts);
        return ERR_NO_MEMORY;
    }
    while (starts[nth] = at, (step = BookEntryNext(u->stream, room, &at, &tag, &body, &len)) == 1)
        if ((rc = SpineRecord(s, nth++, tag, u->stream + body, len, &seen)) != OK)
            break;
    if (rc == OK && step < 0) {
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: a record at byte %u runs past "
                  "the slot", at);
        rc = ERR_TAGFS_CORRUPTED;
    } else if (rc == OK && (!(seen & (1u << SPINE_RIBBON)) || !(seen & (1u << SPINE_COUNTS)))) {
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: it carries no %s",
                  !(seen & (1u << SPINE_RIBBON)) ? "SpineRibbon" : "SpineCounts");
        rc = ERR_TAGFS_CORRUPTED;
    }
    uint32_t named = 0, given = 0;
    uint32_t k     = rc == OK ? BookUnitLeadWrong(u, starts, nth, &named, &given) : u->count;
    if (k < u->count)
        ksnprintf(s->shape, sizeof(s->shape), "slot %u: sector %u names lead %u where its records give "
                  "it %u (§2.1)", s->index, k, named, given);
    kfree(starts);
    return rc;
}

error_t SpineSlotJudge(SpineSlot* s)
{
    BookUnit u;
    error_t  rc = BookUnitRead(s->block * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, BOOK_UNIT_SLOT, 0, &u);
    if (rc == OK) {
        if (BookUnitBlank(&u)) {
            s->state = SPINE_SLOT_BLANK;
        } else if (!u.whole) {
            s->state = SPINE_SLOT_TORN;
            memcpy(s->why, u.why, sizeof(u.why));
        } else {
            s->seq = u.head.seq;
            rc     = SpineParse(s, &u);
            if (rc == OK || rc == ERR_TAGFS_CORRUPTED) {
                s->state  = rc == OK ? SPINE_SLOT_VALID : SPINE_SLOT_TORN;
                s->stream = u.stream;
                u.stream  = NULL;
                rc        = OK;
            }
        }
    }
    BookUnitLetGo(&u);
    if (s->state != SPINE_SLOT_VALID) {
        kfree(s->root);
        kfree(s->stream);
        s->root   = NULL;
        s->stream = NULL;
        s->roots  = 0;
    }
    return rc;
}

void SpineSlotLetGo(SpineSlot* s)
{
    for (uint32_t i = 0; i < s->nglass; i++)
        kfree(s->glass[i]);
    kfree(s->glass);
    kfree(s->root);
    kfree(s->stream);
    s->glass  = NULL;
    s->root   = NULL;
    s->stream = NULL;
    s->nglass = s->roots = 0;
}

void SpineLetGo(SpineSlot* slots, uint32_t count)
{
    for (uint32_t i = 0; slots && i < count; i++)
        SpineSlotLetGo(&slots[i]);
    kfree(slots);
}

/* A VALID slot kept among the others, newest first: by seq, and a tie by
 * index — the order the mount tries them in. False with no memory. */
static bool SpineKeep(SpineSlot** valid, uint32_t* n, const SpineSlot* s)
{
    SpineSlot* more = kmalloc(sizeof(SpineSlot) * (*n + 1u));
    if (!more)
        return false;
    uint32_t at = 0;
    while (at < *n && (*valid)[at].seq >= s->seq)
        at++;
    if (at)
        memcpy(more, *valid, sizeof(SpineSlot) * at);
    more[at] = *s;
    if (*n > at)
        memcpy(more + at + 1u, *valid + at, sizeof(SpineSlot) * (*n - at));
    kfree(*valid);
    *valid = more;
    (*n)++;
    return true;
}

error_t SpineRead(const VolumeFrame* frame, SpineSlot** out, uint32_t* count, char* what, size_t room)
{
    SpineSlot* slot = kmalloc(sizeof(SpineSlot));
    size_t     said = 0;
    error_t    rc   = slot ? OK : ERR_NO_MEMORY;

    *out   = NULL;
    *count = 0;
    what[0] = '\0';
    for (uint32_t i = 0; rc == OK && i < frame->spine_slots; i++) {
        memset(slot, 0, sizeof(*slot));
        slot->index = i;
        slot->block = frame->spine_block + (uint64_t)i * frame->spine_stride;
        if ((rc = SpineSlotJudge(slot)) != OK)
            break;
        if (slot->state == SPINE_SLOT_TORN)
            kprintf("[TagFS] Spine slot %u at block %llu does not hold: %s — the book is read off "
                    "another\n", i, (unsigned long long)slot->block, slot->why);
        if (said + 1u < room)
            said += ksnprintf(what + said, room - said, "%sslot %u %s", i ? "; " : "", i,
                              slot->state == SPINE_SLOT_BLANK ? "blank" :
                              slot->state == SPINE_SLOT_TORN  ? slot->why : "holds");
        if (slot->state != SPINE_SLOT_VALID)
            SpineSlotLetGo(slot);
        else if (!SpineKeep(out, count, slot))
            rc = ERR_NO_MEMORY;
    }
    if (rc != OK) {
        if (slot)
            SpineSlotLetGo(slot);       /* the one in hand was not kept */
        SpineLetGo(*out, *count);
        *out   = NULL;
        *count = 0;
    }
    kfree(slot);
    return rc;
}
