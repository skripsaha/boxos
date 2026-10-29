"""powercut_lie — the medium that lied (spec_v2 §16.5 п.21): the media a device
leaves that does not keep what it answered it had, planned from a run's record,
judged by volume-read, booted twice, and counted apart from the power's cuts.

Not a program of its own: tools/powercut.py is the way in (`lies` plans,
`checks --lies` judges each medium, `lieboots` picks the ones booted), and the
judge (powercut_judge.py CmdJudge) asks Judge below once its own oracles are
said. The run and the listing's writes are powercut.py's, taken from it by
name as powercut_judge.py takes them.

THE MODEL. PowerCut's own cut is the base, every epoch before K and a subset of
K (powercut.py): a device that keeps all it said it kept. A device that lied
keeps less, and two ways of it are built here, each by the record alone:

    Blot     the writes of epoch K−1, or of K−1 and K−2 — every one of them, or
             their data alone — left out, though the flush that closed their
             epoch was answered; of epoch K every write, or none (the device
             lost the cycle a flush stood behind and went on, or stopped), or
             — K−1's data lost — the writes of K before its first stitch and
             that stitch torn after every sector but its last (C5).
    Smudge   a write of a book's own unit (a stitch, a Tally node, a Spine
             slot) in flight when the power went, and the rest of the 16 KiB
             flash page around it spoilt with it: every sector of the page but
             its own read back as bytes no writer laid (blklog.Smudged); the
             write itself never landed, the writes of its epoch before it did
             — or, a stitch, it landed but its last sector, its StitchHead
             with it, as a paired page tears the one before (C13, C18).

Neither is a cut of the power's model, and neither may be counted as one: a
defect a lie shows is no defect of the order of writes, and a census of
power cuts with lies in it could not say, as Exhaustive does, that a code no
cut shows is gone. So the plan keeps them in a list of their own, volume-read
answers for them into a file of their own, and every verdict on them is keyed
`blot:` or `smudge:`.

ORACLES of a lie, each one line per kind: every medium judged (else HARNESS);
none shows two owners of one block (depth ≤ 2 never does, §3.10 C6, C16, C17);
every harm code a medium shows names the loss — the codes in LOUD, a Smudge's
without DIGEST_MISMATCH — or is held under a caveat row, or is a FAIL. Each
medium is held to the line its row owes (§3.9): a Blot medium that holds a
whole stitch of the record past the last its census sews on says the lie
proven or what settled lost, and MEDIUM_LIED (C17); one that holds none never
says the lie proven (C5, M-2); a Blot's torn form whose data did not land says
MEDIUM_LIED (C5). A Smudge medium that spoilt a stitch the honest medium sews
on says so by the ribbon while the medium still shows the stitch (C13) — by
what settled is lost and MEDIUM_LIED where the torn stitch in flight names it
(C18) — and one that wiped a settled stitch whole with nothing past it is a
FAIL of its own, smudge:wiped: no reader can tell it from a cycle that never
wrote, and C18 says it all the same — on a removable medium the ribbon is
written in flash pages, the page torn here (§16.6 п.2), and no stitch in
flight shares one with a settled stitch. Booted media are booted twice — the
first boot mounts, its session writes, its power button settles it; the
second mounts what the first left — each boot reaching a shell, each mount
the one volume-read predicted (powercut_judge_book.TwoBoots), and nothing the
session left with two owners.
"""

import hashlib
import json
import os
import sys
import random
import re

from powercut import HARM, LoadLines, Outside, RangeSet, Ranges, Stitch

# A flash page of 16 KiB, in 512-byte sectors (§16.5 п.21): the page a mount
# lifts a removable medium's ribbon atom to (volume_book.h BOOK_FLASH_PAGE_BYTES,
# §16.6 п.2), held to it by the selftest (LieAtomTests).
PAGE_SECTORS = 32
DATA_KINDS = ("file", "free")           # blklog_book.py: a block of data, named or not yet
UNIT_KINDS = ("stitch", "sheet", "slot")
# The codes in which a book's census names what its medium lost (bookmount.py,
# stitch_ribbon.py): the ribbon cut short, a closed unit damaged, data that did
# not land or rotted, a slot that fell back — the loud answer to a lie.
LOUD = ("DEAD_BRANCH", "CLASP_STRANGER", "RIBBON_TORN", "STITCH_TORN", "NOT_LANDED", "UNDONE",
        "SLOT_FELL_BACK", "SLOT_TORN", "NODE_COPY_ROTTEN", "RANGE_ROTTEN", "DIGEST_MISMATCH")
