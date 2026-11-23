#include "carry.h"
#include "tagfs_volume.h"
#include "../ink/ink.h"
#include "../helm/helm.h"
#include "../file_map/file_map.h"
#include "atomics.h"

/* The bytes one turn on the volume a file comes from reads of it. */
#define CARRY_PIECE  (128u * 1024u)
/* The arriving files one look of the service takes at a time. */
#define CARRY_LOOK   64u

static void CarryTurn(HelmWaiter* w);
static void CarryEnd(Carry* c);

/* The next turn, on `at`. */
static void CarryNext(Carry* c, TagFSVolume* at, CarryStep step)
{
    c->step = step;
    c->at   = at;
    HelmReady(at, &c->turn);
}

/* The carry ends with `rc`: what is left is let go in a turn of its own. */
static void CarryFail(Carry* c, error_t rc)
{
    if (c->rc == OK)
        c->rc = rc;
    CarryNext(c, TagFSHome(), CARRY_END);
}

// ----------------------------------------------------------------------------
// Waits: a cycle with room, a Helm handed, a copy settled
// ----------------------------------------------------------------------------

/* A cycle the carry waited for ended: the step it waited in is asked again on
 * the volume it was asked of. */
static void CarryCycleCame(SettleWaiter* w, error_t rc)
{
    Carry* c = container_of(w, Carry, wait);
    c->came  = rc;
    HelmReady(c->at, &c->turn);
}

/* No room on `vol` now: the step is asked again once its next cycle ended. */
static void CarryRoomWait(Carry* c, TagFSVolume* vol, CarryStep step)
{
    c->step = step;
    c->at   = vol;
    c->came = OK;
    if (!tagfs_enter(vol)) {
        CarryFail(c, ERR_DEVICE_NOT_READY);
        return;
    }
    SettleWaitLinkCycle(vol, &c->wait, CarryCycleCame);
    tagfs_leave(vol);
}

static void CarryHelmHanded(HelmWaiter* w)
{
    Carry* c = container_of(w, Carry, helm);
    __atomic_store_n(&c->holds, 1u, __ATOMIC_RELEASE);
    HelmReady(c->to, &c->turn);
}

static void CarryHelmGone(HelmWaiter* w, uint64_t lost)
{
    Carry* c = container_of(w, Carry, helm);
    (void)lost;
    CarryFail(c, ERR_DEVICE_NOT_READY);
}

// ----------------------------------------------------------------------------
// Who a program is on the volume a file goes to
// ----------------------------------------------------------------------------

/* Program `id` of `from` — its file there, or its passport — as the volume
 * the carry goes to may know it: the copy this carry made of its file, or
 * its passport. Inside `from`'s door. */
static CarryWho CarryWhoOf(const Carry* c, TagFSVolume* from, uint64_t id)
{
    CarryWho who = { 0, 0, false };
    if (id & TAGFS_PASSPORT_BIT) {
        who.passport = id;
        return who;
    }
    for (uint32_t i = 0; i < c->done; i++)
        if (c->items[i].from == from && c->items[i].fid == id) {
            who.fid = c->items[i].arrived;
            return who;
        }
    FileRecord* rec = id ? FileMapRecordHold(from->state.files, id, NULL) : NULL;
    if (rec) {
        who.passport = rec->passport;
        FileRecordRelease(rec);
    }
    return who;
}

/* The id `who` is spoken by on `to`: the copy's, else the passport — 0 when
 * `to` holds no file of the program, which would retire whatever it said
 * there (retire/retire.h). Inside `to`'s door. */
static uint64_t CarryWhoOn(const Carry* c, CarryWho who)
{
    if (who.fid)
        return who.fid;
    return who.passport && TagFSHoldsProgram(c->to, who.passport) ? who.passport : 0;
}

// ----------------------------------------------------------------------------
// SURVEY, on the volume the file comes from
// ----------------------------------------------------------------------------

