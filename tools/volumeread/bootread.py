"""bootread — what the UEFI loader makes of a format-v2 volume's kernel before it
jumps (spec_v2 §9.2, §16.7 пп.2, 4, 6): the slot the book stands on, by the
rule every reader of a book keeps (§1.4); the digest the DIGESTS tree of that
slot's Tally names for each block BOOT names; and the verdict — attested, does
not match, or not attested and why — in the very words the loader's Lectern
says it (src/boot/uefi/tagboot_lectern.c). The oracle the host stand
(tools/tagboot_stand.c, which builds that same Lectern) is held to line for
line on every landing of self-test's group A (selftest_book_boot.py).

Its own code, as every reader of this package: the slots by tally.SlotRead,
the Tally a slot names by tally.Tally — a slot is stood on when every
authoritative root reads — and the DIGESTS tree by the nodes TreeRead read.
What is the loader's is the walk along them: the blocks in order, each node
entered once and a copy of it that did not hold said as it is entered, the
first block that does not match ending the walk — a match decides nothing
until the last block, and a block the book cannot vouch for leaves the kernel
not attested only when no block is found broken (§16.7 п.4, F2).
"""

from . import bookread, tally
from .bookground import BookGround
from .bookread import Short
from .ondisk import BLOCK

WHO = "the kernel the deed names"


def Loaded(boot):
    """(blocks, None): the blocks the loader loads of the kernel BOOT names —
    as many as its bytes take, every block it names when it names no bytes
    (tagboot.c TagFsLoadKernel) — or (None, why) when its bytes overrun them."""
    have = boot["kernel_blocks"] * BLOCK
    if boot["kernel_bytes"] > have:
        return None, "the kernel is said to be %u bytes and its blocks hold %u" % (boot["kernel_bytes"], have)
    if boot["kernel_bytes"] == 0:
        return boot["kernel_blocks"], None
    return (boot["kernel_bytes"] + BLOCK - 1) // BLOCK, None


def Covers(lo, hi, v):
    return (lo is None or lo <= v) and (hi is None or v < hi)


def RangeText(lo, hi):
    return "[%u, %s)" % (0 if lo is None else lo, "the end" if hi is None else hi)


class Lectern:
    """The loader's reading of the book on the ground `g`: `lines` as it says
    them, then the verdict — `says` one of "held", "broken", "unchecked",
    and `line`, its sentence."""

    def __init__(self, g, first, blocks):
        self.g, self.lines, self.entered = g, [], set()
        self.says, self.line = None, None
        tree = self._Stand()
        if self.says is None:
            self._Attest(tree, first, blocks)
        self.lines.append(self.line)

    def _Stand(self):
        """The slots judged in their order, those that hold tried newest first
        (§1.4): the DIGESTS tree of the first whose authoritative roots all
        read — None for a Tally that has none — or the verdict when no slot
        stands."""
        g = self.g
        slots = [tally.SlotRead(g, i) for i in range(g.frame["spine_slots"])]
        for s in slots:
            if s.state == "torn":
                self.lines.append("Spine slot %u at block %u does not hold — the book is read off another"
                                  % (s.index, s.block))
        held = sorted((s for s in slots if s.state == "valid"), key=lambda s: -s.seq)
        if not held:
            return self._Unchecked("none of the %u slots of its Spine holds" % len(slots))
        for s in held:
            t = tally.Tally(g, s)
            if t.refused is not None:
                first = next(tree for tree in sorted(s.roots) if tree in tally.AUTHORITY and
                             not s.roots[tree].flags & tally.ROOT_DERIVED and t.trees[tree].refused)
                self.lines.append("slot %u (seq %u) names a Tally that does not read: its %s root will not read "
                                  "— the next slot is read" % (s.index, s.seq, tally.TREE_NAMES[first]))
                continue
            self.lines.append("the book stands on slot %u (seq %u)" % (s.index, s.seq))
            self.lanes = bookread.SeedLanes(g.uuid)
            tree = t.trees.get(tally.DIGESTS)
            if tree is not None:
                self._Enter(tree.nodes[0])
            return tree
        return self._Unchecked("no slot that holds names a Tally that reads")

    def _Unchecked(self, why):
        self.says, self.line = "unchecked", "%s is not attested: %s" % (WHO, why)

    def _Enter(self, node):
        """A node of DIGESTS reached for the first time: a copy before the one
        read that did not hold is said."""
        if id(node) in self.entered:
            return
        self.entered.add(id(node))
        if node.copy:
            self.lines.append("the DIGESTS node of level %u at block(s) %s: %u of its copies do not hold — copy %u "
                              "is read" % (node.level, "/".join(str(b) for b in node.blocks), node.copy,
                                           node.copy + 1))

    def _Digest(self, tree, v):
        """("named", digest), ("unnamed",) or ("rotten", lo, hi) for block v:
        the path from the root down, each node entered, to the leaf whose
        range holds it; then the leaf's elements in key order, as
        TallyDigestIn walks them (volume_tally.h)."""
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

    def _Attest(self, tree, first, blocks):
        unchecked = None
        for k in range(blocks):
            v = first + k
            got = self._Digest(tree, v)
            if got[0] == "named":
                raw = self.g.Block(v)
                if raw is None or bookread.Integrity(raw, self.lanes) != got[1]:
                    self.says = "broken"
                    self.line = "%s does not match the book: block %u (volume block %u)" % (WHO, k, v)
                    return
            elif unchecked is None and got[0] == "unnamed":
                unchecked = "its Tally names no digest of its block %u (volume block %u)" % (k, v)
            elif unchecked is None:
                unchecked = ("its block %u (volume block %u) lies in the DIGESTS range of blocks %s, which does "
                             "not read" % (k, v, RangeText(got[1], got[2])))
        if unchecked is not None:
            return self._Unchecked(unchecked)
        self.says, self.line = "held", "%s is attested by the book: %u block(s)" % (WHO, blocks)


def BootRead(medium, start=None):
    """(lines, verdict): what the loader says of the volume's kernel, line by
    line, and its verdict — "held", "broken", "unchecked", or None where it
    reads no book: a volume of format v1, a Deed that names no kernel or whose
    FRAME it refuses (those are said in the lines)."""
    from .medium import StandOnDeed
    st = StandOnDeed(medium, start)
    if st.format != "v2":
        return ["a format-v%s volume — the loader takes its kernel as it always has, held to no book"
                % st.format[1:]], None
    g = BookGround(medium, start)
    boot = g.deed["boot"]
    if g.frame_why:
        return ["this deed's FRAME does not hold: %s" % g.frame_why], None
    if not boot or not boot["kernel_block"] or not boot["kernel_blocks"]:
        return ["this volume's deed names no kernel"], None
    blocks, why = Loaded(boot)
    if why:
        return [why], None
    head = "the deed names the kernel at volume block %u, %u block(s) for its %u byte(s)" % (
        boot["kernel_block"], blocks, boot["kernel_bytes"])
    lec = Lectern(g, boot["kernel_block"], blocks)
    return [head] + lec.lines, lec.says
