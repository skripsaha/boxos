"""selftest_book_boot — group A: the landings the UEFI loader's Lectern is held
to (spec_v2 §16.7 пп.4, 6): the kernel BOOT names on the book as a binder lays
it, attested; the DIGESTS leaf that names its first block rotten in its first
copy — the second read, attested — and in both — not attested; the newest slot
torn, and a newer slot whose DIGESTS root reads in neither copy — the older
slot stood on, attested; the Spine's ring empty — not attested; a byte of the
kernel's code flipped, and a byte of the zeros past its end in its last block
— the block named, does not match. Each a fork of the base selftest_book.py
lays, the census's words for it, and the verdict bootread.py says — which the
host stand (tools/tagboot_stand.sh) then holds the Lectern itself to, line for
line, on every one of them written out by --emit; and logcheck's bookboot
boots A7 through OVMF."""

from . import bookread, tally
from .bootread import WHO, BootRead, Loaded
from .bookground import BookGround
from .medium import Unreadable
from .ondisk import BLOCK, SECTOR, SECTORS_PER_BLOCK
from .selftest_book import Found
from .selftest_cases import Case


def KernelLeaf(bt, block):
    """The DIGESTS leaf of the base's Tally that names `block`, and the range
    of blocks its parent gives it: [its first key, the next leaf's) — the last
    leaf's running to the end (None) — or the whole tree for a root that is a
    leaf (lo None too)."""
    leaves = sorted((n for n in bt.plan.nodes if n.tree == tally.DIGESTS and n.level == 0),
                    key=lambda n: n.items[0][0])
    for i, n in enumerate(leaves):
        for key, body in n.items:
            if key <= block < key + len(tally.DigestsDecode(body, "a DIGESTS element", [])):
                if len(leaves) == 1:
                    return n, None, None
                return n, n.items[0][0], leaves[i + 1].items[0][0] if i + 1 < len(leaves) else None
    raise Unreadable("no DIGESTS leaf of the base names block %u, the kernel's first" % block)


def Verdict(c):
    """The oracle's verdict over the landing the census was taken of: its
    lines, and its last — the sentence the loader stops or jumps on."""
    lines, _says = BootRead(c.medium)
    return lines


