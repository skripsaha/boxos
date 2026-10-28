"""stitch_ribbon — the ribbon walked from the slot a mount stands on (spec_v2
§4.5 п.5, pass 1): the stitches sewn on and the Quires the chain runs through,
then the direct scan of what lies past its end, each unit there joined into its
branch (§2.2, §3.9). The records it reads, a unit's three classes and the place
a unit lies in are stitch.py's."""

from .bookread import UNIT_STITCH
from .ondisk import SECTORS_PER_BLOCK
from . import tally
from .stitch import FindUnit, Lies, MEDIUM_LIED, Peek, Place, Unit

# ---------------------------------------------------------------------------
# The ribbon, walked from a slot (§4.5 п.5, pass 1)
# ---------------------------------------------------------------------------


class Ribbon:
    """The ribbon from the `at` of the slot a mount stands on, as pass 1 of
    §4.5 п.5 reads it: the stitches sewn on (`stitches`, each a Unit whole or
    closed and damaged) and the Quires the chain runs through; then the direct
    scan of what lies past its end (§2.2), each unit there joined to the one
    its StitchHead names into branches, and what each branch is (§3.9) —
    `said` [(code, severity, sentence)] for the census, `torn` the stitch of
    this history torn right after the end, `lied` when the medium is shown
    not to have kept what it confirmed.

    `taken` is the ground the book and the Tally's files hold, which a
    next_quire may not name (§3.7's table)."""

    def __init__(self, g, slot, taken=(), scan=True):
        self.g, self.slot, self.taken = g, slot, list(taken)
        self.stitches, self.quires, self.said, self.torn, self.past = [], [], [], None, []
        self.lied = bool(slot.flags & tally.SPINE_MEDIUM_LIED)
        r = slot.ribbon
        self.seq, self.clasp = slot.stitch_seq, slot.stitch_clasp
        self.quire = (r["quire_block"], r["quire_blocks"])
        self.next = None
        if not Lies(g, self.quire) or not r["quire_block"] <= r["at"] < sum(self.quire):
            self.Say("RIBBON_TORN", "damage", "slot %u puts the ribbon on at block %u of the Quire %u+%u, "
                     "which does not hold it or lies outside the data run or off an atom — no stitch past "
                     "its Tally is read" % (slot.index, r["at"], *self.quire))
            self.end = None
            return
        self.end = Place(self.quire, (r["at"] - r["quire_block"]) * SECTORS_PER_BLOCK)
        self.quires.append(self.quire)
        self.Named((r["next_block"], r["next_blocks"]), "slot %u's SpineRibbon" % slot.index)
        self._Sew()
        if scan:
            self._Scan()

    def Say(self, code, severity, text):
        self.said.append((code, severity, text))

    def Named(self, quire, who):
        """`quire` taken as the next_quire when one is named and it may be
        (§3.7's table: in the data run, on an atom, clear of the book and of
        the files) — said when it may not."""
        if not quire[1] or quire == self.quire:
            return
        if not Lies(self.g, quire) or any(s < quire[0] + quire[1] and quire[0] < s + n for s, n in self.taken):
            self.Say("RIBBON_TORN", "damage", "%s names Quire %u+%u as the ribbon's next, which lies "
                     "outside the data run, off an atom, or on the book or a file — the ribbon is not "
                     "taken past it there" % (who, quire[0], quire[1]))
            return
        self.next = quire
        if quire not in self.quires:
            self.quires.append(quire)

    def _Follows(self, u, at_next):
        """Does the StitchHead of `u` name the last stitch taken as the one
        before it — by its seq and by its Clasp (§2.7, H-2) — and, begun at
        the start of the next_quire, the Quire the last one ended in as the
        one it came from (§4.5 п.5 (б))? A StitchHead that does not read
        names nothing either way (rule (в) takes such a unit on the word of
        the stitch after it)."""
        h = u.head
        if h is None:
            return True
        if h.prev_clasp != self.clasp or h.prev != (u.seq - self.seq if self.seq else 0):
            return False
        return not at_next or h.came == self.quire

    def _Take(self, u):
        self.stitches.append(u)
        self.seq, self.clasp = u.seq, u.clasp
        if u.places[-1].quire != self.quire:
            self.quire, self.next = u.places[-1].quire, None
            if self.quire not in self.quires:
                self.quires.append(self.quire)
        self.end = u.end
        if u.head is not None:
            self.Named(u.head.next, "stitch seq %u" % u.seq)
            self.lied = self.lied or bool(u.head.flags & MEDIUM_LIED)
        wrong = u.LeadsWrong()
        if wrong:
            self.Say("RECORD_SHAPE", "damage", "stitch %u's sector(s) %s name lead(s) %s where its records "
                     "begin at %s (§2.1) — were a sector before them to rot, its walk would be taken up in "
                     "the wrong place" % (u.seq, ", ".join(str(k) for k, _h, _w in wrong),
                                          ", ".join(str(h) for _k, h, _w in wrong),
                                          ", ".join(str(w) for _k, _h, w in wrong)))

    def _Sew(self):
        """Rules (а)-(г) of §4.5 п.5, until none takes the next unit; a seq
        that rises at every stitch bounds the walk, and the data run's length
        bounds it again (`hops ≥ data_blocks`, meta_pool_mount.c). When rule
        (а) and rule (б) both give a whole stitch that names the last one by
        its Clasp, the one of the larger seq is taken (§16.3 п.11, M-6): a
        retry always takes a new, larger seq and carries the same records of
        its cycle, and the waiters were answered by it — the other is the
        pieces of a retry, said, and not applied.

        Rules (б) and (б′) are weighed only where rule (а) gives no whole
        stitch, or gives one that nothing past it names (_Kept): a writer
        lays nothing that names a try the medium refused (settle/ribbon.c
        RibbonRetry), so a stitch named after it was kept, and no retry of
        the one before it lies anywhere. The unit read to tell is the next
        step's rule (а): every unit of the chain is read once."""
        hops, ahead = 0, None
        while self.end is not None and hops <= self.g.data_end:
            hops += 1
            key = (self.end.quire, self.end.off, self.next, self.seq)
            u = ahead[1] if ahead is not None and ahead[0] == key else FindUnit(self.g, self.end, self.next, self.seq)
            here = u if u is not None and u.Sound() and self._Follows(u, False) else None
            kept, ahead = (self._Kept(here) if here is not None and self.next else (False, None))
            v = there = None
            if self.next and not kept:
                there, blank = self._Further()
                if here is None and there is None and not blank and self.end.quire != self.next:
                    v = FindUnit(self.g, Place(self.next, 0), None, self.seq)
            if here is not None or there is not None:
                take = self._Retried(here, there)
                self._Take(take)
                if take is not here:
                    ahead = None
                continue
            if not any(self._Closed(c, c is v) for c in (u, v)
                       if c is not None and not c.Sound() and not c.bad):
                return

    def _Kept(self, s):
        """Does a unit past `s` name it — its StitchHead, read, naming its seq
        and its Clasp — at its end or at the start of the next_quire it names?
        Then the writer was told `s` was kept. [that, and (the key the unit at
        its end was read by, that unit) for the next step]."""
        hop = s.head.next if Lies(self.g, s.head.next) else (self.next if s.places[-1].quire == self.quire else None)
        f = FindUnit(self.g, s.end, hop, s.seq)
        names = lambda x: x is not None and x.head is not None and x.Names() == s.seq and x.head.prev_clasp == s.clasp
        kept = names(f) or (bool(hop) and s.end.quire != hop and names(FindUnit(self.g, Place(hop, 0), None, s.seq)))
        return kept, ((s.end.quire, s.end.off, hop, s.seq), f)

    def _Further(self):
        """Rules (б) and (б′) (§4.5 п.5, §16.4 п.4): the stitch the ribbon
        goes on with in the next_quire — the whole one at its start naming the
        last stitch taken by its Clasp and the Quire it ended in as the one it
        came from, or, where none lies there, the first such further in: a
        unit holding a sector there stepped over to its end, a sector holding
        none of a seq past the last one stepped over alone — a retry is
        written on the atom past the try refused before it, never over it.
        While a whole stitch naming the last one so, with a larger seq, lies
        right after the one found — a try refused that landed, and the retry
        after it (§16.3 п.11) — that one is taken in its place. What was
        walked over is said as the pieces of a retry: no lie. The walk ends at
        the next_quire's end. [the stitch, None when nothing there goes on;
        whether no sector there holds a seq past the last one]."""
        n = self.next
        total = n[1] * SECTORS_PER_BLOCK
        self.g.Run(n[0] * SECTORS_PER_BLOCK, total)
        pos, passed, take, blank = 0, [], None, True
        while pos < total and take is None:
            p = Place(n, pos)
            _raw, h = self.g.Sector(p.Vlba())
            if h is None or h["kind"] != UNIT_STITCH or h["seq"] <= self.seq:
                pos += 1
                continue
            blank = False
            if h["at"] == 0:
                u = Unit(self.g, p, h["of"], h["seq"], Peek(self.g, p))
                if u.Sound() and self._Follows(u, True):
                    take = u
                    break
            if h["seq"] not in passed:
                passed.append(h["seq"])
            pos = pos - h["at"] + h["of"] if h["at"] < h["of"] else pos + 1
        while take is not None:
            hop = take.head.next if Lies(self.g, take.head.next) else None
            f = FindUnit(self.g, take.end, hop, take.seq)
            if f is None or not f.Sound() or not self._Follows(f, True):
                break
            passed.append(take.seq)
            take = f
        if take is not None and (take.start.quire != n or take.start.off):
            self.Say("DEAD_BRANCH", "note", "no whole stitch at the start of the next_quire %u+%u names stitch %u "
                     "and goes on from there: seq %s walked over to seq %u at its sector %u, which names it and the "
                     "Quire the first try came from — the pieces of a retry, not applied (§16.4 п.4)"
                     % (n[0], n[1], self.seq, ", ".join(map(str, passed)) if passed else "none that reads",
                        take.seq, take.start.off))
        return take, blank

    def _Retried(self, here, there):
        """The stitch rule (а) gives (`here`) or rules (б), (б′) give
        (`there`); when both do, the one of the larger seq, the other said as
        the pieces of a retry (§16.3 п.11) — no lie: the medium kept a write
        it refused, and the cycle was written again."""
        if here is None or there is None or here.seq == there.seq:
            return here if here is not None else there
        take, left = (here, there) if here.seq > there.seq else (there, here)
        self.Say("DEAD_BRANCH", "note", "stitch seq %u at the end of the ribbon and seq %u in its "
                 "next_quire are both whole and both name stitch %u by its Clasp — a write the medium refused "
                 "and kept, and its retry (M-6): seq %u, the larger, is taken; seq %u is the pieces of a retry "
                 "and is not applied (§16.3 п.11)" % (here.seq, there.seq, self.seq, take.seq, left.seq))
        return take

    def _Closed(self, u, at_next):
        """Rule (в): `u` closed and damaged when its StitchHead, if it reads,
        names the last stitch taken (an identity rot does not lend a stitch
        of another history), and the sound stitch after it names its seq and,
        where its Clasp reads, that Clasp."""
        if not self._Follows(u, at_next):
            return False
        hop = (u.head.next if u.head is not None and Lies(self.g, u.head.next) else None) or self.next
        f = FindUnit(self.g, u.end, hop, u.seq)
        if f is None or not f.Sound() or f.Names() != u.seq or \
                (u.clasp is not None and f.parsed.prev_clasp != u.clasp):
            return False
        self._Take(u)
        # A sector of another unit where this one's should be: an unordered
        # write or a lie, for a stitch with a larger seq lies after it (§3.9).
        foreign = [k for k in u.Lost() if u.places[k] is not None and
                   self.g.Sector(u.places[k].Vlba())[1] is not None]
        self.lied = self.lied or bool(foreign)
        self.Say("STITCH_TORN", "damage", "stitch %u is closed and damaged: sector(s) %s of its %u will "
                 "not read, and %u byte(s) of its records with them; what reads by `lead` is replayed "
                 "(§3.9)%s" % (u.seq, ", ".join(map(str, u.Lost())), u.of, u.lost,
                               "; sector(s) %s hold another unit's — a write out of order or a lie, and "
                               "a later stitch landed: MEDIUM_LIED" % ", ".join(map(str, foreign))
                               if foreign else ""))
        self._Take(f)
        return True

    def _Scan(self):
        """The direct scan (§2.2): the rest of the last Quire and all of its
        next, every sector of the ribbon past the last seq taken — each unit
        found, what it is said to be by the branch it belongs to (§3.9), and
        a unit no stitch is shaped like said as such."""
        if self.end is None:
            return
        room = self.end.Left() + (self.next[1] * SECTORS_PER_BLOCK if self.next else 0)
        places = [self.end.On(k, self.next) for k in range(room)]
        units = {}
        for i, p in enumerate(places):
            _raw, h = self.g.Sector(p.Vlba())
            if h is not None and h["kind"] == UNIT_STITCH and h["seq"] > self.seq and h["seq"] not in units \
                    and i >= h["at"]:
                units[h["seq"]] = Unit(self.g, places[i - h["at"]], h["of"], h["seq"], self.next)
        for s in sorted(units):
            u = units[s]
            if u.bad:
                self.Say("RECORD_SHAPE", "damage", "a unit of the ribbon at sector %u, seq %u, is %s: no "
                         "stitch is so shaped (§1.5, §2.2) — the ribbon is not taken past it, and its "
                         "records are not read" % (u.start.Vlba(), s, u.bad))
        tail = FindUnit(self.g, self.end, self.next, self.seq)
        self.torn = tail if tail is not None and not tail.Sound() and not tail.bad else None
        self.past = [units[s] for s in sorted(units) if not units[s].bad]
        for root, members in self._Branches():
            self._Branch(root, members)

    def _Branches(self):
        """[(root, members)]: the units past the end joined into branches,
        each to the unit its StitchHead names when that one lies past the end
        too (a seq named is always below the namer's, so the walk ends)."""
        by_seq, roots = {w.seq: w for w in self.past}, {}
        for w in self.past:
            r = w
            while r.Names() in by_seq:
                r = by_seq[r.Names()]
            roots.setdefault(r.seq, []).append(w)
        return [(by_seq[s], roots[s]) for s in sorted(roots)]

    def _Branch(self, r, members):
        """What one branch past the end is (§3.9's table), said once, by what
        its first unit `r` names and what its units name of each other."""
        seqs, n, L = ", ".join(str(w.seq) for w in members), r.Names(), self.seq
        tail = self.torn is not None and r.seq == self.torn.seq
        sewn = {u.seq: u.clasp for u in self.stitches}
        sewn[self.slot.stitch_seq] = self.slot.stitch_clasp
        by_seq = {w.seq: w for w in self.past}
        lost = sorted({w.Names() for w in members if w.Names() is not None and w.Names() > L and
                       (w.Names() not in by_seq or not by_seq[w.Names()].Sound())})
        # A stitch of the chain whose Clasp rotted names no Clasp to hold
        # the one after it to: that one is taken at its word.
        ours = n in sewn and (sewn[n] is None or r.head.prev_clasp == sewn[n])
        if n is not None and not ours and (n in sewn or n <= L):
            if tail:
                self.torn = None
            self.Say("CLASP_STRANGER", "note", "seq %s past the end of the ribbon: a dead branch of another "
                     "history — stitch seq %u names stitch %u with %s; it is not sewn on" % (
                         seqs, r.seq, n, "a Clasp that is not its own (0x%08X, not 0x%08X)"
                         % (r.head.prev_clasp, sewn[n]) if n in sewn else "which this ribbon does not hold"))
        elif lost:
            self.lied = True
            self._Lost(r, members, lost, seqs)
        elif tail and (n is None or n == L):
            self._Torn(self.torn)
        elif n is None:
            self.Say("DEAD_BRANCH", "note", "past the end of the ribbon lie pieces of seq %s whose StitchHead "
                     "does not read: a retry, or nothing known — no loss of what settled is shown" % seqs)
        elif not r.Sound():
            self.Say("DEAD_BRANCH", "note", "past the end of the ribbon lie pieces of seq %s naming stitch %u "
                     "of it: a retry cut while it was written — no loss of what settled is shown" % (seqs, n))
        elif n == L:
            self.Say("DEAD_BRANCH", "damage", "stitch seq %u names the end of the ribbon, seq %u, by its Clasp, "
                     "and lies where the ribbon does not go on — not the first atom after it, nor the start "
                     "of its next_quire naming its Quire (§4.5 п.5): seq %s, settled, are not taken"
                     % (r.seq, L, seqs))
        else:
            # A whole root naming a stitch of the chain before its end, by
            # that stitch's own Clasp: two stitches name one, and the ribbon
            # goes on by the one it holds — said loud, no lie shown (§16.3
            # п.10). A retry is not this: it names the end (п.11).
            self.Say("DEAD_BRANCH", "damage", "the ribbon forks at stitch %u: seq %u names it by its Clasp, as "
                     "seq %u of the ribbon does, and lies past the ribbon's end — seq %s, settled, are not "
                     "taken (§16.3 п.10)" % (n, r.seq, next(u.seq for u in self.stitches if u.seq > n), seqs))

    def _Lost(self, r, members, lost, seqs):
        """A branch some unit of which names one that settled — for it
        began only after that one's flush — and is not on the medium whole."""
        absent = [m for m in lost if m not in {w.seq for w in self.past}]
        if r.Sound() and r.Names() in absent:
            self.Say("DEAD_BRANCH", "damage", "the lie is proven: stitch seq %u lies past the end of the "
                     "ribbon and names seq %u, which the medium does not hold — a flush that lied, the "
                     "cycles after it written over a hole (FAST'13, H-2); seq %s are not applied — "
                     "MEDIUM_LIED" % (r.seq, r.Names(), seqs))
            return
        namer = min((w for w in members if w.Names() in lost), key=lambda w: w.seq)
        self.Say("DEAD_BRANCH", "damage", "what settled is lost: seq %s past the end of the ribbon — stitch "
                 "seq %u names %u, begun only after %u had settled, and %u %s (H-3) — MEDIUM_LIED"
                 % (seqs, namer.seq, namer.Names(), namer.Names(), namer.Names(),
                    "is not on the medium" if namer.Names() in absent else "does not read whole"))

    def _Torn(self, t):
        """The stitch right after the end, torn, and nothing past it names
        it: a cut while it was written — or, every sector of it there and
        some sum or its Clasp not holding, a cut or rot, said loud (M-4)."""
        if t.Whole():
            self.Say("RIBBON_TORN", "damage", "the last cycle does not read whole: stitch seq %u has all %u "
                     "sectors and its stream or its Clasp does not hold — a cut or rot (M-4)" % (t.seq, t.of))
        else:
            self.Say("RIBBON_TORN", "note", "the ribbon ends on stitch %u; seq %u past it is torn — %u of "
                     "its %u sectors hold, a cut while it was written; its numbers are not given again"
                     % (self.seq, t.seq, t.of - len(t.Lost()), t.of))

    def Seen(self):
        """[(kind, body, seq)] of every record read of what was not sewn on —
        the torn tail and what lies past — for the numbers it gave, which are
        never given again (§4.5 п.8)."""
        return [(r[0], r[2], u.seq) for u in ([self.torn] if self.torn else []) + self.past for r in u.records]
