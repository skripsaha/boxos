"""bookcensus — the roll call of a volume (spec_v2 §4.6): every owner of
every block, set against the book's mount (bookmount.py).

A volume keeps no bitmap: which blocks are taken is derived from
the names that take them. So the census here is the derivation itself — the
front of the volume before its data run, the Spine's slots, every node of the
Tally of every slot that holds (every copy), the ribbon's Quires from the
earliest slot's `at` to the Quire the ribbon ends in and its next_quire, every
DATA run of every live file, BOOT beside the kernel file it is, the far copy
of the Deed, the blocks held for a rotten range (their own class: held, never
an orphan) and the ground the ribbon let go, lying fallow (M-1). Two owners on
one block are named where they meet. What none of them names is free; there
is no third state, so there is no orphan to find. A bookmark is the one owner
left out: the nodes of its trees and the runs under its Lien (§4.6) neither
this census nor the kernel's derives, so where a Tally names one, what is
free is not counted at all (BookmarksNamed) rather than counted too high.

And beside the account: every finding of the mount, each data block of a
live file held to its digest, and what the kernel of this tree makes of the
volume (KernelBook) — it reads the Spine, the Tally and the ribbon sewn past
it, and sews its own stitches past them where the book holds nothing its
writer could not carry on. The census scenario boots a landing and holds the
kernel's own lines to that.

The owners are the kernel's own list (src/include/ground_owner.h), generated
into ground_owners.py: BOOK_OWNERS.
"""

import hashlib
import json
import os
import re
import struct

from . import bookread, stitch, tally
from .bookmount import BookMount, Blocks
from .bookground import BookGround
from .ground_owners import BOOK_OWNERS, BOOK_OWNER_ALONE
from .ondisk import BLOCK, SECTORS_PER_BLOCK, RunsCount, RunsIntersect, RunsNormal, RunsSubtract, RunsText

SEVERITY = ("damage", "leak", "note")        # exit 1 on any damage or leak


class Finding:
    """One statement about the volume: a stable code, a severity, one sentence
    with the runtime numbers in it, and the blocks it is about as runs."""

    __slots__ = ("code", "severity", "text", "runs")

    def __init__(self, code, severity, text, runs=()):
        self.code, self.severity, self.text, self.runs = code, severity, text, list(runs)


class GroundClaim:
    """One owner's claim on the data run. An alias (the BOOT run) is not
    exclusive: it may coincide with its partner — the one file it is — and
    with nothing else; with no partner found, with any file."""

    __slots__ = ("kind", "label", "runs", "exclusive", "partner")

    def __init__(self, kind, label, runs, exclusive=True, partner=None):
        self.kind, self.label, self.runs = kind, label, RunsNormal(runs)
        self.exclusive, self.partner = exclusive, partner

    def Allows(self, other):
        if self.partner is not None:
            return other.label == self.partner
        return other.kind == "file"


# ---------------------------------------------------------------------------
# Who owns what, piece by piece
# ---------------------------------------------------------------------------


def Sweep(claims):
    """[(start, count, [claims])]: the data run cut where the set of claims
    changes, keeping the pieces that somebody claims."""
    events = []
    for c in claims:
        for s, n in c.runs:
            events.append((s, 1, c))
            events.append((s + n, -1, c))
    events.sort(key=lambda e: (e[0], e[1]))
    active, segs, prev = {}, [], None
    for pos, delta, c in events:
        if prev is not None and pos > prev and active:
            segs.append((prev, pos - prev, list(active)))
        active[c] = active.get(c, 0) + delta
        if not active[c]:
            del active[c]
        prev = pos
    return segs


# A block's digest by its bytes: the check is the same, the WyHash of a block
# met before is not taken again (every landing of the self-test reads the
# same files).
_DIGESTS = {}


def BlockDigest(raw, lanes):
    key = (lanes[0], hashlib.sha1(raw).digest())
    got = _DIGESTS.get(key)
    if got is None:
        got = _DIGESTS[key] = bookread.Integrity(raw, lanes)
    return got


