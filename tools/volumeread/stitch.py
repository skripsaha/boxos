"""stitch — the ribbon: the records a
Settle cycle sews, read and written; a unit of the ribbon, the three classes
of it and the place it lies in. The ribbon walked from the slot a mount
stands on — the direct scan, what lies past the end — is stitch_ribbon.py;
the pen that sews stitches the way a kernel does, for the self-test's
landings, is stitch_pen.py.

A stitch is one unit of kind STITCH: StitchHead, the cycle's records, Clasp,
then PAD to the end of its last atom. It is sewn on only when it names the
Clasp of the stitch before it (`prev_clasp`, H-2) — a stitch of another
history does not fit the chain however high its seq. What is read here is
held as raw records; bookmount.py replays them (pass 2) through the rules of
§3.7 and decodes each kind with RecordDecode.
"""

from .bookread import (Crc32c, Cursor, EntryNext, EntryPut, IdsPut, Short, UnitSeal, VarintPut, Zigzag,
                       HEAD_BYTES, PAYLOAD_BYTES, SECTOR_HEAD, UNIT_STITCH)
from .ondisk import SECTORS_PER_BLOCK
from . import bookread, ondisk, tally
from .tally import Run, RunsGet, RunsPut, TagDecode, TagEncode, TagState, TAG_HAS_VALUE, TAG_OWNED, TAG_RETIRED

# The kinds of a stitch's records (§2.7, §2.8)
STITCH_HEAD, CLASP, UNDONE, BORN, SHAPED, LABELED, QUITCLAIM, COINED, USE_CONTEXT, FROZEN, THAWED = \
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
KIND_NAMES = {STITCH_HEAD: "StitchHead", CLASP: "Clasp", UNDONE: "Undone", BORN: "Born", SHAPED: "Shaped",
              LABELED: "Labeled", QUITCLAIM: "Quitclaim", COINED: "Coined", USE_CONTEXT: "UseContext",
              FROZEN: "Frozen", THAWED: "Thawed"}

# StitchHead.flags (§2.7)
DATA_FLUSHED, CLEAN, MEDIUM_LIED = 1, 2, 4

# Labeled.what (§2.8)
LABEL_NAME, LABEL_FLAGS, LABEL_TAGS, LABEL_KEEP = 1, 2, 4, 8

# The longest stream of a stitch a reader holds: its offsets are 32 bits
# (volume_ribbon.h RIBBON_STREAM_MOST).
STREAM_MOST = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Records, as a writer makes them
# ---------------------------------------------------------------------------


class Record:
    __slots__ = ("kind", "ignorable", "body")

    def __init__(self, kind, body, ignorable=False):
        self.kind, self.body, self.ignorable = kind, bytes(body), ignorable

    def Bytes(self):
        return EntryPut(self.kind, self.body, self.ignorable)


def Data(start, count, ext=None):
    return Run(False, start, count, 0, ext)


def Hole(count):
    return Run(True, 0, count, 0)


def _Slip(slip):
    off, data = slip or (0, b"")
    return VarintPut(off) + VarintPut(len(data)) + data


def SlipDelta(before, after, cut):
    """The Slip a stitch lays on a file's last block (§16.5 п.13) — one rule
    with the kernel's (file_map/tail_slip.c, TailSlipDelta): `before` the
    overlay a reader holds once the record's splices and size stand, `after`
    the writer's, each {offset: byte} of the block, and `cut` the file's bytes
    in it. The span from the first byte the two disagree on to the last, its
    bytes the writer's — a byte it does not cover the zero of the hole beneath
    (over a DATA block the writer covers the span whole); (0, b"") for none."""
    diff = [i for i in range(cut) if after.get(i) != before.get(i)]
    if not diff:
        return (0, b"")
    lo, hi = diff[0], diff[-1] + 1
    return (lo, bytes(after.get(i, 0) for i in range(lo, hi)))


def _Digests(digests):
    return b"".join(d.to_bytes(8, "little") for d in digests)


def Born(fid, flags, size, created_ns, modified_ns, name, tags, runs, digests, slip=None):
    V = VarintPut
    return Record(BORN, V(fid) + V(flags) + V(size) + V(created_ns) + V(Zigzag(modified_ns - created_ns)) +
                  V(len(name)) + name + IdsPut(sorted(tags)) + V(len(runs)) + RunsPut(runs, 0, False)[0] +
                  _Digests(digests) + _Slip(slip))


