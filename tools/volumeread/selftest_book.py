"""selftest_book — the book's landings: one state or one kind of damage planted
in the image's own book, and what the book's census and mount must say of it.

Every landing is a fork of an overlay of the image (the image file is never
written), its Tally read back as the plan the landings aim at (BookTargets):
the base its census must find consistent with nothing to say (B0). The
landings' own files go in the first free run of LANDING_ROOM blocks past the
Tally-Quires (F); an image with no such run is refused in one sentence, never
landed on.

`--emit CASE --out IMG` writes a landing out as a new image: the kernel that
reads the book boots them and is held to this package's prediction.
"""

import struct

from . import binder, bookread, ondisk, stitch, stitch_pen, tally
from .bookcensus import BookCensus
from .bookground import BookGround
from .medium import OverlayMedium, Unreadable
from .ondisk import SECTOR, BLOCK, SECTORS_PER_BLOCK

PADDING = 40                      # files a landing adds to give FILES a second leaf
LANDING_ROOM = PADDING            # blocks the landings' files take at most: PADDING, and no more for Fresh


# ---------------------------------------------------------------------------
# A landing, and how its census is held to it
# ---------------------------------------------------------------------------


class Case:
    """A plant, the codes it must raise, the other damage or leak codes it may
    raise, and the facts about the census and the mount that must hold."""

    def __init__(self, name, plant, must=(), may=(), checks=()):
        self.name, self.plant = name, plant
        self.must, self.may, self.checks = set(must), set(may), list(checks)


def Judge(case, census):
    """(ok, flagged codes, what went wrong)."""
    codes = {x.code for x in census.findings}
    bad = {x.code for x in census.findings if x.severity in ("damage", "leak")}
    wrong = []
    if not case.must <= codes:
        wrong.append("missing %s" % ", ".join(sorted(case.must - codes)))
    extra = bad - case.must - case.may
    if extra:
        wrong.append("unexpected %s" % ", ".join(sorted(extra)))
    for what, check in case.checks:
        try:
            held = check(census)
        except (KeyError, IndexError, TypeError, AttributeError):
            held = False
        if not held:
            wrong.append("not so: " + what)
    return not wrong, sorted(codes), wrong


# ---------------------------------------------------------------------------
# The base: the image's own book
# ---------------------------------------------------------------------------


def FrameDeed(deed, frame, boot, role, frame_bytes=ondisk.VOLUME_FRAME.size):
    """A Deed of eight sectors as FrameOnlyDeed writes one (create_tagfs_book.c):
    GEOMETRY · FRAME · BORN · BOOT · PACKED, the identity and the extent of
    `deed`, sealed. `frame_bytes` past 24 is a FRAME a kernel that knows more
    wrote."""
    payload = ondisk.VOLUME_FRAME.struct.pack(*(frame[k] for k in ondisk.VOLUME_FRAME.names))
    stamps = b""
    for kind, body in ((ondisk.STAMP_GEOMETRY, deed["stamps"][ondisk.STAMP_GEOMETRY]),
                       (ondisk.STAMP_FRAME, payload.ljust(frame_bytes, b"\0")),
                       (ondisk.STAMP_BORN, deed["stamps"][ondisk.STAMP_BORN]),
                       (ondisk.STAMP_BOOT, ondisk.VOLUME_BOOT.struct.pack(*boot)),
                       (ondisk.STAMP_PACKED, deed["stamps"][ondisk.STAMP_PACKED])):
        stamps += struct.pack("<HH", kind, len(body)) + body
        stamps = stamps.ljust((len(stamps) + 3) & ~3, b"\0")
    head = ondisk.VOLUME_DEED.struct.pack(ondisk.DEED_MAGIC, ondisk.VOLUME_DEED.size, len(stamps), 0,
                                          deed["uuid"], deed["sectors"], deed["tail_sector"], role)
    return ondisk.DeedSeal((head + stamps).ljust(ondisk.DEED_SECTORS * SECTOR, b"\0"))


