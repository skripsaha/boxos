"""bookmount — a volume mounted on the host, the way the kernel mounts it:
the Spine's slots, the Tally of the newest slot whose roots read (the rotten
ranges below a root said, not refused), the Tallies of the other slots for
the ground they hold, the ribbon from that slot's `at` (pass 1 in
stitch_ribbon.Ribbon, pass 2 here — every record through one rule, a file's
records held to its base), the data of the newest stitch held to its
digests, and the numbers never given again.

It writes nothing and decides everything a mount decides: which slot, which
files are live and which locked, what of their blocks lies fallow, the next
fid, tag, bookmark and seq — and every reason the volume may only be read
(UnderGlass, §16.1 п.2): a kernel writes only what it can carry on.

The ground's owners and the findings are bookcensus.py's; this is the mount.
"""

from . import bookread, dog_ear, ondisk, stitch, stitch_ribbon, tally
from .bookread import Short
from .ondisk import BLOCK, RunsNormal
from .stitch import (BORN, COINED, FROZEN, LABELED, QUITCLAIM, SHAPED, THAWED, UNDONE, USE_CONTEXT,
                     DATA_FLUSHED, KIND_NAMES, LABEL_FLAGS, LABEL_KEEP, LABEL_NAME, LABEL_TAGS)
from .tally import FileState, Run


UNDONE_WHY = {1: "why 1: its data did not land (NOT_LANDED)",
              2: "why 2: the file did not match its base and was rolled back (the rule of rot)"}


class Mismatch(Exception):
    """A record that does not fit the file it names (§3.7's table: the rule
    of rot) — said, and the file stays as it last matched."""


class Refused(Exception):
    """A stitch that may not be applied at all (§3.7's table): read only."""


def Blocks(size):
    return (size + BLOCK - 1) // BLOCK


def Splice(runs, at, remove, insert):
    """The runs with `remove` blocks taken out from file block `at` and
    `insert` put in their place (§2.8 Shaped; file_record.h FileRecordSplice):
    (new runs, the DATA ground taken out). A run cut by the splice is cut —
    its two ends stay two runs, as a record keeps its Strips."""
    out, gone, pos, end = [], [], 0, at + remove
    placed = False
    for r in runs:
        lo, hi = pos, pos + r.count
        pos = hi
        if hi <= at or lo >= end:
            if lo >= end and not placed:
                out.extend(insert)
                placed = True
            out.append(r)
            continue
        if lo < at:
            out.append(Run(r.hole, r.start, at - lo, r.birth, r.ext))
        if not placed:
            out.extend(insert)
            placed = True
        cut_lo, cut_hi = max(lo, at), min(hi, end)
        if not r.hole:
            gone.append((r.start + cut_lo - lo, cut_hi - cut_lo))
        if hi > end:
            out.append(Run(r.hole, r.start + (end - lo if not r.hole else 0), hi - end, r.birth, r.ext))
    if pos < end:
        raise Mismatch("a splice takes out blocks %u..%u of a file of %u" % (at, end - 1, pos))
    if not placed:
        out.extend(insert)
    return out, gone


def LastBlock(fs):
    """What the file's last block is: (its index, hole or the block it lies
    on) — an overlay of a Slip lies on it until a record changes it (§2.8)."""
    n = Blocks(fs.size)
    if not n:
        return None
    pos = 0
    for r in fs.runs:
        if pos + r.count >= n:
            return n - 1, (None if r.hole else r.start + (n - 1 - pos))
        pos += r.count
    return n - 1, "beyond its runs"


def _Whose(ts):
    """Whose a tag is, in the words a refusal says it in."""
    return "program fid %u" % ts.owner if ts.owner else "the person"


def _Reach(r):
    """The keys of rotten range `r` Facsimile weighs and takes, by their first
    part, [lo, hi) — hi None to the tree's end: of FILES every fid the range
    reaches into, the one its end lies inside too (the kernel's
    mount/facsimile.c RangeKeys)."""
    if r.tree == tally.FILES:
        return r.lo[0], None if r.hi is None else r.hi[0] + (1 if r.hi[1] else 0)
    return r.lo, r.hi


def _Within(reach, x):
    """Is key `x` (a fid, a tag id) inside `reach` (_Reach)?"""
    lo, hi = reach
    return lo <= x and (hi is None or x < hi)


def _Groups(rotten):
    """The rotten ranges of one tree, in key order, as Facsimile weighs them:
    [(range, reach)] runs whose keys reach into each other (two FILES ranges
    that share a fid) — the kernel's mount/facsimile.c GroupFrom."""
    groups = []
    for r in sorted(rotten, key=lambda r: (r.lo,)):
        reach = _Reach(r)
        last = groups[-1] if groups else None
        if last is not None and not any(h is None for _r, (_l, h) in last) and \
                reach[0] < max(h for _r, (_l, h) in last):
            last.append((r, reach))
        else:
            groups.append([(r, reach)])
    return groups


def _Reaches(tree, reach, x):
    """Does rotten range `x` meet the keys `reach` of `tree` (_Reach) — the
    kernel's LookMeets?"""
    lo, hi = reach
    if x.tree != tree:
        return False
    xlo = x.lo[0] if tree == tally.FILES else x.lo
    below = hi is None or xlo < hi
    if x.hi is None:
        return below
    xhi, past = (x.hi[0], x.hi[1] > 0) if tree == tally.FILES else (x.hi, False)
    return below and (lo < xhi or (lo == xhi and past))


