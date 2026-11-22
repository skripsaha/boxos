"""selftest — every census rule planted into an image and caught, the cut logic
proved on a synthetic log, and the format held against the C headers.

A rule nobody has seen fire is a rule that may not work. Each landing lays one
kind of damage — or one state a kernel cycle leaves — over the image's book in
memory (an OverlayMedium — the image file is never written), runs the census on
it, and holds the findings, and the mount's decisions, against what that state
must produce (selftest_book.py and its siblings). The medium under the book is
the book's to answer for too: the ground the kernel finds on it, the sectors it
will not give, no Deed at all, and the cut logic on a log built over it
(selftest_book_medium.py).

`--emit CASE --out IMG` writes one landing's overlay out as a new image, for a
harness to boot: logcheck's `census` scenario boots some of them and holds the
kernel's own lines against this package's prediction.
"""

from .medium import Unreadable
from .selftest_headers import ConstantsCheck, OwnersCheck, AlphabetCheck
from .bookcensus import BOOK_OWNERS, BookCensus
from .selftest_book import BookTargets, Judge
from .selftest_book_medium import GroundCases, LogCases, NoDeed, UnreadCases
from .selftest_book_tally import (Pristine, Circles, SpineCases, RangeCases, TreeShapeCases, ElementCases,
                                  SlotRootCases)
from .selftest_book_ribbon import (RecordCases, MachineRecordCases, RecordRuleCases, RibbonEndCases,
                                   RibbonBranchCases, RibbonFurtherCases, RibbonCases)
from .selftest_book_ribbon_damage import RibbonLossCases, RibbonShapeCases, RibbonBoundCases
from .selftest_book_errata import ErrataCases
from .selftest_book_keep import KeepCases
from .selftest_book_made import MadeCases
from .selftest_book_shown import ShownCases
from .selftest_book_tip_in import TipInCases
from .selftest_book_escheat import EscheatCases, LockedCases
from .selftest_book_facsimile import FacsimileRuleCases, FacsimileReplayCases, BootSlipCases
from .selftest_book_memo import MemoCases
from .selftest_book_boot import BootCases

# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------


def BookCases(bt):
    """Every landing, and [(group, why)] of the groups the image gives nothing
    to aim at — each a red line, never a landing left out in silence."""
    cases, refused = [], []
    for group in (Pristine, Circles, SpineCases, RangeCases, TreeShapeCases, ElementCases, SlotRootCases,
                  RecordCases, MachineRecordCases, RecordRuleCases, RibbonEndCases, RibbonBranchCases,
                  RibbonFurtherCases, RibbonLossCases, RibbonCases, RibbonShapeCases, RibbonBoundCases, ErrataCases,
                  TipInCases, EscheatCases, LockedCases, FacsimileRuleCases, FacsimileReplayCases, BootSlipCases,
                  MemoCases, BootCases, KeepCases, MadeCases, ShownCases, GroundCases, UnreadCases):
        try:
            cases += group(bt)
        except Unreadable as e:
            refused.append((group.__name__, str(e)))
    return cases, refused


def BookRun(medium):
    """[(name, ok, said)] of every landing, and the owner kinds heard — over
    the image's own book. An image that gives the landings no base, or nothing
    to aim at, is one red line and no owner heard (None)."""
    try:
        bt = BookTargets(medium)
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
    for name, ok, said in [NoDeed(bt)] + LogCases(bt):
        out.append((name, ok, "%s — %s" % (said, "as expected" if ok else "NOT so")))
    return out, heard


def BookEmit(medium, name, out):
    """The landing `name` written to `out`: (its name, sectors written, []) —
    or (None, the names of the landings there are, [(group, why)] of the
    groups the image gave nothing to aim at) when there is none of that name,
    so a landing refused is said with why, never as one that does not exist."""
    bt = BookTargets(medium)
    cases, refused = BookCases(bt)
    wanted = [c for c in cases if c.name == name or c.name.split()[0] == name]
    if not wanted:
        return None, [c.name.split()[0] for c in cases], refused
    o = bt.Fork()
    wanted[0].plant(o)
    o.WriteOut(out)
    return wanted[0].name, len(o.over), []


def BookOwnersCheck(heard):
    """What is wrong with the landings' reach: an owner the book's census
    names (bookcensus.BOOK_OWNERS) that no landing's census heard claim a
    block — an owner nobody has seen claim is an owner that may not."""
    unheard = [name for name, _alone in BOOK_OWNERS if name not in heard]
    return ["no landing's census heard %s claim a block" % ", ".join(unheard)] if unheard else []


class Count:
    """The count of cases answered, and each one's line."""

    def __init__(self):
        self.passed = self.total = 0

    def Say(self, ok, line):
        self.total += 1
        self.passed += bool(ok)
        print("self-test: " + line)


def BookCasesRun(count, medium):
    book, owners = BookRun(medium)
    for name, ok, said in book:
        count.Say(ok, "%s — %s" % (name, said))
    if owners is None:
        return
    wrong = BookOwnersCheck(owners)
    count.Say(not wrong, "B-owners — every owner the book's census names is heard in a landing — %s"
              % ("as expected" if not wrong else "; ".join(wrong)))


def Headers(src, count):
    """X1, X2 and X3; False when the source tree is not beside the tool."""
    wrong, n = ConstantsCheck(src)
    if wrong is None:
        print("self-test: X1 constants — %s" % n)
        return False
    count.Say(not wrong, "X1 constants — %u constants and struct fields held against the headers — %s"
              % (n, "as expected" if not wrong else "; ".join(wrong)))
    wrong = OwnersCheck(src)
    count.Say(not wrong, "X2 ground owners — ground_owners.py is what tools/gen_ground_owners.py writes of "
              "the header's owners — %s" % ("as expected" if not wrong else "; ".join(wrong)))
    wrong, n = AlphabetCheck(src)
    if wrong is None:
        print("self-test: X3 alphabet — %s" % n)
        return False
    count.Say(not wrong, "X3 alphabet — %u numbers, layouts and known answers held against volume_book.h, "
              "volume_digest.h, volume_tally.h and volume_ribbon.h — %s" % (n, "as expected" if not wrong else "; ".join(wrong)))
    return True


def Run(medium, src):
    count = Count()
    BookCasesRun(count, medium)
    if not Headers(src, count):
        return 2
    print("self-test: %u of %u cases answered as expected" % (count.passed, count.total))
    return 0 if count.passed == count.total else 1


def Emit(medium, device, name, out):
    try:
        said, landed, refused = BookEmit(medium, name, out)
    except (ValueError, Unreadable) as e:
        print("self-test: %s" % e)
        return 2
    if said is None:
        print("self-test: no case %r; the cases are %s%s" % (name, ", ".join(landed), "".join(
            "; the image gives %s nothing to aim at — %s" % (group, why) for group, why in refused)))
        return 2
    print("self-test: %s written to %s — %u sector(s) differ from %s" % (said, out, landed, device))
    return 0
