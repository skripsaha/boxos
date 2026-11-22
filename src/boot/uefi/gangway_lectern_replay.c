/*
 * The Lectern's replay of the ribbon, as the mount's pass 2 replays it: the
 * stitches sewn on (gangway_lectern_ribbon.c), their records
 * replayed for the files the Lectern follows — those kept in a role, or that
 * the ribbon puts in one, the files called by the names the loader asks for,
 * and the scenes, their names and tags — and the digests every record puts
 * in; and the ids the ribbon coins for the tags a person asks a scene by.
 *
 * The rules are the kernel's (mount/ribbon_transcribe.c, ribbon_records.c)
 * and volume-read's (bookmount.py _Undone, _Stitch, _File), narrowed to what
 * a loader can judge with no mirror of every file (bootbook.py, which this is
 * held to line for line by the host stand): every Undone gathered first, one
 * that does not hold refusing its stitch; a stitch whose records cannot be
 * applied — one of a kind no reader knows and may not step over, one that
 * does not read or is longer than this reader knows, a Born not ACTIVE, a
 * Clasp counting other fresh blocks than its records — refused whole, said;
 * a record of a file followed held to its base and to its rule, the rule of
 * rot passing over every record of that file in the stitch, said — the file
 * as it last matched; a record of a file not followed weighed for nothing but
 * the digests it carries.
 */

#include "gangway_lectern_book.h"

/* ------------------------------------------------------------------------- */
/* A record read                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    int            read;        /* TALLY_BODY_* */
    uint32_t       longer;
    RibbonBorn     born;
    RibbonShaped   shaped;
    RibbonLabeled  labeled;
    RibbonUndone   undone;
    uint64_t       fid, base;   /* Quitclaim's; every file record's fid */
    uint64_t       id;          /* Coined's, Frozen's, Thawed's */
    TallyTag       tag;
} LecternRead;

static const char *KindName(uint64_t kind)
{
    static const char *const names[] = { "", "StitchHead", "Clasp", "Undone", "Born", "Shaped", "Labeled",
                                         "Quitclaim", "Coined", "UseContext", "Frozen", "Thawed" };
    return kind <= RIBBON_KINDS_KNOWN ? names[kind] : "";
}

/* A kind a record of a stitch's stream is read as — StitchHead and Clasp only
 * open and close one. */
static int KindKnown(uint64_t kind)
{
    return kind >= RIBBON_UNDONE && kind <= RIBBON_KINDS_KNOWN;
}

static int FileKind(uint64_t kind)
{
    return kind >= RIBBON_BORN && kind <= RIBBON_QUITCLAIM;
}

/* Record `r` of stitch `u`, of a kind known, read into *d. */
static void RecordRead(const LecternStitch *u, const LecternRecord *r, LecternRead *d)
{
    const uint8_t *body = u->stream + r->body;
    const uint8_t *text = (const uint8_t *)0;
    uint32_t       used = 0;
    uint64_t       n    = 0;

    LecternZero(d, sizeof(*d));
    switch (r->kind) {
    case RIBBON_UNDONE:    d->read = RibbonUndoneGet(body, r->len, &d->undone, &used); break;
    case RIBBON_BORN:      d->read = RibbonBornGet(body, r->len, u->seq, &d->born, &used); break;
    case RIBBON_SHAPED:    d->read = RibbonShapedGet(body, r->len, u->seq, &d->shaped, &used); break;
    case RIBBON_LABELED:   d->read = RibbonLabeledGet(body, r->len, &d->labeled, &used); break;
    case RIBBON_QUITCLAIM: d->read = RibbonQuitclaimGet(body, r->len, &d->fid, &d->base, &used); break;
    case RIBBON_COINED:    d->read = RibbonCoinedGet(body, r->len, &d->id, &d->tag, &used); break;
    case RIBBON_FROZEN:    d->read = RibbonFrozenGet(body, r->len, &d->id, &used); break;
    case RIBBON_THAWED:    d->read = RibbonThawedGet(body, r->len, &d->id, &used); break;
    default:
        d->read = TallyUseGet(body, r->len, &text, &n);
        used    = d->read == TALLY_BODY_BROKEN ? 0 : TallyVarintBytes(n) + (uint32_t)n;
        break;
    }
    d->longer = d->read == TALLY_BODY_BROKEN ? 0 : r->len - used;
    if (r->kind == RIBBON_BORN) d->fid = d->born.fid;
    if (r->kind == RIBBON_SHAPED) d->fid = d->shaped.fid;
    if (r->kind == RIBBON_LABELED) d->fid = d->labeled.fid;
}

/* The runs a Born or a Shaped puts in, walked one by one: `step` over each,
 * its `ctx`. 0 when the walk is stopped. */
typedef int (*RunStep)(void *ctx, const TallyRun *run);

