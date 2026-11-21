"""bootread — which kernel the UEFI loader jumps into on a volume, and why:
the slot the book stands on, by the rule every reader of a book keeps; the
kernel the Deed's BOOT names first,
held to the digests the DIGESTS tree of that slot's Tally keeps of its blocks;
when it is not attested, the kernels the book names by role, in turn
(bootbook.py), each held to its first block's KERNEL, then to the book — the
DIGESTS tree, or the record of the ribbon that put a block in; the first
attested is jumped into. Nothing attested: the kernel nothing shows broken —
BOOT's first — loaded not attested (F2); nothing of that either: the loader
stops with every kernel it tried and why. A kernel chosen, the file the loader
asks for by name — gangway.settings, boot.scene — read and held to the book in
the same reading (Called). All in the very words the loader's Lectern says
(src/boot/uefi/gangway_lectern.c, gangway_succession.c,
gangway_lectern_called.c): the
oracle the host stand (tools/gangway_stand.c, which builds that same Lectern)
is held to line for line on every landing of self-test's group A
(selftest_book_boot.py).

Its own code, as every reader of this package: the slots by tally.SlotRead,
the Tally a slot names by tally.Tally — a slot is stood on when every
authoritative root reads — and the DIGESTS tree by the nodes TreeRead read.
What is the loader's is the walk along them: a kernel's blocks in order, each
node entered once in the reading and a copy of it that did not hold said as it
is entered, the first block that does not match ending the walk — a match
decides nothing until the last block, and a block the book cannot vouch for
leaves the kernel not attested only when no block is found broken (§16.7 п.4).
"""

from . import bookread, ondisk, tally
from .bookground import BookGround
from .bookread import Short
from .bootbook import BootBook, NameAnswers, Scene
from .ondisk import BLOCK

WHO = "the kernel the deed names"

# The most a kernel may be, and the header it opens with (gangway_lectern.h
# KERNEL_MAX_SIZE; kernel_entry.asm: "KERNEL" at byte 2).
KERNEL_MAX_SIZE = 0x2000000

# The files Gangway asks the volume for by name, in the order it asks, and the
# most of each it reads: the person's settings (src/include/
# boxos_gangway_settings.h GANGWAY_SETTINGS_FILE, settings_chosen.h
# SETTINGS_FILE_MAX) and the scene the machine starts with (boxos_scene.h
# BOX_SCENE_BOOT, BOX_SCENE_FILE_MOST).
CALLED = b"gangway.settings"
CALLED_MOST = 65536
SCENE = b"boot.scene"
SCENE_MOST = 1 << 20
ASKED = ((CALLED, CALLED_MOST), (SCENE, SCENE_MOST))

# The most of a file's name the loader keeps (LecternFile.name: a line's room
# less its end, LECTERN_LINE_BYTES - 1): what a scene is named and judged by.
NAME_KEPT = 255


def Loaded(boot):
    """(blocks, None): the blocks the loader loads of the kernel BOOT names —
    as many as its bytes take, every block it names when it names no bytes
    (gangway_succession.c) — or (None, why) when its bytes overrun them."""
    have = boot["kernel_blocks"] * BLOCK
    if boot["kernel_bytes"] > have:
        return None, "it is said to be %u bytes and its blocks hold %u" % (boot["kernel_bytes"], have)
    if boot["kernel_bytes"] == 0:
        return boot["kernel_blocks"], None
    return (boot["kernel_bytes"] + BLOCK - 1) // BLOCK, None


def Covers(lo, hi, v):
    return (lo is None or lo <= v) and (hi is None or v < hi)


def RangeText(lo, hi):
    return "[%u, %s)" % (0 if lo is None else lo, "the end" if hi is None else hi)