# A Smudge spoils the book's own units, never a block of data: a file that
# reads other bytes than its own after one is the harm C18 forbids (0
# ERR_CHECKSUM), no answer to the loss — while a Blot deeper than the check of
# the last cycle is answered by the rot a read finds (§3.9, the last row).
LOUD_BY = {"blot": LOUD, "smudge": tuple(c for c in LOUD if c != "DIGEST_MISMATCH")}
# The lines a whole stitch past a hole draws (stitch_ribbon.py _Lost, the
# kernel's ribbon_scan.c in the same words): the lie proven (C17), what
# settled lost (C18).
PROVEN, SETTLED_LOST = "the lie is proven", "what settled is lost"
STITCH_SEQ = re.compile(r"stitch seq (\d+)(?:, (\d+) sector)?")
# Either line counts what settled is lost with its seqs — runs "a..b", ", ..."
# where the kernel's line had no room — and says the largest seq found past
# the end, which no seq given next repeats (§3.9, C17).
LOST_COUNT = re.compile(r"what settled is lost: at most (\d+) stitch\(es\), seq ([0-9., ]*?)(?:;| —)")
LARGEST = re.compile(r"no seq up to (\d+), the largest found")


# ---------------------------------------------------------------------------
# plan — which lies are built
# ---------------------------------------------------------------------------


def Data(w):
    return bool(w["touches"]) and all(t["kind"] in DATA_KINDS for t in w["touches"])


def Unit(w):
    return any(t["kind"] in UNIT_KINDS for t in w["touches"])


def Page(w):
    """The sectors of the 16 KiB pages a write meets, but its own, as [lba, n]."""
    lo = w["lba"] // PAGE_SECTORS * PAGE_SECTORS
    hi = (w["lba"] + w["sectors"] + PAGE_SECTORS - 1) // PAGE_SECTORS * PAGE_SECTORS
    return [[a, b - a] for a, b in ((lo, w["lba"]), (w["lba"] + w["sectors"], hi)) if b > a]


def Torn(w):
    """A write landed but its last sector — its first sector, a stitch's
    StitchHead, with the rest — as a tear [index, mask]; None for a write of
    one sector, which has no such prefix."""
    return [w["index"], (1 << (w["sectors"] - 1)) - 1] if w["sectors"] > 1 else None