# PowerCut checks thousands of cuts of one record, each census in a process of
# its own, and nearly every block each holds to its digest is one every other
# holds too — the base's files. WyHash in Python is nine tenths of such a
# census (race's record: 1.36 s of 1.52), so a run takes each block's once and
# hands the memo on (volume-read cut --digests-out, --digests). Keyed as above,
# by the bytes and the seed's lane, a memo of another volume or another run
# names nothing a census meets: it never stands in for a digest it was not.
def DigestMemoWrite(path):
    """The digests this process took, written whole to a sibling and renamed:
    a reader never meets half of a memo."""
    tmp = "%s.%d" % (path, os.getpid())
    with open(tmp, "w") as fh:
        json.dump([[lane, key.hex(), got] for (lane, key), got in _DIGESTS.items()], fh)
    os.replace(tmp, path)


def DigestMemoRead(path):
    """A memo DigestMemoWrite kept, taken as this process's own."""
    with open(path) as fh:
        for lane, key, got in json.load(fh):
            _DIGESTS[(lane, bytes.fromhex(key))] = got


# The copies of a node that did not hold the kernel names one line each before
# it says the rest by their count (mount/tally_read_internal.h TALLY_COPIES_SAID).
KERNEL_COPIES_SAID = 4


def BookmarksNamed(m):
    """Does the Tally of a slot that holds name a bookmark — a BOOKMARKS or a
    LIEN tree not empty? The ground a bookmark holds (§4.6: the nodes of its
    trees, the runs under its Lien) neither this census nor the kernel's
    derives, so neither counts what is free (book_ground.c)."""
    return any(s.state == "valid" and any(tree in (tally.BOOKMARKS, tally.LIEN) and root.items
                                          for tree, root in s.roots.items()) for s in m.slots)


def KernelMendedSaid(t):
    """The lines the kernel says of the copies that did not hold: one each
    for the first KERNEL_COPIES_SAID of a node, the rest in one line."""
    failed = {}
    for node, _k, _why in t.mended:
        failed[id(node)] = failed.get(id(node), 0) + 1
    return sum(min(n, KERNEL_COPIES_SAID) for n in failed.values())


# What settled is lost, counted at the most, and the largest seq found past
# the end, as the ribbon's line says them (stitch_ribbon.py _Lost, ribbon_scan.c
# RibbonLost; §3.9, C17): the census holds the kernel's line to both numbers.
LOST_SAID = re.compile(r"what settled is lost: at most (\d+) stitch\(es\).*no seq up to (\d+), the largest found")


def KernelLostSaid(said):
    """[[stitches lost, the largest seq found]] of each line of the ribbon
    that counts what settled is lost, in the order it is said."""
    return [[int(x) for x in m.groups()] for code, _s, text in said if code == "DEAD_BRANCH"
            for m in [LOST_SAID.search(text)] if m]


def MirrorRuns(runs):
    """A file's runs as the print names them, each by its kind — 0 DATA, 1 a
    hole, 2 SHARED (book_ground.c BookFilePrinted): each run that goes on from
    the one before it — two holes, or ground of one kind where the last
    ended — one with it."""
    out = []
    for r in runs:
        kind = 1 if r.hole else 2 if tally.RunShared(r) else 0
        if out and out[-1][0] == kind and (r.hole or out[-1][1] + out[-1][2] == r.start):
            out[-1][2] += r.count
        else:
            out.append([kind, 0 if r.hole else r.start, r.count])
    return out


def MirrorPrint(files):
    """The print a mount gives of the live files it holds (book_mount.c
    BookMirrorPrint) — CRC32C over each in the order of its fid: le64 fid ·
    le64 size · le32 name bytes · the name · le32 tags · le32 each tag id
    the kernel holds (to TAGFS_MAX_TAG_ID), rising · each run (MirrorRuns)
    as u8 kind · le64 start · le64 blocks · le32 runs."""
    out = bytearray()
    for fid in sorted(files):
        fs = files[fid]
        tags = sorted(t for t in set(fs.tags) if t < tally.KERNEL_TAG_END - 1)
        runs = MirrorRuns(fs.runs)
        out += struct.pack("<QQI", fid, fs.size, len(fs.name)) + fs.name
        out += struct.pack("<I", len(tags)) + b"".join(struct.pack("<I", t) for t in tags)
        out += b"".join(struct.pack("<BQQ", h, st, n) for h, st, n in runs) + struct.pack("<I", len(runs))
    return bookread.Crc32c(bytes(out))