static void CarryFileLetGo(Carry* c)
{
    for (uint32_t i = 0; i < c->tag_count; i++)
        kfree(c->tags[i].text);
    kfree(c->tags);
    kfree(c->shown);
    kfree(c->name);
    kfree(c->seen_tags);
    kfree(c->seen_shown);
    c->tags             = NULL;
    c->tag_count        = 0;
    c->shown            = NULL;
    c->shown_count      = 0;
    c->name             = NULL;
    c->seen_tags        = NULL;
    c->seen_shown       = NULL;
    c->seen_tag_count   = 0;
    c->seen_shown_count = 0;
}

/* Whether `rec` is still the file the carry read, as a person sees one: what
 * it holds and when it was written, its name, tags, marks, term and the
 * programs it shows its tags to. A volume laying its tail in its block, or
 * sharing its blocks with another file's, changes none of it. */
static bool CarrySame(const FileRecord* rec, void* ctx)
{
    const Carry* c    = ctx;
    const uint32_t own = TAGFS_FILE_HEAD;      /* the volume's own: it follows what the file holds */
    if (rec->size != c->size || rec->modified_time != c->modified_ns || (rec->flags & ~own) != (c->flags & ~own) ||
        rec->passport != c->passport || rec->tag_count != c->seen_tag_count ||
        rec->shown_count != c->seen_shown_count || strlen(c->name) != rec->name_len ||
        memcmp(c->name, rec->name, rec->name_len) != 0 ||
        ((rec->flags & TAGFS_FILE_DELETED) && rec->keep_until != c->keep_until))
        return false;
    return memcmp(rec->tags, c->seen_tags, sizeof(uint32_t) * rec->tag_count) == 0 &&
           memcmp(rec->shown_to, c->seen_shown, sizeof(uint64_t) * rec->shown_count) == 0;
}

/* The file's tags, their text copied and whose they are as `to` may know them. */
static error_t CarryTagsRead(Carry* c, TagFSVolume* from, const FileRecord* rec)
{
    c->tags = rec->tag_count ? kmalloc(sizeof(CarryTag) * rec->tag_count) : NULL;
    if (rec->tag_count && !c->tags)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < rec->tag_count; i++) {
        TagSpelling sp;
        TagOwner    owner;
        bool        system = false;
        if (!TagRegistrySpelling(from->state.registry, rec->tags[i], &sp, &system, &owner))
            continue;
        CarryTag* t  = &c->tags[c->tag_count];
        t->person    = owner.kind == TAG_OWNER_PERSON;
        t->whose     = t->person ? (CarryWho){ 0, 0, false } : CarryWhoOf(c, from, owner.id);
        t->key_len   = sp.key_len;
        t->has_value = sp.value != NULL;
        t->value_len = sp.value ? sp.value_len : 0;
        t->text      = kmalloc(t->key_len + t->value_len + 1u);
        if (!t->text)
            return ERR_NO_MEMORY;
        memcpy(t->text, sp.key, t->key_len);
        if (t->value_len)
            memcpy(t->text + t->key_len, sp.value, t->value_len);
        c->tag_count++;
    }
    return OK;
}

/* The programs the file shows its tags to, as `to` may know them: every
 * program (0) stays every program. */
static error_t CarryShownRead(Carry* c, TagFSVolume* from, const FileRecord* rec)
{
    c->shown = rec->shown_count ? kmalloc(sizeof(CarryWho) * rec->shown_count) : NULL;
    if (rec->shown_count && !c->shown)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < rec->shown_count; i++)
        c->shown[c->shown_count++] = rec->shown_to[i] ? CarryWhoOf(c, from, rec->shown_to[i])
                                                       : (CarryWho){ .every = true };
    return OK;
}

/* What a move holds the file to (CarrySame): its tags and readers as ids. */
static error_t CarrySeen(Carry* c, const FileRecord* rec)
{
    c->seen_tags  = rec->tag_count ? kmalloc(sizeof(uint32_t) * rec->tag_count) : NULL;
    c->seen_shown = rec->shown_count ? kmalloc(sizeof(uint64_t) * rec->shown_count) : NULL;
    if ((rec->tag_count && !c->seen_tags) || (rec->shown_count && !c->seen_shown))
        return ERR_NO_MEMORY;
    if (rec->tag_count)
        memcpy(c->seen_tags, rec->tags, sizeof(uint32_t) * rec->tag_count);
    if (rec->shown_count)
        memcpy(c->seen_shown, rec->shown_to, sizeof(uint64_t) * rec->shown_count);
    c->seen_tag_count   = rec->tag_count;
    c->seen_shown_count = rec->shown_count;
    return OK;
}