class LiePlanner:
    def __init__(self, listing):
        self.listing, self.by_epoch = listing, {}
        for w in listing["writes"]:
            self.by_epoch.setdefault(w["epoch"], []).append(w)
        self.media, self.seen = [], {}

    def Add(self, mid, kind, k, keep, lost, spoilt, why, tear=None):
        """One medium, and the earlier one it is sector for sector, if any:
        identity by provenance, as the power's plan collapses its own."""
        state = {}
        for e in range(k):
            for w in self.by_epoch.get(e, []):
                if w["seq"] not in lost:
                    state.update((w["lba"] + i, w["seq"]) for i in range(w["sectors"]))
        for w in self.by_epoch.get(k, []):
            mask = (1 << w["sectors"]) - 1 if w["index"] in keep else \
                tear[1] if tear and tear[0] == w["index"] else 0
            state.update((w["lba"] + i, w["seq"]) for i in range(w["sectors"]) if mask >> i & 1)
        for lba, n in spoilt:
            state.update((x, "smudged") for x in range(lba, lba + n))
        key = hashlib.sha1(repr(sorted(state.items(), key=lambda kv: kv[0])).encode()).digest()
        same = self.seen.setdefault(key, mid)
        self.media.append({"id": mid, "kind": kind, "epoch": k, "keep": Ranges(keep), "tear": tear,
                           "blot": Ranges(lost), "smudge": spoilt, "same": None if same == mid else same, "why": why})

    def Blots(self, k):
        ws = self.by_epoch.get(k, [])
        always = {w["index"] for w in ws if Outside(w)}
        for d in (1, 2):
            if k - d < 0:
                break
            before = [w for e in range(k - d, k) for w in self.by_epoch.get(e, []) if not Outside(w)]
            if not before:
                continue
            data = [w for w in before if Data(w)]
            ways = [("all", before)] + ([("data", data)] if data and len(data) < len(before) else [])
            for what, lost in ways:
                self.Add("b%d-d%d-%s" % (k, d, what), "blot", k, {w["index"] for w in ws},
                         {w["seq"] for w in lost}, [], "epochs %d..%d's %s lost behind their flushes, "
                         "epoch %d whole" % (k - d, k - 1, "writes" if what == "all" else "data", k))
            if d == 1:
                self.Add("b%d-d1-none" % k, "blot", k, always, {w["seq"] for w in before}, [],
                         "epoch %d's writes lost behind its flush, and nothing of epoch %d" % (k - 1, k))
                self.BlotTorn(k, ws, always, data if len(data) < len(before) else [])

    def BlotTorn(self, k, ws, always, data):
        """C5: epoch K−1's data lost behind its flush, and epoch K cut inside
        its first stitch — the writes before it laid, it torn after every
        sector but its last, so the stitch whose data did not land has a torn
        one after it."""
        vol = [w for w in ws if not Outside(w)]
        p = next((p for p, w in enumerate(vol) if Stitch(w)), None)
        tear = Torn(vol[p]) if p is not None else None
        if data and tear:
            self.Add("b%d-d1-data-torn" % k, "blot", k, always | {w["index"] for w in vol[:p]},
                     {w["seq"] for w in data}, [], "epoch %d's data lost behind its flush, and epoch %d cut "
                     "inside its first stitch, entry %d" % (k - 1, k, vol[p]["seq"]), tear)

    def Smudges(self, k):
        ws = self.by_epoch.get(k, [])
        always = {w["index"] for w in ws if Outside(w)}
        vol = [w for w in ws if not Outside(w)]
        for p, w in enumerate(vol):
            spoilt = Page(w) if Unit(w) else []
            if not spoilt:
                continue
            before = always | {v["index"] for v in vol[:p]}
            self.Add("m%d-%d" % (k, w["index"]), "smudge", k, before, set(), spoilt,
                     "entry %d (%s) in flight, the rest of its 16 KiB page spoilt" % (w["seq"], w["touches"][0]["kind"]))
            # The page a stitch began in held a unit before it: torn as a
            # paired page tears the one before, its StitchHead laid to name it.
            tear = Torn(w) if Stitch(w) and w["lba"] % PAGE_SECTORS else None
            if tear:
                self.Add("m%d-%d-t" % (k, w["index"]), "smudge", k, before, set(), spoilt,
                         "entry %d (stitch) laid but its last sector, the rest of its 16 KiB page spoilt"
                         % w["seq"], tear)

    def Run(self):
        for e in self.listing["epochs"]:
            if e["epoch"]:
                self.Blots(e["epoch"])
            self.Smudges(e["epoch"])
        kinds = [m["kind"] for m in self.media]
        distinct = sum(1 for m in self.media if m["same"] is None)
        return {"media": self.media, "counts": {"blot": kinds.count("blot"), "smudge": kinds.count("smudge"),
                                                 "distinct": distinct, "collapsed": len(self.media) - distinct}}


def CmdLies(a):
    with open(a.list) as fh:
        json.dump(LiePlanner(json.load(fh)).Run(), sys.stdout, sort_keys=True)
    return 0


def CmdLieBoots(a):
    """The lie media booted twice: of each kind the first medium of every
    signature in plan order, then a seeded draw of the rest, --sample of each
    kind in all. Tab-separated: id, epoch, keep, tear, blot, smudge, why."""
    plan, results = Plan(a.dir)
    rng = random.Random(int(a.seed, 16))
    for kind in ("blot", "smudge"):
        mine = [m for m in plan["media"] if m["kind"] == kind and m["same"] is None and
                "error" not in results.get(m["id"], {"error": 1})]
        firsts, seen = [], set()
        for m in mine:
            sig = tuple(sorted({(f[0], f[1]) for f in results[m["id"]]["findings"]}))
            if sig not in seen:
                seen.add(sig)
                firsts.append((m, "the first %s medium of %s" % (kind, " ".join("%s/%s" % x for x in sig)
                                                                 or "a clean census")))
        rest = [m for m in mine if m not in [f[0] for f in firsts]]
        rng.shuffle(rest)
        for m, why in (firsts + [(m, "sampled") for m in rest])[:a.sample]:
            print("\t".join((m["id"], str(m["epoch"]), m["keep"] or "none",
                             "%d:0x%x" % tuple(m["tear"]) if m.get("tear") else "-", m["blot"] or "-",
                             ",".join("%d+%d" % tuple(x) for x in m["smudge"]) or "-", why)))
    return 0


