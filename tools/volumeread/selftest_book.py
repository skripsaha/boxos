"""selftest_book — format v2's landings: a book laid over the image, one state
or one kind of damage planted in it, and what the book's census and mount must
say of it.

An image self-test is handed in format v1 (`make VOLUME_FORMAT=v1`; mkfs lays
v2 by default since step 2.4) has the book laid here first, over it in memory, as
`create_tagfs --frame-only` and then a binder would lay it: the Deed made
again with FRAME in place of LAYOUT, the front grains blank, every live file
of the v1 image in a row from the data run's first block with the kernel first
and its bytes copied there, and then the Tally of seq 1 in two Tally-Quires,
slot 0 at seq 2, an empty ribbon Quire and its next_quire (binder.Bind) — the
book of exactly the files the v1 image holds, which its census must find
consistent with nothing to say (B0). Every landing is a fork of that overlay;
the image file is never written. A format-v2 image handed to self-test is its
own base.

Between the files and the Tally the base keeps LANDING_ROOM blocks free — the
landings' own files go there (F) — so where the Tally lands, and so whether a
landing has room, does not hang on how many blocks the image's files happen
to take. An image with no room for the files, that room and the book is
refused in one sentence, never landed on.

`--emit CASE --out IMG` writes a landing out as a new image: the kernel that
reads the book (step 2.3f) boots them and is held to this package's
prediction, as the census scenario holds the v1 mount.
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
# The base: the v1 image's files, laid as format v2 lays them, and bound
# ---------------------------------------------------------------------------


def FrameDeed(deed, frame, boot, role, frame_bytes=ondisk.VOLUME_FRAME.size):
    """A Deed of eight sectors as FrameOnlyDeed writes one (create_tagfs_v2.c):
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


def _InRow(c0):
    """The live files in the order mkfs lays them: the BOOT file first, then
    as the v1 image has them, by their first block."""
    live = [e for e in c0.files.values() if e["state"] == "live"]
    return sorted(live, key=lambda e: ("fid %u '%s'" % (e["fid"], e["record"].name) != c0.boot_file,
                                       e["record"].extents_at_mount[0][0]
                                       if e["record"].extents_at_mount else 0, e["fid"]))


def _V1Bytes(f, rec, blocks):
    """The first `blocks` blocks of a v1 file, read through its extents."""
    out = []
    for s, n in rec.extents_at_mount:
        for b in range(s, s + n):
            if len(out) == blocks:
                return out
            raw = f.vread(f.layout["data_block"] + b)
            if raw is None:
                raise Unreadable("block %u of %s will not read" % (b, rec.name))
            out.append(raw)
    if len(out) < blocks:
        raise Unreadable("%s names fewer blocks than its size needs" % rec.name)
    return out


def BookBase(t):
    """(overlay, content, plan): the book of the v1 image's files (see the
    module's account). The data run from block 1024 = 256 * (1 + 3) on a
    ground of 1 MiB grains: the Deed's grain and one grain to each of the
    three Spine slots, the stride a grain (§1.2: grain_bytes / block_bytes,
    the volume's own GEOMETRY)."""
    c0 = t.c0
    f, medium = c0.f, c0.medium
    over = OverlayMedium(medium)
    base, uuid = f.base, f.deed["uuid"]
    grain = f.deed["geometry"]["grain_bytes"] // BLOCK
    frame = dict(data_block=grain * (1 + ondisk.FRAME_SPINE_SLOTS), spine_block=grain,
                 spine_slots=ondisk.FRAME_SPINE_SLOTS, spine_stride=grain)
    need = sum(max(1, (e["record"].size + BLOCK - 1) // BLOCK) for e in _InRow(c0))
    end = f.deed["tail_sector"] // SECTORS_PER_BLOCK
    if frame["data_block"] + need + LANDING_ROOM > end:
        raise ValueError("its files take %u blocks from block %u, and with the landings' %u the book's data "
                         "run would reach block %u, past its end at block %u"
                         % (need, frame["data_block"], LANDING_ROOM, frame["data_block"] + need + LANDING_ROOM,
                            end))
    over.Put(base + SECTORS_PER_BLOCK, bytes((frame["data_block"] - 1) * BLOCK))
    lanes = bookread.SeedLanes(uuid)
    content = tally.Content(made_ns=f.deed["born"]["created_unix"] * 10**9)
    at, boot = frame["data_block"], None
    for e in _InRow(c0):
        rec = e["record"]
        need = (rec.size + BLOCK - 1) // BLOCK
        blocks = _V1Bytes(f, rec, need)
        over.Put(base + at * SECTORS_PER_BLOCK, b"".join(blocks) + bytes(BLOCK * (need == 0)))
        runs = [tally.Run(False, at, need, 1)] if need else []
        content.files[e["fid"]] = tally.FileState(
            e["fid"], gen=1, flags=rec.flags, size=rec.size, created_ns=rec.created * 10**9,
            modified_ns=rec.modified * 10**9, name=rec.name.encode(), tags=sorted(set(rec.tags)),
            runs=runs)
        for i, raw in enumerate(blocks):
            content.digests[at + i] = bookread.Integrity(raw, lanes)
        if boot is None and "fid %u '%s'" % (e["fid"], rec.name) == c0.boot_file:
            boot = (at, max(need, 1), rec.size, 0)
        at += max(need, 1)
    for tid, (key, value) in c0.by_id.items():
        content.tags[tid] = tally.TagState(tally.TAG_HAS_VALUE if value is not None else 0, key, value)
    content.next_fid = max(content.files) + 1
    content.next_tag = max(content.tags) + 1 if content.tags else 0
    content.next_bookmark = 1
    boot = boot or (0, 0, 0, 0)
    for role, vlba in ((ondisk.DEED_ROLE_HEAD, 0), (ondisk.DEED_ROLE_TAIL, f.deed["tail_sector"])):
        over.Put(base + vlba, FrameDeed(f.deed, frame, boot, role))
    ground = BookGround(over)
    plan = binder.Bind(content, ground, after=at + LANDING_ROOM)
    binder.BindWrite(over, ground, plan)
    return over, content, plan


class BookTargets:
    """What the landings aim at, off the base and its census (B0).

    S — the live file of fewest blocks (at least one), lowest fid first, that
        is not the BOOT file; L — the live file of most blocks other than S;
    F — the first free run of the data run of LANDING_ROOM blocks or more
        that the Tally-Quires do not take (a landing grows the Tally there),
        where a landing's new files put their bytes; fid, tag — the next file
        id and tag id the book has not given.

    The base is the book laid over a format-v1 image's files (`t`, its
    Targets), or a format-v2 image's own (`medium`), its Tally read back as
    the plan the landings aim at."""

    def __init__(self, t=None, medium=None):
        self.t = t
        if t is not None:
            self.base, self.content, self.plan = BookBase(t)
        else:
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