class Lectern:
    """The loader's reading of the book on the ground `g`, each line said into
    `say`: the slot it stands on — `slot` and its Tally `t`, or `why` none
    does — and Attest."""

    def __init__(self, g, say):
        self.g, self.say, self.entered = g, say, set()
        self.slot = self.t = self.tree = self.why = None
        self.lanes = bookread.SeedLanes(g.uuid)
        self._Stand()

    def _Stand(self):
        """The slots judged in their order, those that hold tried newest first
        (§1.4): the first whose authoritative roots all read stood on, the
        DIGESTS tree of its Tally kept — None for a Tally that has none."""
        g = self.g
        slots = [tally.SlotRead(g, i) for i in range(g.frame["spine_slots"])]
        for s in slots:
            if s.state == "torn":
                self.say("Spine slot %u at block %u does not hold — the book is read off another" % (s.index, s.block))
        held = sorted((s for s in slots if s.state == "valid"), key=lambda s: -s.seq)
        if not held:
            self.why = "none of the %u slots of its Spine holds" % len(slots)
            return
        for s in held:
            t = tally.Tally(g, s)
            if t.refused is not None:
                first = next(tree for tree in sorted(s.roots) if tree in tally.AUTHORITY and
                             not s.roots[tree].flags & tally.ROOT_DERIVED and t.trees[tree].refused)
                self.say("slot %u (seq %u) names a Tally that does not read: its %s root will not read "
                         "— the next slot is read" % (s.index, s.seq, tally.TREE_NAMES[first]))
                continue
            self.say("the book stands on slot %u (seq %u)" % (s.index, s.seq))
            self.slot, self.t = s, t
            root = s.roots.get(tally.DIGESTS)
            self.tree = t.trees[tally.DIGESTS] if root and not root.flags & tally.ROOT_DERIVED else None
            if self.tree is not None:
                self._Enter(self.tree.nodes[0])
            return
        self.why = "no slot that holds names a Tally that reads"

    def _Enter(self, node):
        """A node of DIGESTS reached for the first time: a copy before the one
        read that did not hold is said."""
        if node.blocks[0] in self.entered:
            return
        self.entered.add(node.blocks[0])
        if node.copy:
            self.say("the DIGESTS node of level %u at block(s) %s: %u of its copies do not hold — copy %u is read"
                     % (node.level, "/".join(str(b) for b in node.blocks), node.copy, node.copy + 1))

    def _Digest(self, v):
        """("named", digest), ("unnamed",) or ("rotten", lo, hi) for block v:
        the path from the root down, each node entered, to the leaf whose
        range holds it; then the leaf's elements in key order, as
        TallyDigestIn walks them (volume_tally.h)."""
        tree = self.tree
        if tree is None:
            return ("unnamed",)
        node = tree.nodes[0]
        while node.level > 0:
            level = node.level - 1
            rot = next((r for r in tree.rotten if r.level == level and Covers(r.lo, r.hi, v)), None)
            if rot is not None:
                return ("rotten", rot.lo, rot.hi)
            node = next((n for n in tree.nodes if n.level == level and Covers(n.lo, n.hi, v)), None)
            if node is None:
                return ("unnamed",)
            self._Enter(node)
        for key, body in node.items:
            if key > v:
                return ("unnamed",)
            try:
                digests = tally.DigestsDecode(body, "a DIGESTS element", [])
            except Short:
                return ("rotten", node.lo, node.hi)
            if v - key < len(digests):
                return ("named", digests[v - key])
        return ("unnamed",)

    def Attest(self, who, blocks, overlay):
        """("held" | "broken" | "unchecked", the line said): the kernel `who`,
        its blocks in order — a volume block, or None for a hole, which holds
        zeros the book names by its run — each held to the digest the ribbon
        names of it (`overlay`) or the DIGESTS tree."""
        if self.slot is None:
            return self._Verdict("unchecked", "%s is not attested: %s" % (who, self.why))
        unchecked = None
        for k, v in enumerate(blocks):
            if v is None:
                continue
            got = ("named", overlay[v]) if v in overlay else self._Digest(v)
            if got[0] == "named":
                if bookread.Integrity(self.g.Block(v), self.lanes) != got[1]:
                    return self._Verdict("broken", "%s does not match the book: block %u (volume block %u)"
                                         % (who, k, v))
            elif unchecked is None and got[0] == "unnamed":
                unchecked = "its Tally names no digest of its block %u (volume block %u)" % (k, v)
            elif unchecked is None:
                unchecked = ("its block %u (volume block %u) lies in the DIGESTS range of blocks %s, which does "
                             "not read" % (k, v, RangeText(got[1], got[2])))
        if unchecked is not None:
            return self._Verdict("unchecked", "%s is not attested: %s" % (who, unchecked))
        return self._Verdict("held", "%s is attested by the book: %u block(s)" % (who, len(blocks)))

    def _Verdict(self, says, line):
        self.say(line)
        return says, line