def KernelBook(g, m, held, free, fallow):
    """What the kernel of this tree makes of a format-v2 volume, in the
    numbers its own lines carry (mount/book_mount.c): refused — and in which
    words: first a book on ground the kernel's own survey of the medium does
    not take, which is no volume it mounts at all (tagfs.c volume_take_ground,
    GroundSurvey) — or mounted, for how many reasons only to be read (the book's own,
    and a Tally that names a bookmark, whose ground no writer of this kernel
    takes without its map; or a ribbon whose end the walk does not know, which
    no writer goes on from), standing on which slot, with how many
    files live, which locked by fid, what ranges rotten, which read from an
    older slot in its place (Facsimile) and from which, which copies read
    in the other's place, how many blocks held, how many shapes said, the
    numbers it gives next, and its data run with the blocks of it its block
    map derives free (block_map_derived.c) — `free`, the census's own, None
    where neither counts it (BookmarksNamed) — less a block promised to each
    live file whose tail a Slip lays (block_map.h, AllotmentFreeUnpromised;
    §16.6 п.1). It reads no DIGESTS leaf at the
    mount unless a FILES range rots (§16.3 п.12): a DIGESTS range is said
    rotten only then.

    And the ribbon past the Tally (mount/ribbon_collate.c, ribbon_transcribe.c,
    ribbon_verify.c): every line the mount says of it from the walk on, by its
    census code — the kernel says each as `ribbon CODE:` — the stitches sewn
    on, the last of them and how many applied, whether the medium is shown to
    have lied, the blocks the ribbon let go lying fallow (`fallow`, the
    census's claim), whether the last stitch says CLEAN — a halt's — the
    next seq past every sector of the book read, and on
    a medium that lied the fresh blocks of the newest stitches it reads
    before the ribbon is replayed, and how many stitches (`lied_check`);
    and what its mirror holds when the mount is done — the print of its live
    files (MirrorPrint) and of its Use Context, and the bytes of the latter."""
    st = g.stand
    if not st.kernels:
        return {"mounts": False, "refusal": "%s, and this volume was found at %s" % (st.kernel_why, st.how)}
    if g.block_bytes != BLOCK:
        return {"mounts": False, "refusal": "laid out in %u-byte blocks" % g.block_bytes}
    if g.frame_why:
        return {"mounts": False, "refusal": "bears a FRAME that does not hold"}
    if m.refusal is not None:
        return {"mounts": False, "refusal": ("none of the %u slots of its Spine holds" % len(m.slots)
                                             if m.tally is None and not any(s.state == "valid" for s in m.slots)
                                             else "no slot that holds names a Tally that reads")}
    t, rot, r = m.tally, m.FilesRot(), m.ribbon
    said, ribbon = m.said[:m.ribbon_said], {}
    for code, _s, _t in m.said[m.ribbon_said:]:
        ribbon[code] = ribbon.get(code, 0) + 1
    return {"mounts": True, "refusal": None, "slot": m.slot.index, "files": len(m.files),
            "locked": sorted(m.locked),
            "glass": len(m.glass) + (free is None) + (r is not None and r.end is None),
            "rotten": [{"tree": tally.TREE_NAMES[x.tree], "count": x.count} for x in t.rotten
                       if x.tree != tally.DIGESTS or rot],
            "facsimile": sorted([tally.TREE_NAMES[tree], index] for tree, index in m.facsimile),
            "mended": len(t.mended), "mended_said": KernelMendedSaid(t), "held": RunsCount(held),
            "shape": sum(1 for code, _s, _t in said if code == "RECORD_SHAPE"),
            "torn_slots": sum(1 for code, _s, _t in said if code == "SLOT_TORN"),
            "fell_back": sum(1 for code, _s, _t in said if code == "SLOT_FELL_BACK"),
            "next_fid": m.counts["next_fid"], "next_tag": m.counts["next_tag"],
            "next_bookmark": m.counts["next_bookmark"], "next_seq": m.counts["next_seq"],
            "data_blocks": g.data_end - g.data_block,
            "free": None if free is None else max(0, RunsCount(free) - sum(1 for fs in m.files.values() if fs.slips)),
            "ribbon": ribbon, "lost": KernelLostSaid(m.said[m.ribbon_said:]), "sewn": len(r.stitches),
            "last": r.stitches[-1].seq if r.stitches else m.slot.stitch_seq,
            "applied": len(m.applied), "lied": r.lied, "lied_check": m.lied_check, "fallow": RunsCount(fallow),
            "clean": bool(r.stitches and r.stitches[-1].head and r.stitches[-1].head.flags & stitch.CLEAN),
            "files_print": MirrorPrint(m.files), "use_print": bookread.Crc32c(m.use or b""),
            "use_bytes": len(m.use or b"")}