def Shaped(fid, base, size, mtime_ns, splices, digests, slip=None):
    V = VarintPut
    out, cursor = [V(fid), V(base), V(size), V(mtime_ns), V(len(splices))], 0
    for at, remove, runs in splices:
        body, cursor = RunsPut(runs, 0, False, cursor)
        out += [V(at), V(remove), V(len(runs)), body]
    return Record(SHAPED, b"".join(out) + _Digests(digests) + _Slip(slip))


def Labeled(fid, base, name=None, flags=None, add=(), drop=(), keep=None):
    """keep is the deletion's one verb: the nanoseconds a file is kept until, or 0 to bring it back."""
    V = VarintPut
    what = (LABEL_NAME if name is not None else 0) | (LABEL_FLAGS if flags is not None else 0) | \
        (LABEL_TAGS if add or drop else 0) | (LABEL_KEEP if keep is not None else 0)
    out = V(fid) + V(base) + V(what)
    if name is not None:
        out += V(len(name)) + name
    if flags is not None:
        out += V(flags[0]) + V(flags[1])
    if what & LABEL_TAGS:
        out += IdsPut(sorted(add)) + IdsPut(sorted(drop))
    if what & LABEL_KEEP:
        out += V(keep)
    return Record(LABELED, out)


def Quitclaim(fid, base):
    return Record(QUITCLAIM, VarintPut(fid) + VarintPut(base))


def Coined(tid, key=b"", value=None, retired=False, owner=0):
    """A tag given its id; `owner` the fid of the program whose own tag it is,
    0 for the person's."""
    flags = (TAG_RETIRED if retired else
             (TAG_HAS_VALUE if value is not None else 0) | (TAG_OWNED if owner else 0))
    return Record(COINED, VarintPut(tid) + TagEncode(TagState(flags, key, value, owner)))


def UseContext(text):
    return Record(USE_CONTEXT, VarintPut(len(text)) + text)


def Undone(seq_back, why, fids):
    return Record(UNDONE, VarintPut(seq_back) + VarintPut(why) + IdsPut(sorted(fids)))


def Frozen(bid, scope_mode, scope, name):
    out = VarintPut(bid) + VarintPut(scope_mode)
    if scope_mode == 1:
        out += IdsPut(sorted(scope))
    return Record(FROZEN, out + VarintPut(len(name)) + name)


def Thawed(bid):
    return Record(THAWED, VarintPut(bid))


def Unknown(kind, ignorable, body):
    return Record(kind, body, bool(ignorable))


# ---------------------------------------------------------------------------
# Records, as a reader decodes them
# ---------------------------------------------------------------------------


def DigestCount(runs):
    """How many digests a record carries for the runs it puts in (§2.8): one
    a block of every DATA run, none for a run SHARED (its blocks are
    another's), the stored blocks of one CODEC (§8.3)."""
    n = 0
    for r in runs:
        exts = {} if r.hole else Extensions(r.ext)
        if r.hole or tally.EXT_SHARED in exts:
            continue
        if tally.EXT_CODEC in exts:
            c = Cursor(exts[tally.EXT_CODEC])
            c.V()
            n += c.V()
        else:
            n += r.count
    return n


def Extensions(ext):
    """{kind: body} of a run's TLV list of extensions."""
    out, at = {}, 0
    while ext:
        state, tag, body, n, at = EntryNext(ext, at)
        if state < 0:
            raise Short("a run's extensions do not walk")
        if state == 0:
            break
        out.setdefault(tag >> 1, ext[body:body + n])
    return out


def _SlipGet(c):
    off = c.V()
    return off, c.Take(c.V())


