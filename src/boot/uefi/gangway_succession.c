/*
 * The Succession — which kernel the UEFI loader jumps into on a volume: the
 * kernel the Deed's BOOT names first, held to the DIGESTS tree of the slot
 * the book stands on with no walk of FILES; when it is not attested, the
 * kernels the book names by
 * role of their files (gangway_lectern_tree.c, _ribbon.c, _replay.c), role by
 * role in the order they are asked — kernel, boot, system — the newest gen
 * first and of one gen the larger fid, each once. A file's tags are its
 * user's and no part of the choice.
 *
 * A kernel is tried as it would be jumped into: what of it may be loaded — no
 * run of an extension this loader does not read (CODEC, COPY), its bytes
 * neither none nor more than KERNEL_MAX_SIZE; its first block as it loads,
 * the Slips of the ribbon laid when it is the last, held to the header the
 * kernel opens with — cheap, and the end of most files kept in the system's role;
 * then its blocks placed where it is jumped into and held to the book as the
 * medium gives them (Attest) — a Slip's bytes are held by the sums of the
 * stitch that carries them (§2.8) — the digest the ribbon's record of a block
 * carries before DIGESTS. The first attested is jumped into, its Slips laid.
 * One found broken, or refused, is named and passed; one the book cannot
 * vouch for is put off (F2). Nothing attested: BOOT, when nothing shows it
 * broken, is loaded not attested, else the first put off; nothing of that
 * either, and every kernel tried is said with why, and the sentence the
 * loader stops with. Line for line volume-read's bootread.py Succession.
 *
 * A kernel chosen, the file the loader asks for by name is read in the same
 * reading of the book (gangway_lectern_called.c) — the book past its Tally
 * read for it first when no kernel by role was looked for.
 */

#include "gangway_lectern_book.h"

/* A kernel to try: its name in the lines, its runs in file order, its size,
 * the Slips on its last block (NULL none), and whether the ribbon's digests
 * are asked first. */
typedef struct {
    LecternLine       who;
    const LecternRun *runs;
    uint64_t          nruns;
    uint64_t          size;
    const uint8_t    *tail;
    int               overlay;
} LecternCandidate;

typedef enum { TRY_HELD, TRY_BROKEN, TRY_UNCHECKED, TRY_REFUSED, TRY_UNPLACED } LecternTried;

/* The choice in the making. */
typedef struct {
    LecternBook      b;
    LecternKernel   *kernel;
    LecternLine     *tried;      /* the verdicts of the kernels refused or found broken, in turn */
    uint64_t         ntried, captried;
    LecternRun       boot_run;
    LecternCandidate boot;
    int              boot_unchecked;
    LecternFile     *put_off;    /* the first kernel by role the book could not vouch for */
} LecternChoice;

static uint64_t BlocksOf(uint64_t bytes)
{
    return bytes / VOLUME_BLOCK_BYTES + (bytes % VOLUME_BLOCK_BYTES != 0);
}

/* A verdict kept for the list the loader stops with. Lines, and kernels to
 * try, are moved and cleared by LecternCopy and LecternZero, never assigned
 * whole: a struct of this size assigned is a memcpy the compiler calls, and
 * the loader has none (the clang→PE path links no library). */
static void Kept(LecternChoice *s, const LecternLine *l)
{
    if (LecternGrow(&s->b, (void **)&s->tried, s->ntried, &s->captried, sizeof(LecternLine)))
        LecternCopy(&s->tried[s->ntried++], l, sizeof(*l));
}

/* A kernel the book names by role, to try: "kernel fid N". */
static void ByTagCandidate(LecternCandidate *c, const LecternFile *f)
{
    LecternZero(c, sizeof(*c));
    LineStart(&c->who);
    LinePut(&c->who, "kernel fid ");
    LineNum(&c->who, f->fid);
    c->runs    = f->runs;
    c->nruns   = f->nruns;
    c->size    = f->size;
    c->tail    = f->tail;
    c->overlay = 1;
}