/* The file as it lies, opened to be read whole — the handle's view keeps the
 * ground it names while it is read, whatever is written meanwhile — and what
 * a move holds it to. A record changed between the open and the look is
 * looked at again. Inside the door. */
static error_t CarryRead(Carry* c, TagFSVolume* from, uint64_t fid)
{
    FileRecord* rec = NULL;
    for (uint32_t look = 0; look < 3u && !rec; look++) {
        tagfs_close(from, c->src);
        c->src     = tagfs_open(from, fid, TAGFS_HANDLE_READ);
        c->src_vol = from;
        if (!c->src)
            return ERR_FILE_NOT_FOUND;
        rec = FileMapRecordHold(from->state.files, fid, NULL);
        if (rec && rec != c->src->rec) {
            FileRecordRelease(rec);
            rec = NULL;
        }
    }
    error_t rc = rec ? OK : ERR_BUSY;
    if (rc == OK && (rec->flags & TAGFS_FILE_ROLE_MASK)) {
        kprintf("[TagFS] file %llu of volume %u is kept by the system — a volume's maker lays such a file, "
                "no carry takes it\n", (unsigned long long)fid, TagFSVolumeNumber(from));
        rc = ERR_PERMISSION_DENIED;
    }
    if (rc == OK) {
        c->flags       = rec->flags;
        c->size        = rec->size;
        c->created_ns  = rec->created_time;
        c->modified_ns = rec->modified_time;
        c->keep_until  = (rec->flags & TAGFS_FILE_DELETED) ? rec->keep_until : 0;
        c->passport    = rec->passport;
        c->made_by     = rec->made_by ? CarryWhoOf(c, from, rec->made_by) : (CarryWho){ 0, 0, false };
        c->name        = kmalloc(rec->name_len + 1u);
        rc             = c->name ? OK : ERR_NO_MEMORY;
    }
    if (rc == OK) {
        memcpy(c->name, rec->name, rec->name_len);
        c->name[rec->name_len] = '\0';
        rc = CarryTagsRead(c, from, rec);
    }
    if (rc == OK)
        rc = CarryShownRead(c, from, rec);
    if (rc == OK)
        rc = CarrySeen(c, rec);
    FileRecordRelease(rec);
    return rc;
}

static void CarrySurvey(Carry* c)
{
    CarryItem* it = &c->items[c->done];
    if (!tagfs_enter(it->from)) {
        CarryFail(c, ERR_DEVICE_NOT_READY);
        return;
    }
    error_t rc = CarryRead(c, it->from, it->fid);
    tagfs_leave(it->from);
    if (rc != OK) {
        CarryFail(c, rc);
        return;
    }
    c->offset = 0;
    CarryNext(c, c->to, CARRY_BORN);
}

// ----------------------------------------------------------------------------
// BORN, on the volume the file goes to
// ----------------------------------------------------------------------------

/* The file's tags spoken on `to`, into `ids` (room for every one): how many,
 * and in *dropped the program words no program of `to` could speak. */
static error_t CarryTagsSpoken(Carry* c, uint32_t* ids, uint32_t* n, uint32_t* dropped)
{
    *n       = 0;
    *dropped = 0;
    for (uint32_t i = 0; i < c->tag_count; i++) {
        const CarryTag* t     = &c->tags[i];
        uint64_t        who   = t->person ? 0 : CarryWhoOn(c, t->whose);
        TagOwner        owner = t->person ? TagOwnerPerson() : (TagOwner){ TAG_OWNER_PROGRAM, who };
        if (!t->person && !who) {
            (*dropped)++;
            continue;
        }
        TagSpelling sp = { .key = t->text, .key_len = t->key_len,
                           .value = t->has_value ? t->text + t->key_len : NULL, .value_len = t->value_len };
        error_t rc = TagRegistryIntern(c->to->state.registry, owner, &sp, &ids[*n]);
        if (rc != OK)
            return rc;
        (*n)++;
    }
    return OK;
}