def Plan(d):
    path = os.path.join(d, "lies.json")
    if not os.path.exists(path):
        return None, {}
    with open(path) as fh:
        plan = json.load(fh)
    return plan, {r["id"]: r for r in LoadLines(os.path.join(d, "lies.jsonl"))}


# ---------------------------------------------------------------------------
# judge — the lie's oracles, counted apart
# ---------------------------------------------------------------------------


def Held(run, m):
    """The stitches of the record up to a medium's epoch, and of those the
    sectors it holds as the last write laid them: (seqs, seq -> its sectors'
    device LBAs held, seq -> its sectors), of the writes it applies — a torn
    write's sectors by its mask, a Smudge's spoilt sectors no one's
    (blklog_book.py names a stitch's sectors `stitch seq S[, N sector(s)]`)."""
    lost, keep, tear = RangeSet(m["blot"]), RangeSet(m["keep"]), m.get("tear")
    seqs, owner, of = set(), {}, {}
    for w in sorted(run.listing["writes"], key=lambda w: w["seq"]):
        if w["epoch"] > m["epoch"]:
            continue
        mask = 0 if w["seq"] in lost else -1 if w["epoch"] < m["epoch"] or w["index"] in keep else \
            tear[1] if tear and tear[0] == w["index"] else 0
        for t in w["touches"]:
            hit = STITCH_SEQ.match(t["what"]) if t["kind"] == "stitch" else None
            if hit:
                seqs.add(int(hit.group(1)))
                of[int(hit.group(1))] = int(hit.group(2) or 1)
            for x in range(t["first_lba"], t["first_lba"] + t["sectors"]):
                if mask >> (x - w["lba"]) & 1:
                    owner[x] = int(hit.group(1)) if hit else None
    for lba, n in m.get("smudge") or []:
        owner.update((x, None) for x in range(lba, lba + n))
    held = {}
    for x, seq in owner.items():
        if seq is not None:
            held.setdefault(seq, set()).add(x)
    return seqs, held, of


def Stitches(run, m):
    """The stitches of the record a medium holds whole: seq -> its sectors'
    device LBAs."""
    _seqs, held, of = Held(run, m)
    return {seq: lbas for seq, lbas in held.items() if len(lbas) >= of[seq]}


def SeqRuns(text):
    """The seqs a line names as runs "a..b, c" — and whether it cut them
    short (", ...")."""
    seqs, cut = set(), False
    for part in (p.strip() for p in text.split(",") if p.strip()):
        if part == "...":
            cut = True
        elif ".." in part:
            lo, hi = part.split("..")
            seqs.update(range(int(lo), int(hi) + 1))
        else:
            seqs.add(int(part))
    return seqs, cut


def Counted(run, m, last, line):
    """What a Blot medium's line of what settled is lost owes (C17), '' when
    it is said: its count the seqs it names; every stitch of the record past
    the census's last, below the largest it holds whole, that the medium does
    not hold whole named — and none it does; and the largest seq found the
    largest of a stitch it holds a sector of past that last."""
    count, largest = LOST_COUNT.search(line), LARGEST.search(line)
    if not count or not largest:
        return "says what settled is lost without its count or the largest seq found: %s" % line[:120]
    seqs, held, of = Held(run, m)
    whole = {seq for seq, lbas in held.items() if len(lbas) >= of[seq]}
    named, cut = SeqRuns(count.group(2))
    top = max(seq for seq in whole if seq > last)
    want = {seq for seq in seqs if last < seq < top and seq not in whole}
    found = max(seq for seq in held if seq > last)
    if not cut and int(count.group(1)) != len(named):
        return "counts %s stitch(es) lost and names %d: %s" % (count.group(1), len(named), count.group(2))
    if not cut and want - named:
        return "leaves stitch(es) %s out of what settled is lost, which it does not hold whole" % Ranges(want - named)
    if named & whole:
        return "names stitch(es) %s lost, which it holds whole" % Ranges(named & whole)
    if int(largest.group(1)) != found:
        return "says %s the largest seq found, where it holds stitch %d past %d" % (largest.group(1), found, last)
    return ""


def Loud(r, kind):
    """What a census said of a loss: the codes the lie's KIND is answered by,
    and MEDIUM_LIED."""
    said = sorted({f[0] for f in r.get("findings", []) if f[0] in LOUD_BY[kind]})
    return said + (["MEDIUM_LIED"] if (r.get("book") or {}).get("lied") else [])