class BookCensus:
    """Every owner of every block of a volume, its mount's findings, and the
    account: the findings, what the kernel would make of it, every file."""

    def __init__(self, medium, forced_start=None):
        self.medium = medium
        self.g = g = BookGround(medium, forced_start)
        self.findings, self.claims, self.held, self.boot_fid = [], [], [], None
        self._Deed()
        self.mount = m = BookMount(g)
        for code, severity, text in m.said:
            self.Say(code, severity, text)
        for why in m.glass:
            self.Say("UNDER_GLASS", "note", "the volume may only be read: %s" % why)
        self._Claims()
        self._Held()
        self._Quires()
        self._Sweep()
        if m.refusal is None:
            self._Boot()
            self._Digests()
        self.free_runs = RunsSubtract([(g.data_block, g.data_end - g.data_block)],
                                      RunsNormal([r for c in self.claims for r in c.runs]))
        self.counted = m.tally is None or not BookmarksNamed(m)
        fallow = [r for c in self.claims if c.kind == "fallow" for r in c.runs]
        self.kernel = KernelBook(g, m, self.held, self.free_runs if self.counted else None, fallow)
        self.kernel["slipped"] = [{"fid": fid, "name": fs.name.decode("utf-8", "replace"),
                                   "content": self.Content(fid).decode("latin-1"),
                                   "at": (Blocks(fs.size) - 1) * BLOCK + min(off for off, _d in fs.slips)}
                                  for fid, fs in sorted(m.files.items()) if fs.slips and self.Content(fid) is not None]
        self.kernel["double"] = sorted([s, n] for x in self.findings if x.code == "DOUBLE_OWNER" for s, n in x.runs)

    def Say(self, code, severity, text, runs=()):
        self.findings.append(Finding(code, severity, text, runs))

    def Claim(self, kind, label, runs, partner=None):
        c = GroundClaim(kind, label, RunsIntersect(RunsNormal(runs), [(0, self.g.data_end + 1)]),
                        BOOK_OWNER_ALONE[kind], partner)
        if c.runs:
            self.claims.append(c)
        return c

    def Named(self, kind, label, names):
        """One owner's claim made of its names, (start, count, who, slot): a
        node's copy by the node's seq and the slot whose Tally names it, a
        Quire by the slot whose ribbon goes on in it, a run of a file by its
        fid and no slot (None). One name given alike by two slots is one
        owner — an unchanged node two Tallies name (§2.9), a Quire two
        ribbons go on in — as many times over as the slot that gives it most;
        any other two names that meet on a block are two owners of it, said:
        two nodes of one Tally on one block, two runs of one file, two Quires
        apart. The kernel's census keeps the same rule (ground_census.c
        RunsOneName), and two owners of one kind are found nowhere else: the
        claim merges its runs."""
        groups = {}
        for start, count, who, slot in names:
            groups.setdefault((start, count, who), []).append(slot)
        kept = []
        for (start, count, _who), slots in groups.items():
            most = max([slots.count(x) for x in set(slots) if x is not None], default=0)
            kept += [(start, count)] * (slots.count(None) + most)
        doubles, reach = [], None
        for start, count in sorted(kept):
            if reach is not None and start < reach:
                doubles.append((start, min(start + count, reach) - start))
            reach = start + count if reach is None else max(reach, start + count)
        doubles = RunsIntersect(RunsNormal(doubles), [(0, self.g.data_end + 1)])
        if doubles:
            self.Say("DOUBLE_OWNER", "damage", "block(s) %s are claimed by %s and %s at once"
                     % (RunsText(doubles), label, label), doubles)
        return self.Claim(kind, label, kept)

    # --- the ground -------------------------------------------------------

    def _Deed(self):
        st = self.g.stand
        if st.stood_on == "tail":
            self.Say("DEED_HEAD_BAD", "damage", "the deed at the head of the ground at sector %u does not "
                     "answer (%s) — the volume stands on the far copy" % (st.start, st.head_why))
        elif st.tail is None and st.far_end is None:
            self.Say("DEED_TAIL_BAD", "damage", "the far copy the head names at sector %u: %s — the "
                     "volume stands on one deed" % (st.deed["tail_sector"], st.tail_why))
        if st.far_end is not None:
            self.Say("GROWTH_INTERRUPTED", "note", "a valid far copy of this volume at sector %u claims %u "
                     "sectors against the head's %u — a growth caught between its two writes; the head is "
                     "what the volume stands on" % (st.far_end["at_sector"], st.far_end["sectors"],
                                                    st.deed["sectors"]))
        if not st.kernels:
            self.Say("GROUND_REFUSED", "damage", "%s, and this volume was found at %s" % (st.kernel_why,
                                                                                       st.how))

    def _Claims(self):
        g, m = self.g, self.mount
        spine = [(tally.SlotBlock(g, i), 1) for i in range(g.frame["spine_slots"])] if g.frame else []
        self.Claim("front", "the front of the volume", RunsSubtract([(0, g.data_block)], RunsNormal(spine)))
        self.Claim("spine", "the Spine's slots", spine)
        self.Claim("far_deed", "the far copy of the Deed", [(g.data_end, 1)])
        if m.tally is None:
            return
        # A rotten node's blocks are still its Tally's: its parent names them,
        # and what lies there is no one else's to be given. Every copy a
        # pointer names, however many (§16.2 п.17).
        stood = m.slot.index
        nodes = [(b, n.sheets, n.seq, stood) for n in m.tally.Nodes() for b in n.blocks] + m.older_nodes + \
            [(b, r.ptr["sheets"], r.ptr["seq"], stood) for r in m.tally.rotten if r.ptr for b in r.ptr["blocks"]]
        self.Named("tally", "the Tallies' nodes", nodes)
        quires = [(q[0], q[1], 0, stood) for q in (m.ribbon.quires if m.ribbon else [])] + m.older_quires
        self.Named("ribbon", "the ribbon's Quires", quires)
        for fid in sorted(m.files):
            self.Named("file", self.Label(fid), [(s, n, fid, None) for s, n in m.files[fid].Owned()])
        # A run SHARED owns beside the block's DATA owner, or alone once it
        # has gone, and beside other runs SHARED — of its own file too: never
        # two owners of a block but with an owner of another kind (§8.2).
        for fid in sorted(m.files):
            self.Claim("shared", self.Label(fid), m.files[fid].Shared())
        # What lies past the volume's last block is no one's to hold: a run a
        # record named there is cut at the volume's end (ribbon_transcribe.c
        # RibbonStageFallow).
        owned = RunsNormal([r for c in self.claims for r in c.runs] + [(self.g.data_end + 1, 1 << 64)])
        self.Claim("fallow", "the ground the ribbon let go (M-1)", RunsSubtract(m.fallow, owned))

    def Label(self, fid):
        fs = self.mount.files.get(fid)
        return "fid %u '%s'" % (fid, fs.name.decode("utf-8", "replace") if fs else "?")

    def _Held(self):
        """The blocks held for what rotted (§4.5 п.3, §16.1 п.5): what the
        locked files are known to name; and, when a FILES range is rotten —
        a range of files whose blocks no name that reads tells — every block
        with a digest and the whole key range of a rotten DIGESTS leaf, less
        every block a living owner names. A DIGESTS range rotten alone holds
        nothing: every owner is known, and a mount reads DIGESTS leaves only
        when that range is there (§4.5 п.3: inner nodes at the mount)."""
        g, m = self.g, self.mount
        if m.tally is None:
            return
        want = [r for runs in m.locked_runs.values() for r in [(x.start, x.count) for x in runs if not x.hole]]
        if m.FilesRot():
            want += [(b, 1) for b in set(m.tally.content.digests) | set(m.overlay)]
            for r in m.tally.rotten:
                if r.tree == tally.DIGESTS:
                    hi = g.data_end if r.hi is None else min(r.hi, g.data_end)
                    want.append((r.lo, max(0, hi - r.lo)))
        want = RunsIntersect(RunsNormal(want), [(g.data_block, g.data_end - g.data_block)])
        self.held = RunsSubtract(want, self.Owned())
        if self.held:
            self.Claim("held", "the blocks held for what rotted", self.held)
            self.Say("HELD_BLOCKS", "note", "%u block(s) — %s — are held: no name that reads owns them, and "
                     "a rotten range or a locked file may; they are given to no one until an explicit act "
                     "lets them go (§2.5)" % (RunsCount(self.held), RunsText(self.held)), self.held)

    def Owned(self):
        return RunsNormal([r for c in self.claims if c.kind not in ("held", "boot") for r in c.runs])

    def _Quires(self):
        """A Tally-Quire is held whole while one node in it lives (§16.5
        п.17, Р-d3): every block of a Quire — the stood slot's ribbon Quire
        long, on the grid of the volume's blocks — a Tally's node lies in that
        no claim names, nor the held, is the Tallies' too (mount/ground_census.c
        GroundQuiresHeld, one rule)."""
        m = self.mount
        tallies = [c for c in self.claims if c.kind == "tally"]
        q = m.slot.ribbon["quire_blocks"] if m.tally is not None and m.slot is not None else 0
        if not tallies or not q:
            return
        quires = RunsNormal([(s // q * q, -(-(s + n) // q) * q - s // q * q) for c in tallies for s, n in c.runs])
        named = RunsNormal([r for c in self.claims for r in c.runs] + list(self.held))
        slack = RunsIntersect(RunsSubtract(quires, named), [(0, self.g.data_end + 1)])
        if slack:
            tallies[0].runs = RunsNormal(tallies[0].runs + slack)

    def _Sweep(self):
        doubles = {}
        for s, n, cs in Sweep(self.claims):
            excl = [c for c in cs if c.exclusive]
            if len(excl) >= 2 or any(not a.Allows(o) for a in cs if not a.exclusive for o in excl):
                doubles.setdefault(tuple(sorted(c.label for c in cs)), []).append((s, n))
        for labels, runs in doubles.items():
            runs = RunsNormal(runs)
            self.Say("DOUBLE_OWNER", "damage", "block(s) %s are claimed by %s at once"
                     % (RunsText(runs), " and ".join(labels)), runs)

    def _Boot(self):
        boot, m = self.g.deed["boot"], self.mount
        if boot is None or not boot["kernel_blocks"]:
            return
        run = (boot["kernel_block"], boot["kernel_blocks"])
        holders = [fid for fid, fs in m.files.items() if fs.size == boot["kernel_bytes"] and
                   RunsNormal(fs.Ground()) == [run] and boot["kernel_blocks"] == max(1, -(-fs.size // BLOCK))]
        if len(holders) != 1:
            self.Say("BOOT_MISMATCH", "damage", "the head deed's BOOT run, block %u for %u blocks and %u "
                     "bytes, is not one live file's whole ground (%u files fit it) — the loaders read it"
                     % (run[0], run[1], boot["kernel_bytes"], len(holders)))
            # Its blocks are kept from being given all the same, as the
            # kernel's census keeps them (book_ground.c BookGroundBoot).
            self.Claim("boot", "BOOT", [run])
            return
        self.boot_fid = holders[0]
        self.Claim("boot", "BOOT", [run], partner=self.Label(holders[0]))

    def _Digests(self):
        """Every data block of every live file held to its digest — the
        ribbon's for fresh data, the Tally's DIGESTS for the rest; a block in
        a rotten DIGESTS range reads unchecked (§4.5 п.3)."""
        m, lanes = self.mount, bookread.SeedLanes(self.g.uuid)
        rotten = [r for r in m.tally.rotten if r.tree == tally.DIGESTS]
        digests = dict(m.tally.content.digests)
        digests.update(m.overlay)
        for fid in sorted(m.files):
            wrong, none = [], []
            for s, n in m.files[fid].Ground():
                for b in range(s, s + n):
                    if any(r.Holds(b) for r in rotten):
                        continue
                    want = digests.get(b)
                    if want is None:
                        none.append((b, 1))
                    elif BlockDigest(self.g.Block(b) or b"", lanes) != want:
                        wrong.append((b, 1))
            for runs, what in ((RunsNormal(wrong), "do not match their digests"),
                               (RunsNormal(none), "carry no digest in the Tally or the ribbon")):
                if runs:
                    self.Say("DIGEST_MISMATCH", "damage", "%s: block(s) %s %s — a read of them answers "
                             "ERR_CHECKSUM" % (self.Label(fid), RunsText(runs), what), runs)

    # --- the files -----------------------------------------------------------

    def Content(self, fid):
        """A live file's bytes: its runs, a hole as zeros, its Slips laid on
        its last block in the order of their stitches (§2.8) — None when the
        medium will not give a block of it."""
        fs = self.mount.files.get(fid)
        if fs is None:
            return None
        out = bytearray()
        for r in fs.runs:
            for i in range(r.count):
                raw = bytes(BLOCK) if r.hole else self.g.Block(r.start + i)
                if raw is None:
                    return None
                out += raw
        last = (len(out) // BLOCK - 1) * BLOCK
        for off, data in fs.slips:
            out[last + off:last + off + len(data)] = data
        return bytes(out[:fs.size])

    def Sha256(self, fid):
        data = self.Content(fid)
        return hashlib.sha256(data).hexdigest() if data is not None else None

    def Kinds(self):
        return {c.kind for c in self.claims}

    # --- the answer ------------------------------------------------------------

    def Counts(self):
        return {s: sum(1 for x in self.findings if x.severity == s) for s in SEVERITY}

    def Verdict(self):
        c = self.Counts()
        return "inconsistent" if c["damage"] or c["leak"] else "consistent"

    def ExitCode(self):
        return 0 if self.Verdict() == "consistent" else 1

    def Sorted(self):
        return sorted(self.findings, key=lambda x: SEVERITY.index(x.severity))

    def HumanLines(self):
        g, m, st = self.g, self.mount, self.g.stand
        out = ["ground sector %u (%s), volume %s — format v2" % (st.start, st.how, g.uuid.hex())]
        if st.stood_on == "head" and st.tail is not None:
            out.append("the deed at the head and its far copy agree — %u sectors, far copy at %u"
                       % (st.deed["sectors"], st.deed["tail_sector"]))
        out.append("the Spine: %s" % "; ".join(
            "slot %u %s" % (s.index, "seq %u (Tally %u, stitch %u)" % (s.seq, s.tally_seq, s.stitch_seq)
                            if s.state == "valid" else s.state) for s in m.slots))
        if m.tally is not None:
            out.append("the book stands on slot %u: Tally seq %u — %s" % (m.slot.index, m.tally.seq, ", ".join(
                "%s %u node(s)" % (tally.TREE_NAMES.get(k, k), len(t.nodes)) for k, t in sorted(m.tally.trees.items()))))
            r = m.ribbon
            out.append("the ribbon from block %u: %u stitch(es)%s, its Quires %s" % (
                m.slot.ribbon["at"], len(r.stitches), ", seq %u to %u" % (r.stitches[0].seq, r.stitches[-1].seq)
                if r.stitches else "", RunsText(RunsNormal(r.quires))))
            used = RunsCount(RunsIntersect(RunsNormal([x for c in self.claims for x in c.runs]),
                                           [(g.data_block, g.data_end - g.data_block)]))
            out.append("data run %u blocks from block %u — %u owned, %s; %u live file(s), %u locked"
                       % (g.data_end - g.data_block, g.data_block, used,
                          "%u free" % (g.data_end - g.data_block - used) if self.counted else
                          "free not counted: a Tally of it names a bookmark, whose ground this census does not "
                          "derive", len(m.files), len(m.locked)))
            out.append("the next file id %(next_fid)u, tag id %(next_tag)u, bookmark %(next_bookmark)u, "
                       "seq %(next_seq)u" % m.counts)
        if m.refusal is not None:
            out.append("the book does NOT mount: %s" % m.refusal)
        else:
            out.append("the book mounts%s" % (" — read only" if m.glass else ""))
        k = self.kernel
        out.append("the kernel in this tree would NOT mount this volume: %s" % k["refusal"] if not k["mounts"] else
                   "the kernel in this tree mounts it%s on slot %u: %u file(s), %u locked, %u rotten "
                   "range(s), %u block(s) held" % (" under glass" if k["glass"] else ", its writer going on where "
                                                    "the ribbon ends", k["slot"], k["files"], len(k["locked"]),
                                                    len(k["rotten"]), k["held"]))
        for x in self.Sorted():
            out.append("%s %s: %s" % (x.severity, x.code, x.text))
        c = self.Counts()
        out.append("%s — %u damage, %u leak, %u note" % (self.Verdict(), c["damage"], c["leak"], c["note"]))
        return out

    def Json(self):
        g, st, m = self.g, self.g.stand, self.mount
        deed = {key: (val.hex() if isinstance(val, bytes) else val)
                for key, val in st.deed.items() if key not in ("stamps", "magic")}
        for key in ("born", "packed"):
            if deed.get(key):
                deed[key] = {a: (b.split(b"\0", 1)[0].decode("utf-8", "replace") if isinstance(b, bytes) else b)
                             for a, b in deed[key].items()}
        files = [self._FileJson(fid, "live") for fid in sorted(m.files)] + \
            [dict(fid=fid, state="locked", why=why, visible=False) for fid, why in sorted(m.locked.items())]
        return {
            "medium": {"path": self.medium.path, "sectors": self.medium.SizeSectors()},
            "ground": {"start": st.start, "sectors": st.sectors, "how": st.how, "stood_on": st.stood_on,
                       "kernel_ground": bool(st.kernels)},
            "deed": deed,
            "tail": {"ok": st.tail is not None, "why": st.tail_why,
                     "far_end_sectors": st.far_end["sectors"] if st.far_end else None},
            "files": files,
            "census": {"data_block": g.data_block, "data_end": g.data_end,
                       "free": RunsCount(self.free_runs) if self.counted else None,
                       "claims": [{"kind": c.kind, "label": c.label, "runs": [list(r) for r in c.runs]}
                                  for c in self.claims],
                       "held": [list(r) for r in self.held],
                       "double": [list(r) for x in self.findings if x.code == "DOUBLE_OWNER" for r in x.runs]},
            "kernel": dict(self.kernel),
            "book": self._BookJson(),
            "findings": [{"code": x.code, "severity": x.severity, "text": x.text,
                          "runs": [list(r) for r in x.runs]} for x in self.Sorted()],
            "verdict": self.Verdict()}

    def _FileJson(self, fid, state):
        fs = self.mount.files[fid]
        return {"fid": fid, "state": state, "name": fs.name.decode("utf-8", "replace"), "size": fs.size,
                "gen": fs.gen, "flags": fs.flags, "keep_ns": fs.keep_ns, "tags": list(fs.tags),
                "tag_text": [self.mount.tags[t].Text().decode("utf-8", "replace") for t in fs.tags if t in self.mount.tags],
                "runs": [["hole" if r.hole else "shared" if tally.RunShared(r) else "data", r.start, r.count, r.birth]
                         for r in fs.runs],
                "extents": [list(x) for x in fs.Ground()], "slips": len(fs.slips), "visible": True,
                "sha256": self.Sha256(fid)}

    def _BookJson(self):
        m = self.mount
        slots = [{"index": s.index, "block": s.block, "state": s.state, "why": s.why, "seq": s.seq,
                  "tally_seq": s.tally_seq, "stitch_seq": s.stitch_seq, "stitch_clasp": s.stitch_clasp,
                  "roots": {tally.TREE_NAMES.get(k, str(k)): {"level": r.level, "items": r.items,
                                                              "flags": r.flags} for k, r in s.roots.items()},
                  "ribbon": s.ribbon, "counts": s.counts, "flags": s.flags} for s in m.slots]
        out = {"slots": slots, "slot": m.slot.index if m.slot else None, "refusal": m.refusal,
               "under_glass": list(m.glass), "counts": dict(m.counts), "tally": None,
               "ribbon": {"stitches": [], "torn": [], "past": [], "quires": [], "lied": False},
               "locked": {str(f): w for f, w in sorted(m.locked.items())}, "use": m.use.decode("latin-1")}
        if m.tally is not None:
            t = m.tally
            out["tally"] = {"seq": t.seq, "trees": {tally.TREE_NAMES.get(k, str(k)): {
                "nodes": len(tr.nodes), "items": len(tr.items)} for k, tr in t.trees.items()},
                "rotten": [{"tree": tally.TREE_NAMES[r.tree], "lo": r.lo, "hi": r.hi, "count": r.count,
                            "why": r.why} for r in t.rotten],
                "mended": [{"tree": tally.TREE_NAMES[n.tree], "level": n.level, "blocks": n.blocks,
                            "copy": k + 1, "why": why} for n, k, why in t.mended]}
        if m.ribbon is not None:
            r = m.ribbon

            def Unit(u):
                return {"seq": u.seq, "vlba": u.start.Vlba(), "vlbas": [p.Vlba() for p in u.places], "of": u.of,
                        "sound": u.Sound(),
                        "records": len(u.records), "lost_sectors": u.Lost(), "lost_bytes": u.lost,
                        "clasp": u.clasp}
            out["ribbon"] = {"at": m.slot.ribbon["at"], "stitches": [Unit(u) for u in r.stitches],
                             "torn": [Unit(r.torn)] if r.torn else [], "past": [Unit(u) for u in r.past],
                             "quires": [list(q) for q in r.quires], "lied": r.lied}
        return out