/* The programs the file shows its tags to, on `to`, rising, each once. */
static uint32_t CarryShownSpoken(Carry* c, uint64_t* out, uint32_t* dropped)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->shown_count; i++) {
        bool     every = c->shown[i].every;
        uint64_t id    = every ? 0 : CarryWhoOn(c, c->shown[i]);
        if (!every && !id) {
            (*dropped)++;
            continue;
        }
        uint32_t at = 0;
        while (at < n && out[at] < id)
            at++;
        if (at < n && out[at] == id)
            continue;
        memmove(out + at + 1u, out + at, sizeof(uint64_t) * (n - at));
        out[at] = id;
        n++;
    }
    return n;
}

/* The program that made the file, on `to`: its copy, else the card the
 * volume keeps of its passport, else none. */
static uint64_t CarryMakerOn(Carry* c)
{
    uint64_t card = 0;
    if (c->made_by.fid)
        return c->made_by.fid;
    if (c->made_by.passport)
        (void)FileMapProgramFiles(c->to->state.files, c->made_by.passport, NULL, 0, &card);
    return card;
}

static error_t CarryLay(Carry* c)
{
    uint32_t  dropped = 0, shown_dropped = 0, count = 0;
    uint32_t* ids     = kmalloc(sizeof(uint32_t) * ((size_t)c->tag_count + 1u));
    uint64_t* shown   = kmalloc(sizeof(uint64_t) * ((size_t)c->shown_count + 1u));
    error_t   rc      = ids && shown ? OK : ERR_NO_MEMORY;
    if (rc == OK)
        rc = CarryTagsSpoken(c, ids, &count, &dropped);
    if (rc == OK) {
        InkBirth   b = { .name = c->name, .tags = ids, .count = count, .made_by = CarryMakerOn(c),
                         .created_ns = c->created_ns, .modified_ns = c->modified_ns, .shown = shown,
                         .shown_count = CarryShownSpoken(c, shown, &shown_dropped), .passport = c->passport };
        InkReceipt receipt;
        if (!c->inbound) {
            __atomic_add_fetch(&c->to->carry.inbound, 1u, __ATOMIC_ACQ_REL);
            c->inbound = true;
        }
        rc = InkNewArriving(c->to, &b, &c->copy, &receipt);
        if (rc == OK)
            c->copy_gen = receipt.gen;
    }
    if (rc == OK) {
        c->dropped += dropped + shown_dropped;
        if (dropped + shown_dropped)
            kprintf("[TagFS] '%s' is carried to volume %u without %u word(s) of programs that volume does not "
                    "hold\n", c->name, TagFSVolumeNumber(c->to), dropped + shown_dropped);
        c->dst = tagfs_open(c->to, c->copy, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_ARRIVAL);
        rc     = c->dst ? OK : ERR_NO_MEMORY;
    }
    kfree(ids);
    kfree(shown);
    return rc;
}

static void CarryBorn(Carry* c)
{
    if (c->came != OK) {
        CarryFail(c, c->came);
        return;
    }
    if (!tagfs_enter(c->to)) {
        CarryFail(c, ERR_DEVICE_NOT_READY);
        return;
    }
    error_t rc = CarryLay(c);
    tagfs_leave(c->to);
    if (rc == ERR_WOULD_BLOCK)
        CarryRoomWait(c, c->to, CARRY_BORN);
    else if (rc != OK)
        CarryFail(c, rc);
    else if (c->size)
        CarryNext(c, c->items[c->done].from, CARRY_READ);
    else
        CarryNext(c, c->to, CARRY_MARKS);
}

// ----------------------------------------------------------------------------
// READ and WRITE, a piece at a time
// ----------------------------------------------------------------------------

static void CarryPieceRead(Carry* c)
{
    TagFSVolume* from = c->items[c->done].from;
    uint64_t     want = c->size - c->offset < CARRY_PIECE ? c->size - c->offset : CARRY_PIECE;
    uint64_t     got  = 0;
    if (!c->piece && !(c->piece = kmalloc(CARRY_PIECE))) {
        CarryFail(c, ERR_NO_MEMORY);
        return;
    }
    c->src->offset = c->offset;
    error_t rc     = tagfs_read(from, c->src, c->piece, want, &got);
    if (rc == OK && got != want)
        rc = ERR_METADATA_CORRUPTED;    /* the view holds the file the carry read: its size is there */
    if (rc != OK) {
        CarryFail(c, rc);
        return;
    }
    c->piece_len = got;
    c->piece_at  = 0;
    CarryNext(c, c->to, CARRY_WRITE);
}

