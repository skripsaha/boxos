"""tally — the Spine's slots and the Tally (spec_v2 §1.4, §2.5, §2.9): their
records and nodes, written and read, and the Tally a slot names read off a
medium.

A slot is one block of eight sectors of kind SLOT whose stream says where the
Tally is (its roots), where the ribbon goes on (SpineRibbon) and what numbers
the medium has given (SpineCounts). The Tally is a set of trees of nodes, each
node a unit of kind SHEET named by a BookPtr that checks its seq, in two copies
in two Tally-Quires.

The reader here takes every node in its first copy that holds, and in its
second when the first does not; a node neither copy of which holds below a
root is a rotten range of keys, said with the count its parent carries — the
mount (bookmount.py) decides what that locks and holds.

How a Tally is laid down — where each node goes, how a file is cut into
Parcels — is the binder's (binder.py); the encoders of every record and node
are here, beside their decoders.

Bytes in, values out, except where a function says it reads a medium (through
BookGround) or lays sectors over one (an OverlayMedium).
"""

from .bookread import (Cursor, EntryNext, EntryPut, IdsPut, LeadsWrong, PtrGet, Short, UnitSeal, VarintPut,
                       Zigzag, PAYLOAD_BYTES, UNIT_SHEET, UNIT_SLOT)
from . import bookread, ondisk
from .ondisk import BLOCK, SECTOR, SECTORS_PER_BLOCK

# The trees (§2.9) and how their keys are written
FILES, TAGS, USE, BOOKMARKS, LIEN, DIGESTS, MAP, SHARED = 1, 2, 3, 4, 5, 6, 7, 8
TREE_NAMES = {FILES: "FILES", TAGS: "TAGS", USE: "USE", BOOKMARKS: "BOOKMARKS", LIEN: "LIEN",
              DIGESTS: "DIGESTS", MAP: "MAP", SHARED: "SHARED"}
AUTHORITY = (FILES, TAGS, USE, BOOKMARKS, LIEN, DIGESTS)
PAIR_KEYS = (FILES, LIEN)
ROOT_DERIVED = 1                     # SpineRoot.flags bit 0

# The slot's records (§1.4) and the node's head (§2.9)
SPINE_HEAD, SPINE_ROOT, SPINE_RIBBON, SPINE_COUNTS, SPINE_FLAGS = 1, 2, 3, 4, 5
SPINE_MEDIUM_LIED = 1
NODE_HEAD = 1

# A run (§2.8) and its extensions (§8.2-8.4)
RUN_HOLE, RUN_EXT = 1, 2
EXT_CODEC, EXT_SHARED, EXT_COPY = 1, 2, 3

TAG_HAS_VALUE, TAG_RETIRED = 1, 2    # Coined / TAGS flags (§2.8, §6.4)
KERNEL_TAG_END = 1 << 31             # §6.4: the kernel gives ids below bit 31 (TAGFS_MAX_TAG_ID 0x7FFFFFFE)
TAG_KEY_WIDTH = 255                  # tag_spelling.h:33 TAG_KEY_WIDTH
TAG_TEXT_ROOM = 4074                 # tag_spelling.h:39 TAG_TEXT_WIDTH less the ':' — key and value

SHEET_PAYLOAD = SECTORS_PER_BLOCK * PAYLOAD_BYTES        # 3904: a Sheet's room for a stream


# ---------------------------------------------------------------------------
# What the Tally holds, in memory
# ---------------------------------------------------------------------------


class Run:
    """One run of a file: `count` blocks of a hole, or of the ground from
    `start`; `birth` the seq of the stitch (or Tally) that put it in; `ext`
    the TLV list of its extensions as it came, or None."""

    __slots__ = ("hole", "start", "count", "birth", "ext")

    def __init__(self, hole, start, count, birth, ext=None):
        self.hole, self.start, self.count, self.birth, self.ext = hole, start, count, birth, ext

    def __eq__(self, other):
        return isinstance(other, Run) and (self.hole, self.start, self.count, self.birth, self.ext) == \
            (other.hole, other.start, other.count, other.birth, other.ext)


def RunCarried(r):
    """Whether this reader carries run `r` on (volume_ribbon.h,
    RibbonRunCarried): one with no extension, or a DATA run whose every
    extension is SHARED with an empty body — one at least. A hole with any,
    CODEC, COPY, a kind this reader does not know, a SHARED with a body or a
    list that does not walk is a newer writer's, and the volume is only read."""
    if r.ext is None:
        return True
    if r.hole:
        return False
    at, named = 0, False
    while True:
        state, tag, _body, n, at = EntryNext(r.ext, at)
        if state <= 0:
            return state == 0 and named
        if tag >> 1 != EXT_SHARED or n:
            return False
        named = True


def RunShared(r):
    """Whether run `r` names ground other runs may name too (§8.2): SHARED,
    as this reader carries it on."""
    return r.ext is not None and RunCarried(r)