/* "<who> <what>[n<then>[m<last>]]": a refusal said and kept. */
static void Refused(LecternChoice *s, const LecternCandidate *c, const char *what, uint64_t n, const char *then,
                    uint64_t m, const char *last)
{
    LecternLine l;
    LineStart(&l);
    LinePut(&l, c->who.text);
    LinePut(&l, what);
    if (then) {
        LineNum(&l, n);
        LinePut(&l, then);
    }
    if (last) {
        LineNum(&l, m);
        LinePut(&l, last);
    }
    LecternSay(&s->b, &l);
    Kept(s, &l);
}

/* The header the kernel opens with (kernel_entry.asm): "KERNEL" at byte 2 —
 * or why not, said. */
static int HeaderHeld(LecternChoice *s, const LecternCandidate *c, const uint8_t *first)
{
    static const char magic[6] = { 'K', 'E', 'R', 'N', 'E', 'L' };
    for (uint32_t i = 0; i < sizeof(magic); i++) {
        if (first[2 + i] == (uint8_t)magic[i]) continue;
        Refused(s, c, " is no kernel: its first block does not begin with KERNEL", 0, (const char *)0, 0,
                (const char *)0);
        return 0;
    }
    return 1;
}

/* What may be loaded of the kernel, and its first block as it loads held to
 * its header: 1 so; 0 refused, said. */
static int Loadable(LecternChoice *s, const LecternCandidate *c)
{
    const char *none = (const char *)0;
    uint8_t    *first;
    int         read, held;

    for (uint64_t r = 0; r < c->nruns; r++) {
        if (c->runs[r].carried != RUN_CARRIED_NOT) continue;
        Refused(s, c, " cannot be loaded: a run of it carries an extension this loader does not read (CODEC or COPY, "
                      "§8)", 0, none, 0, none);
        return 0;
    }
    if (!c->size) {
        Refused(s, c, " cannot be loaded: it is empty", 0, none, 0, none);
        return 0;
    }
    if (c->size > KERNEL_MAX_SIZE) {
        Refused(s, c, " cannot be loaded: it is ", c->size, " bytes, more than the ", KERNEL_MAX_SIZE,
                " this loader places");
        return 0;
    }
    if (!(first = (uint8_t *)LecternTake(&s->b, VOLUME_BLOCK_BYTES))) return 0;
    read = LecternBlockRead(&s->b, (int)c->runs[0].hole, c->runs[0].start, first);
    if (read && BlocksOf(c->size) == 1) LecternTailLaid(c->tail, first);
    held = read && HeaderHeld(s, c, first);
    LecternGive(&s->b, first, VOLUME_BLOCK_BYTES);
    if (!read)
        Refused(s, c, " cannot be loaded: its block 0 (volume block ", c->runs[0].start, ") will not read", 0, none);
    return held;
}

/* The kernel's blocks placed where it is jumped into, read in file order as
 * the medium gives them: the room — NULL when the desk gives none
 * (TRY_UNPLACED), or a block will not read: given back, refused, said. */
static uint8_t *Placed(LecternChoice *s, const LecternCandidate *c, LecternTried *t)
{
    const LecternDesk *d      = s->b.desk;
    uint64_t           blocks = BlocksOf(c->size), k = 0;
    uint8_t           *image  = d->place(d->ctx, blocks * VOLUME_BLOCK_BYTES);

    *t = TRY_UNPLACED;
    if (!image) return image;
    for (uint64_t r = 0; r < c->nruns && k < blocks; r++) {
        for (uint64_t i = 0; i < c->runs[r].count && k < blocks; i++, k++) {
            if (LecternBlockRead(&s->b, (int)c->runs[r].hole, c->runs[r].start + i, image + k * VOLUME_BLOCK_BYTES))
                continue;
            d->unplace(d->ctx, image, blocks * VOLUME_BLOCK_BYTES);
            Refused(s, c, " cannot be loaded: its block ", k, " (volume block ", c->runs[r].start + i,
                    ") will not read");
            *t = TRY_REFUSED;
            return (uint8_t *)0;
        }
    }
    return image;
}

