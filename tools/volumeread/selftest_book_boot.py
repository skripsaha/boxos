"""selftest_book_boot — group A: the landings the UEFI loader's Lectern is held
to: the kernel BOOT names on the book as a binder lays
it, attested; the DIGESTS leaf that names its first block rotten in its first
copy — the second read, attested — and in both — not attested, and loaded
for nothing shows it broken; the newest slot torn, and a newer slot whose
DIGESTS root reads in neither copy — the older slot stood on, attested; the
Spine's ring empty — not attested, loaded; a byte of the kernel's code
flipped, and a byte of the zeros past its end in its last block — the block
named, and no kernel the book names by role attested in its place: the loader
stops. Then a second kernel the book names by the role `boot`, BOOT's flipped:
bound into the Tally, or born on the ribbon past it — attested in BOOT's
place; the ribbon torn past its birth, the tear not applied; an Undone of its
Born — no such kernel, the loader stops; a Shaped of it whose base does not
hold — passed over, the kernel as it was born; its stitch at the start of
the ribbon's next_quire (rule (б)); its stitch closed and damaged, a sector
of it lost and a stitch after it naming it (rule (в)) — the spare attested by
what of its Born reads. Then gangway.settings, the file the loader asks for by
name (bootread.py Called): bound into the Tally, born on the ribbon, a file of
another name renamed so on the ribbon — read and attested, its bytes the
person's; kept deleted on the ribbon — none is read; a byte of it flipped —
its block named, not handed over; two files of the name — the first made
read; born with its bytes in a Slip over a hole — read as the Slip lays them.
Each a fork of the base
selftest_book.py lays, the census's words for it, and the verdict bootread.py
says — which the host stand (tools/gangway_stand.sh) then holds the Lectern
itself to, line for line, on every one of them written out by --emit; and
logcheck's bookboot boots A7 and A9 through OVMF."""

from . import binder, bookread, ondisk, stitch, tally
from .bootread import CALLED, CALLED_MOST, WHO, BootRead, Loaded, Succession
from .bookground import BookGround
from .medium import Unreadable
from .ondisk import BLOCK, SECTOR, SECTORS_PER_BLOCK
from .selftest_book import LANDING_ROOM, Case

STOPS = "no kernel on this volume may be jumped into: "


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
    """The oracle's lines over the landing the census was taken of: its last
    the sentence the loader stops or jumps on."""
    lines, _says = BootRead(c.medium)
    return lines


class Spare:
    """A second kernel for the landings: build/kernel.bin's bytes as the base
    holds them at BOOT, a file `spare.bin` of fid `fid` kept in the role `boot`
    (ondisk.FILE_ROLE_BOOT, in its flags — it wears no tag) at block `start` —
    the end of the last free run of the base past the landings' own room — and
    one block more past it for a landing's own (`junk`)."""

    def __init__(self, bt, first, blocks, size):
        g = bt.ground
        self.blocks, self.size, self.fid = blocks, size, bt.fid
        self.data = [g.Block(first + k) for k in range(blocks)]
        self.digests = [bookread.Integrity(raw, bt.lanes) for raw in self.data]
        free = ondisk.RunsSubtract(bt.c0.free_runs, [(bt.F, LANDING_ROOM)])
        room = [(s, n) for s, n in free if n >= blocks + 1]
        if not room:
            raise Unreadable("the image has no free run of %u blocks for a second kernel" % (blocks + 1))
        s, n = room[-1]
        self.start, self.junk = s + n - blocks - 1, s + n - 1

    def Lay(self, bt, o):
        for k, raw in enumerate(self.data):
            bt.PutBlock(o, self.start + k, raw)

    def Born(self):
        return stitch.Born(fid=self.fid, flags=ondisk.FILE_ACTIVE | ondisk.FILE_ROLE_BOOT, size=self.size,
                           created_ns=1, modified_ns=1, name=b"spare.bin", tags=[],
                           runs=[stitch.Data(self.start, self.blocks)], digests=self.digests)

    def Records(self):
        return [self.Born()]


class Note:
    """gangway.settings for the landings: the person's `text`, a file of fid
    `fid` at block F + `at` of the landings' room, one block — its bytes, the
    rest of the block zeros, and their digest."""

    def __init__(self, bt, fid, at, text, name=CALLED):
        self.fid, self.text, self.name, self.block = fid, text, name, bt.F + at
        self.raw = text + bytes(BLOCK - len(text))
        self.digest = bookread.Integrity(self.raw, bt.lanes)

    def Lay(self, bt, o):
        bt.PutBlock(o, self.block, self.raw)

    def State(self):
        return tally.FileState(self.fid, 1, ondisk.FILE_ACTIVE, len(self.text), 1, 1, self.name, [],
                               [tally.Run(False, self.block, 1, 1)])

    def Born(self):
        return stitch.Born(fid=self.fid, flags=ondisk.FILE_ACTIVE, size=len(self.text), created_ns=1, modified_ns=1,
                           name=self.name, tags=[], runs=[stitch.Data(self.block, 1)], digests=[self.digest])