def RecordDecode(kind, body, seq):
    """A record's fields as a dict, its runs born at `seq`; `longer` the bytes
    past the fields this reader knows (§2.3, M-8). Short when the body ends
    before its fields do."""
    c, d = Cursor(body), {}
    if kind == BORN:
        d.update(fid=c.V(), flags=c.V(), size=c.V(), created_ns=c.V())
        if d["flags"] & ondisk.FILE_DELETED:
            raise Short("its Born is deleted — a file is born whole, and a Labeled KEEP deletes it")
        d["modified_ns"] = d["created_ns"] + c.Z()
        d["name"] = c.Take(c.V())
        d["tags"] = c.Ids()
        d["runs"] = RunsGet(c, c.V(), seq, False)[0]
        d["digests"] = [c.Fixed("<Q") for _ in range(DigestCount(d["runs"]))]
        d["slip"] = _SlipGet(c)
    elif kind == SHAPED:
        d.update(fid=c.V(), base=c.V(), size=c.V(), mtime_ns=c.V())
        splices, cursor = [], 0
        for _ in range(c.V()):
            at, remove = c.V(), c.V()
            runs, cursor = RunsGet(c, c.V(), seq, False, cursor)
            splices.append((at, remove, runs))
        d["splices"] = splices
        d["digests"] = [c.Fixed("<Q") for _ in range(DigestCount([r for _a, _m, rs in splices for r in rs]))]
        d["slip"] = _SlipGet(c)
    elif kind == LABELED:
        d.update(fid=c.V(), base=c.V(), what=c.V())
        d["name"] = c.Take(c.V()) if d["what"] & LABEL_NAME else None
        d["flags"] = (c.V(), c.V()) if d["what"] & LABEL_FLAGS else None
        d["add"], d["drop"] = (c.Ids(), c.Ids()) if d["what"] & LABEL_TAGS else ([], [])
        d["keep"] = c.V() if d["what"] & LABEL_KEEP else None
        if d["flags"] and (d["flags"][0] | d["flags"][1]) & ondisk.FILE_DELETED:
            raise Short("its Labeled names the deleted bit in FLAGS — the bit is KEEP's to put on and take off")
    elif kind == QUITCLAIM:
        d.update(fid=c.V(), base=c.V())
    elif kind == COINED:
        d["tid"] = c.V()
        d["tag"] = TagDecode(c, "", [])
    elif kind == USE_CONTEXT:
        d["text"] = c.Take(c.V())
    elif kind == UNDONE:
        d.update(seq_back=c.V(), why=c.V(), fids=c.Ids())
    elif kind == FROZEN:
        d.update(bid=c.V(), scope_mode=c.V())
        d["scope"] = c.Ids() if d["scope_mode"] == 1 else []
        d["name"] = c.Take(c.V())
    elif kind == THAWED:
        d["bid"] = c.V()
    d["longer"] = c.Left()
    return d


def RecordAgain(kind, d):
    """The record a decoded dict says, written again (the circle)."""
    if kind == BORN:
        return Born(d["fid"], d["flags"], d["size"], d["created_ns"], d["modified_ns"], d["name"], d["tags"],
                    d["runs"], d["digests"], d["slip"])
    if kind == SHAPED:
        return Shaped(d["fid"], d["base"], d["size"], d["mtime_ns"], d["splices"], d["digests"], d["slip"])
    if kind == LABELED:
        return Labeled(d["fid"], d["base"], d["name"], d["flags"], d["add"], d["drop"])
    if kind == QUITCLAIM:
        return Quitclaim(d["fid"], d["base"])
    if kind == COINED:
        t = d["tag"]
        return Coined(d["tid"], t.key, t.value, bool(t.flags & TAG_RETIRED), t.owner)
    if kind == USE_CONTEXT:
        return UseContext(d["text"])
    if kind == UNDONE:
        return Undone(d["seq_back"], d["why"], d["fids"])
    if kind == FROZEN:
        return Frozen(d["bid"], d["scope_mode"], d["scope"], d["name"])
    return Thawed(d["bid"])


# ---------------------------------------------------------------------------
# A stitch's stream
# ---------------------------------------------------------------------------


def HeadPut(flags, prev, prev_clasp, tally_back, made_ns, nxt, came):
    V = VarintPut
    return EntryPut(STITCH_HEAD, V(flags) + V(prev) + prev_clasp.to_bytes(4, "little") + V(tally_back) +
                    V(made_ns) + V(nxt[0]) + V(nxt[1]) + V(came[0]) + V(came[1]))