def BootCases(bt):
    g = bt.ground
    boot = g.deed["boot"]
    if not boot or not boot["kernel_blocks"]:
        raise Unreadable("the base's Deed names no kernel")
    first, (blocks, why) = boot["kernel_block"], Loaded(boot)
    if why:
        raise Unreadable(why)
    leaf, lo, hi = KernelLeaf(bt, first)
    root = bt.plan.roots[tally.DIGESTS]
    copies = "/".join(str(b) for b in leaf.blocks)
    tail = boot["kernel_bytes"] % BLOCK
    if tail == 0:
        raise Unreadable("the kernel fills its last block: there are no zeros past its end to flip")
    middle, last = blocks // 2, blocks - 1
    held = "%s is attested by the book: %u block(s)" % (WHO, blocks)
    stands = "the book stands on slot 0 (seq 2)"

    def Flip(o, k, at):
        """A byte of the kernel's block `k` flipped, at `at` of its 4096."""
        lba = g.base + (first + k) * SECTORS_PER_BLOCK + at // SECTOR
        raw = bytearray(o.sectors(lba, 1))
        raw[at % SECTOR] ^= 0x5A
        o.Put(lba, bytes(raw))

    def TornNewest(o):
        """Slot 1 at seq 3, the base's slot sealed again there and only its
        first four sectors written: a Tally write cut short."""
        slot = tally.SlotRead(BookGround(o), 0)
        sectors = tally.SlotSectors(g, 1, 3, slot)
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(sectors[:4]))

    def DigestsUnread(o):
        """Slot 1 at seq 3, the base's slot sealed again there with its
        DIGESTS root named by two blocks of the landings' room, where no node
        was ever written."""
        slot = tally.SlotRead(BookGround(o), 0)
        r = slot.roots[tally.DIGESTS]
        ptr = r.Pointer()
        slot.roots[tally.DIGESTS] = tally.Root(r.flags, r.level, r.items, bookread.PtrPut(
            ptr["seq"], ptr["sheets"], [bt.F, bt.F + ptr["sheets"]]))
        o.Put(g.base + tally.SlotBlock(g, 1) * SECTORS_PER_BLOCK, b"".join(tally.SlotSectors(g, 1, 3, slot)))

    def Ring(o):
        for i in range(g.frame["spine_slots"]):
            bt.Zero(o, tally.SlotBlock(g, i) * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK)

    rotten = ("no slot that holds names a Tally that reads" if root.level == 0 else
              "its block 0 (volume block %u) lies in the DIGESTS range of blocks [%u, %s), which does not read"
              % (first, lo, "the end" if hi is None else hi))
    return [
        Case("A1 the kernel BOOT names, attested by the book", lambda o: None, checks=[
            ("the loader stands on slot 0 and attests all %u block(s) of the kernel" % blocks,
             lambda c: Verdict(c)[-2:] == [stands, held])]),
        Case("A2 copy 1 of the DIGESTS leaf naming the kernel's first block rotten",
             lambda o: bt.RotNode(o, leaf, (0,)), must={"NODE_COPY_ROTTEN"}, checks=[
                 ("the loader reads its copy 2 and attests the kernel",
                  lambda c: Verdict(c)[-2:] == ["the DIGESTS node of level 0 at block(s) %s: 1 of its copies do not "
                                                "hold — copy 2 is read" % copies, held])]),
        Case("A3 both copies of the DIGESTS leaf naming the kernel's first block rotten",
             lambda o: bt.RotNode(o, leaf), must={"RANGE_ROTTEN"} if root.level else {"SPINE_NONE"}, checks=[
                 ("the loader has nothing to check the kernel by, and says why",
                  lambda c: Verdict(c)[-1] == "%s is not attested: %s" % (WHO, rotten))]),
        Case("A4 the newest slot torn", TornNewest, must={"SLOT_TORN"}, checks=[
            ("the loader passes it over, stands on slot 0 and attests the kernel",
             lambda c: Verdict(c)[-3:] == ["Spine slot 1 at block %u does not hold — the book is read off another"
                                           % tally.SlotBlock(g, 1), stands, held])]),
        Case("A5 the Spine's ring empty", Ring, must={"SPINE_NONE"}, checks=[
            ("the loader has nothing to check the kernel by",
             lambda c: Verdict(c)[-1] == "%s is not attested: none of the %u slots of its Spine holds"
             % (WHO, g.frame["spine_slots"]))]),
        Case("A6 a byte of the kernel's code flipped", lambda o: Flip(o, middle, 100), must={"DIGEST_MISMATCH"},
             checks=[("the loader names the kernel's block %u" % middle,
                      lambda c: Verdict(c)[-1] == "%s does not match the book: block %u (volume block %u)"
                      % (WHO, middle, first + middle))]),
        Case("A7 a byte of the zeros past the kernel's end in its last block flipped",
             lambda o: Flip(o, last, BLOCK - 1), must={"DIGEST_MISMATCH"}, checks=[
                 ("the kernel's %u bytes are as they were, and the loader names its last block, %u"
                  % (boot["kernel_bytes"], last),
                  lambda c: c.g.Block(first + last)[:tail] == g.Block(first + last)[:tail] and
                  Verdict(c)[-1] == "%s does not match the book: block %u (volume block %u)"
                  % (WHO, last, first + last))]),
        Case("A8 a newer slot whose DIGESTS root reads in neither copy", DigestsUnread, must={"SLOT_FELL_BACK"},
             checks=[("the loader passes slot 1 over for its DIGESTS root, stands on slot 0 and attests the kernel",
                      lambda c: Verdict(c)[-3:] == ["slot 1 (seq 3) names a Tally that does not read: its DIGESTS "
                                                    "root will not read — the next slot is read", stands, held])]),
    ]