class BookTargets:
    """What the landings aim at, off the base and its census (B0).

    S — the live file of fewest blocks (at least one), lowest fid first, that
        is not the BOOT file; L — the live file of most blocks other than S;
    F — the first free run of the data run of LANDING_ROOM blocks or more
        that the Tally-Quires do not take (a landing grows the Tally there),
        where a landing's new files put their bytes; fid, tag — the next file
        id and tag id the book has not given.

    The base is the image's own book (`medium`), its Tally read back as the
    plan the landings aim at."""

    def __init__(self, medium):
        self.base = OverlayMedium(medium)
        m = BookCensus(self.base).mount
        if m.tally is None:
            raise Unreadable("the book of the image does not mount: %s" % m.refusal)
        self.content, self.plan = m.tally.content, binder.ReadPlan(m.tally)
        self.c0 = BookCensus(self.base)
        self.ground = BookGround(self.base)
        self.lanes = bookread.SeedLanes(self.ground.uuid)
        boot = self.c0.boot_fid
        live = [fs for fs in self.content.files.values() if fs.Blocks() >= 1 and fs.fid != boot]
        if len(live) < 2:
            raise Unreadable("the image has no two files a landing can aim at")
        self.S = min(live, key=lambda fs: (fs.Blocks(), fs.fid))
        self.L = min((fs for fs in live if fs is not self.S), key=lambda fs: (-fs.Blocks(), fs.fid))
        quires = [(self.plan.a, 2 * (self.plan.b - self.plan.a))]
        self.F = next((s for s, n in ondisk.RunsSubtract(self.c0.free_runs, quires) if n >= LANDING_ROOM), None)
        if self.F is None:
            raise Unreadable("the image has no free run of %u blocks past the Tally-Quires for the landings' "
                             "files" % LANDING_ROOM)
        self.fid, self.tag = self.content.next_fid, self.content.next_tag
        self.maxfid = max(self.content.files)

    def Fork(self):
        o = OverlayMedium(self.base.base)
        o.over = dict(self.base.over)
        return o

    def Pen(self, o):
        return stitch_pen.StitchPen(o, BookGround(o))

    def PutBlock(self, o, b, data):
        o.Put(self.ground.base + b * SECTORS_PER_BLOCK, bytes(data).ljust(BLOCK, b"\0"))

    @staticmethod
    def Pattern(seed):
        return bytes(((i * 31 + seed) & 0xFF) for i in range(BLOCK))

    def Fresh(self, o, b, n, seed):
        """`n` blocks of a pattern at F + b, and their digests."""
        data = [self.Pattern(seed + k) for k in range(n)]
        for k, raw in enumerate(data):
            self.PutBlock(o, self.F + b + k, raw)
        return [bookread.Integrity(raw, self.lanes) for raw in data]

    def Rot(self, o, vlba, at=100):
        """A byte of the sector at `vlba` flipped: its sum no longer holds."""
        lba = self.ground.base + vlba
        raw = bytearray(o.sectors(lba, 1))
        raw[at] ^= 0x5A
        o.Put(lba, bytes(raw))

    def BumpLead(self, o, vlba):
        """The sector at `vlba` sealed again with a lead one past its own:
        whole, and lying about where its first record begins (§2.1)."""
        lba = self.ground.base + vlba
        raw = bytearray(o.sectors(lba, 1))
        lead = struct.unpack_from("<H", raw, 4)[0]
        struct.pack_into("<H", raw, 4, lead + 1 if lead else bookread.HEAD_BYTES)
        o.Put(lba, bookread.SectorSeal(bytes(raw), self.ground.uuid, vlba))

    def Zero(self, o, vlba, count):
        o.Put(self.ground.base + vlba, bytes(count * SECTOR))

    def Node(self, tree, level=0, index=-1):
        nodes = [n for n in self.plan.nodes if n.tree == tree and n.level == level]
        if not nodes or (index != -1 and index >= len(nodes)):
            raise Unreadable("the tally has no %s node of level %u numbered %d"
                             % (tally.TREE_NAMES[tree], level, index))
        return nodes[index]

    def RotNode(self, o, node, copies=(0, 1)):
        for c in copies:
            self.Rot(o, node.blocks[c] * SECTORS_PER_BLOCK)

    def Reitem(self, o, node, edit):
        """`node` written again in place, both copies, with the same seq: its
        items through `edit(list of (key, body))`. A pointer checks the seq,
        not what the node says (§2.5)."""
        items = edit(list(node.items))
        binder.NodeWrite(o, self.ground, self.plan.seq, node.blocks, node.sheets, node.tree, node.level,
                        items)


def Label(fs):
    return "fid %u '%s'" % (fs.fid, fs.name.decode("utf-8", "replace"))


def Glass(c, *words):
    """The mount stands under glass, every one of `words` in one reason."""
    return any(all(w in g for w in words) for g in c.mount.glass)


def Errata(c, fid, why):
    """The mount's verdict rolls `fid` back for `why` (1 NOT_LANDED, 2 the
    rule of rot) — held in memory for the session's first stitch to lay down
    as an Undone, and no reason of glass says it (§16.5 п.4)."""
    return any(w == why and fid in fids for _s, w, fids in c.mount.errata) and not Glass(c, "roll")


def Found(c, code, *words):
    return any(x.code == code and all(w in x.text for w in words) for x in c.findings)


def Born(b, fs, digests, name, size=None):
    """A Born of a new file `fs` (fid) whose one DATA run is at F + b."""
    size = BLOCK * len(digests) if size is None else size
    return stitch.Born(fid=fs, flags=ondisk.FILE_ACTIVE, size=size, created_ns=1, modified_ns=1,
                       name=name, tags=(), runs=[stitch.Data(b, len(digests))], digests=digests)