def Fresh(records):
    """Clasp.fresh: the blocks of fresh data a stitch names — the DATA runs of
    its Born and Shaped records but SHARED ones (§2.7)."""
    n = 0
    for r in records:
        if r.kind in (BORN, SHAPED):
            d = RecordDecode(r.kind, r.body, 0)
            runs = d["runs"] if r.kind == BORN else [x for _a, _m, rs in d["splices"] for x in rs]
            n += sum(x.count for x in runs if not x.hole and tally.EXT_SHARED not in Extensions(x.ext))
    return n


def StitchStream(head, records, fresh=None):
    """(stream, starts, clasp): StitchHead, the records, Clasp — its `fresh`
    the records' own unless a landing names another."""
    parts = [head] + [r.Bytes() for r in records]
    starts, at = [], 0
    for p in parts:
        starts.append(at)
        at += len(p)
    body = b"".join(parts)
    clasp = Crc32c(body)
    starts.append(at)
    fresh = Fresh(records) if fresh is None else fresh
    return body + EntryPut(CLASP, VarintPut(len(records)) + VarintPut(fresh) +
                           clasp.to_bytes(4, "little")), starts, clasp


class Parsed:
    """A stitch's stream read whole: its StitchHead's fields, its records
    [(kind, ignorable, body, offset)], its Clasp's and where the Clasp
    begins — `why` when the stream does not read from StitchHead to a Clasp
    that holds (§2.7)."""

    __slots__ = ("why", "flags", "prev", "prev_clasp", "tally", "made_ns", "next", "came", "records",
                 "count", "fresh", "clasp", "clasp_at", "longer", "end")

    def __init__(self, stream):
        self.why, self.records, self.clasp, self.clasp_at, self.longer = None, [], None, None, 0
        self.end = 0      # the stream's useful bytes: its records to the Clasp's end (§4.2)
        self.flags = self.prev = self.prev_clasp = self.tally = self.made_ns = self.count = self.fresh = 0
        self.next = self.came = (0, 0)
        try:
            self._Read(stream)
        except Short as e:
            self.why = str(e)

    def _Read(self, stream):
        at = self.Head(stream)
        while True:
            start = at
            state, tag, body, n, at = EntryNext(stream, at)
            if state < 0:
                raise Short("a record at byte %u runs past the stitch" % start)
            if state == 0:
                raise Short("it ends with no Clasp")
            if tag >> 1 == CLASP:
                c = Cursor(stream, body, body + n)
                self.count, self.fresh, self.clasp = c.V(), c.V(), c.Fixed("<I")
                self.longer += c.Left()
                if self.count != len(self.records):
                    raise Short("its Clasp counts %u records and %u lie before it"
                                % (self.count, len(self.records)))
                if self.clasp != Crc32c(stream[:start]):
                    raise Short("its Clasp's sum is not its stream's")
                if at < len(stream) and stream[at] != 0:
                    raise Short("a record follows its Clasp")
                self.clasp_at = start
                self.end      = at
                return
            self.records.append((tag >> 1, bool(tag & 1), stream[body:body + n], start))

    def Starts(self):
        """Where each record of the stream begins, StitchHead to Clasp — what
        every sector's `lead` is said from (§2.1)."""
        return [0] + [r[3] for r in self.records] + [self.clasp_at]

    def Head(self, stream):
        """The StitchHead's fields: where the record after it begins."""
        state, tag, body, n, at = EntryNext(stream, 0)
        if state != 1 or tag >> 1 != STITCH_HEAD:
            raise Short("it opens with no StitchHead")
        c = Cursor(stream, body, body + n)
        self.flags, self.prev, self.prev_clasp = c.V(), c.V(), c.Fixed("<I")
        self.tally, self.made_ns = c.V(), c.V()
        self.next, self.came = (c.V(), c.V()), (c.V(), c.V())
        self.longer = c.Left()
        return at