/* The kernel jumped into, or loaded not attested: its Slips laid, and what
 * the loader is told. */
static void Chosen(LecternChoice *s, const LecternCandidate *c, uint8_t *image, AttestSays says)
{
    LecternKernel *k      = s->kernel;
    uint64_t       blocks = BlocksOf(c->size), n = 0;
    LecternTailLaid(c->tail, image + (blocks - 1u) * VOLUME_BLOCK_BYTES);
    for (uint32_t i = 0; i <= c->who.at; i++) k->line[i] = c->who.text[i];
    k->says   = says;
    k->image  = image;
    k->bytes  = c->size;
    k->blocks = blocks;
    k->first  = c->runs[0].hole ? 0 : c->runs[0].start;
    k->runs   = 0;
    for (uint64_t r = 0; r < c->nruns && n < blocks; r++, k->runs++) n += c->runs[r].count;
}

/* One kernel tried. */
static LecternTried Try(LecternChoice *s, const LecternCandidate *c)
{
    const LecternDesk *d      = s->b.desk;
    uint64_t           blocks = BlocksOf(c->size);
    LecternLine        verdict;
    LecternTried       t;

    if (!Loadable(s, c)) return TRY_REFUSED;
    uint8_t *image = Placed(s, c, &t);
    if (!image) return t;
    AttestSays says = LecternAttest(&s->b, c->who.text, c->runs, c->nruns, blocks, image, c->overlay, &verdict);
    if (says == ATTEST_HELD) {
        Chosen(s, c, image, ATTEST_HELD);
        return TRY_HELD;
    }
    d->unplace(d->ctx, image, blocks * VOLUME_BLOCK_BYTES);
    if (says == ATTEST_UNCHECKED) return TRY_UNCHECKED;
    Kept(s, &verdict);
    return TRY_BROKEN;
}

/* ------------------------------------------------------------------------- */
/* The kernels in turn                                                       */
/* ------------------------------------------------------------------------- */

/* The kernel BOOT names: said, and a kernel to try — 0 when its bytes overrun
 * its blocks, refused (bootread.py Loaded says it the same way). */
static int BootNamed(LecternChoice *s, const VolumeBoot *boot)
{
    uint64_t    have = (uint64_t)boot->kernel_blocks * VOLUME_BLOCK_BYTES;
    uint64_t    blocks = boot->kernel_bytes ? BlocksOf(boot->kernel_bytes) : boot->kernel_blocks;
    LecternLine l;

    LineStart(&s->boot.who);
    LinePut(&s->boot.who, "the kernel the deed names");
    if (boot->kernel_bytes > have) {
        Refused(s, &s->boot, " cannot be loaded: it is said to be ", boot->kernel_bytes, " bytes and its blocks hold ",
                have, "");
        return 0;
    }
    s->boot_run   = (LecternRun){ boot->kernel_block, blocks, 0, RUN_CARRIED_PLAIN };
    s->boot.runs  = &s->boot_run;
    s->boot.nruns = 1;
    s->boot.size  = boot->kernel_bytes ? boot->kernel_bytes : blocks * VOLUME_BLOCK_BYTES;
    LineStart(&l);
    LinePut(&l, "the deed names the kernel at volume block ");
    LineNum(&l, boot->kernel_block);
    LinePut(&l, ", ");
    LineNum(&l, blocks);
    LinePut(&l, " block(s) for its ");
    LineNum(&l, boot->kernel_bytes);
    LinePut(&l, " byte(s)");
    LecternSay(&s->b, &l);
    return 1;
}

static int Outside(const LecternBook *b, const LecternFile *f)
{
    uint64_t low = b->desk->frame.data_block, end = b->desk->block_end;
    for (uint64_t r = 0; r < f->nruns; r++)
        if (!f->runs[r].hole &&
            (f->runs[r].start < low || f->runs[r].start > end || f->runs[r].count > end - f->runs[r].start))
            return 1;
    return 0;
}

