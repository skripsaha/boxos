"""selftest — every census rule planted into an image and caught, the cut logic
proved on a synthetic log, and the format held against the C headers.

A rule nobody has seen fire is a rule that may not work. Each case here lays
one kind of damage — or one state a kernel cycle leaves — over the image in
memory (an OverlayMedium — the image file is never written), runs the census on
it, and holds the findings, and the mount's decisions, against what that state
must produce. The image has to be consistent to begin with (C0), because every
case is measured as a difference from it; and it is sealed, as mkfs leaves every
volume, so a plant that changes the pool or the registry either seals again what
it laid down (Reseal — what a Settle cycle's last write leaves) or leaves the
seal as it was and is read the way the mount reads a seal the medium does not
bear out. NoSeal writes both copies of the Ledger the way a kernel before the
seal writes them.

`--emit CASE --out IMG` writes one case's overlay out as a new image, for a
harness to boot: logcheck's `census` scenario boots some of them and holds the
kernel's own lines against this package's prediction.

The book of format v2 has landings of its own (selftest_book.py): over the
book laid on the v1 image's files, or over a format-v2 image's own book, when
that is what self-test is handed — no case of format v1 has a volume to run
on then, and X2, which only they hear, is said not held.
"""

from .medium import OverlayMedium, StandOnDeed, Unreadable
from .census import GroundCensus
from .ground_owners import GROUND_OWNERS
from .selftest_plants import Targets, PlantNoDeed
from .selftest_cases import Cases, Judge, Kinds, FrameCases, LogCases
from .selftest_headers import ConstantsCheck, OwnersCheck, AlphabetCheck
from .bookcensus import BOOK_OWNERS, BookCensus
from .selftest_book import BookTargets
from .selftest_book_tally import (Pristine, Circles, SpineCases, RangeCases, TreeShapeCases, ElementCases,
                                  SlotRootCases)
from .selftest_book_ribbon import (RecordCases, RecordRuleCases, RibbonEndCases, RibbonBranchCases,
                                   RibbonFurtherCases, RibbonLossCases, RibbonCases, RibbonShapeCases,
                                   RibbonBoundCases)

# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------


def BookCases(bt):
    """Every landing, and [(group, why)] of the groups the image gives nothing
    to aim at — each a red line, never a landing left out in silence."""
    cases, refused = [], []
    for group in (Pristine, Circles, SpineCases, RangeCases, TreeShapeCases, ElementCases, SlotRootCases,
                  RecordCases, RecordRuleCases, RibbonEndCases, RibbonBranchCases, RibbonFurtherCases,
                  RibbonLossCases, RibbonCases, RibbonShapeCases, RibbonBoundCases):
        try:
            cases += group(bt)
        except Unreadable as e:
            refused.append((group.__name__, str(e)))
    return cases, refused


def BookRun(t=None, medium=None):
    """[(name, ok, said)] of every landing, and the owner kinds heard — over
    the book laid on a format-v1 image (`t`), or a format-v2 image's own. An
    image the book cannot be laid on, or that gives the landings nothing to
    aim at, is one red line and no owner heard (None)."""
    try:
        bt = BookTargets(t, medium)
    except (Unreadable, ValueError) as e:
        return [("B the book's landings", False, "NOT so: the image gives them no base — %s" % e)], None
    cases, refused = BookCases(bt)
    out, heard = [(name, False, "NOT so: the image gives these landings nothing to aim at — %s" % why)
                  for name, why in refused], set()
    for case in cases:
        o = bt.Fork()
        try:
            case.plant(o)
            census = BookCensus(o)
            ok, codes, wrong = Judge(case, census)
            heard |= census.Kinds()
            said = "flagged %s — %s" % (", ".join(codes) or "nothing",
                                         "as expected" if ok else "; ".join(wrong))
        except (Unreadable, IOError, ValueError) as e:
            ok, said = False, "NOT so: %s" % e
        out.append((case.name, ok, said))
    return out, heard


def BookEmit(t, name, out, medium=None):
    """The landing `name` written to `out`; None when there is none of that
    name, or (its name, sectors written)."""
    bt = BookTargets(t, medium)
    wanted = [c for c in BookCases(bt)[0] if c.name == name or c.name.split()[0] == name]
    if not wanted:
        return None
    o = bt.Fork()
    wanted[0].plant(o)
    o.WriteOut(out)
    return wanted[0].name, len(o.over)


def BookNames(t, medium=None):
    return [c.name.split()[0] for c in BookCases(BookTargets(t, medium))[0]]


def BookOwnersCheck(heard):
    """What is wrong with the landings' reach: an owner the book's census
    names (bookcensus.BOOK_OWNERS) that no landing's census heard claim a
    block — an owner nobody has seen claim is an owner that may not."""
    unheard = [name for name, _alone in BOOK_OWNERS if name not in heard]
    return ["no landing's census heard %s claim a block" % ", ".join(unheard)] if unheard else []


def Start(medium):
    try:
        c0 = GroundCensus(medium)
    except (Unreadable, IOError) as e:
        print("self-test: the image handed to self-test is unreadable — %s" % e)
        return None
    if c0.Verdict() != "consistent":
        print("self-test: the image handed to self-test is not consistent to begin with:")
        for line in c0.HumanLines():
            print("self-test:   " + line)
        return None
    return Targets(c0)


