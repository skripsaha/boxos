"""stitch — the ribbon (spec_v2 §2.7, §2.8, §3.9, §4.5 п.5): the records a
Settle cycle sews, read and written; the ribbon walked from the slot a mount
stands on — the three classes of a unit, the direct scan, what lies past the
end; and a pen that sews stitches the way a kernel does, for the self-test's
landings.

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
from . import bookread, tally
from .tally import Run, RunsGet, RunsPut, TagDecode, TagEncode, TagState, TAG_HAS_VALUE, TAG_RETIRED

# The kinds of a stitch's records (§2.7, §2.8)
STITCH_HEAD, CLASP, UNDONE, BORN, SHAPED, LABELED, QUITCLAIM, COINED, USE_CONTEXT, FROZEN, THAWED = \
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
KIND_NAMES = {STITCH_HEAD: "StitchHead", CLASP: "Clasp", UNDONE: "Undone", BORN: "Born", SHAPED: "Shaped",
              LABELED: "Labeled", QUITCLAIM: "Quitclaim", COINED: "Coined", USE_CONTEXT: "UseContext",
              FROZEN: "Frozen", THAWED: "Thawed"}

# StitchHead.flags (§2.7)
DATA_FLUSHED, CLEAN, MEDIUM_LIED = 1, 2, 4

# Labeled.what (§2.8)
LABEL_NAME, LABEL_FLAGS, LABEL_TAGS = 1, 2, 4


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


def Labeled(fid, base, name=None, flags=None, add=(), drop=()):
    V = VarintPut
    what = (LABEL_NAME if name is not None else 0) | (LABEL_FLAGS if flags is not None else 0) | \
        (LABEL_TAGS if add or drop else 0)
    out = V(fid) + V(base) + V(what)
    if name is not None:
        out += V(len(name)) + name
    if flags is not None:
        out += V(flags[0]) + V(flags[1])
    if what & LABEL_TAGS:
        out += IdsPut(sorted(add)) + IdsPut(sorted(drop))
    return Record(LABELED, out)


def Quitclaim(fid, base):
    return Record(QUITCLAIM, VarintPut(fid) + VarintPut(base))


def Coined(tid, key=b"", value=None, retired=False):
    flags = (TAG_RETIRED if retired else 0) | (TAG_HAS_VALUE if value is not None and not retired else 0)
    return Record(COINED, VarintPut(tid) + TagEncode(TagState(flags, key, value)))


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
        return Coined(d["tid"], t.key, t.value, bool(t.flags & TAG_RETIRED))
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
                 "count", "fresh", "clasp", "clasp_at", "longer")

    def __init__(self, stream):
        self.why, self.records, self.clasp, self.clasp_at, self.longer = None, [], None, None, 0
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
    (§16.2 п.11), so one larger than a block is not asked of it."""
    room = start.Left() + (hop[1] * SECTORS_PER_BLOCK if hop else 0)
    if of < SECTORS_PER_BLOCK or of % SECTORS_PER_BLOCK:
        return "%u sector(s) long — no whole number of atoms of %u" % (of, SECTORS_PER_BLOCK)
    if start.off % SECTORS_PER_BLOCK:
        return "begun at sector %u of its Quire, off a block's boundary" % start.off
    if of > room:
        return "%u sectors long, more than its Quire and the next hold (%u)" % (of, room)
    return None


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
    reads there (a unit's own word on where it runs on to)."""
    raw, h = g.Sector(place.Vlba())
    if h is None or h["at"] != 0:
        return None
    p = Parsed(b"")
    try:
        p.Head(raw[HEAD_BYTES:])
    except Short:
        return None
    return p.next if p.next[1] else None


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
# The ribbon, walked from a slot (§4.5 п.5, pass 1)
# ---------------------------------------------------------------------------


class Ribbon:
    """The ribbon from the `at` of the slot a mount stands on, as pass 1 of
    §4.5 п.5 reads it: the stitches sewn on (`stitches`, each a Unit whole or
    closed and damaged) and the Quires the chain runs through; then the direct
    scan of what lies past its end (§2.2), each unit there joined to the one
    its StitchHead names into branches, and what each branch is (§3.9) —
    `said` [(code, severity, sentence)] for the census, `torn` the stitch of
    this history torn right after the end, `lied` when the medium is shown
    not to have kept what it confirmed.

    `taken` is the ground the book and the Tally's files hold, which a
    next_quire may not name (§3.7's table)."""

    def __init__(self, g, slot, taken=(), scan=True):
        self.g, self.slot, self.taken = g, slot, list(taken)
        self.stitches, self.quires, self.said, self.torn, self.past = [], [], [], None, []
        self.lied = bool(slot.flags & tally.SPINE_MEDIUM_LIED)
        r = slot.ribbon
        self.seq, self.clasp = slot.stitch_seq, slot.stitch_clasp
        self.quire = (r["quire_block"], r["quire_blocks"])
        self.next = None
        if not r["quire_blocks"] or not r["quire_block"] <= r["at"] < sum(self.quire):
            self.Say("RIBBON_TORN", "damage", "slot %u puts the ribbon on at block %u, outside the Quire "
                     "%u+%u it names — no stitch past its Tally is read" % (slot.index, r["at"], *self.quire))
            self.end = None
            return
        self.end = Place(self.quire, (r["at"] - r["quire_block"]) * SECTORS_PER_BLOCK)
        self.quires.append(self.quire)
        self.Named((r["next_block"], r["next_blocks"]), "slot %u's SpineRibbon" % slot.index)
        self._Sew()
        if scan:
            self._Scan()

    def Say(self, code, severity, text):
        self.said.append((code, severity, text))

    def Named(self, quire, who):
        """`quire` taken as the next_quire when one is named and it may be
        (§3.7's table: in the data run, on an atom, clear of the book and of
        the files) — said when it may not."""
        if not quire[1] or quire == self.quire:
            return
        g, grid = self.g, max(1, self.g.atom // 4096)
        if (quire[0] < g.data_block or quire[0] + quire[1] > g.data_end or quire[0] % grid or
                any(s < quire[0] + quire[1] and quire[0] < s + n for s, n in self.taken)):
            self.Say("RIBBON_TORN", "damage", "%s names Quire %u+%u as the ribbon's next, which lies "
                     "outside the data run, off an atom, or on the book or a file — the ribbon is not "
                     "taken past it there" % (who, quire[0], quire[1]))
            return
        self.next = quire
        if quire not in self.quires:
            self.quires.append(quire)

    def _Follows(self, u, at_next):
        """Does the StitchHead of `u` name the last stitch taken as the one
        before it — by its seq and by its Clasp (§2.7, H-2) — and, begun at
        the start of the next_quire, the Quire the last one ended in as the
        one it came from (§4.5 п.5 (б))? A StitchHead that does not read
        names nothing either way (rule (в) takes such a unit on the word of
        the stitch after it)."""
        h = u.head
        if h is None:
            return True
        if h.prev_clasp != self.clasp or h.prev != (u.seq - self.seq if self.seq else 0):
            return False
        return not at_next or h.came == self.quire

    def _Take(self, u):
        self.stitches.append(u)
        self.seq, self.clasp = u.seq, u.clasp
        if u.places[-1].quire != self.quire:
            self.quire, self.next = u.places[-1].quire, None
            if self.quire not in self.quires:
                self.quires.append(self.quire)
        self.end = u.end
        if u.head is not None:
            self.Named(u.head.next, "stitch seq %u" % u.seq)
            self.lied = self.lied or bool(u.head.flags & MEDIUM_LIED)
        wrong = u.LeadsWrong()
        if wrong:
            self.Say("RECORD_SHAPE", "damage", "stitch %u's sector(s) %s name lead(s) %s where its records "
                     "begin at %s (§2.1) — were a sector before them to rot, its walk would be taken up in "
                     "the wrong place" % (u.seq, ", ".join(str(k) for k, _h, _w in wrong),
                                          ", ".join(str(h) for _k, h, _w in wrong),
                                          ", ".join(str(w) for _k, _h, w in wrong)))

    def _Sew(self):
        """Rules (а)-(г) of §4.5 п.5, until none takes the next unit; a seq
        that rises at every stitch bounds the walk, and the data run's length
        bounds it again (`hops ≥ data_blocks`, meta_pool_mount.c)."""
        hops = 0
        while self.end is not None and hops <= self.g.data_end:
            hops += 1
            u = FindUnit(self.g, self.end, self.next, self.seq)
            if u is not None and u.Sound() and self._Follows(u, False):
                self._Take(u)
                continue
            v = None
            if self.next and self.end.quire != self.next:
                v = FindUnit(self.g, Place(self.next, 0), None, self.seq)
                if v is not None and v.Sound() and self._Follows(v, True):
                    self._Take(v)
                    continue
            if not any(self._Closed(c, c is v) for c in (u, v)
                       if c is not None and not c.Sound() and not c.bad):
                return

    def _Closed(self, u, at_next):
        """Rule (в): `u` closed and damaged when its StitchHead, if it reads,
        names the last stitch taken (an identity rot does not lend a stitch
        of another history), and the sound stitch after it names its seq and,
        where its Clasp reads, that Clasp."""
        if not self._Follows(u, at_next):
            return False
        hop = (u.head.next if u.head is not None and u.head.next[1] else None) or self.next
        f = FindUnit(self.g, u.end, hop, u.seq)
        if f is None or not f.Sound() or f.Names() != u.seq or \
                (u.clasp is not None and f.parsed.prev_clasp != u.clasp):
            return False
        self._Take(u)
        # A sector of another unit where this one's should be: an unordered
        # write or a lie, for a stitch with a larger seq lies after it (§3.9).
        foreign = [k for k in u.Lost() if u.places[k] is not None and
                   self.g.Sector(u.places[k].Vlba())[1] is not None]
        self.lied = self.lied or bool(foreign)
        self.Say("STITCH_TORN", "damage", "stitch %u is closed and damaged: sector(s) %s of its %u will "
                 "not read, and %u byte(s) of its records with them; what reads by `lead` is replayed "
                 "(§3.9)%s" % (u.seq, ", ".join(map(str, u.Lost())), u.of, u.lost,
                               "; sector(s) %s hold another unit's — a write out of order or a lie, and "
                               "a later stitch landed: MEDIUM_LIED" % ", ".join(map(str, foreign))
                               if foreign else ""))
        self._Take(f)
        return True

    def _Scan(self):
        """The direct scan (§2.2): the rest of the last Quire and all of its
        next, every sector of the ribbon past the last seq taken — each unit
        found, what it is said to be by the branch it belongs to (§3.9), and
        a unit no stitch is shaped like said as such."""
        if self.end is None:
            return
        room = self.end.Left() + (self.next[1] * SECTORS_PER_BLOCK if self.next else 0)
        places = [self.end.On(k, self.next) for k in range(room)]
        units = {}
        for i, p in enumerate(places):
            _raw, h = self.g.Sector(p.Vlba())
            if h is not None and h["kind"] == UNIT_STITCH and h["seq"] > self.seq and h["seq"] not in units \
                    and i >= h["at"]:
                units[h["seq"]] = Unit(self.g, places[i - h["at"]], h["of"], h["seq"], self.next)
        for s in sorted(units):
            u = units[s]
            if u.bad:
                self.Say("RECORD_SHAPE", "damage", "a unit of the ribbon at sector %u, seq %u, is %s: no "
                         "stitch is so shaped (§1.5, §2.2) — the ribbon is not taken past it, and its "
                         "records are not read" % (u.start.Vlba(), s, u.bad))
        tail = FindUnit(self.g, self.end, self.next, self.seq)
        self.torn = tail if tail is not None and not tail.Sound() and not tail.bad else None
        self.past = [units[s] for s in sorted(units) if not units[s].bad]
        for root, members in self._Branches():
            self._Branch(root, members)

    def _Branches(self):
        """[(root, members)]: the units past the end joined into branches,
        each to the unit its StitchHead names when that one lies past the end
        too (a seq named is always below the namer's, so the walk ends)."""
        by_seq, roots = {w.seq: w for w in self.past}, {}
        for w in self.past:
            r = w
            while r.Names() in by_seq:
                r = by_seq[r.Names()]
            roots.setdefault(r.seq, []).append(w)
        return [(by_seq[s], roots[s]) for s in sorted(roots)]

    def _Branch(self, r, members):
        """What one branch past the end is (§3.9's table), said once, by what
        its first unit `r` names and what its units name of each other."""
        seqs, n, L = ", ".join(str(w.seq) for w in members), r.Names(), self.seq
        tail = self.torn is not None and r.seq == self.torn.seq
        sewn = {u.seq: u.clasp for u in self.stitches}
        sewn[self.slot.stitch_seq] = self.slot.stitch_clasp
        by_seq = {w.seq: w for w in self.past}
        lost = sorted({w.Names() for w in members if w.Names() is not None and w.Names() > L and
                       (w.Names() not in by_seq or not by_seq[w.Names()].Sound())})
        # A stitch of the chain whose Clasp rotted names no Clasp to hold
        # the one after it to: that one is taken at its word.
        ours = n in sewn and (sewn[n] is None or r.head.prev_clasp == sewn[n])
        if n is not None and not ours and (n in sewn or n <= L):
            if tail:
                self.torn = None
            self.Say("CLASP_STRANGER", "note", "seq %s past the end of the ribbon: a dead branch of another "
                     "history — stitch seq %u names stitch %u with %s; it is not sewn on" % (
                         seqs, r.seq, n, "a Clasp that is not its own (0x%08X, not 0x%08X)"
                         % (r.head.prev_clasp, sewn[n]) if n in sewn else "which this ribbon does not hold"))
        elif lost:
            self.lied = True
            self._Lost(r, members, lost, seqs)
        elif tail and (n is None or n == L):
            self._Torn(self.torn)
        elif n is None:
            self.Say("DEAD_BRANCH", "note", "past the end of the ribbon lie pieces of seq %s whose StitchHead "
                     "does not read: a retry, or nothing known — no loss of what settled is shown" % seqs)
        elif not r.Sound():
            self.Say("DEAD_BRANCH", "note", "past the end of the ribbon lie pieces of seq %s naming stitch %u "
                     "of it: a retry cut while it was written — no loss of what settled is shown" % (seqs, n))
        elif n == L:
            self.Say("DEAD_BRANCH", "damage", "stitch seq %u names the end of the ribbon, seq %u, by its Clasp, "
                     "and lies where the ribbon does not go on — not the first atom after it, nor the start "
                     "of its next_quire naming its Quire (§4.5 п.5): seq %s, settled, are not taken"
                     % (r.seq, L, seqs))
        else:
            self.Say("DEAD_BRANCH", "damage", "the ribbon forks at stitch %u: seq %u names it by its Clasp, as "
                     "seq %u, sewn on by rule (а), does — a retry after a write the medium refused and kept "
                     "(M-6); seq %s, settled, are not taken" % (
                         n, r.seq, next(u.seq for u in self.stitches if u.seq > n), seqs))

    def _Lost(self, r, members, lost, seqs):
        """A branch some unit of which names one that settled — for it
        began only after that one's flush — and is not on the medium whole."""
        absent = [m for m in lost if m not in {w.seq for w in self.past}]
        if r.Sound() and r.Names() in absent:
            self.Say("DEAD_BRANCH", "damage", "the lie is proven: stitch seq %u lies past the end of the "
                     "ribbon and names seq %u, which the medium does not hold — a flush that lied, the "
                     "cycles after it written over a hole (FAST'13, H-2); seq %s are not applied — "
                     "MEDIUM_LIED" % (r.seq, r.Names(), seqs))
            return
        namer = min((w for w in members if w.Names() in lost), key=lambda w: w.seq)
        self.Say("DEAD_BRANCH", "damage", "what settled is lost: seq %s past the end of the ribbon — stitch "
                 "seq %u names %u, begun only after %u had settled, and %u %s (H-3) — MEDIUM_LIED"
                 % (seqs, namer.seq, namer.Names(), namer.Names(), namer.Names(),
                    "is not on the medium" if namer.Names() in absent else "does not read whole"))

    def _Torn(self, t):
        """The stitch right after the end, torn, and nothing past it names
        it: a cut while it was written — or, every sector of it there and
        some sum or its Clasp not holding, a cut or rot, said loud (M-4)."""
        if t.Whole():
            self.Say("RIBBON_TORN", "damage", "the last cycle does not read whole: stitch seq %u has all %u "
                     "sectors and its stream or its Clasp does not hold — a cut or rot (M-4)" % (t.seq, t.of))
        else:
            self.Say("RIBBON_TORN", "note", "the ribbon ends on stitch %u; seq %u past it is torn — %u of "
                     "its %u sectors hold, a cut while it was written; its numbers are not given again"
                     % (self.seq, t.seq, t.of - len(t.Lost()), t.of))

    def Seen(self):
        """[(kind, body, seq)] of every record read of what was not sewn on —
        the torn tail and what lies past — for the numbers it gave, which are
        never given again (§4.5 п.8)."""
        return [(r[0], r[2], u.seq) for u in ([self.torn] if self.torn else []) + self.past for r in u.records]


# ---------------------------------------------------------------------------
# A pen: stitches sewn the way a kernel sews them
# ---------------------------------------------------------------------------


class Sewn:
    __slots__ = ("seq", "of", "vlbas", "stream", "starts", "clasp", "sectors")

    def __init__(self, seq, of, vlbas, stream, starts, clasp, sectors):
        self.seq, self.of, self.vlbas, self.stream, self.starts = seq, of, vlbas, stream, starts
        self.clasp, self.sectors = clasp, sectors

    def LostAt(self, k):
        """The bytes a reader loses when sector k alone does not read: from
        where the entry the sector cuts begins to the first entry begun past
        it — said from what the pen laid down, not from the reader's walk."""
        lo, hi = k * PAYLOAD_BYTES, (k + 1) * PAYLOAD_BYTES
        spans = list(zip(self.starts, self.starts[1:] + [len(self.stream)]))
        first = min(s for s, e in spans if s < hi and e > lo)
        return min([s for s, _e in spans if s >= hi] + [len(self.stream)]) - first


class StitchPen:
    """Sews stitches on the ribbon of the newest slot that holds, from its
    `at`: each from the first atom after the last, in whole atoms, on into the
    next_quire past a Quire's end, its StitchHead naming the last one's seq
    and Clasp (or what it is told to)."""

    def __init__(self, over, g):
        slots = [tally.SlotRead(g, i) for i in range(g.frame["spine_slots"])]
        slot = max((s for s in slots if s.state == "valid"), key=lambda s: s.seq)
        r = slot.ribbon
        self.over, self.g, self.came = over, g, None
        self.place = Place((r["quire_block"], r["quire_blocks"]),
                           (r["at"] - r["quire_block"]) * SECTORS_PER_BLOCK)
        self.next = (r["next_block"], r["next_blocks"]) if r["next_blocks"] else None
        self.last, self.clasp, self.tally_seq = slot.stitch_seq, slot.stitch_clasp, slot.tally_seq
        self.next_seq = max([g.top_seq] + [s.seq for s in slots]) + 1

    def Room(self):
        return self.place.Left()

    def Hop(self, back_to=None):
        """On to the start of the next_quire, as a cycle whose stitch the
        medium refused goes on (M-6): `back_to` the last stitch that landed,
        which the next one names."""
        if not self.next:
            raise ValueError("the ribbon names no next_quire to go on in")
        self.came, self.place, self.next = self.place.quire, Place(self.next, 0), None
        if back_to is not None:
            self.last, self.clasp = back_to.seq, back_to.clasp

    def Sew(self, records, flags=0, seq=None, prev_clasp=None, made_ns=0, write=True, names=None,
            fresh=None, of=None):
        """One stitch of `records`; `write` False leaves the medium as it was
        — a stitch that never landed — and goes on as though it had. What a
        landing may make a stitch say that a kernel's would not: `names` (a
        Sewn) the stitch before it, `prev_clasp` that one's Clasp, `fresh`
        its Clasp's count of fresh blocks, `of` its sectors."""
        seq = seq or self.next_seq
        last, clasp = (names.seq, names.clasp) if names is not None else (self.last, self.clasp)
        came = self.came if self.place.off == 0 and self.came else (0, 0)
        head = HeadPut(flags, seq - last if last else 0, clasp if prev_clasp is None else prev_clasp,
                       seq - self.tally_seq, made_ns, self.next or (0, 0), came)
        stream, starts, clasp = StitchStream(head, records, fresh)
        grid = self.g.atom // 512
        if of is None:
            of = -(-len(stream) // PAYLOAD_BYTES)
            of = -(-of // grid) * grid
        places = [self.place.On(k, self.next) for k in range(of)]
        if None in places:
            raise ValueError("the ribbon has no room for a stitch of %u sectors" % of)
        vlbas = [p.Vlba() for p in places]
        sectors = UnitSeal(stream, starts, UNIT_STITCH, seq, vlbas, self.g.uuid)
        if write:
            for v, sector in zip(vlbas, sectors):
                self.over.Put(self.g.base + v, sector)
        end = self.place.On(of, self.next)
        if end is None or end.quire != self.place.quire:
            self.came, self.next = self.place.quire, None
        self.place = end
        self.last, self.clasp, self.next_seq = seq, clasp, max(self.next_seq, seq + 1)
        return Sewn(seq, of, vlbas, stream, starts, clasp, sectors)


def Circle(ribbon, sewn):
    """[what differs] between stitches the pen sewed and the ribbon read back:
    each sewn on whole, its stream the pen's, and each record of it decoded
    and written again the same bytes (§2.4: one value, one spelling)."""
    wrong, by_seq = [], {u.seq: u for u in ribbon.stitches}
    for s in sewn:
        u = by_seq.get(s.seq)
        if u is None or not u.Sound():
            wrong.append("stitch %u is not sewn on whole" % s.seq)
            continue
        if u.stream[:len(s.stream)] != s.stream:
            wrong.append("stitch %u reads back other bytes" % s.seq)
        for kind, _ign, body, _at in u.records:
            if kind in KIND_NAMES and RecordAgain(kind, RecordDecode(kind, body, u.seq)).body != body:
                wrong.append("stitch %u's %s is written again as other bytes" % (s.seq, KIND_NAMES[kind]))
    return wrong
