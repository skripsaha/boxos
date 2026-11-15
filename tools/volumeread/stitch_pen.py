"""stitch_pen — a pen that sews stitches the way a kernel does, for the
self-test's landings, and the circle that holds what
it sewed against what the ribbon reads back (stitch_ribbon.py Ribbon)."""

from .bookread import PAYLOAD_BYTES, UNIT_STITCH, UnitSeal
from .ondisk import SECTORS_PER_BLOCK
from . import tally
from .stitch import HeadPut, KIND_NAMES, Place, RecordAgain, RecordDecode, StitchStream

# ---------------------------------------------------------------------------
# A pen: stitches sewn the way a kernel sews them
# ---------------------------------------------------------------------------


class Sewn:
    __slots__ = ("seq", "of", "vlbas", "stream", "starts", "clasp", "sectors")

    def __init__(self, seq, of, vlbas, stream, starts, clasp, sectors):
        self.seq, self.of, self.vlbas, self.stream, self.starts = seq, of, vlbas, stream, starts
        self.clasp, self.sectors = clasp, sectors

    def LostAt(self, k):
        """The bytes a reader loses when sector k alone does not read: from
        where the entry the sector cuts begins to the first entry begun past
        it — said from what the pen laid down, not from the reader's walk."""
        lo, hi = k * PAYLOAD_BYTES, (k + 1) * PAYLOAD_BYTES
        spans = list(zip(self.starts, self.starts[1:] + [len(self.stream)]))
        first = min(s for s, e in spans if s < hi and e > lo)
        return min([s for s, _e in spans if s >= hi] + [len(self.stream)]) - first


class StitchPen:
    """Sews stitches on the ribbon of the newest slot that holds, from its
    `at`: each from the first atom after the last, in whole atoms, on into the
    next_quire past a Quire's end, its StitchHead naming the last one's seq
    and Clasp (or what it is told to)."""

    def __init__(self, over, g):
        slots = [tally.SlotRead(g, i) for i in range(g.frame["spine_slots"])]
        slot = max((s for s in slots if s.state == "valid"), key=lambda s: s.seq)
        r = slot.ribbon
        self.over, self.g, self.came, self.again = over, g, None, False
        self.place = Place((r["quire_block"], r["quire_blocks"]),
                           (r["at"] - r["quire_block"]) * SECTORS_PER_BLOCK)
        self.next = (r["next_block"], r["next_blocks"]) if r["next_blocks"] else None
        self.last, self.clasp, self.tally_seq = slot.stitch_seq, slot.stitch_clasp, slot.tally_seq
        self.next_seq = max([g.top_seq] + [s.seq for s in slots]) + 1

    def Room(self):
        return self.place.Left()

    def Hop(self, back_to=None):
        """On to the start of the next_quire, as a cycle whose stitch the
        medium refused goes on (M-6): `back_to` the last stitch that landed,
        which the next one names."""
        if not self.next:
            raise ValueError("the ribbon names no next_quire to go on in")
        self.came, self.place, self.next = self.place.quire, Place(self.next, 0), None
        if back_to is not None:
            self.last, self.clasp = back_to.seq, back_to.clasp

    def Again(self, back_to):
        """On past a try refused inside the next_quire the cycle went on into
        (§16.4 п.4): the retry on the atom right after the try — never over it
        — naming `back_to`, the last stitch that landed, and the Quire the
        first try came from."""
        self.again = True
        self.last, self.clasp = back_to.seq, back_to.clasp

    def Sew(self, records, flags=0, seq=None, prev_clasp=None, made_ns=0, write=True, names=None,
            fresh=None, of=None):
        """One stitch of `records`; `write` False leaves the medium as it was
        — a stitch that never landed — and goes on as though it had. What a
        landing may make a stitch say that a kernel's would not: `names` (a
        Sewn) the stitch before it, `prev_clasp` that one's Clasp, `fresh`
        its Clasp's count of fresh blocks, `of` its sectors."""
        seq = seq or self.next_seq
        last, clasp = (names.seq, names.clasp) if names is not None else (self.last, self.clasp)
        came = self.came if self.came and (self.place.off == 0 or self.again) else (0, 0)
        self.again = False
        head = HeadPut(flags, seq - last if last else 0, clasp if prev_clasp is None else prev_clasp,
                       seq - self.tally_seq, made_ns, self.next or (0, 0), came)
        stream, starts, clasp = StitchStream(head, records, fresh)
        grid = self.g.atom // 512
        if of is None:
            of = -(-len(stream) // PAYLOAD_BYTES)
            of = -(-of // grid) * grid
        places = [self.place.On(k, self.next) for k in range(of)]
        if None in places:
            raise ValueError("the ribbon has no room for a stitch of %u sectors" % of)
        vlbas = [p.Vlba() for p in places]
        sectors = UnitSeal(stream, starts, UNIT_STITCH, seq, vlbas, self.g.uuid)
        if write:
            for v, sector in zip(vlbas, sectors):
                self.over.Put(self.g.base + v, sector)
        end = self.place.On(of, self.next)
        if end is None or end.quire != self.place.quire:
            self.came, self.next = self.place.quire, None
        self.place = end
        self.last, self.clasp, self.next_seq = seq, clasp, max(self.next_seq, seq + 1)
        return Sewn(seq, of, vlbas, stream, starts, clasp, sectors)


def Circle(ribbon, sewn):
    """[what differs] between stitches the pen sewed and the ribbon read back:
    each sewn on whole, its stream the pen's, and each record of it decoded
    and written again the same bytes (§2.4: one value, one spelling)."""
    wrong, by_seq = [], {u.seq: u for u in ribbon.stitches}
    for s in sewn:
        u = by_seq.get(s.seq)
        if u is None or not u.Sound():
            wrong.append("stitch %u is not sewn on whole" % s.seq)
            continue
        if u.stream[:len(s.stream)] != s.stream:
            wrong.append("stitch %u reads back other bytes" % s.seq)
        for kind, _ign, body, _at in u.records:
            if kind in KIND_NAMES and RecordAgain(kind, RecordDecode(kind, body, u.seq)).body != body:
                wrong.append("stitch %u's %s is written again as other bytes" % (s.seq, KIND_NAMES[kind]))
    return wrong