def Pieces(stream, valid, leads):
    """What can be read of a stream some sectors of which are lost (`valid[k]`
    False), taking the walk up again after a gap at the `lead` of the first
    sector after it that names one (§2.1): (records [(kind, ignorable, body,
    offset)], the StitchHead (a Parsed) or None, the Clasp's crc or None, the
    bytes lost — from where the record the gap cut begins to where the walk is
    taken up)."""
    of, records, head, clasp, lost = len(valid), [], None, None, 0
    size = of * PAYLOAD_BYTES

    def Whole(lo, hi):
        return all(valid[k] for k in range(lo // PAYLOAD_BYTES, (hi - 1) // PAYLOAD_BYTES + 1))

    def Resume(j):
        return next((k * PAYLOAD_BYTES + leads[k] - HEAD_BYTES for k in range(j, of)
                     if valid[k] and leads[k]), None)

    pos = 0
    while pos is not None and pos < size:
        state, tag, body, n, nxt = EntryNext(stream, pos)
        if state == 0 and valid[pos // PAYLOAD_BYTES]:
            break
        if state == 1 and Whole(pos, nxt):
            if tag >> 1 == STITCH_HEAD and pos == 0:
                head = Parsed(b"")
                try:
                    head.Head(stream)
                    head.why = None
                except Short:
                    head = None
            elif tag >> 1 == CLASP:
                c = Cursor(stream, body, body + n)
                try:
                    c.V(), c.V()
                    clasp = c.Fixed("<I")
                except Short:
                    pass
                break
            else:
                records.append((tag >> 1, bool(tag & 1), stream[body:body + n], pos))
            pos = nxt
            continue
        k = pos // PAYLOAD_BYTES
        bad = next((j for j in range(k, of) if not valid[j]), k)
        r = Resume(bad + 1)
        lost += (r if r is not None else min(size, (bad + 1) * PAYLOAD_BYTES)) - pos
        pos = r
    return records, head, clasp, lost


# ---------------------------------------------------------------------------
# Where on the ribbon: a Quire and an offset into it
# ---------------------------------------------------------------------------


class Place:
    """A sector of the ribbon: its Quire (block, blocks) and its offset there.
    Past the Quire's end the ribbon goes on at the start of the Quire named
    next (§2.7)."""

    __slots__ = ("quire", "off")

    def __init__(self, quire, off):
        self.quire, self.off = tuple(quire), off

    def Vlba(self):
        return self.quire[0] * SECTORS_PER_BLOCK + self.off

    def Left(self):
        return self.quire[1] * SECTORS_PER_BLOCK - self.off

    def On(self, k, nxt):
        """The place `k` sectors on, into `nxt` past the Quire's end — None
        when it would run off a Quire with nothing named after it."""
        if k < self.Left():
            return Place(self.quire, self.off + k)
        if not nxt or not nxt[1] or k - self.Left() >= nxt[1] * SECTORS_PER_BLOCK:
            return None
        return Place(nxt, k - self.Left())


def Misshapen(start, of, hop):
    """Why a unit of `of` sectors begun at `start` is no stitch the format
    allows (§2.2: a whole number of atoms, one at least; §1.5: begun on an
    atom; on into the next Quire and no further) — None when it is one. The
    atom held to is a block's: no writer's is smaller, and a reader takes the
    next unit where the last one ends by its writer's atom, not by its own
    (§16.2 п.11), so one larger than a block is not asked of it. Nor is a
    stream past 2^32 - 1 bytes read: a reader's offsets in it are 32 bits
    (volume_ribbon.h RibbonShape) — no writer's stitch, of two Quires at the
    most, comes near it."""
    room = start.Left() + (hop[1] * SECTORS_PER_BLOCK if hop else 0)
    if of < SECTORS_PER_BLOCK or of % SECTORS_PER_BLOCK:
        return "%u sector(s) long — no whole number of atoms of %u" % (of, SECTORS_PER_BLOCK)
    if start.off % SECTORS_PER_BLOCK:
        return "begun at sector %u of its Quire, off a block's boundary" % start.off
    if of > room:
        return "%u sectors long, more than its Quire and the next hold (%u)" % (of, room)
    if of * PAYLOAD_BYTES > STREAM_MOST:
        return "%u sectors long — a stream of more than %u bytes, past what a reader holds a stitch in" % (
            of, STREAM_MOST)
    return None


def Lies(g, quire):
    """Does `quire` lie where a Quire of the ribbon may — in the data run,
    begun on an atom (§3.7's table)? A Quire a slot or a unit's own
    StitchHead names is read only then: no word of the medium sends a reader
    past the volume's ground."""
    grid = max(1, g.atom // 4096)
    return bool(quire[1]) and g.data_block <= quire[0] and quire[0] + quire[1] <= g.data_end and \
        not quire[0] % grid


class Unit:
    """A unit of the ribbon begun at `start`: its seq and `of`, where each of
    its sectors lies, which of them hold as its own (`valid`) and which only
    say so in a head whose sum does not hold (`present` — a sector there,
    spoilt after it was written), and what reads of it — the stitch whole
    (`parsed`) or in pieces. `bad` says why it is no stitch at all."""

    __slots__ = ("start", "seq", "of", "bad", "places", "valid", "present", "leads", "stream", "parsed",
                 "records", "head", "clasp", "lost", "end")

    def __init__(self, g, start, of, seq, hop):
        self.start, self.seq, self.of = start, seq, of
        self.places, self.valid, self.present, self.leads = [], [], [], []
        self.stream, self.parsed, self.records, self.head, self.clasp, self.lost, self.end = \
            b"", None, [], None, None, 0, None
        self.bad = Misshapen(start, of, hop)
        if self.bad:
            return
        self.places = [start.On(k, hop) for k in range(of)]
        pay = []
        for k, p in enumerate(self.places):
            raw, h = g.Sector(p.Vlba())
            mine = lambda x: x["kind"] == UNIT_STITCH and x["seq"] == seq and x["of"] == of and x["at"] == k
            ok = h is not None and mine(h)
            self.valid.append(ok)
            self.present.append(ok or (raw is not None and mine(SECTOR_HEAD.Unpack(raw))))
            self.leads.append(h["lead"] if ok else 0)
            pay.append(raw[HEAD_BYTES:] if ok else bytes(PAYLOAD_BYTES))
        self.stream = b"".join(pay)
        self.end = start.On(of, hop)
        self.parsed = Parsed(self.stream) if all(self.valid) else None
        if self.Sound():
            self.records, self.head, self.clasp = self.parsed.records, self.parsed, self.parsed.clasp
        else:
            self.records, self.head, self.clasp, self.lost = Pieces(self.stream, self.valid, self.leads)

    def Sound(self):
        """Whole by its sectors, and read from StitchHead to a Clasp that holds."""
        return self.parsed is not None and self.parsed.why is None

    def Whole(self):
        """Are all its sectors there, sealed at its seq — however many of
        their sums hold (§3.9: then a cut or rot, not a cut alone)?"""
        return bool(self.present) and all(self.present)

    def Names(self):
        """The seq of the stitch its StitchHead names before it — 0 for none,
        None when its StitchHead does not read."""
        if self.head is None:
            return None
        return self.seq - self.head.prev if self.head.prev else 0

    def Lost(self):
        return [k for k, ok in enumerate(self.valid) if not ok]

    def LeadsWrong(self):
        """[(sector, lead, the lead its records give it)] of a sound unit."""
        return bookread.LeadsWrong(self.parsed.Starts(), self.leads) if self.Sound() else []


def Peek(g, place):
    """The next_quire the StitchHead in the sector at `place` names, when one
    reads there and the Quire lies where one may (a unit's own word on where
    it runs on to)."""
    raw, h = g.Sector(place.Vlba())
    if h is None or h["at"] != 0:
        return None
    p = Parsed(b"")
    try:
        p.Head(raw[HEAD_BYTES:])
    except Short:
        return None
    return p.next if Lies(g, p.next) else None


def FindUnit(g, place, hop, after):
    """The unit of seq past `after` that begins at `place`, by the first sector
    holding one from there on — None when the first such sector belongs to a
    unit begun elsewhere, or there is none in the rest of the Quire and the
    next."""
    if place is None:
        return None
    limit = place.Left() + (hop[1] * SECTORS_PER_BLOCK if hop else 0)
    g.Run(place.Vlba(), place.Left())
    if hop:
        g.Run(hop[0] * SECTORS_PER_BLOCK, hop[1] * SECTORS_PER_BLOCK)
    for j in range(limit):
        p = place.On(j, hop)
        if p is None:
            return None
        _raw, h = g.Sector(p.Vlba())
        if h is not None and h["kind"] == UNIT_STITCH and h["seq"] > after:
            if h["at"] != j:
                return None
            return Unit(g, place, h["of"], h["seq"], (Peek(g, place) if j == 0 else None) or hop)
    return None


# ---------------------------------------------------------------------------
# X3: the ribbon's numbers, and its golden stitch, against the header
# ---------------------------------------------------------------------------


def GoldenStitch():
    """volume_ribbon.h's golden stitch, by this module's own encoders: seq 7 at
    sector 0x4000 of the volume 00 01 .. 0F, one block, a record of every kind
    a stitch carries and one of a kind no reader knows, to step over."""
    head = HeadPut(DATA_FLUSHED, 2, 0xC0FFEE11, 4, 1700000000123456789, (3000, 256), (2560, 256))
    records = [Undone(1, 1, [5, 6]),
               Coined(42, b"key", b"val"),
               Born(5, ondisk.FILE_ACTIVE, 5000, 1700000000000000000, 1700000000000000007, b"golden",
                    [3, 5, 300], [Data(1024, 1), Hole(1)], [0x1122334455667788], (100, b"tail")),
               Shaped(6, 1, 9000, 2, [(1, 1, [Data(4096, 1)]), (0, 0, [Hole(1)])], [0x99AABBCCDDEEFF00]),
               Labeled(7, 2, name=b"renamed", flags=(4, 2), add=[9], drop=[3]),
               Quitclaim(8, 3),
               UseContext(b"work,play"),
               Frozen(1, 1, [2, 3], b"snap"),
               Thawed(1),
               Unknown(31, 1, b"\x07")]
    stream, starts, _clasp = StitchStream(head, records)
    vlbas = list(range(0x4000, 0x4000 + SECTORS_PER_BLOCK))
    return b"".join(UnitSeal(stream, starts, UNIT_STITCH, 7, vlbas, bookread.GOLDEN_UUID))


# The numbers of the ribbon's alphabet (volume_ribbon.h): the kinds, the
# flags and bits this module writes and reads by.
RIBBON_NUMBERS = (
    ("RIBBON_STITCH_HEAD", STITCH_HEAD), ("RIBBON_CLASP", CLASP), ("RIBBON_UNDONE", UNDONE), ("RIBBON_BORN", BORN),
    ("RIBBON_SHAPED", SHAPED), ("RIBBON_LABELED", LABELED), ("RIBBON_QUITCLAIM", QUITCLAIM),
    ("RIBBON_COINED", COINED), ("RIBBON_USE_CONTEXT", USE_CONTEXT), ("RIBBON_FROZEN", FROZEN),
    ("RIBBON_THAWED", THAWED), ("RIBBON_KINDS_KNOWN", max(KIND_NAMES)), ("STITCH_DATA_FLUSHED", DATA_FLUSHED),
    ("STITCH_CLEAN", CLEAN), ("STITCH_MEDIUM_LIED", MEDIUM_LIED), ("LABEL_NAME", LABEL_NAME),
    ("LABEL_FLAGS", LABEL_FLAGS), ("LABEL_TAGS", LABEL_TAGS), ("LABEL_KEEP", LABEL_KEEP), ("RUN_EXT_CODEC", tally.EXT_CODEC),
    ("RUN_EXT_SHARED", tally.EXT_SHARED), ("RUN_EXT_COPY", tally.EXT_COPY), ("UNDONE_NOT_LANDED", 1),
    ("UNDONE_ROT", 2), ("RIBBON_STREAM_MOST", STREAM_MOST))


def RibbonCheck(defines):
    """What is wrong here against volume_ribbon.h — its numbers read out of it
    (`defines`) — and how many things were held: every kind and bit of the
    ribbon (why 1 and 2 of an Undone are bookmount.py's UNDONE_WHY), and the
    sum of the golden stitch reached by this module's own encoders, as the
    kernel's ribbon suite reaches it by the header's."""
    wrong = ["%s: volume_ribbon.h says %r and stitch.py %r" % (c, defines.get(c), py)
             for c, py in RIBBON_NUMBERS if defines.get(c) != py]
    have, c = bookread.Crc32c(GoldenStitch()), "RIBBON_GOLDEN_STITCH_CRC"
    if have != defines.get(c):
        wrong.append("the golden stitch's sum: 0x%08X where %s is %s" % (
            have, c, "0x%08X" % defines[c] if defines.get(c) is not None else "missing"))
    return wrong, len(RIBBON_NUMBERS) + 1