class FileState:
    """A file as the book holds it: its header, its runs and — read off a
    Tally — the Parcels they were cut into; `slips` the Slip overlays laid
    on its last block, in the order of their stitches."""

    __slots__ = ("fid", "gen", "flags", "size", "created_ns", "modified_ns", "name", "tags", "runs",
                 "parcels", "slips", "keep_ns")

    def __init__(self, fid, gen, flags, size, created_ns, modified_ns, name, tags, runs, parcels=None,
                 slips=None, keep_ns=0):
        self.fid, self.gen, self.flags, self.size = fid, gen, flags, size
        self.created_ns, self.modified_ns, self.name = created_ns, modified_ns, bytes(name)
        self.tags, self.runs = list(tags), list(runs)
        self.parcels, self.slips = parcels, list(slips or [])
        self.keep_ns = keep_ns      # with FILE_DELETED: the nanoseconds the file is kept until; 0 otherwise

    def Copy(self):
        return FileState(self.fid, self.gen, self.flags, self.size, self.created_ns, self.modified_ns,
                         self.name, self.tags, self.runs, self.parcels, self.slips, self.keep_ns)

    def Blocks(self):
        return sum(r.count for r in self.runs)

    def Ground(self):
        return [(r.start, r.count) for r in self.runs if not r.hole]

    def Owned(self):
        """The ground the file owns alone: its DATA runs not SHARED."""
        return [(r.start, r.count) for r in self.runs if not r.hole and not RunShared(r)]

    def Shared(self):
        """The ground it names beside other owners (§8.2)."""
        return [(r.start, r.count) for r in self.runs if RunShared(r)]


class TagState:
    __slots__ = ("flags", "key", "value")

    def __init__(self, flags, key, value):
        self.flags, self.key, self.value = flags, key, value

    def Text(self):
        return self.key + (b":" + self.value if self.value is not None else b"")


class Content:
    """Everything a Tally materialises: what the binder lays down and what
    the reader gives back."""

    def __init__(self, made_ns=0):
        self.files, self.tags, self.digests = {}, {}, {}
        # A file locked by name, as what of it read lay: {fid: (header FileState
        # or None, [(file block, runs)])} — a Tally names it again so (§16.4 п.5).
        self.kept = {}
        self.use = b""
        self.next_fid = self.next_tag = 0
        self.next_bookmark = 1
        self.made_ns = made_ns

    @staticmethod
    def Copy(c):
        out = Content(c.made_ns)
        out.files = {fid: fs.Copy() for fid, fs in c.files.items()}
        out.tags = dict(c.tags)
        out.digests = dict(c.digests)
        out.kept = dict(c.kept)
        out.use, out.next_fid, out.next_tag, out.next_bookmark = c.use, c.next_fid, c.next_tag, c.next_bookmark
        return out


# ---------------------------------------------------------------------------
# Slots (§1.4)
# ---------------------------------------------------------------------------


class Root:
    """A SpineRoot: the tree's flags, its root's level, its count, and the
    BookPtr to its root as bytes."""

    __slots__ = ("flags", "level", "items", "ptr")

    def __init__(self, flags, level, items, ptr):
        self.flags, self.level, self.items, self.ptr = flags, level, items, bytes(ptr)

    def Pointer(self):
        return PtrGet(self.ptr)


class Slot:
    __slots__ = ("index", "block", "state", "why", "seq", "tally_seq", "stitch_seq", "stitch_clasp",
                 "made_ns", "roots", "ribbon", "counts", "flags", "glass", "seqs", "starts", "shape")

    def __init__(self, index, block):
        self.index, self.block = index, block
        self.state, self.why, self.seq = "blank", None, 0
        self.tally_seq = self.stitch_seq = self.stitch_clasp = self.made_ns = 0
        self.roots, self.ribbon, self.counts, self.flags = {}, None, None, 0
        self.glass, self.seqs, self.starts, self.shape = [], [], [], []


def SlotBlock(g, i):
    return g.frame["spine_block"] + i * g.frame["spine_stride"]


def _Exact(c, what, glass):
    """A body read to its known end: what is left was written by a kernel
    that knows more (§2.3, M-8)."""
    if c.Left():
        glass.append("%s is %u byte(s) longer than this reader knows — written by a kernel that "
                     "knows more" % (what, c.Left()))


def SlotParse(stream, slot):
    """The slot's records into `slot`; Short when it does not parse."""
    at, kinds = 0, []
    while True:
        slot.starts.append(at)
        state, tag, body, n, at = EntryNext(stream, at)
        if state == 0:
            slot.starts.pop()
            break
        if state < 0:
            raise Short("a record at byte %u runs past the slot" % at)
        kind, c = tag >> 1, Cursor(stream, body, body + n)
        if not kinds and kind != SPINE_HEAD:
            raise Short("its first record is of kind %u, not SpineHead" % kind)
        kinds.append(kind)
        if kind == SPINE_HEAD and len(kinds) == 1:
            slot.tally_seq, slot.stitch_seq = c.V(), c.V()
            slot.stitch_clasp, slot.made_ns = c.Fixed("<I"), c.V()
        elif kind == SPINE_ROOT:
            tree, flags, level, items = c.V(), c.V(), c.V(), c.V()
            start = c.at
            c.Ptr()
            if tree in slot.roots:
                raise Short("it names a root of tree %u twice" % tree)
            slot.roots[tree] = Root(flags, level, items, stream[start:c.at])
        elif kind == SPINE_RIBBON and slot.ribbon is None:
            slot.ribbon = dict(at=c.V(), quire_block=c.V(), quire_blocks=c.V(), next_block=c.V(),
                               next_blocks=c.V())
        elif kind == SPINE_COUNTS and slot.counts is None:
            slot.counts = dict(next_fid=c.V(), next_tag=c.V(), next_bookmark=c.V())
        elif kind == SPINE_FLAGS:
            slot.flags = c.V()
        elif kind in (SPINE_HEAD, SPINE_RIBBON, SPINE_COUNTS):
            raise Short("it carries a second record of kind %u" % kind)
        elif not tag & 1:
            slot.glass.append("slot %u carries a record of kind %u this reader does not know and may "
                              "not step over — written by a kernel that knows more" % (slot.index, kind))
            continue
        else:
            continue
        _Exact(c, "slot %u's record of kind %u" % (slot.index, kind), slot.glass)
    if slot.ribbon is None or slot.counts is None:
        raise Short("it carries no %s" % ("SpineRibbon" if slot.ribbon is None else "SpineCounts"))