/* One window of the piece written to the copy, under its Helm — asked a
 * window at a time, so what else works on the file goes between two. */
static void CarryPieceWrite(Carry* c)
{
    if (c->came != OK) {
        CarryFail(c, c->came);
        return;
    }
    if (!tagfs_enter(c->to)) {
        CarryFail(c, ERR_DEVICE_NOT_READY);
        return;
    }
    if (!__atomic_load_n(&c->holds, __ATOMIC_ACQUIRE)) {
        c->helm = (HelmWaiter){ .hand = CarryHelmHanded, .gone = CarryHelmGone };
        bool held = HelmAsk(c->to, c->dst->ofe, &c->helm);
        if (!held) {
            tagfs_leave(c->to);
            return;      /* handed (CarryHelmHanded), or gone (CarryHelmGone): the next turn is theirs */
        }
        __atomic_store_n(&c->holds, 1u, __ATOMIC_RELEASE);
    }
    PlotSource src   = { .kernel = c->piece + c->piece_at, .bytes = c->piece_len - c->piece_at };
    uint64_t   inked = 0;
    c->dst->offset   = c->offset + c->piece_at;
    error_t rc       = HelmWriteWindow(c->to, c->dst, &src, &inked);
    __atomic_store_n(&c->holds, 0u, __ATOMIC_RELEASE);
    HelmLetGo(c->to, c->dst->ofe, &c->helm);
    tagfs_leave(c->to);
    c->piece_at += inked;
    if (rc == ERR_WOULD_BLOCK) {
        CarryRoomWait(c, c->to, CARRY_WRITE);
    } else if (rc != OK && inked == 0) {
        CarryFail(c, rc);
    } else if (c->piece_at < c->piece_len) {
        CarryNext(c, c->to, CARRY_WRITE);
    } else {
        c->offset += c->piece_len;
        if (c->offset < c->size)
            CarryNext(c, c->items[c->done].from, CARRY_READ);
        else
            CarryNext(c, c->to, CARRY_MARKS);
    }
}

// ----------------------------------------------------------------------------
// MARKS, SETTLE and LEAVE
// ----------------------------------------------------------------------------

/* The copy arrived — and kept until its term, when the file lay deleted. */
static void CarryMarks(Carry* c)
{
    if (c->came != OK) {
        CarryFail(c, c->came);
        return;
    }
    if (!tagfs_enter(c->to)) {
        CarryFail(c, ERR_DEVICE_NOT_READY);
        return;
    }
    InkReceipt receipt;
    error_t    rc = InkArrived(c->to, c->copy, c->keep_until, &receipt);
    if (rc == OK)
        c->copy_gen = receipt.gen;
    tagfs_leave(c->to);
    if (rc == ERR_WOULD_BLOCK) {
        CarryRoomWait(c, c->to, CARRY_MARKS);
        return;
    }
    if (rc != OK) {
        CarryFail(c, rc);
        return;
    }
    c->items[c->done].arrived = c->copy;
    tagfs_close(c->to, c->dst);
    c->dst  = NULL;
    c->copy = 0;
    if (!c->move) {
        CarryNext(c, c->items[c->done].from, CARRY_LEAVE);
        return;
    }
    c->step = CARRY_SETTLE;
    c->at   = c->to;
    if (!tagfs_enter(c->to)) {
        CarryFail(c, ERR_DEVICE_NOT_READY);
        return;
    }
    error_t now = SettleWaitNow(c->to, c->copy_gen);
    if (now == ERR_WOULD_BLOCK)
        SettleWaitLink(c->to, &c->wait, c->copy_gen, CarryCycleCame);
    tagfs_leave(c->to);
    if (now != ERR_WOULD_BLOCK) {
        c->came = now;
        HelmReady(c->to, &c->turn);
    }
}