def Book(medium):
    """Is the image's volume one of format v2? Then its own book is the base
    the landings stand on, and no case of format v1 has a volume to run on."""
    try:
        return StandOnDeed(medium, None).format == "v2"
    except (Unreadable, IOError):
        return False


class Count:
    """The count of cases answered, and each one's line."""

    def __init__(self):
        self.passed = self.total = 0

    def Say(self, ok, line):
        self.total += 1
        self.passed += bool(ok)
        print("self-test: " + line)


def V1Cases(t, medium, count):
    """Every case of format v1 over the image; the owner kinds heard."""
    heard = set()
    for case in Cases(t):
        over = OverlayMedium(medium)
        case.plant(over)
        census = GroundCensus(over)
        ok, codes, wrong = Judge(case, census)
        heard |= Kinds(census)
        count.Say(ok, "%s — flagged %s — %s" % (case.name, ", ".join(codes) or "nothing",
                                                 "as expected" if ok else "; ".join(wrong)))
    over = OverlayMedium(medium)
    PlantNoDeed(t, over)
    try:
        GroundCensus(over)
        ok, said = False, "the census read a volume with no deed at either end"
    except Unreadable as e:
        ok, said = True, "unreadable: %s" % e
    count.Say(ok, "C16 no deed — %s — %s" % (said, "as expected" if ok else "expected unreadable"))
    for name, ok, said in FrameCases(t, medium) + LogCases(t, medium):
        count.Say(ok, "%s — %s — %s" % (name, said, "as expected" if ok else "NOT so"))
    return heard


def BookCasesRun(count, t=None, medium=None):
    book, owners = BookRun(t, medium)
    for name, ok, said in book:
        count.Say(ok, "%s — %s" % (name, said))
    if owners is None:
        return
    wrong = BookOwnersCheck(owners)
    count.Say(not wrong, "B-owners — every owner the book's census names is heard in a landing — %s"
              % ("as expected" if not wrong else "; ".join(wrong)))


def Headers(src, heard, count):
    """X1, X2 (when the cases of format v1 ran: `heard` not None) and X3; False
    when the source tree is not beside the tool."""
    wrong, n = ConstantsCheck(src)
    if wrong is None:
        print("self-test: X1 constants — %s" % n)
        return False
    count.Say(not wrong, "X1 constants — %u constants and struct fields held against the headers — %s"
              % (n, "as expected" if not wrong else "; ".join(wrong)))
    if heard is None:
        print("self-test: X2 ground owners — not held: the owners of format v1 are heard only by its cases, "
              "and none runs on a format-v2 image")
    else:
        wrong = OwnersCheck(src, heard)
        count.Say(not wrong, "X2 ground owners — the %u owners ground_owners.py lists are the header's, and "
                  "each is heard in a case — %s" % (len(GROUND_OWNERS), "as expected" if not wrong
                                                   else "; ".join(wrong)))
    wrong, n = AlphabetCheck(src)
    if wrong is None:
        print("self-test: X3 alphabet — %s" % n)
        return False
    count.Say(not wrong, "X3 alphabet — %u numbers, layouts and known answers held against volume_book.h, "
              "volume_digest.h, volume_tally.h and volume_ribbon.h — %s" % (n, "as expected" if not wrong else "; ".join(wrong)))
    return True


def Run(medium, src):
    count, heard = Count(), None
    if Book(medium):
        print("self-test: a format-v2 image — the book's landings stand on its own book")
        BookCasesRun(count, medium=medium)
    else:
        t = Start(medium)
        if t is None:
            return 2
        print("self-test: aiming at S = %s (%u block(s)), L = %s (%u), F = data block %u"
              % (t.Label(t.S), t.Srec.Blocks(), t.Label(t.L), t.Lrec.Blocks(), t.F))
        heard = V1Cases(t, medium, count)
        BookCasesRun(count, t)
    if not Headers(src, heard, count):
        return 2
    print("self-test: %u of %u cases answered as expected" % (count.passed, count.total))
    return 0 if count.passed == count.total else 1


def Emit(medium, device, name, out):
    book = Book(medium)
    t = None if book else Start(medium)
    if t is None and not book:
        return 2
    wanted = [] if book else [c for c in Cases(t) if c.name == name or c.name.split()[0] == name]
    try:
        if wanted:
            over = OverlayMedium(medium)
            wanted[0].plant(over)
            over.WriteOut(out)
            said, sectors = wanted[0].name, len(over.over)
        else:
            landed = BookEmit(t, name, out, medium if book else None)
            if landed is None:
                print("self-test: no case %r; the cases are %s" % (name, ", ".join(
                    ([] if book else [c.name.split()[0] for c in Cases(t)]) +
                    BookNames(t, medium if book else None))))
                return 2
            said, sectors = landed
    except (ValueError, Unreadable) as e:
        print("self-test: %s" % e)
        return 2
    print("self-test: %s written to %s — %u sector(s) differ from %s" % (said, out, sectors, device))
    return 0