static int RunsPut(const LecternStitch *u, uint64_t kind, const LecternRead *d, RunStep step, void *ctx)
{
    uint64_t cursor = 0, digests = 0;
    uint32_t at = 0;
    if (kind == RIBBON_BORN) {
        for (uint64_t i = 0; i < d->born.nruns; i++) {
            TallyRun run;
            RibbonRunGet(d->born.runs, d->born.runs_bytes, &at, &cursor, u->seq, &run);
            if (!step(ctx, &run)) return 0;
        }
        return 1;
    }
    for (uint64_t i = 0; i < d->shaped.nsplice; i++) {
        RibbonSplice s;
        uint32_t     in = 0;
        RibbonSpliceNext(d->shaped.splices, d->shaped.splices_bytes, &at, &cursor, u->seq, &digests, &s);
        uint64_t c = s.cursor;
        for (uint64_t k = 0; k < s.ninsert; k++) {
            TallyRun run;
            RibbonRunGet(s.runs, s.runs_bytes, &in, &c, u->seq, &run);
            if (!step(ctx, &run)) return 0;
        }
    }
    return 1;
}

static int FreshStep(void *ctx, const TallyRun *run)
{
    uint64_t *n = (uint64_t *)ctx, f = RibbonRunFresh(run);
    *n = f > ~(uint64_t)0 - *n ? ~(uint64_t)0 : *n + f;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Skips and refusals                                                        */
/* ------------------------------------------------------------------------- */

static uint64_t SkipAt(const LecternBook *b, uint64_t seq, uint64_t fid)
{
    uint64_t lo = 0, hi = b->nskip;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        const LecternSkip *s = &b->skip[mid];
        if (s->seq < seq || (s->seq == seq && s->fid < fid)) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

static int Skipped(const LecternBook *b, uint64_t seq, uint64_t fid)
{
    uint64_t at = SkipAt(b, seq, fid);
    return at < b->nskip && b->skip[at].seq == seq && b->skip[at].fid == fid;
}

static int SkipAdd(LecternBook *b, uint64_t seq, uint64_t fid)
{
    uint64_t at = SkipAt(b, seq, fid);
    if (at < b->nskip && b->skip[at].seq == seq && b->skip[at].fid == fid) return 1;
    if (!LecternGrow(b, (void **)&b->skip, b->nskip, &b->capskip, sizeof(LecternSkip))) return 0;
    for (uint64_t i = b->nskip; i > at; i--) b->skip[i] = b->skip[i - 1u];
    b->skip[at] = (LecternSkip){ seq, fid };
    b->nskip++;
    return 1;
}

/* "stitch S is not applied: <why>", once a stitch. 1, or 0 no room. */
static int Refuse(LecternBook *b, const LecternStitch *u, const LecternLine *why)
{
    LecternLine l;
    if (LecternListHas(b->refused, b->nrefused, u->seq)) return 1;
    if (!LecternListAdd(b, &b->refused, &b->nrefused, &b->caprefused, u->seq)) return 0;
    LineStart(&l);
    LinePut(&l, "stitch ");
    LineNum(&l, u->seq);
    LinePut(&l, " is not applied: ");
    LinePut(&l, why->text);
    LecternSay(b, &l);
    return 1;
}

static int Sewn(const LecternBook *b, uint64_t seq)
{
    for (uint64_t i = 0; i < b->nstitches; i++)
        if (b->stitches[i]->seq == seq) return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Before the files: the files the ribbon puts in a role or names as asked  */
/* ------------------------------------------------------------------------- */

/* Every file a Labeled puts in a role, gives a name the loader asks for or a
 * scene's name, in the order of the ribbon — followed from the Tally on,
 * whatever its records come to. */
void LecternBefore(LecternBook *b)
{
    for (uint64_t s = 0; s < b->nstitches; s++) {
        const LecternStitch *u = b->stitches[s];
        for (uint32_t i = 0; i < u->nrecords; i++) {
            const LecternRecord *r = &u->records[i];
            LecternRead          d;
            if (r->kind != RIBBON_LABELED) continue;
            RecordRead(u, r, &d);
            if (d.read == TALLY_BODY_BROKEN) continue;
            int role  = (d.labeled.what & LABEL_FLAGS) && LecternRoleOf(d.labeled.set);
            int named = (d.labeled.what & LABEL_NAME) &&
                        (LecternNamed(b->called, b->ncalled, d.labeled.name, d.labeled.name_len) ||
                         LecternSceneNamed(d.labeled.name, d.labeled.name_len));
            if ((role || named) && !LecternListAdd(b, &b->interest, &b->ninterest, &b->capinterest, d.fid))
                return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Pass 1: the Undones (bookmount.py _Undone)                                */
/* ------------------------------------------------------------------------- */

/* Stitch `u`'s Undone `d`, the record `i` of it: its fids passed over in the
 * stitch it names, said; or the stitch refused. 1, or 0 no room. */
static int UndoneTaken(LecternBook *b, const LecternStitch *u, uint32_t i, const LecternRead *d)
{
    LecternLine why, l;
    uint64_t    back = d->undone.seq_back, target = u->seq - back, id = 0;
    uint32_t    at = 0;
    int         before = 0;

    for (uint32_t k = 0; k < i; k++) before |= u->records[k].kind != RIBBON_UNDONE;
    LineStart(&why);
    if (before || !back) {
        LinePut(&why, "it carries an Undone after another record");
        return Refuse(b, u, &why);
    }
    if (back >= u->seq || target <= b->head.stitch_seq) return 1;
    if (!Sewn(b, target)) {
        LinePut(&why, "it carries an Undone of seq ");
        LineNum(&why, target);
        LinePut(&why, ", which is no stitch of this ribbon before it");
        return Refuse(b, u, &why);
    }
    for (uint64_t k = 0; k < d->undone.nfid; k++) {
        TallyIdNext(d->undone.fids, d->undone.fids_bytes, &at, &id, k == 0);
        if (!SkipAdd(b, target, id)) return 0;
    }
    LineStart(&l);
    LinePut(&l, "stitch ");
    LineNum(&l, u->seq);
    LinePut(&l, "'s Undone passes over stitch ");
    LineNum(&l, target);
    LinePut(&l, "'s records of ");
    LineNum(&l, d->undone.nfid);
    LinePut(&l, " file(s)");
    LecternSay(b, &l);
    return 1;
}

static int LecternUndone(LecternBook *b)
{
    for (uint64_t s = 0; s < b->nstitches; s++) {
        const LecternStitch *u = b->stitches[s];
        for (uint32_t i = 0; i < u->nrecords; i++) {
            LecternRead d;
            LecternLine why;
            if (u->records[i].kind != RIBBON_UNDONE) continue;
            RecordRead(u, &u->records[i], &d);
            if (d.read != TALLY_BODY_BROKEN) {
                if (!UndoneTaken(b, u, i, &d)) return 0;
                continue;
            }
            LineStart(&why);
            LinePut(&why, "it carries an Undone that does not read");
            if (!Refuse(b, u, &why)) return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Pass 2: whether a stitch may be applied (bookmount.py _Stitch, _Fresh)     */
/* ------------------------------------------------------------------------- */

/* Why stitch `u` may not be applied, off its records alone, the first reason,
 * into *why — empty when it may. */
static void Refusal(const LecternBook *b, const LecternStitch *u, LecternLine *why)
{
    uint64_t named = 0;
    LineStart(why);
    if (u->longer) {
        LinePut(why, "its StitchHead or Clasp is ");
        LineNum(why, u->longer);
        LinePut(why, " byte(s) longer than this reader knows");
        return;
    }
    for (uint32_t i = 0; i < u->nrecords; i++) {
        const LecternRecord *r = &u->records[i];
        LecternRead          d;
        if (!KindKnown(r->kind)) {
            if (r->ignorable) continue;
            LinePut(why, "it carries a record of kind ");
            LineNum(why, r->kind);
            LinePut(why, " this reader does not know and may not step over");
            return;
        }
        RecordRead(u, r, &d);
        if (d.read == TALLY_BODY_BROKEN || d.longer) {
            LinePut(why, "its ");
            LinePut(why, KindName(r->kind));
            LinePut(why, d.longer ? " is " : " does not read");
            if (d.longer) LineNum(why, d.longer);
            if (d.longer) LinePut(why, " byte(s) longer than this reader knows");
            return;
        }
        if (r->kind == RIBBON_BORN && !Skipped(b, u->seq, d.fid) && !(d.born.flags & TALLY_FILE_ACTIVE)) {
            LinePut(why, "its Born of fid ");
            LineNum(why, d.fid);
            LinePut(why, " is not ACTIVE");
            return;
        }
        if (r->kind == RIBBON_BORN || r->kind == RIBBON_SHAPED) RunsPut(u, r->kind, &d, FreshStep, &named);
    }
    if (u->sound && named != u->clasp.fresh) {
        LinePut(why, "its Clasp names ");
        LineNum(why, u->clasp.fresh);
        LinePut(why, " fresh block(s) and its records ");
        LineNum(why, named);
    }
}

/* ------------------------------------------------------------------------- */
/* Pass 2: a file followed, as a record leaves it (bookmount.py _File)        */
/* ------------------------------------------------------------------------- */

/* A file of a stitch in hand: as the stitch leaves it so far — `alive`, `f`
 * — and whether a record of it did not fit (the rule of rot). */
typedef struct {
    uint64_t    fid;
    int         alive;
    int         failed;
    int         held;      /* the Lectern followed it before the stitch */
    LecternFile f;
} LecternHand;

/* What a file's last block is (bookmount.py LastBlock): none for no bytes, a
 * hole, a block of the ground, or beyond its runs — and its index. */
typedef struct {
    int      kind;      /* 0 none, 1 a hole, 2 a block, 3 beyond its runs */
    uint64_t index;
    uint64_t block;
} LecternLast;

static uint64_t Blocks(uint64_t size)
{
    return size / VOLUME_BLOCK_BYTES + (size % VOLUME_BLOCK_BYTES != 0);
}

static LecternLast LastOf(const LecternFile *f)
{
    LecternLast last = { 0, 0, 0 };
    uint64_t    n = Blocks(f->size), pos = 0;
    if (!n) return last;
    last.index = n - 1u;
    for (uint64_t i = 0; i < f->nruns; i++) {
        if (f->runs[i].count >= n - pos) {
            last.kind  = f->runs[i].hole ? 1 : 2;
            last.block = f->runs[i].hole ? 0 : f->runs[i].start + (n - 1u - pos);
            return last;
        }
        pos += f->runs[i].count;
    }
    last.kind = 3;
    return last;
}

static uint64_t RunBlocks(const LecternFile *f)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < f->nruns; i++)
        n = f->runs[i].count > ~(uint64_t)0 - n ? ~(uint64_t)0 : n + f->runs[i].count;
    return n;
}

/* The rule every record of a file ends with: its runs cover the blocks its
 * size needs. */
static int Covers(const LecternFile *f, LecternLine *why)
{
    uint64_t have = RunBlocks(f), need = Blocks(f->size);
    if (have == need) return 1;
    LinePut(why, "its runs cover ");
    LineNum(why, have);
    LinePut(why, " blocks and its ");
    LineNum(why, f->size);
    LinePut(why, " bytes need ");
    LineNum(why, need);
    return 0;
}

/* The Slips on the file's last block cut by its size (file_map/tail_slip.c
 * TailSlipKept, bookmount.py _SlipsCut): what lay past it is gone. */
static void TailCut(LecternFile *f)
{
    uint64_t cut = f->size ? f->size - (Blocks(f->size) - 1u) * VOLUME_BLOCK_BYTES : 0;
    for (uint64_t i = cut; f->tail && i < VOLUME_BLOCK_BYTES; i++)
        f->tail[VOLUME_BLOCK_BYTES + i / 8u] &= (uint8_t)~(1u << (i % 8u));
}

/* A Slip laid on the file's last block (§2.8), held to its rules — inside the
 * block, a file of bytes, not past its size (bookmount.py _Slip). 1, 0 it
 * does not fit (why said), -1 no room. */
static int TailLay(LecternBook *b, LecternFile *f, const RibbonSlip *slip, LecternLine *why)
{
    uint64_t last = Blocks(f->size) ? Blocks(f->size) - 1u : 0;
    if (!slip->len) return 1;
    if (!f->size || slip->off > VOLUME_BLOCK_BYTES || slip->len > VOLUME_BLOCK_BYTES - slip->off ||
        last * VOLUME_BLOCK_BYTES + slip->off + slip->len > f->size) {
        LinePut(why, "its Slip of ");
        LineNum(why, slip->len);
        LinePut(why, " bytes at ");
        LineNum(why, slip->off);
        LinePut(why, " does not lie inside the last block of ");
        LineNum(why, f->size);
        LinePut(why, " bytes");
        return 0;
    }
    if (!f->tail) {
        if (!(f->tail = (uint8_t *)LecternTake(b, LECTERN_TAIL_BYTES))) return -1;
        LecternZero(f->tail, LECTERN_TAIL_BYTES);
    }
    for (uint64_t i = 0; i < slip->len; i++) {
        uint64_t at = slip->off + i;
        f->tail[at] = slip->bytes[i];
        f->tail[VOLUME_BLOCK_BYTES + at / 8u] |= (uint8_t)(1u << (at % 8u));
    }
    return 1;
}

/* A run into a file's list as it comes. */
typedef struct {
    LecternBook *b;
    LecternRun  *runs;
    uint64_t     n, cap;
} LecternRunList;

static int RunAppend(LecternRunList *l, const LecternRun *run)
{
    if (!LecternGrow(l->b, (void **)&l->runs, l->n, &l->cap, sizeof(LecternRun))) return 0;
    l->runs[l->n++] = *run;
    return 1;
}

static int RunTaken(void *ctx, const TallyRun *run)
{
    LecternRun r = { run->start, run->count, run->hole, RibbonRunCarried(run) };
    return RunAppend((LecternRunList *)ctx, &r);
}

/* The file's runs with `remove` blocks taken out from file block `at` and the
 * runs `ins` put in their place (bookmount.py Splice, file_record.h
 * FileRecordSplice): a run cut by the splice stays two runs. 1, 0 the splice
 * takes out blocks past the file's end (why said), -1 no room. */
static int Spliced(LecternBook *b, LecternFile *f, uint64_t at, uint64_t remove, const LecternRunList *ins,
                   LecternLine *why)
{
    LecternRunList out = { b, (LecternRun *)0, 0, 0 };
    uint64_t       pos = 0, end = remove > ~(uint64_t)0 - at ? ~(uint64_t)0 : at + remove;
    int            placed = 0, ok = 1;

    for (uint64_t i = 0; ok && i < f->nruns; i++) {
        LecternRun r = f->runs[i];
        uint64_t   lo = pos, hi = pos + r.count;
        pos = hi;
        if (hi <= at || lo >= end) {
            for (uint64_t k = 0; ok && lo >= end && !placed && k < ins->n; k++) ok = RunAppend(&out, &ins->runs[k]);
            placed |= lo >= end;
            ok = ok && RunAppend(&out, &r);
            continue;
        }
        LecternRun head = { r.start, at - lo, r.hole, r.carried };
        LecternRun tail = { r.start + (r.hole ? 0 : end - lo), hi - end, r.hole, r.carried };
        if (lo < at) ok = RunAppend(&out, &head);
        for (uint64_t k = 0; ok && !placed && k < ins->n; k++) ok = RunAppend(&out, &ins->runs[k]);
        placed = 1;
        if (ok && hi > end) ok = RunAppend(&out, &tail);
    }
    for (uint64_t k = 0; ok && !placed && k < ins->n; k++) ok = RunAppend(&out, &ins->runs[k]);
    if (!ok) {
        LecternGive(b, out.runs, out.cap * sizeof(LecternRun));
        return -1;
    }
    LecternGive(b, f->runs, f->capruns * sizeof(LecternRun));
    f->runs    = out.runs;
    f->nruns   = out.n;
    f->capruns = out.cap;
    if (pos >= end) return 1;
    LinePut(why, "a splice takes out blocks ");
    LineNum(why, at);
    LinePut(why, "..");
    LineNum(why, end - 1u);
    LinePut(why, " of a file of ");
    LineNum(why, pos);
    return 0;
}

/* "its base names the file at X and the file stands at Y" (Y "nothing"): the
 * base a record names against the file's gen. 1 when they agree. */
static int BaseHolds(const LecternHand *h, uint64_t seq, uint64_t base, LecternLine *why)
{
    uint64_t at = seq - base;
    if (h->alive && h->f.gen == at) return 1;
    LinePut(why, "its base names the file at ");
    LineNum(why, at);
    LinePut(why, " and the file stands at ");
    if (h->alive)
        LineNum(why, h->f.gen);
    else
        LinePut(why, "nothing");
    return 0;
}

/* A Born: the file as it is born — passed over when the file is there. */
static int BornApplied(LecternBook *b, LecternHand *h, const LecternStitch *u, const LecternRead *d,
                       LecternLine *why)
{
    LecternRunList runs = { b, (LecternRun *)0, 0, 0 };
    LecternFile    f;

    if (h->alive) return 1;
    LecternZero(&f, sizeof(f));
    f.fid    = d->fid;
    f.gen    = u->seq;
    f.size   = d->born.size;
    f.flags  = d->born.flags;
    f.called = LecternNamed(b->called, b->ncalled, d->born.name, d->born.name_len);
    LecternFileNamed(&f, d->born.name, d->born.name_len);
    if (!LecternFileTags(b, &f, d->born.tags, d->born.tags_bytes, d->born.ntags) ||
        !RunsPut(u, RIBBON_BORN, d, RunTaken, &runs)) {
        LecternGive(b, runs.runs, runs.cap * sizeof(LecternRun));
        LecternFileGive(b, &f);
        return -1;
    }
    f.runs    = runs.runs;
    f.nruns   = runs.n;
    f.capruns = runs.cap;
    LecternFileGive(b, &h->f);
    h->f     = f;
    h->alive = 1;
    if (!Covers(&h->f, why)) return 0;
    return TailLay(b, &h->f, &d->born.slip, why);
}

/* A Labeled's tags put on and taken off the file, as the mount does
 * (bookmount.py _File): every one put on that the file wears already, or
 * taken off that it does not wear, weighed against the tags it wore before —
 * then on and off. Whether each one put on is a tag the book coined is not
 * weighed: a loader holds no list of every tag (bootbook.py narrows the same).
 * 1, 0 it does not fit, -1 no room. */
static int Retagged(LecternBook *b, LecternFile *f, const RibbonLabeled *r, LecternLine *why)
{
    uint32_t at = 0;
    uint64_t id = 0;
    int      fits = 1;
    for (uint64_t i = 0; fits && i < r->nadd; i++)
        fits = TallyIdNext(r->add, r->add_bytes, &at, &id, i == 0) && !LecternListHas(f->tags, f->ntags, id);
    at = 0;
    for (uint64_t i = 0; fits && i < r->ndrop; i++)
        fits = TallyIdNext(r->drop, r->drop_bytes, &at, &id, i == 0) && LecternListHas(f->tags, f->ntags, id);
    if (!fits) {
        LinePut(why, "it puts on a tag already worn or takes off one not worn");
        return 0;
    }
    at = 0;
    for (uint64_t i = 0; i < r->nadd; i++) {
        TallyIdNext(r->add, r->add_bytes, &at, &id, i == 0);
        if (!LecternListAdd(b, &f->tags, &f->ntags, &f->captags, id)) return -1;
    }
    at = 0;
    for (uint64_t i = 0; i < r->ndrop; i++) {
        TallyIdNext(r->drop, r->drop_bytes, &at, &id, i == 0);
        uint64_t k = 0;
        while (f->tags[k] != id) k++;
        for (; k + 1u < f->ntags; k++) f->tags[k] = f->tags[k + 1u];
        f->ntags--;
    }
    return 1;
}

/* A Labeled: its flags set and cleared, as the record says — the role among
 * them — the deleted bit its KEEP verb puts on or takes off: a file kept
 * deleted is no kernel for the loader to start (gangway_succession.c), nor a
 * file it asks for (gangway_lectern_called.c) — its name, and the tags it
 * puts on and takes off: what a scene is asked by. 1, 0 it does not fit, -1
 * no room. */
static int LabeledApplied(LecternBook *b, LecternHand *h, const LecternRead *d, LecternLine *why)
{
    const RibbonLabeled *r = &d->labeled;
    if (r->what & LABEL_TAGS) {
        int retagged = Retagged(b, &h->f, r, why);
        if (retagged <= 0) return retagged;
    }
    if (r->what & LABEL_NAME) {
        h->f.called = LecternNamed(b->called, b->ncalled, r->name, r->name_len);
        LecternFileNamed(&h->f, r->name, r->name_len);
    }
    if (r->what & LABEL_FLAGS) h->f.flags = (h->f.flags | r->set) & ~r->clear;
    if (r->what & LABEL_KEEP)
        h->f.flags = r->keep_ns ? h->f.flags | TALLY_FILE_DELETED : h->f.flags & ~(uint64_t)TALLY_FILE_DELETED;
    if (r->what & LABEL_SHOWN)
        h->f.flags = r->nshown ? h->f.flags | TALLY_FILE_SHOWN : h->f.flags & ~(uint64_t)TALLY_FILE_SHOWN;
    if (r->what & LABEL_HEAD)
        h->f.flags = r->head_len ? h->f.flags | TALLY_FILE_HEAD : h->f.flags & ~(uint64_t)TALLY_FILE_HEAD;
    return 1;
}

/* A Shaped: its splices, each at its place in the file as it stands after it
 * (§2.8), its size; the Slips cut, and gone when the last block changed. */
static int ShapedApplied(LecternBook *b, LecternHand *h, const LecternStitch *u, const LecternRead *d,
                         LecternLine *why)
{
    LecternLast last = LastOf(&h->f), now;
    uint64_t    cursor = 0, digests = 0, end = 0;
    uint32_t    at = 0;

    for (uint64_t i = 0; i < d->shaped.nsplice; i++) {
        RibbonSplice   s;
        LecternRunList ins = { b, (LecternRun *)0, 0, 0 };
        uint32_t       in  = 0;
        uint64_t       c, put = 0;
        RibbonSpliceNext(d->shaped.splices, d->shaped.splices_bytes, &at, &cursor, u->seq, &digests, &s);
        c = s.cursor;
        for (uint64_t k = 0; k < s.ninsert; k++) {
            TallyRun run;
            RibbonRunGet(s.runs, s.runs_bytes, &in, &c, u->seq, &run);
            put += run.count;
            if (!RunTaken(&ins, &run)) {
                LecternGive(b, ins.runs, ins.cap * sizeof(LecternRun));
                return -1;
            }
        }
        int spliced = Spliced(b, &h->f, end + s.at, s.remove, &ins, why);
        LecternGive(b, ins.runs, ins.cap * sizeof(LecternRun));
        if (spliced <= 0) return spliced;
        end = end + s.at + put;
    }
    h->f.size = d->shaped.size;
    now       = LastOf(&h->f);
    if (d->shaped.nsplice && h->f.tail &&
        (now.kind != last.kind || now.index != last.index || now.block != last.block))
        LecternZero(h->f.tail + VOLUME_BLOCK_BYTES, VOLUME_BLOCK_BYTES / 8u);
    TailCut(&h->f);
    if (!Covers(&h->f, why)) return 0;
    return TailLay(b, &h->f, &d->shaped.slip, why);
}

/* Record `d` of kind `kind` in stitch `u` on the file in hand: 1 applied, 0 it
 * does not fit (why said), -1 no room. */
static int Applied(LecternBook *b, LecternHand *h, const LecternStitch *u, uint64_t kind, const LecternRead *d,
                   LecternLine *why)
{
    if (kind == RIBBON_BORN) return BornApplied(b, h, u, d, why);
    uint64_t base = kind == RIBBON_SHAPED ? d->shaped.base : kind == RIBBON_LABELED ? d->labeled.base : d->base;
    if (!BaseHolds(h, u->seq, base, why)) return 0;
    h->f.gen = u->seq;
    if (kind == RIBBON_QUITCLAIM) {
        h->alive = 0;
        return 1;
    }
    return kind == RIBBON_LABELED ? LabeledApplied(b, h, d, why) : ShapedApplied(b, h, u, d, why);
}

/* ------------------------------------------------------------------------- */
/* Pass 2: a stitch's records for the files followed                         */
/* ------------------------------------------------------------------------- */

typedef struct {
    LecternHand *at;
    uint64_t     n, cap;
} LecternHands;

/* The file in hand for record `d` of kind `kind` — taken up the first time:
 * a file followed as the Lectern holds it, or one a Born of which is kept in
 * a role, called by the name asked for, or that the ribbon puts in one or
 * names so. NULL with *none for a file not followed; NULL with no room. */
static LecternHand *HandOf(LecternBook *b, LecternHands *hs, uint64_t kind, const LecternRead *d, int *none)
{
    LecternFile *held = LecternFileOf(b, d->fid);
    LecternHand  h;
    *none = 0;
    for (uint64_t i = 0; i < hs->n; i++)
        if (hs->at[i].fid == d->fid) return &hs->at[i];
    LecternZero(&h, sizeof(h));
    h.fid = d->fid;
    if (held) {
        h.held  = 1;
        h.alive = !held->gone;
        if (h.alive && !LecternFileCopy(b, &h.f, held)) return (LecternHand *)0;
    } else {
        if (kind != RIBBON_BORN ||
            (!LecternRoleOf(d->born.flags) && !LecternNamed(b->called, b->ncalled, d->born.name, d->born.name_len) &&
             !LecternSceneNamed(d->born.name, d->born.name_len) &&
             !LecternListHas(b->interest, b->ninterest, d->fid))) {
            *none = 1;
            return (LecternHand *)0;
        }
    }
    if (!LecternGrow(b, (void **)&hs->at, hs->n, &hs->cap, sizeof(LecternHand))) {
        LecternFileGive(b, &h.f);
        return (LecternHand *)0;
    }
    hs->at[hs->n] = h;
    return &hs->at[hs->n++];
}

/* "stitch S's K of fid F does not fit the file: <why> — the file stays as it
 * last matched": the rule of rot, every record of the file in the stitch
 * passed over from here. 1, or 0 no room. */
static int Rot(LecternBook *b, LecternHand *h, const LecternStitch *u, uint64_t kind, const LecternLine *why)
{
    LecternLine l;
    h->failed = 1;
    LecternFileGive(b, &h->f);
    LineStart(&l);
    LinePut(&l, "stitch ");
    LineNum(&l, u->seq);
    LinePut(&l, "'s ");
    LinePut(&l, KindName(kind));
    LinePut(&l, " of fid ");
    LineNum(&l, h->fid);
    LinePut(&l, " does not fit the file: ");
    LinePut(&l, why->text);
    LinePut(&l, " — the file stays as it last matched");
    LecternSay(b, &l);
    return SkipAdd(b, u->seq, h->fid);
}

/* The files in hand laid down as the stitch leaves them: one that did not fit
 * as it was — followed all the same — and one gone, gone. 1, or 0 no room. */
static int HandsLaid(LecternBook *b, LecternHands *hs)
{
    int ok = 1;
    for (uint64_t i = 0; i < hs->n; i++) {
        LecternHand *h    = &hs->at[i];
        LecternFile  gone;
        LecternZero(&gone, sizeof(gone));
        gone.fid  = h->fid;
        gone.gone = 1;
        LecternFile *held = LecternFileOf(b, h->fid);
        if (h->failed || !h->alive) {
            LecternFileGive(b, &h->f);
            if (held && !h->failed) {
                LecternFileGive(b, held);
                held->gone = 1;
            } else if (!held && ok) {
                ok = LecternFilePut(b, &gone);
            }
            continue;
        }
        h->f.gone = 0;
        if (ok) ok = LecternFilePut(b, &h->f);
        if (!ok) LecternFileGive(b, &h->f);
    }
    LecternGive(b, hs->at, hs->cap * sizeof(LecternHand));
    return ok;
}

/* Stitch `u`'s records of the files followed, each through the rule of its
 * kind (bookmount.py _Stitch). 1, or 0 no room. */
static int StitchApplied(LecternBook *b, const LecternStitch *u)
{
    LecternHands hs = { (LecternHand *)0, 0, 0 };
    int          rc = 1;

    for (uint32_t i = 0; rc && i < u->nrecords; i++) {
        const LecternRecord *r = &u->records[i];
        LecternRead          d;
        LecternLine          why;
        int                  none;
        if (!FileKind(r->kind)) continue;
        RecordRead(u, r, &d);
        if (Skipped(b, u->seq, d.fid)) continue;
        LecternHand *h = HandOf(b, &hs, r->kind, &d, &none);
        if (!h) {
            rc = none;
            continue;
        }
        LineStart(&why);
        int applied = Applied(b, h, u, r->kind, &d, &why);
        if (applied < 0) rc = 0;
        if (applied == 0) rc = Rot(b, h, u, r->kind, &why);
    }
    return HandsLaid(b, &hs) && rc;
}

int LecternReplay(LecternBook *b)
{
    if (!LecternUndone(b)) return 0;
    for (uint64_t s = 0; s < b->nstitches; s++) {
        const LecternStitch *u = b->stitches[s];
        LecternLine          why;
        if (LecternListHas(b->refused, b->nrefused, u->seq)) continue;
        Refusal(b, u, &why);
        if (why.at ? !Refuse(b, u, &why) : !StitchApplied(b, u)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* The digests the ribbon carries                                            */
/* ------------------------------------------------------------------------- */

static uint64_t OverlayAt(const LecternBook *b, uint64_t v)
{
    uint64_t lo = 0, hi = b->noverlay;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (b->overlay[mid].block < v) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

int LecternOverlaid(const LecternBook *b, uint64_t v, uint64_t *digest)
{
    uint64_t at = OverlayAt(b, v);
    if (at >= b->noverlay || b->overlay[at].block != v) return 0;
    *digest = b->overlay[at].digest;
    return 1;
}

/* Whether block `v` lies in a run of the kernels to try. */
static int KernelBlock(LecternFile *const *kernels, uint64_t n, uint64_t v)
{
    for (uint64_t i = 0; i < n; i++)
        for (uint64_t r = 0; r < kernels[i]->nruns; r++) {
            const LecternRun *run = &kernels[i]->runs[r];
            if (!run->hole && v >= run->start && v - run->start < run->count) return 1;
        }
    return 0;
}

/* The digests a record carries laid on the blocks it puts in, in order — a
 * DATA run's not SHARED, one a block — those of the kernels' blocks kept, a
 * later over an earlier (bookmount.py, overlay.update). */
typedef struct {
    LecternBook        *b;
    LecternFile *const *kernels;
    uint64_t            n;
    const uint8_t      *digests;
    uint64_t            ndigests, used;
    int                 ok;
} LecternLaid;

static int DigestLaid(void *ctx, const TallyRun *run)
{
    LecternLaid *l = (LecternLaid *)ctx;
    LecternBook *b = l->b;
    if (!RibbonRunFresh(run)) return 1;
    for (uint64_t i = 0; i < run->count && l->used < l->ndigests; i++, l->used++) {
        uint64_t v = run->start + i, at = OverlayAt(b, v);
        if (!KernelBlock(l->kernels, l->n, v)) continue;
        if (at >= b->noverlay || b->overlay[at].block != v) {
            if (!LecternGrow(b, (void **)&b->overlay, b->noverlay, &b->capoverlay, sizeof(LecternDigest))) {
                l->ok = 0;
                return 0;
            }
            for (uint64_t k = b->noverlay; k > at; k--) b->overlay[k] = b->overlay[k - 1u];
            b->noverlay++;
        }
        b->overlay[at] = (LecternDigest){ v, TallyDigestAt(l->digests, l->used) };
    }
    return 1;
}

int LecternOverlay(LecternBook *b, LecternFile *const *kernels, uint64_t n)
{
    for (uint64_t s = 0; s < b->nstitches; s++) {
        const LecternStitch *u = b->stitches[s];
        if (LecternListHas(b->refused, b->nrefused, u->seq)) continue;
        for (uint32_t i = 0; i < u->nrecords; i++) {
            const LecternRecord *r = &u->records[i];
            LecternRead          d;
            if (r->kind != RIBBON_BORN && r->kind != RIBBON_SHAPED) continue;
            RecordRead(u, r, &d);
            if (Skipped(b, u->seq, d.fid)) continue;
            LecternLaid l = { b, kernels, n, (const uint8_t *)0, 0, 0, 1 };
            l.digests  = r->kind == RIBBON_BORN ? d.born.digests : d.shaped.digests;
            l.ndigests = r->kind == RIBBON_BORN ? d.born.ndigests : d.shaped.ndigests;
            RunsPut(u, r->kind, &d, DigestLaid, &l);
            if (!l.ok) return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* The ids the ribbon coins                                                  */
/* ------------------------------------------------------------------------- */

int LecternCoinedIds(LecternBook *b, const LecternSpelling *sp, uint64_t **ids, uint64_t *n, uint64_t *cap)
{
    for (uint64_t s = 0; s < b->nstitches; s++) {
        const LecternStitch *u = b->stitches[s];
        if (LecternListHas(b->refused, b->nrefused, u->seq)) continue;
        for (uint32_t i = 0; i < u->nrecords; i++) {
            LecternRead d;
            if (u->records[i].kind != RIBBON_COINED) continue;
            RecordRead(u, &u->records[i], &d);
            if (d.read == TALLY_BODY_BROKEN) continue;
            if (LecternSpells(sp, &d.tag)) {
                if (!LecternListAdd(b, ids, n, cap, d.id)) return 0;
                continue;
            }
            for (uint64_t k = 0; k < *n; k++) {
                if ((*ids)[k] != d.id) continue;
                for (; k + 1u < *n; k++) (*ids)[k] = (*ids)[k + 1u];
                (*n)--;
                break;
            }
        }
    }
    return 1;
}