def SlotRead(g, i):
    """Slot `i` as the mount reads it (§1.4): valid only when all eight
    sectors hold as one SLOT unit and its stream parses; blank when it holds
    nothing at all; torn otherwise."""
    slot = Slot(i, SlotBlock(g, i))
    sectors = g.Run(slot.block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK)
    slot.seqs = [h["seq"] for _r, h in sectors if h is not None]
    if all(raw is not None and raw == bytes(SECTOR) for raw, _h in sectors):
        return slot
    stream, why = g.Unit(slot.block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK, UNIT_SLOT)
    if stream is None:
        slot.state, slot.why = "torn", why
        return slot
    slot.seq = slot.seqs[0]
    try:
        SlotParse(stream, slot)
    except Short as e:
        slot.state, slot.why = "torn", "its stream does not parse: %s" % e
        return slot
    slot.state = "valid"
    slot.shape = LeadsSaid("slot %u" % i, slot.starts, [h["lead"] for _r, h in sectors])
    return slot


def LeadsSaid(what, starts, leads):
    """[a sentence] when a unit read whole names in some sector a lead its
    own records do not give it (§2.1) — what a reader takes a stream up by
    when a sector before is lost; [] when every lead is its records'."""
    wrong = LeadsWrong(starts, leads)
    if not wrong:
        return []
    return ["%s: sector(s) %s name lead(s) %s where its records begin at %s (§2.1)"
            % (what, ", ".join(str(k) for k, _h, _w in wrong), ", ".join(str(h) for _k, h, _w in wrong),
               ", ".join(str(w) for _k, _h, w in wrong))]


def SlotStream(slot):
    """(stream, starts): SpineHead, the roots by tree, SpineRibbon,
    SpineCounts, SpineFlags when any flag is set — then PAD."""
    V = VarintPut
    r, n = slot.ribbon, slot.counts
    records = [EntryPut(SPINE_HEAD, V(slot.tally_seq) + V(slot.stitch_seq) +
                        slot.stitch_clasp.to_bytes(4, "little") + V(slot.made_ns))]
    for tree in sorted(slot.roots):
        root = slot.roots[tree]
        records.append(EntryPut(SPINE_ROOT, V(tree) + V(root.flags) + V(root.level) + V(root.items) +
                                root.ptr))
    records.append(EntryPut(SPINE_RIBBON, b"".join(V(r[k]) for k in ("at", "quire_block", "quire_blocks",
                                                                     "next_block", "next_blocks"))))
    records.append(EntryPut(SPINE_COUNTS, V(n["next_fid"]) + V(n["next_tag"]) + V(n["next_bookmark"])))
    if slot.flags:
        records.append(EntryPut(SPINE_FLAGS, V(slot.flags)))
    starts, at = [], 0
    for rec in records:
        starts.append(at)
        at += len(rec)
    stream = b"".join(records)
    if len(stream) > SHEET_PAYLOAD:
        raise ValueError("a slot of %u bytes does not fit its block" % len(stream))
    return stream, starts


def SlotSectors(g, index, seq, slot):
    """`slot`'s records as the eight sectors of slot `index`, sealed at seq."""
    stream, starts = SlotStream(slot)
    vlba = SlotBlock(g, index) * SECTORS_PER_BLOCK
    return UnitSeal(stream, starts, UNIT_SLOT, seq, list(range(vlba, vlba + SECTORS_PER_BLOCK)), g.uuid)


# ---------------------------------------------------------------------------
# Nodes (§2.9)
# ---------------------------------------------------------------------------


class Node:
    """A node of a Tally: its tree, level and items [(key, body bytes)]; the
    count its parent carries for it; where its copies lie (`blocks`), how
    many Sheets it takes and the seq it was sealed at — a constant of the
    node, which a later Tally may name unchanged (§2.9, §4.1)."""

    __slots__ = ("tree", "level", "items", "count", "sheets", "blocks", "seq", "off", "copy", "lo", "hi", "shape")

    def __init__(self, tree, level, items, count=0, sheets=1, blocks=(), seq=0):
        self.tree, self.level, self.items, self.count = tree, level, list(items), count
        self.sheets, self.blocks, self.seq = sheets, list(blocks), seq
        self.off = self.copy = None
        self.lo = self.hi = None
        self.shape = []

    def FirstKey(self):
        return self.items[0][0]


def KeyFull(tree, key):
    return VarintPut(key[0]) + VarintPut(key[1]) if tree in PAIR_KEYS else VarintPut(key)


def KeyPut(tree, key, prev):
    """A key as the distance from the one before it (§2.9): for a pair, d1,
    and when d1 is 0 the distance of the second number, else it whole."""
    if tree not in PAIR_KEYS:
        return VarintPut(key - prev)
    d1 = key[0] - prev[0]
    return VarintPut(0) + VarintPut(key[1] - prev[1]) if d1 == 0 else VarintPut(d1) + VarintPut(key[1])