/* The copy settled — or the cycle refused it, and the file stays where it was;
 * the copy arrived either way, and is counted. */
static void CarrySettled(Carry* c)
{
    if (c->came != OK) {
        c->done++;
        CarryFail(c, c->came);
        return;
    }
    CarryNext(c, c->items[c->done].from, CARRY_LEAVE);
}

/* A move lets the file go where it was — only the record the carry read; a
 * file changed since stays, and the move answers ERR_BUSY. A copy closes what
 * it read. Then the next item, or the answer. */
static void CarryLeave(Carry* c)
{
    CarryItem* it = &c->items[c->done];
    if (c->came != OK) {
        CarryFail(c, c->came);
        return;
    }
    if (c->move) {
        if (!tagfs_enter(it->from)) {
            CarryFail(c, ERR_DEVICE_NOT_READY);
            return;
        }
        InkReceipt receipt;
        error_t    rc = InkQuitclaimIf(it->from, it->fid, CarrySame, c, c->warrant, &receipt);
        tagfs_leave(it->from);
        if (rc == ERR_WOULD_BLOCK) {
            CarryRoomWait(c, it->from, CARRY_LEAVE);
            return;
        }
        if (rc == ERR_BUSY)
            kprintf("[TagFS] file %llu of volume %u changed while it was carried to volume %u: it stays where it "
                    "was, beside its copy (file %llu there)\n", (unsigned long long)it->fid,
                    TagFSVolumeNumber(it->from), TagFSVolumeNumber(c->to), (unsigned long long)it->arrived);
        if (rc != OK && rc != ERR_FILE_NOT_FOUND) {
            c->done++;      /* the copy arrived: it is counted, the move of it refused */
            CarryFail(c, rc);
            return;
        }
    }
    tagfs_close(c->src_vol, c->src);
    c->src = NULL;
    CarryFileLetGo(c);
    c->done++;
    if (c->done < c->count)
        CarryNext(c, c->items[c->done].from, CARRY_SURVEY);
    else
        CarryNext(c, TagFSHome(), CARRY_END);
}

// ----------------------------------------------------------------------------
// END
// ----------------------------------------------------------------------------

/* What the carry still holds let go — a copy cut short let go on its volume,
 * or left for the volume's service when it cannot be now — and the answer. */
static void CarryEnd(Carry* c)
{
    if (__atomic_load_n(&c->holds, __ATOMIC_ACQUIRE)) {
        /* Handed the copy's Helm (CarryHelmHanded), and ended before its turn
         * wrote: given back, so nothing else waits on it for ever. */
        __atomic_store_n(&c->holds, 0u, __ATOMIC_RELEASE);
        HelmLetGo(c->to, c->dst->ofe, &c->helm);
    }
    if (c->dst) {
        tagfs_close(c->to, c->dst);
        c->dst = NULL;
    }
    if (c->copy) {
        InkReceipt receipt;
        bool       gone = tagfs_enter(c->to);
        if (gone) {
            gone = InkArrivalLetGo(c->to, c->copy, &receipt) == OK;
            tagfs_leave(c->to);
        }
        if (!gone)
            __atomic_store_n(&c->to->carry.sweep_owed, 1u, __ATOMIC_RELEASE);
        c->copy = 0;
    }
    if (c->inbound) {
        __atomic_sub_fetch(&c->to->carry.inbound, 1u, __ATOMIC_ACQ_REL);
        c->inbound = false;
    }
    if (c->src) {
        tagfs_close(c->src_vol, c->src);
        c->src = NULL;
    }
    CarryFileLetGo(c);
    kfree(c->piece);
    c->piece = NULL;
    if (c->rc != OK)
        kprintf("[TagFS] a carry to volume %u ended after %u of %u file(s) (error %u)\n", TagFSVolumeNumber(c->to),
                c->done, c->count, (unsigned)c->rc);
    c->ended(c);
}

static void CarryTurn(HelmWaiter* w)
{
    Carry* c = container_of(w, Carry, turn);
    switch (c->step) {
    case CARRY_SURVEY: CarrySurvey(c); break;
    case CARRY_BORN:   CarryBorn(c); break;
    case CARRY_READ:   CarryPieceRead(c); break;
    case CARRY_WRITE:  CarryPieceWrite(c); break;
    case CARRY_MARKS:  CarryMarks(c); break;
    case CARRY_SETTLE: CarrySettled(c); break;
    case CARRY_LEAVE:  CarryLeave(c); break;
    case CARRY_END:    CarryEnd(c); break;
    }
}

