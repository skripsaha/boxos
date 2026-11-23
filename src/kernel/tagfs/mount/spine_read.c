#include "spine_read.h"
#include "book_unit.h"

/*
 * The Spine's slots, read and judged (spine_read.h). A slot's stream is read
 * by the one codec every reader and writer of a slot keeps (volume_tally.h),
 * and its shape — which records, in which order, how many of each — judged
 * by the one rule every reader of a slot keeps (volume_spine.h), the UEFI
 * loader's and volume-read's tally.py SlotParse too; what is kept here is
 * what the kernel says of it.
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

/*
 * One step's answer into the slot (volume_spine.h, SpineSlotStep), in the
 * words the slot's lines have always said it: a reason the volume may only be
 * read kept with it, a reason it is torn said into s->why. OK, or
 * ERR_TAGFS_CORRUPTED for a torn slot, or ERR_NO_MEMORY.
 */
static error_t SpineSaid(SpineSlot* s, SpineSays says, const SpineWalk* w)
{
    char               words[160];
    unsigned long long kind = (unsigned long long)w->kind;

    switch (says) {
    case SPINE_SAYS_RECORD:
    case SPINE_SAYS_WHOLE:
        return OK;
    case SPINE_SAYS_UNKNOWN:
        ksnprintf(words, sizeof(words), "slot %u carries a record of kind %llu this reader does not "
                  "know and may not step over — written by a kernel that knows more", s->index, kind);
        return SpineSlotGlass(s, words) ? OK : ERR_NO_MEMORY;
    case SPINE_SAYS_LONGER:
        ksnprintf(words, sizeof(words), "slot %u's record of kind %llu is %u byte(s) longer than this "
                  "reader knows — written by a kernel that knows more", s->index, kind, w->longer);
        return SpineSlotGlass(s, words) ? OK : ERR_NO_MEMORY;
    case SPINE_SAYS_NOT_HEAD:
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: its first record is of kind %llu, "
                  "not SpineHead", kind);
        break;
    case SPINE_SAYS_SECOND:
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: it carries a second record of "
                  "kind %llu", kind);
        break;
    case SPINE_SAYS_BROKEN:
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: its record of kind %llu does not "
                  "read", kind);
        break;
    case SPINE_SAYS_TREE_TWICE:
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: it names a root of tree %llu "
                  "twice", (unsigned long long)w->root.tree);
        break;
    case SPINE_SAYS_RUNS_PAST:
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: a record at byte %u runs past "
                  "the slot", w->at);
        break;
    case SPINE_SAYS_NO_RIBBON:
    case SPINE_SAYS_NO_COUNTS:
        ksnprintf(s->why, sizeof(s->why), "its stream does not parse: it carries no %s",
                  says == SPINE_SAYS_NO_RIBBON ? "SpineRibbon" : "SpineCounts");
        break;
    }
    return ERR_TAGFS_CORRUPTED;
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

/* The slot's stream, record by record (volume_spine.h), what it says read into
 * the slot, and the leads of its sectors held to where its records begin. */
static error_t SpineParse(SpineSlot* s, const BookUnit* u)
{
    uint32_t  room   = u->count * BOOK_PAYLOAD_BYTES;
    uint32_t* starts = kmalloc(sizeof(uint32_t) * (room / 2u + 1u));   /* a record is 2 bytes at least */
    uint32_t  roots  = SpineRootsNamed(u->stream, room);
    error_t   rc     = OK;
    SpineWalk w;
    SpineSays says;

    if (!starts || (roots && !(s->root = kmalloc(sizeof(SpineRoot) * roots)))) {
        kfree(starts);
        return ERR_NO_MEMORY;
    }
    memset(&w, 0, sizeof(w));
    do {
        starts[w.nth] = w.at;
        says          = SpineSlotStep(u->stream, room, &w);
        if ((says == SPINE_SAYS_RECORD || says == SPINE_SAYS_LONGER) && w.kind == SPINE_ROOT)
            s->root[s->roots++] = w.root;
        rc = SpineSaid(s, says, &w);
    } while (rc == OK && says != SPINE_SAYS_WHOLE);
    s->head   = w.head;
    s->ribbon = w.ribbon;
    s->counts = w.counts;
    s->flags  = w.flags;
    uint32_t named = 0, given = 0;
    uint32_t k     = rc == OK ? BookUnitLeadWrong(u, starts, w.nth, &named, &given) : u->count;
    if (k < u->count)
        ksnprintf(s->shape, sizeof(s->shape), "slot %u: sector %u names lead %u where its records give "
                  "it %u (§2.1)", s->index, k, named, given);
    kfree(starts);
    return rc;
}