def Lines(r, words):
    """The ribbon's lines a census said that begin with `words`: the lie
    proven counts what settled is lost too, and is no line of it (M-2)."""
    return [f[2] for f in r.get("findings", []) if f[0] == "DEAD_BRANCH" and f[2].startswith(words)]


def Honest(run, m):
    """The power's cut a Smudge medium spoils: the same epoch, the same writes
    kept and torn, nothing spoilt — its result, or None when the plan has
    none."""
    keep = RangeSet(m["keep"])
    c = next((c for c in run.plan["cuts"] if c["epoch"] == m["epoch"] and c["tear"] == m.get("tear")
              and RangeSet(c["keep"]) == keep), None)
    return run.results.get(c["same"] or c["id"]) if c else None


def Proven(run, m, r):
    """The line a Blot medium owes, '' when it is said (§3.9): a whole stitch
    past the last its census sews on — the lie proven or what settled lost,
    and MEDIUM_LIED (C17); none — never the lie proven (C5, M-2); data a
    stitch named not landed with that stitch's successor torn — MEDIUM_LIED
    (C5)."""
    last, lied = r["book"].get("last") or 0, r["book"].get("lied")
    past = sorted(seq for seq in Stitches(run, m) if seq > last)
    said = Lines(r, PROVEN) + Lines(r, SETTLED_LOST)
    if past and not (said and lied):
        return "%s holds stitch(es) %s whole past the last its census sews on, %s, and %s" % (
            m["id"], Ranges(past), last, "does not mark the medium MEDIUM_LIED" if said else
            "says neither the lie proven nor what settled lost (codes %s)" % (Loud(r, "blot") or "none"))
    wrong = Counted(run, m, last, said[0]) if past else ""
    if wrong:
        return "%s holds stitch(es) %s whole past the last its census sews on, %s, and %s" % (
            m["id"], Ranges(past), last, wrong)
    if not past and Lines(r, PROVEN):
        return "%s holds no whole stitch past the last its census sews on, %s, and says the lie proven: %s" % (
            m["id"], last, Lines(r, PROVEN)[0][:120])
    if m.get("tear") and any(f[0] == "NOT_LANDED" for f in r["findings"]) and not lied:
        return "%s rolls data a stitch named back with that stitch's successor torn, and does not mark the " \
               "medium MEDIUM_LIED" % m["id"]
    return ""


def Spoilt(run, m, r):
    """The line a Smudge medium owes for the stitches it spoilt that the
    honest medium sews on, each sector where the stitch lies — on into its
    next Quire too: (quiet, wiped). Quiet, '' when it is said: the medium
    still shows the stitch — a sector of it left, a stitch sewn after it, or
    the torn stitch in flight, whose StitchHead names the last one — and the
    census says nothing of it by the ribbon (a rot is no answer, C18), or the
    torn stitch names a spoilt one and the census does not say what settled
    is lost with MEDIUM_LIED (C13, C18). Wiped: settled stitches spoilt whole
    with nothing past them, lost with no word — no reader can tell them from
    cycles that never wrote, and C18 says them all the same."""
    honest = Honest(run, m)
    if not honest or "error" in honest:
        return "", ""
    bad = {x for lba, n in m["smudge"] for x in range(lba, lba + n)}
    sewn = honest["book"].get("sewn", [])
    left = [seq for seq, lbas in sewn if not set(lbas) <= bad]
    hit = [seq for seq, lbas in sewn if bad & set(lbas)]
    named = m.get("tear") is not None
    shown = [seq for seq in hit if named or seq in left or any(x > seq for x in left)]
    wiped = sorted(set(hit) - set(shown))
    gone = "%s spoilt settled stitch(es) %s whole, nothing past them" % (m["id"], Ranges(wiped)) if wiped else ""
    if not shown:
        return "", gone
    if not Loud(r, "smudge"):
        return "%s spoilt stitch(es) %s, which the medium still shows, and its census says nothing of them by " \
               "the ribbon (codes %s)" % (m["id"], Ranges(shown), sorted({f[0] for f in r["findings"]}) or "none"), gone
    if named and max(seq for seq, _l in sewn) in hit and not (Lines(r, SETTLED_LOST) and r["book"].get("lied")):
        return "%s's torn stitch names stitch %d, spoilt, and its census does not say what settled is lost with " \
               "MEDIUM_LIED (codes %s)" % (m["id"], max(seq for seq, _l in sewn), Loud(r, "smudge")), gone
    return "", gone