class Candidate:
    """A kernel to try: its name in the lines, its runs (tally.Run), its size
    in bytes, and the Slips laid on its last block [(offset, bytes)]."""

    __slots__ = ("who", "runs", "size", "slips")

    def __init__(self, who, runs, size, slips=()):
        self.who, self.runs, self.size, self.slips = who, list(runs), size, list(slips)

    def Blocks(self):
        """Its blocks in file order — a volume block, None for a hole — as many
        as its bytes take."""
        out = []
        for r in self.runs:
            out += [None if r.hole else r.start + i for i in range(r.count)]
        return out[:(self.size + BLOCK - 1) // BLOCK]


class Succession:
    """The loader's choice on the ground `g`: `lines` as it says them, `says`
    "held" (attested, jumped into), "unchecked" (loaded not attested) or
    "broken" (no kernel may be jumped into: it stops). With `called` — names
    (bytes) and the most of each read, in the order asked — and a kernel
    chosen, the files called so: `content[name]` the bytes of each read,
    `found[name]` "held" or "unchecked" then; neither for one not read. With
    `asked` {which: (tags, pick)}, the file `which` of those called is asked
    by those tags (bytes) instead of by its name, as a desk's ask asks it:
    the scene `pick` falls on of those that wear them all — with none, the
    file by its name in its place."""

    def __init__(self, g, called=(), asked=None):
        self.g, self.called, self.lines, self.tried, self.deferred, self.says = g, tuple(called), [], [], [], None
        self.book = None
        self.content, self.found = {}, {}
        self._Choose()
        if self.called and self.says != "broken":
            for which, (name, most) in enumerate(self.called):
                self._Called(name, most, (asked or {}).get(which))

    def _Names(self):
        return tuple(name for name, _most in self.called)

    def _Choose(self):
        g = self.g
        boot = g.deed["boot"]
        named = bool(boot and boot["kernel_block"] and boot["kernel_blocks"])
        first = self._Boot(boot) if named else None
        self.lec = Lectern(g, self.lines.append)
        said = self._Try(first, {}) if first is not None else None
        if said == "held":
            return
        if self.lec.slot is not None:
            self.book = BootBook(g, self.lec.slot, self.lec.t, self.lines.append, self._Names())
            for fs in self.book.candidates:
                if self._Try(Candidate("kernel fid %u" % fs.fid, fs.runs, fs.size, fs.slips),
                             self.book.overlay) == "held":
                    return
        self._Last(first if said == "unchecked" else None, named)

    def _Picked(self, tags, pick):
        """The scene followed that the book does not keep deleted and that
        matches every word asked — wears a tag of the ids TAGS and the ribbon
        give it, or is called it, by the whole name or its stem — the one
        `pick` falls on counted by fid — and how many do."""
        ids = [self.book.TagIds(word) for word in tags]
        wear = sorted(fid for fid, fs in self.book.files.items()
                      if not fs.flags & ondisk.FILE_DELETED and Scene(bytes(fs.name)[:NAME_KEPT]) and
                      all(set(fs.tags) & found or NameAnswers(bytes(fs.name)[:NAME_KEPT], word)
                          for word, found in zip(tags, ids)))
        return (wear[pick % len(wear)] if wear else None), len(wear)

    def _Called(self, called, most, asked=None):
        """The file `called` names (gangway_lectern_called.c): the first made of
        those the book does not keep deleted, said when there are more — or,
        `asked` (tags, pick), the scene picked of those that wear them all,
        said with how many, and with none the file by its name in its place,
        said; what may be read of it, `most` bytes at most; its
        blocks held to the book, the Slips laid on its last."""
        g, lec, name = self.g, self.lec, called.decode()
        label = "the scene carrying " + ", ".join(t.decode() for t in asked[0]) if asked else name
        if lec.slot is None:
            self._Say("%s is not read: %s" % (label, lec.why))
            return
        if self.book is None:
            self.book = BootBook(g, lec.slot, lec.t, self.lines.append, self._Names())
        if asked:
            fid, count = self._Picked(*asked)
            if fid is None:
                self._Say("no scene on this volume carries %s — %s is read in its place"
                          % (", ".join(t.decode() for t in asked[0]), name))
                asked = None
            else:
                self._Say("%u %s %s — fid %u, the one picked, is read"
                          % (count, "scene carries" if count == 1 else "scenes carry",
                             ", ".join(t.decode() for t in asked[0]), fid))
                fs = self.book.files[fid]
                name = bytes(fs.name)[:NAME_KEPT].decode(errors="replace")
        if not asked:
            named = sorted(fid for fid, fs in self.book.files.items()
                           if fs.name == called and not fs.flags & ondisk.FILE_DELETED)
            if not named:
                self._Say("no file on this volume is called %s" % name)
                return
            if len(named) > 1:
                self._Say("%u files are called %s — fid %u, the first made, is read" % (len(named), name, named[0]))
            fs = self.book.files[named[0]]
        who = "%s (fid %u)" % (name, fs.fid)
        for r in fs.runs:
            if not tally.RunCarried(r):
                self._Say("%s cannot be read: a run of it carries an extension this loader does not read (CODEC or "
                          "COPY, §8)" % who)
                return
            if not r.hole and (r.start < g.data_block or r.start + r.count > g.data_end):
                self._Say("%s cannot be read: it names ground outside the data run" % who)
                return
        if fs.size > most:
            self._Say("%s cannot be read: it is %u bytes, more than the %u this loader reads"
                      % (who, fs.size, most))
            return
        if fs.size == 0:
            self._Say("%s is empty" % who)
            self.content[called], self.found[called] = b"", "held"
            return
        blocks = Candidate(who, fs.runs, fs.size).Blocks()
        unread = next((k for k, v in enumerate(blocks) if v is not None and g.Block(v) is None), None)
        if unread is not None:
            self._Say("%s cannot be read: its block %u (volume block %u) will not read" % (who, unread, blocks[unread]))
            return
        says, _line = lec.Attest(who, blocks, self.book.overlay)
        if says == "broken":
            return
        raw = bytearray(b"".join(bytes(BLOCK) if v is None else g.Block(v) for v in blocks))
        last = (len(blocks) - 1) * BLOCK
        for off, data in fs.slips:
            raw[last + off:last + off + len(data)] = data
        self.content[called], self.found[called] = bytes(raw[:fs.size]), says

    def _Say(self, line):
        self.lines.append(line)

    def _Boot(self, boot):
        blocks, why = Loaded(boot)
        if why:
            self._Refused("%s cannot be loaded: %s" % (WHO, why))
            return None
        size = boot["kernel_bytes"] or blocks * BLOCK
        self._Say("the deed names the kernel at volume block %u, %u block(s) for its %u byte(s)"
                  % (boot["kernel_block"], blocks, boot["kernel_bytes"]))
        return Candidate(WHO, [tally.Run(False, boot["kernel_block"], blocks, 0)], size)

    def _Refused(self, line):
        self._Say(line)
        self.tried.append(line)
        return "refused"

    def _First(self, c, blocks):
        """The kernel's first block as it is loaded — zeros for a hole, its
        Slips laid when it is the last too — or None when it will not read."""
        raw = bytes(BLOCK) if blocks[0] is None else self.g.Block(blocks[0])
        if raw is not None and len(blocks) == 1:
            raw = bytearray(raw)
            for off, data in c.slips:
                raw[off:off + len(data)] = data
        return raw

    def _Try(self, c, overlay):
        """One kernel: what may be loaded of it, its header, its blocks held to
        the book. Its verdict, said."""
        if not all(tally.RunCarried(r) for r in c.runs):
            return self._Refused("%s cannot be loaded: a run of it carries an extension this loader does not "
                                 "read (CODEC or COPY, §8)" % c.who)
        if c.size == 0:
            return self._Refused("%s cannot be loaded: it is empty" % c.who)
        if c.size > KERNEL_MAX_SIZE:
            return self._Refused("%s cannot be loaded: it is %u bytes, more than the %u this loader places"
                                 % (c.who, c.size, KERNEL_MAX_SIZE))
        blocks = c.Blocks()
        first = self._First(c, blocks)
        if first is None:
            return self._Refused("%s cannot be loaded: its block 0 (volume block %u) will not read"
                                 % (c.who, blocks[0]))
        if first[2:8] != b"KERNEL":
            return self._Refused("%s is no kernel: its first block does not begin with KERNEL" % c.who)
        unread = next((k for k, v in enumerate(blocks) if k and v is not None and self.g.Block(v) is None), None)
        if unread is not None:
            return self._Refused("%s cannot be loaded: its block %u (volume block %u) will not read"
                                 % (c.who, unread, blocks[unread]))
        says, line = self.lec.Attest(c.who, blocks, overlay)
        if says == "unchecked":
            self.deferred.append(c)
        elif says == "broken":
            self.tried.append(line)
        else:
            self.says = "held"
        return says

    def _Last(self, unchecked, named):
        """Nothing attested: the kernel nothing shows broken — BOOT's, then the
        first put off — loaded not attested; nothing of that either, every
        kernel tried and why, and the sentence the loader stops with."""
        c = unchecked or (self.deferred[0] if self.deferred else None)
        if c is not None:
            self.says = "unchecked"
            self._Say("no kernel is attested by the book: %s is jumped into not attested — nothing shows it broken"
                      % c.who)
            return
        self.says = "broken"
        if self.tried:
            self._Say("every kernel tried, and why none is jumped into:")
            for line in self.tried:
                self._Say("  " + line)
            self._Say("no kernel on this volume may be jumped into: %u tried, none holds to the book"
                      % len(self.tried))
        elif self.lec.slot is None:
            self._Say("this volume's deed names no kernel, and its book does not read: %s" % self.lec.why)
        else:
            self._Say("this volume's deed names no kernel, and its book names none by role")


def BootRead(medium, start=None, called=(), asked=None):
    """(lines, verdict): what the loader says of the volume's kernels, line by
    line, and its verdict — "held", "unchecked", "broken" — or None where it
    reads no book: a Deed that bears no FRAME, or one whose FRAME it refuses
    (said in the lines). With `called`, the lines of the files called so after
    the choice's, `asked` asking some by tags (Succession)."""
    from .medium import StandOnDeed
    if ondisk.STAMP_FRAME not in StandOnDeed(medium, start).deed["stamps"]:
        return ["this deed bears no FRAME, so its volume is no book — an older one, or not TagFS's"], None
    g = BookGround(medium, start)
    if g.frame_why:
        return ["this deed's FRAME does not hold: %s" % g.frame_why], None
    s = Succession(g, called, asked)
    return s.lines, s.says