error_t CarryBegin(Carry* c)
{
    if (!c->count || !c->items || !c->to || !TagFSVolumeMounted(c->to))
        return ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < c->count; i++)
        if (!c->items[i].from || c->items[i].from == c->to)
            return ERR_INVALID_ARGUMENT;
    c->rc      = OK;
    c->done    = 0;
    c->dropped = 0;
    c->came    = OK;
    c->holds   = 0;
    c->turn    = (HelmWaiter){ .run = CarryTurn };
    CarryNext(c, c->items[0].from, CARRY_SURVEY);
    return OK;
}

// ----------------------------------------------------------------------------
// The service: what arrives with no carry to finish it
// ----------------------------------------------------------------------------

/* Every file still arriving on `vol` below `ceiling` let go — the files a
 * carry begun since lays lie past it: whether none was refused. Inside the
 * door. */
static bool CarrySweep(TagFSVolume* vol, uint64_t ceiling)
{
    uint64_t       ids[CARRY_LOOK];
    uint64_t       from   = 1;
    uint32_t       let_go = 0;
    bool           all    = true;
    const uint32_t set    = TAGFS_ARRIVING_MEMBERS;
    for (;;) {
        TagFSPage page;
        memset(&page, 0, sizeof(page));
        if (TagBitmapQueryPage(vol->state.bitmap_index, &set, 1, NULL, 0, NULL, 0, from, ids, CARRY_LOOK, false,
                               &page) != 0)
            return false;
        for (uint32_t i = 0; i < page.delivered && ids[i] < ceiling; i++) {
            InkReceipt receipt;
            if (InkArrivalLetGo(vol, ids[i], &receipt) == OK)
                let_go++;
            else
                all = false;
        }
        if (!page.next || page.next >= ceiling)
            break;
        from = page.next;
    }
    if (let_go)
        kprintf("[TagFS] volume %u: %u file(s) a carry left arriving, let go — a copy cut short is not the "
                "file\n", TagFSVolumeNumber(vol), let_go);
    return all;
}

void CarryServe(TagFSVolume* vol)
{
    CarryState* s = &vol->carry;
    if (!__atomic_load_n(&s->sweep_owed, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->inbound, __ATOMIC_ACQUIRE))
        return;
    if (__atomic_exchange_n(&s->busy, 1u, __ATOMIC_ACQUIRE))
        return;
    if (!__atomic_load_n(&s->held, __ATOMIC_ACQUIRE) && tagfs_enter(vol)) {
        /* The ceiling first, then the carries: a copy below the ceiling was
         * born before it, by a carry counted before it was born — still
         * counted, and nothing is let go, or over, and its copy arrived or is
         * nobody's. A carry counted since lays its copy past the ceiling. */
        __atomic_store_n(&s->sweep_owed, 0u, __ATOMIC_RELEASE);
        TagFSLockTake(&vol->state.lock);
        uint64_t ceiling = vol->book.next_fid;
        TagFSLockDrop(&vol->state.lock);
        if (__atomic_load_n(&s->inbound, __ATOMIC_ACQUIRE) || !CarrySweep(vol, ceiling))
            __atomic_store_n(&s->sweep_owed, 1u, __ATOMIC_RELEASE);
        tagfs_leave(vol);
    }
    __atomic_store_n(&s->busy, 0u, __ATOMIC_RELEASE);
}

void CarryMountReset(TagFSVolume* vol)
{
    __atomic_store_n(&vol->carry.sweep_owed, 1u, __ATOMIC_RELEASE);
}

bool CarryHold(TagFSVolume* vol, bool held)
{
    bool was = __atomic_exchange_n(&vol->carry.held, held ? 1u : 0u, __ATOMIC_ACQ_REL) != 0;
    if (held)
        while (__atomic_load_n(&vol->carry.busy, __ATOMIC_ACQUIRE))
            cpu_pause();
    return was;
}
