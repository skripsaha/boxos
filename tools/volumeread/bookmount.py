"""bookmount — a format-v2 volume mounted on the host, the way spec_v2 §4.5
mounts it: the Spine's slots (п.2), the Tally of the newest slot whose roots
read (п.3, the rotten ranges below a root said, not refused), the Tallies of
the other slots for the ground they hold (п.4), the ribbon from that slot's
`at` (п.5: pass 1 in stitch.Ribbon, pass 2 here — every record through the
one rule of §3.7, a file's records held to its base), the data of the newest
stitch held to its digests (п.6), and the numbers never given again (п.8).

It writes nothing and decides everything a mount decides: which slot, which
files are live and which locked, what of their blocks lies fallow, the next
fid, tag, bookmark and seq — and every reason the volume may only be read
(UnderGlass, §16.1 п.2): a kernel writes only what it can carry on.

The ground's owners and the findings are bookcensus.py's; this is the mount.
"""

from . import bookread, ondisk, stitch, tally
from .bookread import Short
from .ondisk import BLOCK, RunsNormal
from .stitch import (BORN, COINED, FROZEN, LABELED, QUITCLAIM, SHAPED, THAWED, UNDONE, USE_CONTEXT,
                     DATA_FLUSHED, KIND_NAMES, LABEL_FLAGS, LABEL_NAME, LABEL_TAGS)
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