def ItemBytes(tree, item, prev):
    """One item as a node carries it, its key written from `prev`'s."""
    k = KeyPut(tree, item[0], prev)
    return VarintPut(len(k) + len(item[1])) + k + item[1]


def KeyGet(c, tree, prev):
    if tree not in PAIR_KEYS:
        return prev + c.V()
    d1 = c.V()
    return (prev[0], prev[1] + c.V()) if d1 == 0 else (prev[0] + d1, c.V())


def NodeStream(tree, level, items):
    """(stream, starts) of a node: NodeHead, then each item's length, key and
    body. The first item's key is written as its distance from the NodeHead's
    first key — itself — so as 0."""
    first = items[0][0]
    out = [EntryPut(NODE_HEAD, VarintPut(tree) + VarintPut(level) + VarintPut(len(items)) +
                    KeyFull(tree, first))]
    starts, at, prev = [0], len(out[0]), first
    for item in items:
        e = ItemBytes(tree, item, prev)
        starts.append(at)
        out.append(e)
        at += len(e)
        prev = item[0]
    return b"".join(out), starts


def NodeParse(stream, glass):
    """A node's (tree, level, items, where each begins — the NodeHead and
    every item): Short when it does not parse — a head that is not a
    NodeHead, keys that do not rise, or a byte other than PAD past its last
    item."""
    state, tag, body, n, at = EntryNext(stream, 0)
    if state != 1 or tag >> 1 != NODE_HEAD:
        raise Short("it opens with no NodeHead")
    c = Cursor(stream, body, body + n)
    tree, level, count = c.V(), c.V(), c.V()
    first = (c.V(), c.V()) if tree in PAIR_KEYS else c.V()
    _Exact(c, "a NodeHead", glass)
    items, prev, starts = [], first, [0]
    for i in range(count):
        starts.append(at)
        c = Cursor(stream, at)
        size = c.V()
        end = c.at + size
        if end > len(stream):
            raise Short("item %u runs past the node" % i)
        c.end = end
        key = KeyGet(c, tree, prev)
        if (i and key <= prev) or key < first:
            raise Short("the keys do not rise at item %u" % i)
        items.append((key, stream[c.at:end]))
        prev, at = key, end
    if at < len(stream) and stream[at] != 0:
        raise Short("a byte other than PAD follows its last item")
    return tree, level, items, starts


def NodeSectors(g, seq, block, sheets, stream, starts):
    vlba = block * SECTORS_PER_BLOCK
    return UnitSeal(stream, starts, UNIT_SHEET, seq, list(range(vlba, vlba + SECTORS_PER_BLOCK * sheets)),
                    g.uuid)


def NodeRead(g, ptr, glass, lo=None, hi=None):
    """The node a BookPtr names, from its first copy that holds (§2.5) — and
    holds as the child its parent names: every key inside [lo, hi), the
    range the parent's separators give it (§2.9: a separator is the least
    key of its subtree). (node or None, [(copy, why)] of the copies that did
    not)."""
    bad, sectors = [], SECTORS_PER_BLOCK * ptr["sheets"]
    for k, block in enumerate(ptr["blocks"]):
        stream, why = g.Unit(block * SECTORS_PER_BLOCK, sectors, UNIT_SHEET, ptr["seq"])
        if stream is None:
            bad.append((k, why))
            continue
        mine = []
        try:
            tree, level, items, starts = NodeParse(stream, mine)
        except Short as e:
            bad.append((k, "it does not parse: %s" % e))
            continue
        out = [key for key, _b in items if (lo is not None and key < lo) or (hi is not None and key >= hi)]
        if out:
            bad.append((k, "its keys run outside [%s, %s), the range its parent gives it — %s first"
                        % (lo, "the end" if hi is None else hi, out[0])))
            continue
        glass.extend(mine)
        node = Node(tree, level, items, sheets=ptr["sheets"], blocks=ptr["blocks"], seq=ptr["seq"])
        node.copy = k
        node.shape = LeadsSaid("the %s node of level %u at block %u" % (TREE_NAMES.get(tree, tree), level, block),
                               starts, [h["lead"] for _r, h in g.Run(block * SECTORS_PER_BLOCK, sectors)])
        return node, bad
    return None, bad


# ---------------------------------------------------------------------------
# Elements (§2.9's table)
# ---------------------------------------------------------------------------


def RunsGet(c, n, birth_of, form_tally, cursor=0):
    """(`n` runs, the cursor after them): a DATA run's start is written as its
    distance from the end of the record's DATA run before it (`cursor`, §2.8);
    in the Tally's form it carries birth_delta, the distance of its birth
    below `birth_of` (the node's seq), in the ribbon's its birth is
    `birth_of`, the stitch's. Refused as the kernel's codec refuses it
    (volume_tally.h TallyRunGet): a run of no blocks, one that starts below
    block 0 or runs past block 2^64 - 1, one born before seq 0."""
    runs = []
    for _ in range(n):
        head = c.V()
        count, hole, ext = head >> 2, bool(head & RUN_HOLE), bool(head & RUN_EXT)
        if count == 0:
            raise Short("a run of no blocks")
        start, birth = 0, birth_of
        if not hole:
            start = cursor + c.Z()
            if start < 0:
                raise Short("a run starts before block 0")
            if start + count > bookread.U64:
                raise Short("a run runs past block 2^64 - 1")
            cursor = start + count
            if form_tally:
                birth = birth_of - c.V()
                if birth < 0:
                    raise Short("a run is born before seq 0")
        runs.append(Run(hole, start, count, birth, c.Take(c.V()) if ext else None))
    return runs, cursor