/* *s judged from the unit *u its read (`rc`) laid in, let go of here. */
static error_t SpineSlotFrom(SpineSlot* s, BookUnit* u, error_t rc)
{
    if (rc == OK) {
        if (BookUnitBlank(u)) {
            s->state = SPINE_SLOT_BLANK;
        } else if (!u->whole) {
            s->state = SPINE_SLOT_TORN;
            memcpy(s->why, u->why, sizeof(u->why));
            for (uint32_t k = 0; u->state && k < u->count && !s->unread; k++)
                s->unread = u->state[k] == BOOK_SECTOR_UNREAD;
        } else {
            s->seq = u->head.seq;
            rc     = SpineParse(s, u);
            if (rc == OK || rc == ERR_TAGFS_CORRUPTED) {
                s->state  = rc == OK ? SPINE_SLOT_VALID : SPINE_SLOT_TORN;
                s->stream = u->stream;
                u->stream = NULL;
                rc        = OK;
            }
        }
    }
    BookUnitLetGo(u);
    if (s->state != SPINE_SLOT_VALID) {
        kfree(s->root);
        kfree(s->stream);
        s->root   = NULL;
        s->stream = NULL;
        s->roots  = 0;
    }
    return rc;
}

error_t SpineSlotJudge(TagFSVolume* vol, SpineSlot* s)
{
    BookUnit u;
    error_t  rc = BookUnitRead(vol, s->block * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, BOOK_UNIT_SLOT, 0, &u);
    return SpineSlotFrom(s, &u, rc);
}

error_t SpineSlotJudgeSectors(TagFSVolume* vol, SpineSlot* s, const uint8_t* sectors)
{
    BookUnit u;
    error_t  rc = BookUnitOf(vol, s->block * VOLUME_BLOCK_SECTORS, VOLUME_BLOCK_SECTORS, BOOK_UNIT_SLOT, 0, sectors, &u);
    return SpineSlotFrom(s, &u, rc);
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

/* A VALID slot kept among the others in the order the mount tries them in —
 * every reader of a book's (volume_spine.h, SpineSlotNewer): newest first, by
 * seq, and a tie by index. False with no memory. */
static bool SpineKeep(SpineSlot** valid, uint32_t* n, const SpineSlot* s)
{
    SpineSlot* more = kmalloc(sizeof(SpineSlot) * (*n + 1u));
    if (!more)
        return false;
    uint32_t at = 0;
    while (at < *n && !SpineSlotNewer(s->seq, s->index, (*valid)[at].seq, (*valid)[at].index))
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

error_t SpineRead(TagFSVolume* vol, const VolumeFrame* frame, SpineSlot** out, uint32_t* count, char* what, size_t room)
{
    SpineSlot* slot   = kmalloc(sizeof(SpineSlot));
    size_t     said   = 0;
    bool       unread = false;
    error_t    rc     = slot ? OK : ERR_NO_MEMORY;

    *out   = NULL;
    *count = 0;
    what[0] = '\0';
    for (uint32_t i = 0; rc == OK && i < frame->spine_slots; i++) {
        memset(slot, 0, sizeof(*slot));
        slot->index = i;
        slot->block = frame->spine_block + (uint64_t)i * frame->spine_stride;
        if ((rc = SpineSlotJudge(vol, slot)) != OK)
            break;
        if (slot->state == SPINE_SLOT_TORN)
            kprintf("[TagFS] Spine slot %u at block %llu does not hold: %s — the book is read off "
                    "another\n", i, (unsigned long long)slot->block, slot->why);
        unread |= slot->state == SPINE_SLOT_TORN && slot->unread;
        if (said + 1u < room)
            said += ksnprintf(what + said, room - said, "%sslot %u %s", i ? "; " : "", i,
                              slot->state == SPINE_SLOT_BLANK ? "blank" :
                              slot->state == SPINE_SLOT_TORN  ? slot->why : "holds");
        if (slot->state != SPINE_SLOT_VALID)
            SpineSlotLetGo(slot);
        else if (!SpineKeep(out, count, slot))
            rc = ERR_NO_MEMORY;
    }
    if (rc == OK && *count == 0 && unread) {
        TagFSNoteReadRefused(vol);
        rc = ERR_READ_FAILED;
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