/* The files followed that are kept in a role and not kept deleted, in the order they are tried,
 * into `out` (room for every file followed): role by role — kernel, boot,
 * system — the newest gen first and of one gen the larger fid, each once — a
 * file naming ground outside the data run is the mount's to lock, and no
 * kernel. Their count. */
static uint64_t Order(LecternBook *b, LecternFile **out)
{
    static const uint64_t roles[] = { TALLY_FILE_ROLE_KERNEL, TALLY_FILE_ROLE_BOOT, TALLY_FILE_ROLE_SYSTEM };
    uint64_t n = 0;
    for (uint32_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        uint64_t from = n;
        for (uint64_t f = 0; f < b->nfiles; f++) {
            LecternFile *file = &b->files[f];
            if (file->gone || (file->flags & TALLY_FILE_DELETED) || LecternRoleOf(file->flags) != roles[i] ||
                Outside(b, file))
                continue;
            uint64_t at = n++;
            for (; at > from && (out[at - 1u]->gen < file->gen ||
                                 (out[at - 1u]->gen == file->gen && out[at - 1u]->fid < file->fid));
                 at--)
                out[at] = out[at - 1u];
            out[at] = file;
        }
    }
    return n;
}

/* "the ribbon past slot i's Tally: N stitch(es) sewn on, seq a to b" — or no
 * stitch sewn on. */
static void RibbonSaid(LecternBook *b)
{
    LecternLine l;
    LineStart(&l);
    LinePut(&l, "the ribbon past slot ");
    LineNum(&l, b->slot_index);
    LinePut(&l, "'s Tally: ");
    if (!b->nstitches) {
        LinePut(&l, "no stitch sewn on");
    } else {
        LineNum(&l, b->nstitches);
        LinePut(&l, " stitch(es) sewn on, seq ");
        LineNum(&l, b->stitches[0]->seq);
        LinePut(&l, " to ");
        LineNum(&l, b->stitches[b->nstitches - 1u]->seq);
    }
    LecternSay(b, &l);
}

/* What the book names past its Tally, in the order volume-read's bootbook.py
 * says it: the ribbon, the files it puts in a role or names as asked, the
 * files of FILES kept in one or called so, and the ribbon's records replayed
 * for them — once, b->booked saying how it went. 1, or 0 no room. */
static int BookRead(LecternBook *b)
{
    int read = LecternSew(b);
    if (read) {
        RibbonSaid(b);
        LecternBefore(b);
        read = !b->starved && LecternFiles(b) && LecternReplay(b);
    }
    b->booked = read ? 1 : -1;
    return read;
}

/* The kernels the book names by role, tried in turn, the ribbon's digests of
 * their blocks gathered first: TRY_HELD when one is jumped into, the first
 * put off kept; TRY_UNPLACED when none can be placed; else TRY_REFUSED. */
static LecternTried ByTag(LecternChoice *s)
{
    LecternBook  *b = &s->b;
    LecternFile **kernels;
    LecternTried  t = TRY_REFUSED;
    uint64_t      n;

    if (!BookRead(b)) return t;
    if (!(kernels = (LecternFile **)LecternTake(b, (b->nfiles ? b->nfiles : 1u) * sizeof(LecternFile *)))) return t;
    n = Order(b, kernels);
    if (!LecternOverlay(b, kernels, n)) n = 0;
    for (uint64_t i = 0; i < n; i++) {
        LecternCandidate c;
        ByTagCandidate(&c, kernels[i]);
        LecternTried tried = Try(s, &c);
        if (tried == TRY_UNCHECKED && !s->put_off) s->put_off = kernels[i];
        if (tried == TRY_HELD || tried == TRY_UNPLACED) {
            t = tried;
            break;
        }
    }
    LecternGive(b, kernels, (b->nfiles ? b->nfiles : 1u) * sizeof(LecternFile *));
    return t;
}