def Judge(j):
    plan, results = Plan(j.run.dir)
    if plan is None:
        return
    for kind in ("blot", "smudge"):
        mine = [m for m in plan["media"] if m["kind"] == kind and m["same"] is None]
        judged = [m for m in mine if "error" not in results.get(m["id"], {"error": "no answer"})]
        if len(judged) < len(mine):
            m = next(m for m in mine if m not in judged)
            j.Out(kind + ":checker", "harness", "volume-read could not judge %d of %d %s media, e.g. %s: %s" % (
                len(mine) - len(judged), len(mine), kind, m["id"], results.get(m["id"], {}).get("error", "no answer")))
        Census(j, kind, judged, results)
        said = [(Proven(j.run, m, results[m["id"]]), "") if kind == "blot" else Spoilt(j.run, m, results[m["id"]])
                for m in judged]
        quiet, wiped = [q for q, _w in said if q], [w for _q, w in said if w]
        key = kind + (":proven" if kind == "blot" else ":said")
        j.Out(key, "fail" if quiet else "behaves", "%d of %d %s media do not say what their loss owes: %s"
              % (len(quiet), len(judged), kind, "; ".join(quiet[:2])) if quiet else
              "every one of %d %s media says what its loss owes" % (len(judged), kind))
        if kind == "smudge":
            j.Out("smudge:wiped", "fail" if wiped else "behaves", "%d of %d smudge media leave no sector of a "
                  "settled stitch and nothing past it, so no reader can say what settled is lost — C18 says it; "
                  "a mount on a removable medium writes the ribbon in flash pages so that no new stitch shares "
                  "one with a settled stitch (§16.6 п.2), a fixed one in the medium's atom: %s" % (
                      len(wiped), len(judged), "; ".join(wiped[:2])) if wiped else
                  "no one of %d smudge media wipes a settled stitch whole with nothing past it" % len(judged))
    Boots(j, plan, results)


def Census(j, kind, judged, results):
    """Two owners of a block — never, at a depth of two or less — and every
    harm code a lie left: its kind's LOUD, or held under a caveat row, or a
    FAIL."""
    census = {}
    for m in judged:
        for code, sev, text in results[m["id"]]["findings"]:
            if sev in HARM:
                census.setdefault(code, [m["id"], text, 0])[2] += 1
    two = census.pop("DOUBLE_OWNER", None)
    j.Out(kind + ":owners", "fail" if two else "behaves", "%d of %d %s media show two owners of a block, first "
          "%s: %s" % (two[2], len(judged), kind, two[0], two[1]) if two else
          "no one of %d %s media shows two owners of a block" % (len(judged), kind))
    loud = ["%s %d" % (code, e[2]) for code, e in sorted(census.items()) if code in LOUD_BY[kind]]
    j.Out(kind + ":loud", "behaves", "%d %s media judged apart from the power's cuts; the census names the loss "
          "as %s" % (len(judged), kind, ", ".join(loud) or "nothing harmful"))
    for code, (first, text, n) in sorted(census.items()):
        if code not in LOUD_BY[kind]:
            key = "%s:code:%s" % (kind, code)
            j.Out(key, "defect" if j.caveats.get(key) else "fail", "%s (%d of %d %s media), first %s: %s%s" % (
                code, n, len(judged), kind, first, text, "" if j.caveats.get(key) else
                " — no caveat names it; its step is the owner's to assign"))


def Boots(j, plan, results):
    """Each lie booted twice (powercut_judge_book.TwoBoots): both boots reach
    a shell, each mount the one predicted, and the session between leaves no
    block of two owners."""
    from powercut_judge_book import TwoBoots
    path = os.path.join(j.run.dir, "lieboots.txt")
    kinds = {m["id"]: m["kind"] for m in plan["media"]}
    rows = [r.split("\t") for r in (open(path).read().splitlines() if os.path.exists(path) else [])]
    for kind in ("blot", "smudge"):
        mine = [r[0] for r in rows if kinds.get(r[0]) == kind]
        if not mine:
            continue
        bad = ["%s: %s" % (mid, w) for mid in mine for w in TwoBoots(j.run, mid, results[mid])[1]]
        j.Out(kind + ":boots", "fail" if bad else "behaves", "%d place(s) where %d %s media booted twice went "
              "otherwise than predicted: %s" % (len(bad), len(mine), kind, "; ".join(bad[:3])) if bad else
              "%d %s media booted twice: each mount the one predicted, nothing the session between left with "
              "two owners" % (len(mine), kind))