def BoundNotes(bt, notes):
    """The base's Tally bound again where it lies with the notes in it, laid:
    a plant."""
    content = tally.Content.Copy(bt.content)
    for n in notes:
        content.files[n.fid] = n.State()
        content.digests[n.block] = n.digest
    content.next_fid = max(n.fid for n in notes) + 1
    plan = binder.Bind(content, bt.ground, a=bt.plan.a)
    if any(plan.next[0] <= n.block < plan.next[0] + plan.next[1] for n in notes):
        raise Unreadable("the Tally bound with gangway.settings would lie over it")

    def Plant(o):
        for n in notes:
            n.Lay(bt, o)
        binder.BindWrite(o, bt.ground, plan)
    return Plant


def Called(c):
    """The oracle's reading of the landing with gangway.settings asked for."""
    return Succession(BookGround(c.medium), ((CALLED, CALLED_MOST),))


def BoundSpare(bt, sp):
    """The base's Tally bound again where it lies with the spare in it, as
    mkfs binds a volume it lays two kernels on: (content, plan)."""
    content = tally.Content.Copy(bt.content)
    content.files[sp.fid] = tally.FileState(sp.fid, 1, ondisk.FILE_ACTIVE | ondisk.FILE_ROLE_BOOT, sp.size, 1, 1,
                                            b"spare.bin", [], [tally.Run(False, sp.start, sp.blocks, 1)])
    content.next_fid = sp.fid + 1
    for k, d in enumerate(sp.digests):
        content.digests[sp.start + k] = d
    plan = binder.Bind(content, bt.ground, a=bt.plan.a)
    if plan.a < sp.junk + 1 and sp.start < plan.next[0] + plan.next[1]:
        raise Unreadable("the Tally bound with a second kernel would lie over it at block %u" % sp.start)
    return content, plan


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
    loaded = "no kernel is attested by the book: %s is jumped into not attested — nothing shows it broken" % WHO
    broken = "%s does not match the book: block %u (volume block %u)" % (WHO, middle, first + middle)
    sp = Spare(bt, first, blocks, boot["kernel_bytes"])
    spare = "kernel fid %u is attested by the book: %u block(s)" % (sp.fid, blocks)
    bound = BoundSpare(bt, sp)

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

    def TwoBound(o):
        sp.Lay(bt, o)
        binder.BindWrite(o, g, bound[1])
        Flip(o, middle, 100)

    def Ribbon(after=(), hop=False):
        """The spare born on the ribbon in a stitch of its own — `hop`, at the
        start of the next_quire — then `after` (o, pen, the stitch it was born
        in); BOOT's code flipped."""
        def Plant(o):
            sp.Lay(bt, o)
            pen = bt.Pen(o)
            if hop:
                pen.Hop()
            born = pen.Sew(sp.Records(), flags=stitch.DATA_FLUSHED)
            for step in after:
                step(o, pen, born)
            Flip(o, middle, 100)
        return Plant

    def TornLabel(o, pen, born):
        """A stitch taking the role off the spare, cut while it was written:
        its last four sectors never landed."""
        s = pen.Sew([stitch.Labeled(fid=sp.fid, base=1, flags=(0, ondisk.FILE_ROLE_MASK))],
                    flags=stitch.DATA_FLUSHED)
        bt.Zero(o, s.vlbas[4], s.of - 4)

    def UndoneBorn(o, pen, born):
        pen.Sew([stitch.Undone(1, 2, [sp.fid])], flags=stitch.DATA_FLUSHED)

    def StrayBase(o, pen, born):
        """A Shaped putting a block of no kernel in place of the spare's first,
        naming as its base a gen the spare never stood at."""
        junk = bt.Pattern(7)
        bt.PutBlock(o, sp.junk, junk)
        pen.Sew([stitch.Shaped(fid=sp.fid, base=2, size=sp.size, mtime_ns=1,
                               splices=[(0, 1, [stitch.Data(sp.junk, 1)])],
                               digests=[bookread.Integrity(junk, bt.lanes)])], flags=stitch.DATA_FLUSHED)

    def Damaged(o, pen, born):
        """A stitch after the spare's, and the last sector of the spare's
        spoilt: what lies before it reads by `lead`."""
        pen.Sew([stitch.UseContext(b"after")], flags=stitch.DATA_FLUSHED)
        bt.Rot(o, born.vlbas[-1])

    text = b"# what I like\nstartup all\ncores 2\n"
    note = Note(bt, bt.fid, 0, text)
    other = Note(bt, bt.fid + 1, 1, b"cores 1\n")
    draft = Note(bt, bt.fid, 0, text, name=b"draft.txt")
    noted = "%s (fid %u) is attested by the book: 1 block(s)" % (CALLED.decode(), note.fid)

    def NoteBorn(o):
        note.Lay(bt, o)
        bt.Pen(o).Sew([note.Born()], flags=stitch.DATA_FLUSHED)

    def NoteRenamed(o):
        BoundNotes(bt, [draft])(o)
        bt.Pen(o).Sew([stitch.Labeled(fid=draft.fid, base=2, name=CALLED)], flags=stitch.DATA_FLUSHED)

    def NoteDeleted(o):
        BoundNotes(bt, [note])(o)
        bt.Pen(o).Sew([stitch.Labeled(fid=note.fid, base=2, keep=1 << 62)], flags=stitch.DATA_FLUSHED)

    def NoteFlipped(o):
        BoundNotes(bt, [note])(o)
        bt.Rot(o, note.block * SECTORS_PER_BLOCK, 10)

    def NoteSlipped(o):
        bt.Pen(o).Sew([stitch.Born(fid=note.fid, flags=ondisk.FILE_ACTIVE, size=len(text), created_ns=1, modified_ns=1,
                                   name=CALLED, tags=[], runs=[stitch.Hole(1)], digests=[], slip=(0, text))],
                      flags=stitch.DATA_FLUSHED)

    def Read(c, fid=None):
        """The note read, attested, its bytes the person's text."""
        s = Called(c)
        return s.found.get(CALLED) == "held" and s.content.get(CALLED) == text and s.lines[-1] == noted.replace(
            "fid %u)" % note.fid, "fid %u)" % (note.fid if fid is None else fid))

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
                 ("the loader has nothing to check the kernel by, says why, and loads it: nothing shows it broken",
                  lambda c: "%s is not attested: %s" % (WHO, rotten) in Verdict(c) and Verdict(c)[-1] == loaded)]),
        Case("A4 the newest slot torn", TornNewest, must={"SLOT_TORN"}, checks=[
            ("the loader passes it over, stands on slot 0 and attests the kernel",
             lambda c: Verdict(c)[-3:] == ["Spine slot 1 at block %u does not hold — the book is read off another"
                                           % tally.SlotBlock(g, 1), stands, held])]),
        Case("A5 the Spine's ring empty", Ring, must={"SPINE_NONE"}, checks=[
            ("the loader has nothing to check the kernel by, and loads it",
             lambda c: Verdict(c)[-2:] == ["%s is not attested: none of the %u slots of its Spine holds"
                                           % (WHO, g.frame["spine_slots"]), loaded])]),
        Case("A6 a byte of the kernel's code flipped", lambda o: Flip(o, middle, 100), must={"DIGEST_MISMATCH"},
             checks=[("the loader names the kernel's block %u, finds no other kernel, and stops" % middle,
                      lambda c: broken in Verdict(c) and Verdict(c)[-1].startswith(STOPS))]),
        Case("A7 a byte of the zeros past the kernel's end in its last block flipped",
             lambda o: Flip(o, last, BLOCK - 1), must={"DIGEST_MISMATCH"}, checks=[
                 ("the kernel's %u bytes are as they were, the loader names its last block, %u, and stops"
                  % (boot["kernel_bytes"], last),
                  lambda c: c.g.Block(first + last)[:tail] == g.Block(first + last)[:tail] and
                  "%s does not match the book: block %u (volume block %u)" % (WHO, last, first + last) in Verdict(c)
                  and Verdict(c)[-1].startswith(STOPS))]),
        Case("A8 a newer slot whose DIGESTS root reads in neither copy", DigestsUnread, must={"SLOT_FELL_BACK"},
             checks=[("the loader passes slot 1 over for its DIGESTS root, stands on slot 0 and attests the kernel",
                      lambda c: Verdict(c)[-3:] == ["slot 1 (seq 3) names a Tally that does not read: its DIGESTS "
                                                    "root will not read — the next slot is read", stands, held])]),
        Case("A9 two kernels bound in the Tally, BOOT's code flipped", TwoBound, must={"DIGEST_MISMATCH"}, checks=[
            ("the loader names BOOT's block %u, then the same of fid %u, and attests the spare, fid %u, kept in the boot role"
             % (middle, bt.c0.boot_fid, sp.fid),
             lambda c: broken in Verdict(c) and "kernel fid %u does not match the book: block %u (volume block %u)"
             % (bt.c0.boot_fid, middle, first + middle) in Verdict(c) and Verdict(c)[-1] == spare)]),
        Case("A10 a second kernel born on the ribbon past the Tally, BOOT's code flipped", Ribbon(),
             must={"DIGEST_MISMATCH"}, checks=[
                 ("the loader sews the stitch on and attests the spare, fid %u, by the digests its Born carries"
                  % sp.fid, lambda c: broken in Verdict(c) and Verdict(c)[-1] == spare)]),
        Case("A11 the ribbon torn past the spare's birth", Ribbon([TornLabel]), must={"DIGEST_MISMATCH"},
             may={"RIBBON_TORN"}, checks=[
                 ("the torn stitch taking its role off is not applied: the spare, fid %u, is attested" % sp.fid,
                  lambda c: "the ribbon past slot 0's Tally: 1 stitch(es) sewn on, seq 3 to 3" in Verdict(c) and
                  Verdict(c)[-1] == spare)]),
        Case("A12 an Undone of the spare's Born", Ribbon([UndoneBorn]), must={"DIGEST_MISMATCH"}, checks=[
            ("the spare is never born: no kernel the book names holds, and the loader stops",
             lambda c: "stitch 4's Undone passes over stitch 3's records of 1 file(s)" in Verdict(c) and
             not any("kernel fid %u " % sp.fid in x for x in Verdict(c)) and Verdict(c)[-1].startswith(STOPS))]),
        Case("A13 a Shaped of the spare whose base does not hold", Ribbon([StrayBase]),
             must={"DIGEST_MISMATCH", "BASE_MISMATCH"}, checks=[
                 ("the Shaped is passed over, and the spare, fid %u, is attested as it was born" % sp.fid,
                  lambda c: "stitch 4's Shaped of fid %u does not fit the file: its base names the file at 2 and the "
                  "file stands at 3 — the file stays as it last matched" % sp.fid in Verdict(c) and
                  Verdict(c)[-1] == spare)]),
        Case("A14 the spare's stitch at the start of the ribbon's next_quire", Ribbon(hop=True),
             must={"DIGEST_MISMATCH"}, checks=[
                 ("rule (б) sews it on, and the spare, fid %u, is attested" % sp.fid,
                  lambda c: "the ribbon past slot 0's Tally: 1 stitch(es) sewn on, seq 3 to 3" in Verdict(c) and
                  Verdict(c)[-1] == spare)]),
        Case("A15 the spare's stitch closed and damaged", Ribbon([Damaged]), must={"DIGEST_MISMATCH", "STITCH_TORN"},
             checks=[("rule (в) sews it on with the stitch after it, and the spare, fid %u, is attested by what of "
                      "its Born reads" % sp.fid,
                      lambda c: "the ribbon past slot 0's Tally: 2 stitch(es) sewn on, seq 3 to 4" in Verdict(c) and
                      Verdict(c)[-1] == spare)]),
        Case("A16 gangway.settings bound in the Tally", BoundNotes(bt, [note]), checks=[
            ("the loader reads gangway.settings, fid %u, attested, its bytes the person's" % note.fid, Read)]),
        Case("A17 gangway.settings born on the ribbon past the Tally", NoteBorn, checks=[
            ("the loader follows its Born and reads it by the digest the Born carries", Read)]),
        Case("A18 a file of another name renamed gangway.settings on the ribbon", NoteRenamed, checks=[
            ("the loader follows the file its Labeled names so from the Tally on and reads it",
             lambda c: Read(c, draft.fid))]),
        Case("A19 gangway.settings kept deleted on the ribbon", NoteDeleted, checks=[
            ("a file kept deleted is none the loader reads",
             lambda c: Called(c).lines[-1] == "no file on this volume is called %s" % CALLED.decode() and
             Called(c).found.get(CALLED) is None)]),
        Case("A20 a byte of gangway.settings flipped", NoteFlipped, must={"DIGEST_MISMATCH"}, checks=[
            ("the loader names its block and hands nothing over",
             lambda c: Called(c).lines[-1] == "%s (fid %u) does not match the book: block 0 (volume block %u)"
             % (CALLED.decode(), note.fid, note.block) and Called(c).found.get(CALLED) is None)]),
        Case("A21 two files called gangway.settings", BoundNotes(bt, [note, other]), checks=[
            ("the loader reads the first made, fid %u, and says there are two" % note.fid,
             lambda c: "2 files are called %s — fid %u, the first made, is read" % (CALLED.decode(), note.fid)
             in Called(c).lines and Read(c))]),
        Case("A22 gangway.settings born with its bytes in a Slip over a hole", NoteSlipped, checks=[
            ("the loader reads the bytes the Slip lays on its one block",
             lambda c: Called(c).found.get(CALLED) == "held" and Called(c).content.get(CALLED) == text)]),
    ]