/* The kernel nothing shows broken, loaded not attested (F2): BOOT's, else the
 * first put off. 1 placed; 0 none, or it would not place or load again. */
static int Unattested(LecternChoice *s)
{
    LecternCandidate c;
    LecternTried     t;
    LecternLine      l;

    if (s->boot_unchecked)
        LecternCopy(&c, &s->boot, sizeof(c));
    else if (s->put_off)
        ByTagCandidate(&c, s->put_off);
    else
        return 0;
    LineStart(&l);
    LinePut(&l, "no kernel is attested by the book: ");
    LinePut(&l, c.who.text);
    LinePut(&l, " is jumped into not attested — nothing shows it broken");
    LecternSay(&s->b, &l);
    uint8_t *image = Placed(s, &c, &t);
    if (image) Chosen(s, &c, image, ATTEST_UNCHECKED);
    return image != (uint8_t *)0;
}

/* Nothing jumped into: every kernel tried and why, and the sentence the loader
 * stops with — said, and the kernel's line. `unplaced` when the desk would not
 * place a kernel (it said why): no other is tried, for it would not either. */
static void Stopped(LecternChoice *s, int unplaced)
{
    LecternBook *b = &s->b;
    LecternLine  stop;

    LineStart(&stop);
    if (unplaced) {
        LinePut(&stop, "no kernel on this volume can be placed where it is jumped into");
    } else if (s->ntried) {
        LinePut(&stop, "every kernel tried, and why none is jumped into:");
        LecternSay(b, &stop);
        for (uint64_t i = 0; i < s->ntried; i++) {
            LineStart(&stop);
            LinePut(&stop, "  ");
            LinePut(&stop, s->tried[i].text);
            LecternSay(b, &stop);
        }
        LineStart(&stop);
        LinePut(&stop, "no kernel on this volume may be jumped into: ");
        LineNum(&stop, s->ntried);
        LinePut(&stop, " tried, none holds to the book");
    } else if (!b->stood) {
        LinePut(&stop, "this volume's deed names no kernel, and its book does not read: ");
        LinePut(&stop, b->why.text);
    } else {
        LinePut(&stop, "this volume's deed names no kernel, and its book names none by role");
    }
    LecternSay(b, &stop);
    for (uint32_t i = 0; i <= stop.at; i++) s->kernel->line[i] = stop.text[i];
    s->kernel->says = ATTEST_BROKEN;
}

void LecternSuccession(const LecternDesk *desk, const VolumeBoot *boot, LecternKernel *kernel,
                       LecternCalled *called, uint32_t ncalled)
{
    LecternChoice s;
    uint64_t      lanes[4];
    LecternTried  t = TRY_REFUSED;
    int           booted;

    LecternZero(&s, sizeof(s));
    LecternZero(kernel, sizeof(*kernel));
    s.b.desk   = desk;
    s.b.called  = called;
    s.b.ncalled = ncalled;
    s.kernel   = kernel;
    WySeedLanes(desk->uuid, 16u, lanes);
    s.b.lane = lanes[0];
    booted   = boot && BootNamed(&s, boot);
    LecternStand(&s.b);
    if (booted) {
        t                = Try(&s, &s.boot);
        s.boot_unchecked = t == TRY_UNCHECKED;
    }
    if (t != TRY_HELD && t != TRY_UNPLACED && s.b.stood && !s.b.starved) t = ByTag(&s);
    if (t != TRY_HELD && (t == TRY_UNPLACED || !Unattested(&s))) Stopped(&s, t == TRY_UNPLACED);
    if (ncalled && kernel->says != ATTEST_BROKEN) {
        if (s.b.stood && !s.b.starved && !s.b.booked) BookRead(&s.b);
        for (uint32_t k = 0; k < ncalled; k++) LecternCalledTake(&s.b, k);
    }
    LecternGive(&s.b, s.tried, s.captried * sizeof(LecternLine));
    LecternClose(&s.b);
}