def RunsPut(runs, birth_of, form_tally, cursor=0):
    """(the runs' bytes, the cursor after them) — RunsGet's inverse."""
    out = []
    for r in runs:
        out.append(VarintPut(r.count << 2 | (RUN_HOLE if r.hole else 0) |
                             (RUN_EXT if r.ext is not None else 0)))
        if not r.hole:
            out.append(VarintPut(Zigzag(r.start - cursor)))
            cursor = r.start + r.count
            if form_tally:
                out.append(VarintPut(birth_of - r.birth))
        if r.ext is not None:
            out.append(VarintPut(len(r.ext)) + r.ext)
    return b"".join(out), cursor


def FileHeadEncode(fs):
    V = VarintPut
    head = (V(fs.gen) + V(fs.flags) + V(fs.size) + V(fs.created_ns) +
            V(Zigzag(fs.modified_ns - fs.created_ns)) + V(len(fs.name)) + fs.name + IdsPut(fs.tags))
    return head + V(fs.keep_ns) if fs.flags & ondisk.FILE_DELETED else head


def FileHeadDecode(fid, body, glass):
    c = Cursor(body)
    gen, flags, size, created = c.V(), c.V(), c.V(), c.V()
    modified = created + c.Z()
    name = c.Take(c.V())
    tags = c.Ids()
    keep = c.V() if flags & ondisk.FILE_DELETED else 0
    if flags & ondisk.FILE_DELETED and not keep:
        raise Short("the header of fid %u is deleted and names no term" % fid)
    _Exact(c, "the FILES header of fid %u" % fid, glass)
    if not flags & ondisk.FILE_ACTIVE:
        raise Short("the header of fid %u is not ACTIVE" % fid)
    return FileState(fid, gen, flags, size, created, modified, name, tags, [], parcels=[], keep_ns=keep)


def ParcelEncode(runs, seq):
    """A Parcel's body: its runs in the Tally's form, the cursor from 0 — each
    Parcel stands alone (§2.9)."""
    return VarintPut(len(runs)) + RunsPut(runs, seq, True)[0]


def ParcelDecode(body, seq, what, glass):
    c = Cursor(body)
    runs = RunsGet(c, c.V(), seq, True)[0]
    _Exact(c, what, glass)
    return runs


def TagEncode(ts):
    V = VarintPut
    if ts.flags & TAG_RETIRED:
        return V(ts.flags)
    out = V(ts.flags) + V(len(ts.key)) + ts.key
    return out + V(len(ts.value)) + ts.value if ts.flags & TAG_HAS_VALUE else out


def TagDecode(c, what, glass):
    """A tag's text as a TAGS element and a Coined record carry it: flags,
    then — unless RETIRED, which names no text — the key and the value when
    HAS_VALUE."""
    flags = c.V()
    if flags & TAG_RETIRED:
        return TagState(flags, b"", None)
    key = c.Take(c.V())
    value = c.Take(c.V()) if flags & TAG_HAS_VALUE else None
    return TagState(flags, key, value)


def TagGlass(tid, ts, glass):
    """What of a tag this kernel cannot carry on (§16.1 п.2, §6.4)."""
    if ts.flags & TAG_RETIRED:
        glass.append("tag id %u is RETIRED — this kernel gives no id twice" % tid)
    if tid >= KERNEL_TAG_END:
        glass.append("tag id %u is past 2^31 - 2, the last this kernel gives" % tid)
    if len(ts.key) > TAG_KEY_WIDTH or len(ts.key) + len(ts.value or b"") > TAG_TEXT_ROOM:
        glass.append("tag id %u's text (%u + %u bytes) is wider than this kernel holds a tag "
                     "(tag_spelling.h)" % (tid, len(ts.key), len(ts.value or b"")))


def DigestsEncode(digests):
    return VarintPut(len(digests)) + b"".join(d.to_bytes(8, "little") for d in digests)


def DigestsDecode(body, what, glass):
    c = Cursor(body)
    out = [c.Fixed("<Q") for _ in range(c.V())]
    _Exact(c, what, glass)
    return out


def InnerEncode(count, ptr):
    return VarintPut(count) + ptr


# ---------------------------------------------------------------------------
# A tree read down from its root
# ---------------------------------------------------------------------------


class Rotten:
    """A range of keys [lo, hi) neither copy of whose node holds: `count` the
    elements under it as its parent says (for FILES, the files); hi None runs
    to the end of the tree."""

    __slots__ = ("tree", "lo", "hi", "count", "level", "ptr", "why")

    def __init__(self, tree, lo, hi, count, level, ptr, why):
        self.tree, self.lo, self.hi, self.count, self.level, self.ptr, self.why = \
            tree, lo, hi, count, level, ptr, why

    def Holds(self, key):
        return self.lo <= key and (self.hi is None or key < self.hi)

    def Touches(self, lo, hi):
        """Does the range meet the keys [lo, hi)?"""
        return self.lo < hi and (self.hi is None or lo < self.hi)


