"""bookcensus — the roll call of a format-v2 volume (spec_v2 §4.6): every
owner of every block, set against the book's mount (bookmount.py).

A format-v2 volume keeps no bitmap: which blocks are taken is derived from
the names that take them. So the census here is the derivation itself — the
front of the volume before its data run, the Spine's slots, every node of the
Tally of every slot that holds (both copies), the ribbon's Quires from the
earliest slot's `at` to the Quire the ribbon ends in and its next_quire, every
DATA run of every live file, BOOT beside the kernel file it is, the far copy
of the Deed, the blocks held for a rotten range (their own class: held, never
an orphan) and the ground the ribbon let go, lying fallow (M-1). Two owners on
one block are named where they meet. What none of them names is free; there
is no third state, so there is no orphan to find.

And beside the account: every finding of the mount, each data block of a
live file held to its digest, and what the kernel of this tree makes of the
volume — which, until it reads a book, is a refusal (deed.c
DeedFormatRefused).

The owners are this module's until the kernel's own list names them
(src/include/ground_owner.h, the mount that reads a book): BOOK_OWNERS.
"""

import hashlib

from . import bookread, tally
from .bookmount import BookMount
from .census import SEVERITY, Finding, GroundClaim, Sweep
from .bookground import BookGround
from .ondisk import BLOCK, RunsCount, RunsIntersect, RunsNormal, RunsSubtract, RunsText

# (name, owns alone), in the order of the account above
BOOK_OWNERS = (("front", True), ("spine", True), ("tally", True), ("ribbon", True), ("file", True),
               ("boot", False), ("far_deed", True), ("held", True), ("fallow", True))
BOOK_OWNER_ALONE = dict(BOOK_OWNERS)

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


def KernelRefusal(g):
    """What the kernel of this tree says of a format-v2 volume at its format
    check (deed.c DeedFormatRefused): it reads no book yet."""
    if g.block_bytes != BLOCK:
        return ("the format-v2 volume was laid out in %u-byte blocks and this kernel speaks %u"
                % (g.block_bytes, BLOCK))
    if g.frame_why:
        return "the deed bears a FRAME that does not hold: %s" % g.frame_why
    return ("format v2 — its deed bears FRAME; this kernel cannot read a format-v2 book — boot a newer "
            "kernel (deed.c DeedFormatRefused)")


class BookCensus:
    """Every owner of every block of a format-v2 volume, its mount's findings,
    and the account in the shape GroundCensus gives a format-v1 one."""

    def __init__(self, medium, forced_start=None):
        self.medium = medium
        self.g = g = BookGround(medium, forced_start)
        self.findings, self.claims, self.held, self.boot_fid = [], [], [], None
        self.kernel = {"mounts": False, "refusal": KernelRefusal(g)}
        self._Deed()
        self.mount = m = BookMount(g)
        for code, severity, text in m.said:
            self.Say(code, severity, text)
        for why in m.glass:
            self.Say("UNDER_GLASS", "note", "the volume may only be read: %s" % why)
        self._Claims()
        self._Held()
        self._Sweep()
        if m.refusal is None:
            self._Boot()
            self._Digests()
        self.free_runs = RunsSubtract([(g.data_block, g.data_end - g.data_block)],
                                      RunsNormal([r for c in self.claims for r in c.runs]))

    def Say(self, code, severity, text, runs=()):
        self.findings.append(Finding(code, severity, text, runs))

    def Claim(self, kind, label, runs, partner=None):
        c = GroundClaim(kind, label, RunsIntersect(RunsNormal(runs), [(0, self.g.data_end + 1)]),
                        BOOK_OWNER_ALONE[kind], partner)
        if c.runs:
            self.claims.append(c)
        return c

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
        # and what lies there is no one else's to be given.
        nodes = [(b, n.sheets) for n in m.tally.Nodes() for b in n.blocks] + m.older_nodes + \
            [(b, r.ptr["sheets"]) for r in m.tally.rotten if r.ptr for b in r.ptr["blocks"]]
        self.Claim("tally", "the Tallies' nodes", nodes)
        quires = (m.ribbon.quires if m.ribbon else []) + m.older_quires
        self.Claim("ribbon", "the ribbon's Quires", quires)
        for fid in sorted(m.files):
            fs = m.files[fid]
            self.Claim("file", self.Label(fid), fs.Ground())
        owned = RunsNormal([r for c in self.claims for r in c.runs])
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
            out.append("data run %u blocks from block %u — %u owned, %u free; %u live file(s), %u locked"
                       % (g.data_end - g.data_block, g.data_block, used, g.data_end - g.data_block - used,
                          len(m.files), len(m.locked)))
            out.append("the next file id %(next_fid)u, tag id %(next_tag)u, bookmark %(next_bookmark)u, "
                       "seq %(next_seq)u" % m.counts)
        if m.refusal is not None:
            out.append("the book does NOT mount: %s" % m.refusal)
        else:
            out.append("the book mounts%s" % (" — read only" if m.glass else ""))
        out.append("the kernel in this tree would NOT mount this volume: %s" % self.kernel["refusal"])
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
            "format": "v2",
            "medium": {"path": self.medium.path, "sectors": self.medium.SizeSectors()},
            "ground": {"start": st.start, "sectors": st.sectors, "how": st.how, "stood_on": st.stood_on,
                       "kernel_ground": bool(st.kernels)},
            "deed": deed,
            "tail": {"ok": st.tail is not None, "why": st.tail_why,
                     "far_end_sectors": st.far_end["sectors"] if st.far_end else None},
            "files": files,
            "census": {"data_block": g.data_block, "data_end": g.data_end,
                       "free": RunsCount(self.free_runs),
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
                "gen": fs.gen, "flags": fs.flags, "tags": list(fs.tags),
                "runs": [["hole" if r.hole else "data", r.start, r.count, r.birth] for r in fs.runs],
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
                return {"seq": u.seq, "vlba": u.start.Vlba(), "of": u.of, "sound": u.Sound(),
                        "records": len(u.records), "lost_sectors": u.Lost(), "lost_bytes": u.lost,
                        "clasp": u.clasp}
            out["ribbon"] = {"at": m.slot.ribbon["at"], "stitches": [Unit(u) for u in r.stitches],
                             "torn": [Unit(r.torn)] if r.torn else [], "past": [Unit(u) for u in r.past],
                             "quires": [list(q) for q in r.quires], "lied": r.lied}
        return out