class BookMount:
    """The mount of the ground `g` (a bookground.BookGround). After it: `refusal`
    (None when the book mounts), `glass` (why it may only be read), `said`
    [(code, severity, sentence)], the slots, the slot stood on, its Tally,
    the rotten ranges of it `facsimile` takes from an older slot — [(tree,
    the slot's index)] as the mount says them —, the ribbon, the live
    `files`, the `locked` ones and what of their runs is known, the tags,
    the Use Context, the digests of fresh data, the ground lying `fallow`,
    the other slots' nodes and Quires, `counts`, and
    `errata` — the mount's verdict the session's first stitch lays down, an
    Undone each: [(seq of the stitch undone, why, [fids rising])] in order
    (§16.5 п.4, the kernel's mount/errata.c)."""

    def __init__(self, g, stand=None, upto=None, only=None, known=None):
        """`stand`: the index of the slot to stand on, whatever the others
        hold; `upto`: the last stitch replayed — the state after it, as a
        Tally written of it holds (binder.BindCheck). Both None for a mount.
        `only`: (tree, reach) — Facsimile's weighing of an older slot (the
        kernel's RibbonFacsimile): its Tally read, its ribbon to `upto`
        replayed for the records that may be of the keys `reach` of `tree`
        alone (_MayBeOf) — no data checked, nothing settled, no range of its
        own weighed: `passed` and `locked`, the Tally's, are what rule 4 asks
        of it (_Facsimile). `known`: of a FILES `only`, the tags {id: tag} the
        mount that weighs it knows — the kernel's registry then: the TAGS of
        the Tally stood on and those a TAGS range laid in gave
        (ribbon_records.c RibbonTagKnown). A Born or Labeled of the keys is
        held to them, not to the older Tally's: the replay applies no Coined
        (_MayBeOf), and the Tally stood on names every tag coined to it."""
        self.g, self.stand, self.upto, self.only, self.known = g, stand, upto, only, known
        self.refusal, self.glass, self.said = None, [], []
        self.slots, self.slot, self.tally, self.ribbon, self.facsimile = [], None, None, None, []
        self.files, self.locked, self.locked_runs, self.tags, self.use = {}, {}, {}, {}, b""
        self.overlay, self.fallow, self.undone, self.mismatched, self.born = {}, [], {}, set(), set()
        self.errata, self._errata = [], {}
        self.older_nodes, self.older_quires = [], []
        self.ribbon_said, self.applied = 0, []
        self.passed = set()                    # stitches to `upto` the replay passed over (Facsimile's rule 4)
        self.dog_ear = dog_ear.Fresh()         # the keys the stitches applied changed (dog_ear.py)
        self.lied_check = None
        self.seen_fid = self.seen_tag = self.seen_bookmark = 0
        self.counts = {}
        if self._Frame() and self._Spine() and self._Tally():
            if only is None:
                self._Older()
            self._Ribbon()
            self._Replay()
            if only is None:
                self._Settle()
        self._Bounds()

    def Say(self, code, severity, text):
        self.said.append((code, severity, text))

    def Glass(self, why):
        if why not in self.glass:
            self.glass.append(why)

    # --- п.1-3 ----------------------------------------------------------------

    def _Frame(self):
        g = self.g
        if g.frame_why:
            self.refusal = "its FRAME does not hold: %s" % g.frame_why
            return False
        if g.block_bytes != BLOCK:
            self.refusal = "it was laid out in %u-byte blocks and the book speaks %u" % (g.block_bytes, BLOCK)
            return False
        if g.frame_bytes > ondisk.VOLUME_FRAME.size:
            self.Glass("its FRAME is %u bytes, longer than the %u this reader knows — written by a kernel "
                       "that knows more (§1.2)" % (g.frame_bytes, ondisk.VOLUME_FRAME.size))
        if g.frame["spine_slots"] != ondisk.FRAME_SPINE_SLOTS:
            self.Say("FRAME_FOREIGN", "note", "its Spine ring has %u slots and this format's maker lays %u — "
                     "a volume of another maker (§1.2)" % (g.frame["spine_slots"], ondisk.FRAME_SPINE_SLOTS))
        if g.base % (g.atom // ondisk.SECTOR):
            self.Glass("its ground begins at sector %u, not on the medium's physical block of %u bytes: "
                       "every write would rewrite its neighbours (§1.5) — repartition the medium"
                       % (g.base, g.atom))
        return True

    def _Spine(self):
        """п.2: every slot; those that hold, newest first."""
        g = self.g
        self.slots = [tally.SlotRead(g, i) for i in range(g.frame["spine_slots"])]
        for s in self.slots:
            if s.state == "torn":
                self.Say("SLOT_TORN", "note", "Spine slot %u at block %u does not hold: %s — the book is "
                         "read off another (§1.4)" % (s.index, s.block, s.why))
        if not any(s.state == "valid" for s in self.slots):
            self.refusal = ("none of the %u slots of its Spine holds (%s) — the book is not read "
                            "(ERR_TAGFS_CORRUPTED)" % (len(self.slots), "; ".join(
                                "slot %u %s" % (s.index, s.why or s.state) for s in self.slots)))
            self.Say("SPINE_NONE", "damage", self.refusal)
            return False
        return True

    def _Tally(self):
        """п.3: the newest slot whose authoritative roots read."""
        for s in sorted((s for s in self.slots if s.state == "valid"), key=lambda s: -s.seq):
            if self.stand is not None and s.index != self.stand:
                continue
            t = tally.Tally(self.g, s)
            if t.refused is None:
                self.slot, self.tally = s, t
                break
            self.Say("SLOT_FELL_BACK", "note", "slot %u (seq %u) names a Tally that does not read: %s — "
                     "the next slot is read (§1.4)" % (s.index, s.seq, t.refused))
        if self.tally is None:
            self.refusal = "no slot that holds names a Tally that reads (ERR_TAGFS_CORRUPTED)"
            self.Say("SPINE_NONE", "damage", self.refusal)
            return False
        t = self.tally
        for why in self.slot.glass + t.glass:
            self.Glass(why)
        for node, k, why in t.mended:
            self.Say("NODE_COPY_ROTTEN", "note", "the %s node of level %u at blocks %s: copy %u does not "
                     "hold — %s; copy %u is read, and the next Tally writes the node again (§2.5)"
                     % (tally.TREE_NAMES[node.tree], node.level, "/".join(map(str, node.blocks)), k + 1,
                        why, node.copy + 1))
        taken = {} if self.only is not None else self._Facsimile(t)
        for r in t.rotten:
            if r in taken:
                s0, n = taken[r]
                self.facsimile.append((r.tree, s0.index))
                self.Say("RANGE_FACSIMILE", "note", "the %s range [%s, %s) rots in every copy (%s): read from "
                         "slot %u's Tally (seq %u) and its ribbon replayed for its keys — %u element(s) "
                         "(§4.5 п.3 (а), Facsimile)" % (tally.TREE_NAMES[r.tree], r.lo,
                                                         "the end" if r.hi is None else r.hi, r.why, s0.index,
                                                         s0.seq, n))
            else:
                self.Say("RANGE_ROTTEN", "damage", self._RangeSentence(r))
        for fid, why in t.gaps:
            self.Say("PARCEL_GAP", "damage", "fid %u does not match its Parcels — %s; it is locked (§2.9, "
                     "§3.9)" % (fid, why))
        for fid, out in t.outside:
            self.Say("EXTENT_OUTSIDE", "damage", "fid %u names blocks %s outside the data run [%u, %u) — it is "
                     "locked" % (fid, ondisk.RunsText(RunsNormal(out)), self.g.data_block, self.g.data_end))
        for why in t.shape:
            self.Say("RECORD_SHAPE", "damage", "the book is written against its own shape — %s" % why)
        self.files = {fid: fs.Copy() for fid, fs in t.content.files.items()}
        self.locked, self.locked_runs = dict(t.locked), dict(t.locked_runs)
        self.tags, self.use = dict(t.content.tags), t.content.use
        if self.only is not None and self.only[0] == tally.FILES:
            self.tags = dict(self.known)
        for r in taken:
            self._FacsimileTaken(t, r, self._facsimile[r])
        return True

    def _Facsimile(self, t):
        """§4.5 п.3 (а), §16.5 п.19 — Facsimile: a rotten range of FILES or
        TAGS the newest older slot that holds reads whole — its Tally and its
        ribbon replayed for the range's keys to the stitch the slot stood on
        is the state after (the state binder.BindCheck holds a Tally to) — is
        taken from there, for every key of it _Reach names. {range: (the
        slot, the elements)};
        the states kept for _FacsimileTaken. A node the Tallies share rots for
        them all: then no slot stands in (honestly rarer than with whole
        Tallies). The kernel's rules (mount/facsimile.c): TAGS weighed before
        FILES; ranges of a tree whose keys reach into each other — two FILES
        ranges that share a fid — weighed as one, taken together or not at
        all; a slot stands in only where no file of the keys is locked in its
        Tally and its ribbon to the stitch, replayed for the keys alone,
        passed over no stitch — refused for a record that may be of them
        (_MayBeOf), for its own StitchHead or Clasp, or for an Undone that
        would not hold. A stitch refused for a record of other keys alone
        leaves the keys whole: the Tally stood on took the stitch in, so its
        writer applied it whole, and a record of other keys changes none of
        these (the kernel's argument, facsimile.c rule 4). A TAGS range the
        replay passed over a stitch of puts the volume under glass (the
        kernel cannot lay a coined tag out again). A DIGESTS range is not
        weighed: the kernel reads a DIGESTS leaf only when a block of it is
        (§16.3 п.12) — none at the mount — and a leaf no copy of which reads
        is laid anew of the data by Escheat alone (§16.5 п.19,
        binder._DigestsWanted); a USE range is said as it is. The tags a
        FILES replay knows are the kernel's registry as it weighs: the Tally
        stood on's, and every TAGS range laid in — taken, or under glass, for
        what the take coined stays coined (LookLaid, RibbonStageLaid)."""
        self._facsimile, taken, known = {}, {}, dict(t.content.tags)
        k = self.slot.stitch_seq
        older = sorted((s for s in self.slots if s.state == "valid" and s.seq < self.slot.seq), key=lambda s: -s.seq)
        for tree in (tally.TAGS, tally.FILES):
            for group in _Groups([r for r in t.rotten if r.tree == tree]):
                his = [h for _r, (_l, h) in group]
                reach = (group[0][1][0], None if None in his else max(his))
                for s0 in older:
                    if s0.stitch_seq > k:
                        continue
                    p = BookMount(self.g, stand=s0.index, upto=k, only=(tree, reach), known=known)
                    sewn = set() if p.ribbon is None else {u.seq for u in p.ribbon.stitches}
                    if p.refusal is not None or p.tally is None or (k != s0.stitch_seq and k not in sewn) or \
                            any(_Reaches(tree, reach, x) for x in p.tally.rotten):
                        continue
                    if tree == tally.FILES and any(_Within(reach, f) for f in p.locked):
                        continue
                    if tree == tally.TAGS:
                        known.update((i, x) for i, x in p.tags.items() if _Within(reach, i))
                    if p.passed:
                        if tree == tally.FILES:
                            continue
                        self.Glass("the rotten TAGS keys %s taken from slot %u: its ribbon to stitch %u passes over "
                                   "a stitch — what the take coined is not laid out again" % (reach, s0.index, k))
                        break
                    for r, _reach in group:
                        self._facsimile[r] = p
                        taken[r] = (s0, len([x for x in (p.files if tree == tally.FILES else p.tags)
                                             if _Within(reach, x)]))
                    break
        return taken

    @staticmethod
    def _InRange(r, p):
        """The elements of state `p` range `r` reaches: files by id, tag ids."""
        lo, hi = _Reach(r)
        keys = p.files if r.tree == tally.FILES else p.tags
        return [x for x in keys if lo <= x and (hi is None or x < hi)]

    def _FacsimileTaken(self, t, r, p):
        """Range `r` taken from state `p`: its files or tags in place of what
        the rotten node held — every fid it reaches into unlocked, as the
        kernel lets go of the files it set aside (tally_leaves.c,
        BookAsideSettled)."""
        t.rotten.remove(r)
        if r.tree == tally.TAGS:
            for i in self._InRange(r, p):
                self.tags[i] = p.tags[i]
            return
        lo, hi = _Reach(r)
        for fid in [f for f in self.locked if lo <= f and (hi is None or f < hi)]:
            self.locked.pop(fid, None)
            self.locked_runs.pop(fid, None)
            t.locked.pop(fid, None)
        for fid in self._InRange(r, p):
            self.files[fid] = p.files[fid].Copy()

    def _RangeSentence(self, r):
        name = tally.TREE_NAMES[r.tree]
        span = "[%s, %s)" % (r.lo, "the end" if r.hi is None else r.hi)
        if r.tree == tally.FILES:
            return ("the FILES range %s rots in every copy (%s): %u file(s) locked by its parent's count "
                    "— none opens, its ids are not given again, its blocks are held (§4.5 п.3)"
                    % (span, r.why, r.count))
        if r.tree == tally.TAGS:
            return ("the TAGS range %s rots in every copy (%s): the names of %u tag id(s) are lost — the "
                    "files keep the ids, nameless" % (span, r.why, r.count))
        if r.tree == tally.DIGESTS:
            return ("the DIGESTS range of blocks %s rots in every copy (%s): those blocks read unchecked%s"
                    % (span, r.why, ", and every one of them no living file names is held, for a FILES range "
                       "rots too (§16.1 п.5)" if self.FilesRot() else " — a mount reads DIGESTS leaves when "
                       "a block is read and learns it there; no FILES range rots, so every owner is known "
                       "and nothing is held (§4.5 п.3, §16.1 п.5)"))
        return "the %s range %s rots in every copy (%s)" % (name, span, r.why)

    def FilesRot(self):
        """Does a FILES range of the Tally stood on rot? Only then is a block
        no name owns possibly a locked file's, and held (§16.1 п.5)."""
        return self.tally is not None and any(r.tree == tally.FILES for r in self.tally.rotten)

    def _Locked(self, fid, files):
        """Is `fid` locked: by the Tally, or as a file of a rotten FILES range
        — its header lost with the range — that no Born of the ribbon gave
        birth to anew (§4.5 п.3 (б)): its records stay with the range."""
        return fid in self.locked or (fid not in files and fid not in self.born and any(
            r.tree == tally.FILES and r.Touches((fid, 0), (fid + 1, 0)) for r in self.tally.rotten))

    def _Older(self):
        """п.4: the slots not stood on hold their Tallies' nodes and their
        ribbons' Quires — read for the ground, leaves not opened. Each named
        as the census takes a slot's name (bookcensus.py Named): a node's
        copy as (block, Sheets, the node's seq, the slot), a Quire as
        (block, blocks, 0, the slot)."""
        for s in self.slots:
            if s.state != "valid" or s is self.slot:
                continue
            for tree, root in s.roots.items():
                t = tally.TreeRead(self.g, tree, root, [], leaves=False)
                self.older_nodes.extend((b, n.sheets, n.seq, s.index) for n in t.nodes for b in n.blocks)
                # A node neither copy of holds is still its slot's: its parent
                # — or the slot, for a root — names its blocks, as the rotten
                # nodes of the slot stood on are (bookcensus.py, the Tally's
                # claim).
                ptrs = [r.ptr for r in t.rotten if r.ptr] + ([root.Pointer()] if t.refused else [])
                self.older_nodes.extend((b, p["sheets"], p["seq"], s.index) for p in ptrs if p for b in p["blocks"])
            self.older_quires.extend((q[0], q[1], 0, s.index)
                                     for q in stitch_ribbon.Ribbon(self.g, s, scan=False).quires)

    # --- п.5-6 ----------------------------------------------------------------

    def _Ribbon(self):
        # The book's ground a next_quire may not name is the Tally's as the
        # census claims it: every copy of every node, a rotten one's too — its
        # parent names those blocks (bookcensus.py, the Tally's claim), and so
        # does the kernel's census when it weighs the next_quire
        # (book_ground.c BookGroundRibbon).
        taken = [(b, n.sheets) for n in self.tally.Nodes() for b in n.blocks]
        taken += [(b, r.ptr["sheets"]) for r in self.tally.rotten if r.ptr for b in r.ptr["blocks"]]
        taken += [r for fs in self.files.values() for r in fs.Ground()]
        self.ribbon_said = len(self.said)
        self.ribbon = stitch_ribbon.Ribbon(self.g, self.slot, taken)
        self.said.extend(self.ribbon.said)
        sewn = [(r[0], r[2], u.seq) for u in self.ribbon.stitches for r in u.records]
        for kind, body, seq in sewn + self.ribbon.Seen():
            if self.upto is None or seq <= self.upto:
                self._See(kind, body, seq)

    def _See(self, kind, body, seq):
        """The numbers a record gives, read or not applied: never given again."""
        try:
            d = stitch.RecordDecode(kind, body, seq)
        except Short:
            return
        if kind == BORN:
            self.seen_fid = max(self.seen_fid, d["fid"] + 1)
        elif kind == COINED:
            self.seen_tag = max(self.seen_tag, d["tid"] + 1)
        elif kind == FROZEN:
            self.seen_bookmark = max(self.seen_bookmark, d["bid"] + 1)

    def _Undone(self):
        """Every Undone of the ribbon, first (pass 1): {seq undone: {fid}} —
        refusing a stitch whose Undone names no stitch between the slot's
        Tally and itself (§3.7's table). To `upto` alone: the Tally written of
        it knew no stitch past it, and absorbs an Undone of one past it that
        names a stitch to it (§2.8) — the kernel's RibbonFacsimile reads none
        of them."""
        sewn = {u.seq for u in self.ribbon.stitches}
        refused = set()
        for u in self.ribbon.stitches:
            if self.upto is not None and u.seq > self.upto:
                break
            for i, (kind, _ign, body, _at) in enumerate(u.records):
                if kind != UNDONE:
                    continue
                try:
                    d = stitch.RecordDecode(kind, body, u.seq)
                except Short as e:
                    self.Glass("stitch %u carries an Undone that does not read (%s)" % (u.seq, e))
                    refused.add(u.seq)
                    continue
                target = u.seq - d["seq_back"]
                if any(k != UNDONE for k, _i, _b, _a in u.records[:i]) or not d["seq_back"]:
                    why = "an Undone after another record"
                elif target <= self.slot.stitch_seq:
                    continue                     # absorbed by the Tally (§2.8)
                elif target not in sewn or target >= u.seq:
                    why = "an Undone of seq %u, which is no stitch of this ribbon before it" % target
                else:
                    self.undone.setdefault(target, set()).update(d["fids"])
                    self.Say("UNDONE", "note", "stitch %u's Undone passes over stitch %u's records of fid(s) %s "
                             "— %s (§2.8)" % (u.seq, target, ", ".join(map(str, d["fids"])),
                                             UNDONE_WHY.get(d["why"], "why %u, which this reader does not "
                                                            "know; the Undone stands all the same" % d["why"])))
                    continue
                self.Glass("stitch %u carries %s — it is not applied" % (u.seq, why))
                refused.add(u.seq)
        return refused

    def _Verify(self):
        """п.6: the data the newest stitch put down held to its digests — when
        it was sewn without DATA_FLUSHED, and the newest two on a medium that
        lied (M-2). A file whose block did not land is rolled back by its
        splice (§3.9): its records of that stitch are passed over, as the
        Undone of the session's first stitch will say (`errata`).
        Clasp.fresh bounds the read before a block of it is read (§2.7): a
        stitch whose records name other fresh blocks than its Clasp is one
        pass 2 does not apply (_Fresh), and none of its data is read. A block
        past the volume's last is one the medium does not give."""
        lanes = bookread.SeedLanes(self.g.uuid)
        check = self.ribbon.stitches[-2:] if self.ribbon.lied else self.ribbon.stitches[-1:]
        held = [u for u in check if u.Sound() and not (u.head.flags & DATA_FLUSHED and not self.ribbon.lied)
                and self._FreshNamed(u) == u.parsed.fresh]
        if self.ribbon.lied:
            # The price of the check, said before a block is read (§16.4 п.1):
            # the kernel's line names the fresh blocks and the stitches.
            self.lied_check = (sum(u.parsed.fresh for u in held), len(check))
        for u in held:
            for kind, _ign, body, _at in u.records:
                if kind in (BORN, SHAPED):
                    self._Landed(u, kind, body, lanes)

    def _Landed(self, u, kind, body, lanes):
        """One Born or Shaped of the stitch held to its data, its blocks read
        in order against its digests until one does not match or the digests
        end."""
        try:
            d = stitch.RecordDecode(kind, body, u.seq)
        except Short:
            return
        if d["fid"] in self.undone.get(u.seq, ()):
            # Passed over already — by an Undone on the ribbon, or by this
            # check's own verdict on an earlier record of the file: one Undone
            # a stitch undone, never a second (§2.8, ribbon_verify.c).
            return
        runs = d["runs"] if kind == BORN else [r for _a, _m, rs in d["splices"] for r in rs]
        fresh = [r for r in runs if not r.hole and tally.EXT_SHARED not in stitch.Extensions(r.ext)]
        bad, k, digests = None, 0, d["digests"]
        for r in fresh:
            for i in range(min(r.count, len(digests) - k)):
                b = r.start + i
                raw = self.g.Block(b) if b <= self.g.data_end else None
                if bookread.Integrity(raw or b"", lanes) != digests[k + i]:
                    bad = b
                    break
            if bad is not None or k + r.count >= len(digests):
                break
            k += r.count
        if bad is None:
            return
        self.undone.setdefault(u.seq, set()).add(d["fid"])
        self._Erratum(u.seq, 1, d["fid"])
        torn = self.ribbon.torn is not None
        self.ribbon.lied = self.ribbon.lied or torn
        self.Say("NOT_LANDED", "note", "fid %u's data in stitch %u did not land (block %u of %u checked does "
                 "not match its digest): its record is rolled back by its splice — ERR_TAGFS_NOT_LANDED%s" % (
                     d["fid"], u.seq, bad, sum(r.count for r in fresh),
                     "; a torn stitch lies after it, so the medium did not keep what it confirmed: a flush "
                     "that lied, or what was written spoilt by the next write (M-2) — MEDIUM_LIED"
                     if torn else ""))

    def _Replay(self):
        """Pass 2: every stitch sewn on, in order, each record through the
        rule of its kind (§3.7) — a stitch that may not be applied is not,
        whole."""
        refused = self._Undone()
        self.passed |= refused
        if self.only is None:
            self._Verify()
        for u in self.ribbon.stitches:
            if self.upto is not None and u.seq > self.upto:
                break
            if u.seq in refused:
                continue
            try:
                files, tags, use, fallow, overlay = self._Stitch(u)
            except Refused as e:
                self.Glass("stitch %u is not applied: %s" % (u.seq, e))
                self.passed.add(u.seq)
                continue
            dog_ear.Stitched(self.dog_ear, self.files, files, self.touched, self.coined, use is not self.use)
            self.files, self.tags, self.use = files, tags, use
            self.fallow.extend(fallow)
            self.overlay.update(overlay)
            self.applied.append(u.seq)

    def _Stitch(self, u):
        """One stitch's records through their rules, on copies of what the
        mount holds. A record that does not fit its file (the rule of rot)
        passes over the file's records from there, and the stitch is gathered
        again from its first record without any of them — the file where the
        stitch found it, as the next mount leaves it once the Undone of the
        verdict passes over every record of that fid in the stitch (§2.8, H-1;
        ribbon_transcribe.c RibbonStitchApplied). Each gathering again passes
        over one fid more, so it ends."""
        while True:
            files, tags, use, fallow, overlay = dict(self.files), dict(self.tags), self.use, [], {}
            self.touched, self.coined = set(), []
            passed = len(self.undone.get(u.seq, ()))
            if u.head is not None and u.head.longer:
                raise Refused("its StitchHead or Clasp is %u byte(s) longer than this reader knows — "
                              "written by a kernel that knows more" % u.head.longer)
            for kind, ign, body, _at in u.records:
                if self.only is None or self._MayBeOf(kind, ign, body, u.seq):
                    use = self._Record(u, kind, ign, body, files, tags, fallow, use, overlay)
            self._Fresh(u)
            if len(self.undone.get(u.seq, ())) == passed:
                return files, tags, use, fallow, overlay

    def _MayBeOf(self, kind, ign, body, seq):
        """May a record be of the keys Facsimile replays (`only`) — the
        kernel's RibbonRecordWithin: its key among them, or not known — a
        body of a kind that names the tree's keys that does not read, a kind
        this reader does not know and may not step over — which, applied,
        refuses the stitch?"""
        tree, reach = self.only
        if kind not in KIND_NAMES or kind in (stitch.STITCH_HEAD, stitch.CLASP):
            return not ign
        if kind not in ((COINED,) if tree == tally.TAGS else (BORN, SHAPED, LABELED, QUITCLAIM)):
            return False
        try:
            d = stitch.RecordDecode(kind, body, seq)
        except Short:
            return True
        return _Within(reach, d["tid"] if kind == COINED else d["fid"])

    @staticmethod
    def _FreshNamed(u):
        """The fresh blocks the stitch's Borns and Shapeds name (§2.7) — one
        that does not read names none (ribbon_records.c RibbonFreshNamed)."""
        n = 0
        for kind, _i, body, _a in u.records:
            try:
                n += stitch.Fresh([stitch.Record(kind, body)]) if kind in (BORN, SHAPED) else 0
            except Short:
                continue
        return n

    @classmethod
    def _Fresh(cls, u):
        """Clasp.fresh held to the fresh blocks the stitch's records name
        (§2.7): the bound a mount sees before it reads any data, so a Clasp
        that says other than its records is a stitch that may not be applied."""
        if not u.Sound():
            return
        named = cls._FreshNamed(u)
        if named != u.parsed.fresh:
            raise Refused("its Clasp names %u fresh block(s) and its records %u (§2.7)" % (u.parsed.fresh, named))

    def _Record(self, u, kind, ign, body, files, tags, fallow, use, overlay):
        if kind not in KIND_NAMES or kind in (stitch.STITCH_HEAD, stitch.CLASP):
            if not ign:
                raise Refused("it carries a record of kind %u this reader does not know and may not step "
                              "over — written by a kernel that knows more" % kind)
            return use
        try:
            d = stitch.RecordDecode(kind, body, u.seq)
        except Short as e:
            raise Refused("its %s does not read (%s)" % (KIND_NAMES[kind], e))
        if d["longer"]:
            raise Refused("its %s is %u byte(s) longer than this reader knows — written by a kernel that "
                          "knows more" % (KIND_NAMES[kind], d["longer"]))
        if kind == UNDONE:
            return use
        if kind in (BORN, SHAPED, LABELED, QUITCLAIM) and d["fid"] in self.undone.get(u.seq, ()):
            fallow.extend(r for r in self._Inserted(kind, d) if r not in fallow)
            return use
        if kind in (SHAPED, LABELED, QUITCLAIM) and self._Locked(d["fid"], files):
            return use                       # a locked file stays as it is locked
        if kind == USE_CONTEXT:
            return d["text"]
        if kind in (FROZEN, THAWED):
            self.Glass("stitch %u carries a %s — a bookmark this kernel cannot hold (§5)"
                       % (u.seq, KIND_NAMES[kind]))
            return use
        if kind == COINED:
            self._Coined(u, d, tags)
            self.coined.append(d["tid"])
            return use
        self.touched.add(d["fid"])
        try:
            self._File(u, kind, d, files, tags, fallow, overlay)
        except Mismatch as e:
            self.mismatched.add(d["fid"])
            self.undone.setdefault(u.seq, set()).add(d["fid"])
            self._Erratum(u.seq, 2, d["fid"])
            self.Say("BASE_MISMATCH", "damage", "stitch %u's %s of fid %u does not fit the file: %s — the "
                     "file stays as it last matched (§3.9, the rule of rot)" % (u.seq, KIND_NAMES[kind],
                                                                               d["fid"], e))
        return use

    @staticmethod
    def _Inserted(kind, d):
        runs = d["runs"] if kind == BORN else [r for _a, _m, rs in d.get("splices", []) for r in rs]
        return [(r.start, r.count) for r in runs if not r.hole]

    def _Coined(self, u, d, tags):
        tid, ts = d["tid"], d["tag"]
        tally.TagGlass(tid, ts, self.glass)
        if ts.flags & tally.TAG_RETIRED:
            return
        have = tags.get(tid)
        if have is not None and (have.key, have.value, have.owner) != (ts.key, ts.value, ts.owner):
            raise Refused("its Coined gives id %u to '%s' of %s, which the volume knows as '%s' of %s"
                          % (tid, ts.Text().decode("utf-8", "replace"), _Whose(ts),
                             have.Text().decode("utf-8", "replace"), _Whose(have)))
        tags[tid] = ts

    def _File(self, u, kind, d, files, tags, fallow, overlay):
        fid, seq = d["fid"], u.seq
        if kind == BORN:
            if fid in files or fid in self.locked:
                raise Refused("its Born gives birth to fid %u, which exists" % fid)
            if not d["flags"] & ondisk.FILE_ACTIVE:
                raise Refused("its Born of fid %u is not ACTIVE — bit 0 of a file's flags is always 1 (§2.8)"
                              % fid)
            self.born.add(fid)
            fs = FileState(fid, seq, d["flags"], d["size"], d["created_ns"], d["modified_ns"], d["name"],
                           d["tags"], d["runs"])
            self._Holds(fs, tags, d["tags"])
        else:
            old = files.get(fid)
            if old is None or old.gen != seq - d["base"]:
                raise Mismatch("its base names the file at %u and the file stands at %s"
                               % (seq - d["base"], "nothing" if old is None else old.gen))
            fs = old.Copy()
            fs.gen = seq
            if kind == QUITCLAIM:
                del files[fid]
                fallow.extend(fs.Ground())
                return
            if kind == LABELED:
                self._Label(fs, d, tags)
                files[fid] = fs
                return
            before = LastBlock(old)
            for at, remove, runs in self._Absolute(d["splices"]):
                fs.runs, gone = Splice(fs.runs, at, remove, runs)
                fallow.extend(gone)
            fs.size, fs.modified_ns, fs.parcels = d["size"], d["mtime_ns"], None
            if d["splices"] and LastBlock(fs) != before:
                fs.slips = []
            fs.slips = self._SlipsCut(fs.slips, fs.size)
        if fs.Blocks() != Blocks(fs.size):
            raise Mismatch("its runs cover %u blocks and its %u bytes need %u" % (fs.Blocks(), fs.size,
                                                                                Blocks(fs.size)))
        self._Slip(fs, d["slip"])
        blocks = [r.start + i for r in self._Runs(kind, d) if not r.hole and
                  tally.EXT_SHARED not in stitch.Extensions(r.ext) for i in range(r.count)]
        overlay.update(zip(blocks, d["digests"]))
        files[fid] = fs

    @staticmethod
    def _Runs(kind, d):
        return d["runs"] if kind == BORN else [r for _a, _m, rs in d["splices"] for r in rs]

    @staticmethod
    def _Absolute(splices):
        """Splices whose `at` counts from the end of the one before, in the
        file as it stands after it (§2.8), as absolute positions."""
        out, end = [], 0
        for at, remove, runs in splices:
            out.append((end + at, remove, runs))
            end = end + at + sum(r.count for r in runs)
        return out

    @staticmethod
    def _Holds(fs, tags, wears):
        unknown = [t for t in wears if t not in tags]
        if unknown:
            raise Mismatch("it wears tag id(s) %s no Coined or TAGS names" % unknown)

    def _Label(self, fs, d, tags):
        if d["what"] & LABEL_NAME:
            fs.name = d["name"]
        if d["what"] & LABEL_FLAGS:
            fs.flags = (fs.flags | d["flags"][0]) & ~d["flags"][1]
            if not fs.flags & ondisk.FILE_ACTIVE:
                raise Refused("its Labeled takes ACTIVE off fid %u — a file leaves by Quitclaim (§2.8)" % fs.fid)
        if d["what"] & LABEL_KEEP:
            fs.keep_ns = d["keep"]
            fs.flags = fs.flags | ondisk.FILE_DELETED if d["keep"] else fs.flags & ~ondisk.FILE_DELETED
        if d["what"] & LABEL_TAGS:
            worn = set(fs.tags)
            if any(t in worn for t in d["add"]) or any(t not in worn for t in d["drop"]):
                raise Mismatch("it puts on a tag already worn or takes off one not worn")
            self._Holds(fs, tags, d["add"])
            fs.tags = sorted((worn | set(d["add"])) - set(d["drop"]))

    @staticmethod
    def _SlipsCut(slips, size):
        """The overlays cut by the file's size (§16.5 п.13): what lay past it
        in the last block is gone, so a growth inside the block reads its
        base, not the bytes a cut took — one rule with the kernel's
        (file_map/tail_slip.c, TailSlipKept)."""
        cut = size - (Blocks(size) - 1) * BLOCK if size else 0
        return [(off, data[:cut - off]) for off, data in slips if off < cut]

    @staticmethod
    def _Slip(fs, slip):
        """A Slip's overlay laid on the file's last block (§2.8), held to its
        rules: inside the block, a file of bytes, not past its size."""
        off, data = slip
        if not data:
            return
        last = Blocks(fs.size) - 1
        if not fs.size or off + len(data) > BLOCK or last * BLOCK + off + len(data) > fs.size:
            raise Mismatch("its Slip of %u bytes at %u does not lie inside the last block of %u bytes"
                           % (len(data), off, fs.size))
        fs.slips = fs.slips + [(off, data)]

    def _Settle(self):
        """What the replay leaves the mount to decide: a file rolled back by
        the rule of rot onto blocks another holds alone is locked (§3.9) — a
        block one of the two names SHARED is no other's to hold (§8.2); a file
        naming ground outside the data run is locked; a run with an extension
        but SHARED alone and flags past 32 bits are what this kernel cannot
        carry on; a Slip it carries on (§16.5 п.13)."""
        g = self.g
        for fid in sorted(self.files):
            fs = self.files[fid]
            out = [r for r in fs.Ground() if r[0] < g.data_block or r[0] + r[1] > g.data_end]
            if out:
                self.Say("EXTENT_OUTSIDE", "damage", "fid %u names blocks %s outside the data run [%u, %u) — "
                         "it is locked" % (fid, ondisk.RunsText(RunsNormal(out)), g.data_block, g.data_end))
                self._Lock(fid, "it names ground outside the data run")
        for fid in sorted(self.mismatched & set(self.files)):
            mine = RunsNormal(self.files[fid].Owned())
            others = RunsNormal([r for f, fs in self.files.items() if f != fid for r in fs.Owned()])
            if ondisk.RunsIntersect(mine, others):
                for _why, fids in self._errata.values():
                    fids.discard(fid)
                self._Lock(fid, "it is rolled back to blocks another file now holds (§3.9)")
        for fs in self.files.values():
            if not all(tally.RunCarried(r) for r in fs.runs):
                self.Glass("a run of fid %u carries an extension this kernel cannot carry on (CODEC, SHARED "
                           "or COPY, §8)" % fs.fid)
            if fs.flags >> 32:
                self.Glass("fid %u carries flags past the 32 bits this kernel holds — written by a kernel "
                           "that knows more" % fs.fid)
        owned = RunsNormal([r for fs in self.files.values() for r in fs.Ground()])
        self.fallow = ondisk.RunsSubtract(RunsNormal(self.fallow), owned)
        self.errata = [(seq, why, sorted(fids)) for seq, (why, fids) in sorted(self._errata.items()) if fids]
        self._BootSlipped()

    def _BootSlipped(self):
        """§16.6 п.4: the file the loader reads the kernel by — the first live
        file by id whose ground lies on the head deed's BOOT run, as the
        kernel names it (book_ground.c BookFileGround) — carrying a Slip. No
        call changes that file, so its tail is neither laid in its block nor
        let go by Escheat, and no Tally could be written past it: the book is
        only read, from the mount on (BookBootSlipped)."""
        boot = self.g.deed["boot"]
        if boot is None or not boot["kernel_blocks"]:
            return
        lo, hi = boot["kernel_block"], boot["kernel_block"] + boot["kernel_blocks"]
        fid = next((f for f in sorted(self.files) if any(s < hi and s + n > lo for s, n in self.files[f].Ground())),
                   None)
        if fid is not None and self.files[fid].slips:
            self.Glass("fid %u, the file the loader reads the kernel by, carries a Slip on its last block — no call "
                       "changes that file, so its tail is neither laid in its block nor let go by Escheat, and no "
                       "Tally could be written past it" % fid)

    def _Erratum(self, seq, why, fid):
        """`fid` rolled back from stitch `seq` for `why` into the verdict: one
        Undone a stitch undone (§2.8), its why the first found — the check of
        the data comes before the replay (errata.c, ErrataAdd)."""
        self._errata.setdefault(seq, (why, set()))[1].add(fid)

    def _Lock(self, fid, why):
        """A live file the replay locked (§3.9): it leaves the mirror, and the
        volume goes under glass — a Tally names no file it cannot name as it
        lay, and a book no Tally is written of cannot go on (UnderGlass)."""
        fs = self.files.pop(fid)
        self.locked[fid] = why
        self.locked_runs[fid] = list(fs.runs)
        self.Glass("fid %u is locked by the replay of the ribbon — no Tally names a file it cannot name as it "
                   "lay, and a book no Tally is written of cannot go on" % fid)

    # --- п.8 --------------------------------------------------------------------

    def _Bounds(self):
        """The numbers past everything the medium has shown (§4.5 п.8): the
        slot's SpineCounts, every fid, tag and bookmark the Tally and the
        ribbon name — the torn tail and the direct scan among them — and the
        seq past every sector of the book read that held."""
        c = self.slot.counts if self.slot else {"next_fid": 0, "next_tag": 0, "next_bookmark": 1}
        fids = list(self.files) + list(self.locked)
        tags = list(self.tags) + [t for fs in self.files.values() for t in fs.tags]
        self.counts = {"next_fid": max([c["next_fid"], self.seen_fid] + [f + 1 for f in fids]),
                       "next_tag": max([c["next_tag"], self.seen_tag] + [t + 1 for t in tags]),
                       "next_bookmark": max(c["next_bookmark"], self.seen_bookmark),
                       "next_seq": self.g.top_seq + 1}