class TreeRead:
    """A tree of one slot, read: every node reached (`nodes`), its leaves'
    items in key order with the seq of the node each lies in (`items`:
    (key, body, seq)), its rotten ranges, the copies that did not hold
    (`mended`: (node, copy, why)), the root that would not read (`refused`),
    and `shape`: what the nodes read say against the format — a lead, or a
    count that is not what lies under it (§2.9: an inner element counts the
    leaf elements under its child, FILES its headers; SpineRoot the tree's).
    `leaves` False walks the inner nodes alone and names the leaves by their
    pointers — what a slot not stood on is read for (§4.5 п.4)."""

    def __init__(self, g, tree, root, glass, leaves=True):
        self.tree, self.nodes, self.items, self.rotten, self.mended, self.shape = tree, [], [], [], [], []
        self.refused = None
        ptr = root.Pointer()
        if ptr is None:
            self.refused = "its root's pointer will not read"
            return
        stack = [(ptr, None, None, root.items, root.level)]
        while stack:
            ptr, lo, hi, count, level = stack.pop()
            if level == 0 and not leaves:
                self.nodes.append(Node(tree, 0, [], count, ptr["sheets"], ptr["blocks"], ptr["seq"]))
                continue
            node, bad = NodeRead(g, ptr, glass, lo, hi)
            if node is not None:
                for k, why in bad:
                    self.mended.append((node, k, why))
            if node is None or node.tree != tree or node.level != level:
                why = ("; ".join("copy %u: %s" % (k + 1, w) for k, w in bad) if node is None else
                       "it is a node of tree %u level %u" % (node.tree, node.level))
                if lo is None and hi is None and not self.nodes:
                    self.refused = why
                    return
                self.rotten.append(Rotten(tree, lo, hi, count, level, ptr, why))
                continue
            node.count, node.lo, node.hi = count, lo, hi
            self.nodes.append(node)
            self.shape.extend(node.shape)
            if level == 0:
                self.items.extend((key, body, node.seq) for key, body in node.items)
                self._Count(node, sum(1 for key, _b in node.items if tree != FILES or key[1] == 0))
                continue
            stack.extend(reversed(self._Kids(node, hi, glass)))

    def _Kids(self, node, hi, glass):
        """The children an inner node names, each with the range of keys its
        separators give it — a pointer that does not read is a rotten range."""
        kids, items = [], node.items
        for i, (key, body) in enumerate(items):
            end = items[i + 1][0] if i + 1 < len(items) else hi
            c = Cursor(body)
            try:
                kc, kp = c.V(), c.Ptr()
            except Short as e:
                self.rotten.append(Rotten(self.tree, key, end, 0, node.level - 1, None, "its pointer: %s" % e))
                continue
            _Exact(c, "an inner item of %s" % TREE_NAMES.get(self.tree, self.tree), glass)
            kids.append((kp, key, end, kc, node.level - 1))
        if len(kids) == len(items):
            self._Count(node, sum(kc for _p, _k, _e, kc, _l in kids))
        return kids

    def _Count(self, node, under):
        if self.tree in TREE_NAMES and under != node.count:
            self.shape.append("the %s node of level %u at block %u counts %u element(s) under it and %s counts "
                              "%u" % (TREE_NAMES[self.tree], node.level, node.blocks[0], under,
                                      "its parent" if node.lo is not None else "its slot's SpineRoot",
                                      node.count))

    def Blocks(self):
        """[(start, count)] of every node reached, both copies."""
        return [(b, n.sheets) for n in self.nodes for b in n.blocks]


# ---------------------------------------------------------------------------
# The Tally a slot names
# ---------------------------------------------------------------------------


# Why the Tally locks a file a DATA run of which lies outside the data run — the
# kernel's words (tally_leaves.c BookFileRun).
OUTSIDE = "it names ground outside the data run"