class BookMount:
    """The mount of the ground `g` (a bookground.BookGround). After it: `refusal`
    (None when the book mounts), `glass` (why it may only be read), `said`
    [(code, severity, sentence)], the slots, the slot stood on, its Tally,
    the ribbon, the live `files`, the `locked` ones and what of their runs is
    known, the tags, the Use Context, the digests of fresh data, the ground
    lying `fallow`, the other slots' nodes and Quires, and `counts`."""

    def __init__(self, g):
        self.g = g
        self.refusal, self.glass, self.said = None, [], []
        self.slots, self.slot, self.tally, self.ribbon = [], None, None, None
        self.files, self.locked, self.locked_runs, self.tags, self.use = {}, {}, {}, {}, b""
        self.overlay, self.fallow, self.undone, self.mismatched, self.born = {}, [], {}, set(), set()
        self.older_nodes, self.older_quires = [], []
        self.seen_fid = self.seen_tag = self.seen_bookmark = 0
        self.counts = {}
        if self._Frame() and self._Spine() and self._Tally():
            self._Older()
            self._Ribbon()
            self._Replay()
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
        for r in t.rotten:
            self.Say("RANGE_ROTTEN", "damage", self._RangeSentence(r))
        for fid, why in t.gaps:
            self.Say("PARCEL_GAP", "damage", "fid %u does not match its Parcels — %s; it is locked (§2.9, "
                     "§3.9)" % (fid, why))
        for why in t.shape:
            self.Say("RECORD_SHAPE", "damage", "the book is written against its own shape — %s" % why)
        self.files = {fid: fs.Copy() for fid, fs in t.content.files.items()}
        self.locked, self.locked_runs = dict(t.locked), dict(t.locked_runs)
        self.tags, self.use = dict(t.content.tags), t.content.use
        return True

    def _RangeSentence(self, r):
        name = tally.TREE_NAMES[r.tree]
        span = "[%s, %s)" % (r.lo, "the end" if r.hi is None else r.hi)
        if r.tree == tally.FILES:
            return ("the FILES range %s rots in both copies (%s): %u file(s) locked by its parent's count "
                    "— none opens, its ids are not given again, its blocks are held (§4.5 п.3)"
                    % (span, r.why, r.count))
        if r.tree == tally.TAGS:
            return ("the TAGS range %s rots in both copies (%s): the names of %u tag id(s) are lost — the "
                    "files keep the ids, nameless" % (span, r.why, r.count))
        if r.tree == tally.DIGESTS:
            return ("the DIGESTS range of blocks %s rots in both copies (%s): those blocks read unchecked%s"
                    % (span, r.why, ", and every one of them no living file names is held, for a FILES range "
                       "rots too (§16.1 п.5)" if self.FilesRot() else " — a mount reads DIGESTS leaves when "
                       "a block is read and learns it there; no FILES range rots, so every owner is known "
                       "and nothing is held (§4.5 п.3, §16.1 п.5)"))
        return "the %s range %s rots in both copies (%s)" % (name, span, r.why)

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
        ribbons' Quires — read for the ground, leaves not opened."""
        for s in self.slots:
            if s.state != "valid" or s is self.slot:
                continue
            for tree, root in s.roots.items():
                self.older_nodes.extend(tally.TreeRead(self.g, tree, root, [], leaves=False).Blocks())
            self.older_quires.extend(stitch.Ribbon(self.g, s, scan=False).quires)

    # --- п.5-6 ----------------------------------------------------------------

    def _Ribbon(self):
        taken = [(b, n.sheets) for n in self.tally.Nodes() for b in n.blocks]
        taken += [r for fs in self.files.values() for r in fs.Ground()]
        self.ribbon = stitch.Ribbon(self.g, self.slot, taken)
        self.said.extend(self.ribbon.said)
        sewn = [(r[0], r[2], u.seq) for u in self.ribbon.stitches for r in u.records]
        for kind, body, seq in sewn + self.ribbon.Seen():
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
        Tally and itself (§3.7's table)."""
        sewn = {u.seq for u in self.ribbon.stitches}
        refused = set()
        for u in self.ribbon.stitches:
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
        splice (§3.9): its records of that stitch are passed over, as an
        Undone of the next session will say — which this kernel cannot write."""
        lanes = bookread.SeedLanes(self.g.uuid)
        check = self.ribbon.stitches[-2:] if self.ribbon.lied else self.ribbon.stitches[-1:]
        for u in check:
            if not u.Sound() or (u.head.flags & DATA_FLUSHED and not self.ribbon.lied):
                continue
            for kind, _ign, body, _at in u.records:
                if kind not in (BORN, SHAPED):
                    continue
                try:
                    d = stitch.RecordDecode(kind, body, u.seq)
                except Short:
                    continue
                runs = d["runs"] if kind == BORN else [r for _a, _m, rs in d["splices"] for r in rs]
                blocks = [r.start + i for r in runs if not r.hole and tally.EXT_SHARED not in
                          stitch.Extensions(r.ext) for i in range(r.count)]
                bad = [b for b, dg in zip(blocks, d["digests"])
                       if bookread.Integrity(self.g.Block(b) or b"", lanes) != dg]
                if bad:
                    self.undone.setdefault(u.seq, set()).add(d["fid"])
                    torn = self.ribbon.torn is not None
                    self.ribbon.lied = self.ribbon.lied or torn
                    self.Say("NOT_LANDED", "note", "fid %u's data in stitch %u did not land (block %u of "
                             "%u checked does not match its digest): its record is rolled back by its "
                             "splice — ERR_TAGFS_NOT_LANDED%s" % (
                                 d["fid"], u.seq, bad[0], len(blocks),
                                 "; a torn stitch lies after it, so the medium did not keep what it "
                                 "confirmed: a flush that lied, or what was written spoilt by the next "
                                 "write (M-2) — MEDIUM_LIED" if torn else ""))
                    self.Glass("fid %u is rolled back by its splice, and a roll-back is a verdict an Undone "
                               "carries, which this kernel does not write" % d["fid"])

    def _Replay(self):
        """Pass 2: every stitch sewn on, in order, each record through the
        rule of its kind (§3.7) — a stitch that may not be applied is not,
        whole."""
        refused = self._Undone()
        self._Verify()
        for u in self.ribbon.stitches:
            if u.seq in refused:
                continue
            files, tags, use, fallow = dict(self.files), dict(self.tags), self.use, []
            try:
                if u.head is not None and u.head.longer:
                    raise Refused("its StitchHead or Clasp is %u byte(s) longer than this reader knows — "
                                  "written by a kernel that knows more" % u.head.longer)
                for kind, ign, body, _at in u.records:
                    use = self._Record(u, kind, ign, body, files, tags, fallow, use)
                self._Fresh(u)
            except Refused as e:
                self.Glass("stitch %u is not applied: %s" % (u.seq, e))
                continue
            self.files, self.tags, self.use = files, tags, use
            self.fallow.extend(fallow)

    @staticmethod
    def _Fresh(u):
        """Clasp.fresh held to the fresh blocks the stitch's records name
        (§2.7): the bound a mount sees before it reads any data, so a Clasp
        that says other than its records is a stitch that may not be applied."""
        if not u.Sound():
            return
        named = stitch.Fresh([stitch.Record(k, b) for k, _i, b, _a in u.records if k in (BORN, SHAPED)])
        if named != u.parsed.fresh:
            raise Refused("its Clasp names %u fresh block(s) and its records %u (§2.7)" % (u.parsed.fresh, named))

    def _Record(self, u, kind, ign, body, files, tags, fallow, use):
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
            return use
        try:
            self._File(u, kind, d, files, tags, fallow)
        except Mismatch as e:
            self.mismatched.add(d["fid"])
            self.Say("BASE_MISMATCH", "damage", "stitch %u's %s of fid %u does not fit the file: %s — the "
                     "file stays as it last matched (§3.9, the rule of rot)" % (u.seq, KIND_NAMES[kind],
                                                                               d["fid"], e))
            self.Glass("fid %u is rolled back by the rule of rot, and a roll-back is a verdict an Undone "
                       "carries, which this kernel does not write" % d["fid"])
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
        if have is not None and (have.key, have.value) != (ts.key, ts.value):
            raise Refused("its Coined gives id %u to '%s', which the volume knows as '%s'"
                          % (tid, ts.Text().decode("utf-8", "replace"), have.Text().decode("utf-8", "replace")))
        tags[tid] = ts

    def _File(self, u, kind, d, files, tags, fallow):
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
        if fs.Blocks() != Blocks(fs.size):
            raise Mismatch("its runs cover %u blocks and its %u bytes need %u" % (fs.Blocks(), fs.size,
                                                                                Blocks(fs.size)))
        self._Slip(fs, d["slip"])
        blocks = [r.start + i for r in self._Runs(kind, d) if not r.hole and
                  tally.EXT_SHARED not in stitch.Extensions(r.ext) for i in range(r.count)]
        self.overlay.update(zip(blocks, d["digests"]))
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
        if d["what"] & LABEL_TAGS:
            worn = set(fs.tags)
            if any(t in worn for t in d["add"]) or any(t not in worn for t in d["drop"]):
                raise Mismatch("it puts on a tag already worn or takes off one not worn")
            self._Holds(fs, tags, d["add"])
            fs.tags = sorted((worn | set(d["add"])) - set(d["drop"]))

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
        the rule of rot whose blocks another holds is locked (§3.9); a file
        naming ground outside the data run is locked; runs carried with an
        extension, and Slips, are what this kernel cannot carry on."""
        g = self.g
        for fid in sorted(self.files):
            fs = self.files[fid]
            out = [r for r in fs.Ground() if r[0] < g.data_block or r[0] + r[1] > g.data_end]
            if out:
                self.Say("EXTENT_OUTSIDE", "damage", "fid %u names blocks %s outside the data run [%u, %u) — "
                         "it is locked" % (fid, ondisk.RunsText(RunsNormal(out)), g.data_block, g.data_end))
                self._Lock(fid, "it names ground outside the data run")
        for fid in sorted(self.mismatched & set(self.files)):
            mine = RunsNormal(self.files[fid].Ground())
            others = RunsNormal([r for f, fs in self.files.items() if f != fid for r in fs.Ground()])
            if ondisk.RunsIntersect(mine, others):
                self._Lock(fid, "it is rolled back to blocks another file now holds (§3.9)")
        for fs in self.files.values():
            if any(r.ext is not None for r in fs.runs):
                self.Glass("a run of fid %u carries an extension this kernel cannot carry on (CODEC, SHARED "
                           "or COPY, §8)" % fs.fid)
            if fs.slips:
                self.Glass("fid %u's last block carries a Slip — this kernel cannot fold a Slip into a "
                           "block (§16.1 п.2)" % fs.fid)
        owned = RunsNormal([r for fs in self.files.values() for r in fs.Ground()])
        self.fallow = ondisk.RunsSubtract(RunsNormal(self.fallow), owned)

    def _Lock(self, fid, why):
        fs = self.files.pop(fid)
        self.locked[fid] = why
        self.locked_runs[fid] = list(fs.runs)

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