class Tally:
    """What the Tally of `slot` holds (§4.5 п.3): its content, the trees as
    read, the rotten ranges, the copies to mend, what this kernel cannot
    carry on (`glass`), and what the slot and the nodes say against the
    format's shape (`shape`). `refused` names the authoritative root that
    would not read — the slot is then passed over (§1.4)."""

    def __init__(self, g, slot):
        self.slot, self.seq = slot, slot.tally_seq
        self.trees, self.rotten, self.glass, self.mended, self.shape = {}, [], [], [], list(slot.shape)
        self.locked, self.locked_runs, self.gaps, self.outside, self.refused = {}, {}, [], [], None
        self.content = Content(slot.made_ns)
        self.content.next_fid = slot.counts["next_fid"]
        self.content.next_tag = slot.counts["next_tag"]
        self.content.next_bookmark = slot.counts["next_bookmark"]
        for tree, root in sorted(slot.roots.items()):
            derived = tree in (MAP, SHARED) or root.flags & ROOT_DERIVED
            if derived or tree not in AUTHORITY:
                if not derived:
                    self.glass.append("slot %u names a tree of kind %u this reader does not know, and "
                                      "not one to derive — written by a kernel that knows more"
                                      % (slot.index, tree))
                t = self.trees[tree] = TreeRead(g, tree, root, [], leaves=False)
                self.shape.extend(t.shape)
                continue
            t = self.trees[tree] = TreeRead(g, tree, root, self.glass, leaves=True)
            if t.refused is not None:
                self.refused = "its %s root will not read — %s" % (TREE_NAMES[tree], t.refused)
                return
            self.shape.extend(t.shape)
            self.rotten.extend(t.rotten)
            self.mended.extend(t.mended)
            if tree in (BOOKMARKS, LIEN) and root.items:
                self.glass.append("its %s tree is not empty — a bookmark this kernel cannot hold "
                                  "(§5)" % TREE_NAMES[tree])
        self._Files(g)
        self._Rest()

    def _Files(self, g):
        """The files of FILES: a header and the Parcels that cover its blocks
        in a row. A file a rotten range touches, whose Parcels do not cover
        it, or a DATA run of which lies outside the data run, is locked
        (§4.5 п.3, §2.9) — what of its runs could be read is kept beside the
        lock, for the blocks it names are held; locked by the Tally, not by
        the replay, the volume stays writable (tally_leaves.c BookFileRun)."""
        t = self.trees.get(FILES)
        files, parts, broken = {}, {}, {}
        for (fid, part), body, seq in (t.items if t else []):
            try:
                if part == 0:
                    files[fid] = FileHeadDecode(fid, body, self.glass)
                else:
                    # birth_delta is counted from the seq of the node the
                    # Parcel lies in, not the Tally's: a later Tally names an
                    # unchanged node as it lies (§2.9, §4.1).
                    parts.setdefault(fid, []).append(
                        (part - 1, ParcelDecode(body, seq, "a Parcel of fid %u" % fid, self.glass)))
            except Short as e:
                broken[fid] = "its element (%u, %u) does not read: %s" % (fid, part, e)
        rotten = [r for r in self.rotten if r.tree == FILES]
        for fid in sorted(set(files) | set(parts) | set(broken)):
            known = [r for _b, runs in parts.get(fid, []) for r in runs]
            if any(r.Touches((fid, 0), (fid + 1, 0)) for r in rotten):
                why = "it lies in a rotten range of FILES"
            else:
                why = broken.get(fid) or self._Cover(fid, files.get(fid), parts.get(fid, []), g)
                if why == OUTSIDE:
                    self.outside.append((fid, [(r.start, r.count) for r in known if not r.hole and
                                               (r.start < g.data_block or r.start + r.count > g.data_end)]))
                elif why:
                    self.gaps.append((fid, why))
            if why:
                self.locked[fid], self.locked_runs[fid] = why, known
                self.content.kept[fid] = (files.get(fid), sorted(parts.get(fid, []), key=lambda p: p[0]))
            else:
                self.content.files[fid] = files[fid]

    @staticmethod
    def _Cover(fid, fs, parcels, g):
        """Why the Parcels of `fid` do not cover its blocks [0, ⌈size/4096⌉)
        in a row, one after another from block 0 (§2.9), or name a DATA run
        outside the data run (OUTSIDE) — in the order the kernel reads them,
        Parcel by Parcel — or None."""
        if fs is None:
            return "fid %u has Parcels and no header" % fid
        at = 0
        for b, runs in parcels:
            if b != at:
                return ("fid %u's Parcel from block %u follows its blocks up to %u — %s"
                        % (fid, b, at, "a hole" if b > at else "an overlap"))
            if any(not r.hole and (r.start < g.data_block or r.start + r.count > g.data_end) for r in runs):
                return OUTSIDE
            fs.parcels.append(runs)
            fs.runs.extend(runs)
            at += sum(r.count for r in runs)
        need = (fs.size + BLOCK - 1) // BLOCK
        if at != need:
            return "fid %u's Parcels cover %u blocks and its %u bytes need %u" % (fid, at, fs.size, need)
        return None

    def _Rest(self):
        c = self.content
        for tid, body, _s in (self.trees[TAGS].items if TAGS in self.trees else []):
            try:
                cur = Cursor(body)
                ts = TagDecode(cur, "tag id %u" % tid, self.glass)
                _Exact(cur, "the TAGS element of id %u" % tid, self.glass)
            except Short as e:
                self.glass.append("the TAGS element of id %u does not read (%s)" % (tid, e))
                continue
            TagGlass(tid, ts, self.glass)
            c.tags[tid] = ts
        for _k, body, _s in (self.trees[USE].items if USE in self.trees else [])[:1]:
            cur = Cursor(body)
            try:
                c.use = cur.Take(cur.V())
                _Exact(cur, "the USE element", self.glass)
            except Short as e:
                self.glass.append("the USE element does not read (%s)" % e)
        items = self.trees[DIGESTS].items if DIGESTS in self.trees else []
        for i, (first, body, _s) in enumerate(items):
            try:
                for k, d in enumerate(DigestsDecode(body, "the DIGESTS element at block %u" % first,
                                                    self.glass)):
                    c.digests[first + k] = d
            except Short as e:
                self.rotten.append(Rotten(DIGESTS, first, items[i + 1][0] if i + 1 < len(items) else None,
                                          0, 0, None, "its element does not read: %s" % e))
        for fs in c.files.values():
            for r in fs.runs:
                if not RunCarried(r):
                    self.glass.append("a run of fid %u carries an extension this kernel cannot carry "
                                      "on (CODEC, SHARED or COPY, §8)" % fs.fid)
                    break

    def Nodes(self):
        return [n for t in self.trees.values() for n in t.nodes]


# ---------------------------------------------------------------------------
# X3: the Tally's numbers, and its golden node and slot, against the header
# ---------------------------------------------------------------------------


class _GoldenGround:
    """Where volume_tally.h seals its golden node and slot: the volume
    00 01 .. 0F, its Spine ring from block 256, 256 blocks apart."""
    uuid = bookread.GOLDEN_UUID
    frame = {"spine_block": 256, "spine_stride": 256}


def GoldenNode():
    """volume_tally.h's golden node, by tally.py's own encoders: a FILES leaf
    of seq 1 at block 0x2000, one Sheet — three files, two of them with a
    Parcel, the last run of which was born at 0 and carries an extension."""
    ext = bytes([0x02, 0x01, 0x07])
    items = [((1, 0), FileHeadEncode(FileState(1, 1, ondisk.FILE_ACTIVE, 5000, 1700000000000000000,
                                               1700000000000000007, b"golden", [3, 5, 300], []))),
             ((1, 1), ParcelEncode([Run(False, 1024, 2, 1)], 1)),
             ((2, 0), FileHeadEncode(FileState(2, 9, ondisk.FILE_ACTIVE | ondisk.FILE_LAID_BIT, 0, 1, 0, b"", [],
                                               []))),
             ((3, 0), FileHeadEncode(FileState(3, 1, ondisk.FILE_ACTIVE, 9000, 2, 2, b"three", [0], []))),
             ((3, 1), ParcelEncode([Run(True, 0, 1, 1), Run(False, 4096, 1, 1), Run(False, 10, 1, 0, ext)], 1))]
    stream, starts = NodeStream(FILES, 0, items)
    return b"".join(NodeSectors(_GoldenGround, 1, 0x2000, 1, stream, starts))


def GoldenSlot():
    """volume_tally.h's golden slot, by tally.py's own slot writer: slot 0 at
    seq 2, every kind of record a slot carries, MEDIUM_LIED among them."""
    slot = Slot(0, None)
    slot.tally_seq, slot.stitch_seq, slot.stitch_clasp = 5, 9, 0xC0FFEE11
    slot.made_ns = 1700000000123456789
    slot.roots = {FILES: Root(0, 1, 70, bookread.PtrPut(5, 1, [2048, 2304])),
                  TAGS: Root(0, 0, 40, bookread.PtrPut(5, 1, [2049, 2305])),
                  DIGESTS: Root(0, 1, 15000, bookread.PtrPut(3, 2, [1500, 1756]))}
    slot.ribbon = dict(at=2600, quire_block=2560, quire_blocks=256, next_block=2816, next_blocks=256)
    slot.counts = dict(next_fid=71, next_tag=52, next_bookmark=1)
    slot.flags = SPINE_MEDIUM_LIED
    return b"".join(SlotSectors(_GoldenGround, 0, 2, slot))


def TallyCheck(defines):
    """What is wrong here against volume_tally.h — its numbers read out of it
    (`defines`) — and how many things were held: every kind and bit this
    module writes and reads by, and the sums of the golden node and slot
    reached by this module's own encoders, as the kernel's book suite
    reaches them by the header's."""
    wrong, n = [], 0
    for py, c in ((FILES, "TALLY_FILES"), (TAGS, "TALLY_TAGS"), (USE, "TALLY_USE"),
                  (BOOKMARKS, "TALLY_BOOKMARKS"), (LIEN, "TALLY_LIEN"), (DIGESTS, "TALLY_DIGESTS"),
                  (MAP, "TALLY_MAP"), (SHARED, "TALLY_SHARED"), (ROOT_DERIVED, "TALLY_ROOT_DERIVED"),
                  (SPINE_HEAD, "SPINE_HEAD"), (SPINE_ROOT, "SPINE_ROOT"), (SPINE_RIBBON, "SPINE_RIBBON"),
                  (SPINE_COUNTS, "SPINE_COUNTS"), (SPINE_FLAGS, "SPINE_FLAGS"),
                  (SPINE_MEDIUM_LIED, "SPINE_MEDIUM_LIED"), (NODE_HEAD, "NODE_HEAD"), (RUN_HOLE, "RUN_HOLE"),
                  (RUN_EXT, "RUN_EXT"), (TAG_HAS_VALUE, "TALLY_TAG_HAS_VALUE"), (TAG_RETIRED, "TALLY_TAG_RETIRED"),
                  (ondisk.FILE_ACTIVE, "TALLY_FILE_ACTIVE"),
                  (ondisk.FILE_UNDONE, "TALLY_FILE_UNDONE"), (ondisk.FILE_DELETED, "TALLY_FILE_DELETED"),
                  (ondisk.FILE_ROLE_MASK, "TALLY_FILE_ROLE_MASK"),
                  (ondisk.FILE_ROLE_SYSTEM, "TALLY_FILE_ROLE_SYSTEM"), (ondisk.FILE_ROLE_BOOT, "TALLY_FILE_ROLE_BOOT"),
                  (ondisk.FILE_ROLE_KERNEL, "TALLY_FILE_ROLE_KERNEL"), (SHEET_PAYLOAD, "TALLY_SHEET_PAYLOAD")):
        n += 1
        if defines.get(c) != py:
            wrong.append("%s: volume_tally.h says %r and tally.py %r" % (c, defines.get(c), py))
    for name, sectors, c in (("the golden node", GoldenNode(), "TALLY_GOLDEN_NODE_CRC"),
                             ("the golden slot", GoldenSlot(), "TALLY_GOLDEN_SLOT_CRC")):
        n += 1
        have = bookread.Crc32c(sectors)
        if have != defines.get(c):
            wrong.append("%s's sum: 0x%08X where %s is %s" % (
                name, have, c, "0x%08X" % defines[c] if defines.get(c) is not None else "missing"))
    return wrong, n


