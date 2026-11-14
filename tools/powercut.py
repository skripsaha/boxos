#!/usr/bin/env python3
"""powercut — the planner and the checker's fan-out of tools/powercut.sh, and the way
in to its judge (tools/powercut_judge.py).

A QEMU blklogwrites record holds every write a machine made and every flush it
asked for. tools/volume-read.py owns that record's format and the volume's: it
lists the writes (`cut --list --json`), builds the medium any subset of an
epoch would have left and judges it (`cut --epoch K --keep S --check --json`).
This file knows neither format. It decides WHICH media to build, asks
volume-read about each, and holds the answers against what the machine said
it had made durable:

    powercut.py step STEPS --step NAME --cmd CMD --before N --ack N --quiet N
                           --line TEXT [--durable NAME:RECORD:FROM:TO] [--noquiet]
    powercut.py plan LIST --seed HEX [--all-max N --sample N --tear-all-max N] > PLAN
    powercut.py checks PLAN BASE LOG BASECHECK [--jobs N] [--lies] [--digests MEMO] > CUTS
    powercut.py lies LIST > LIES
    powercut.py lieboots DIR --sample N --seed HEX > LIEBOOTS
    powercut.py boots DIR --picks final|prefixes:STEP|dedup:A:B --sample N --seed HEX [--by-kind] > BOOTS
    powercut.py judge DIR --path PATH [--caveat KEY=NAME]... [--verify KIND] [--hogged] [--waited S]
                      [--by-kind]
    powercut.py settlecut DIR --path PATH [--caveat KEY=NAME]...
    powercut.py crowd CHECK --name-bytes N --room N [--names N]
    powercut.py forge LIST LOG OUT
    powercut.py selftest

THE MODEL. A cut at epoch K is the base, every write of the epochs before K,
and a subset of epoch K's writes, applied in record order: the medium a power
cut leaves when the device kept some of what it had not yet been told to
flush. Writes outside the volume (firmware on the ESP) are always applied and
never enumerated. An epoch of at most --all-max volume writes is enumerated
whole; a larger one by every prefix, every single omission and a sample drawn
from the seed, and the plan counts exactly how many subsets were NOT built.
The last volume write of every epoch is also torn as a prefix: after every
sector when it is at most --tear-all-max sectors, otherwise after sector 1,
sector n-1 and every 4 KiB boundary — and the plan counts the prefixes it did
not tear. A book's stitch is torn after every sector it has wherever it
stands in its epoch, the writes before it kept: a stitch is one unit
whatever windows it went out in, and a power cut in any 512-byte quantum of
it is a cut the mount must take (spec_v2 §3.10 C2), so no quantum of one is
left to a sample. An epoch with nothing left unbuilt is enumerated whole, and only
there does a defect's absence say anything: a sample cannot show one is gone —
and neither can a record a writer kept writing (a step --noquiet), whose cuts
are one run's timing and not the machine's.
Two cuts whose every sector came from the same write are one medium and are
judged once: the listing carries no content hash (contract C3), so identity is
by provenance, and two writes that happen to carry the same bytes are not
collapsed — the count errs towards judging more, never fewer.
A medium is a PREFIX when some cut that builds it kept its epoch's writes
from the first up to one of them, the next perhaps torn — what a power cut
leaves a device that writes in the order it is asked — and a SUBSET
otherwise, which only a device that lets its cache reach the medium out of
order can leave. --by-kind keeps the two apart in the census, because a fix
may close one and not the other: an order of writes closes every prefix,
and only a flush between two writes closes the subsets that part them.

`judge` and `settlecut` are asked here and answered by tools/powercut_judge.py:
every oracle, one verdict line each, and the report of the run — a book's own
oracles beside it (powercut_judge_book.py). `lies` and `lieboots` are
answered by tools/powercut_lie.py: the media a medium that lied leaves (Blot,
Smudge), planned, judged by `checks --lies` and counted apart from the cuts.

A step's acknowledgement stands behind the first flush after the last write of
its own that touches the ledger — HEAD's sync writes the ledger last and then
flushes — and before the answer was read. Where another writer flushes too
(race, and halt on the AHCI WriteJob) the record cannot say whose that flush
is: `ack:` then proves only that some flush followed the step's last ledger
write before its answer.
"""

import sys

sys.dont_write_bytecode = True

import argparse                      # noqa: E402
import concurrent.futures            # noqa: E402
import hashlib                       # noqa: E402
import json                          # noqa: E402
import os                            # noqa: E402
import random                        # noqa: E402
import struct                        # noqa: E402
import subprocess                    # noqa: E402

VOLUME_READ = os.path.join(os.path.dirname(os.path.abspath(__file__)), "volume-read.py")
HARM = ("damage", "leak")            # the severities that make volume-read say "inconsistent"

# The records the scenarios write, so the judge knows the bytes a surviving
# file must hold: each program's own struct and mix, in the file named.
RECORDS = {
    "anchor":  (0x414E4348, 128),    # apps/anchor_test.c AnchorRecord: magic, seq, sum, 116 zeros
    "persist": (0x42504552, 16),     # apps/persist.c PersistRecord: magic, seq, sum, 0
    "quill":   (0x5155494C, 16),     # apps/quilltest_powercut.c QuillRecord: magic, seq, sum, 0
    "empty":   (0, 0),               # apps/quilltest_powercut.c QuillCrowd: made, never written
    "log":     (0, 48),              # apps/quilltest_book.c QuillLogLine: a line; a record of N is N lines
}


def Mix(a, b):
    """The checksum the three programs share (persist.c fold_checksum)."""
    x = (a ^ (b * 2654435761)) & 0xFFFFFFFF
    x ^= x >> 16
    return (x * 0x85EBCA6B) & 0xFFFFFFFF


def LogLine(n):
    """Line n of quilltest log: its number in eight digits, dots, a newline."""
    return b"%08d" % (n % 100000000) + b"." * (RECORDS["log"][1] - 9) + b"\n"


def RecordBytes(kind, seq):
    """The bytes a record holds; a logger's record of seq is its first seq lines."""
    if kind == "log":
        return b"".join(LogLine(n) for n in range(seq))
    magic, size = RECORDS[kind]
    if size == 0:
        return b""
    head = struct.pack("<III", magic, seq, Mix(magic, seq))
    return head + bytes(size - len(head))


def Ranges(values):
    """Sorted integers as ranges, in volume-read's --keep syntax: 0-49,51,53-60."""
    out, run = [], None
    for v in sorted(values):
        if run and v == run[1] + 1:
            run[1] = v
        else:
            if run:
                out.append(run)
            run = [v, v]
    if run:
        out.append(run)
    return ",".join("%d" % a if a == b else "%d-%d" % (a, b) for a, b in out)


def RangeSet(text):
    out = set()
    for item in filter(None, text.split(",")):
        a, _sep, b = item.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


def LoadLines(path):
    if not os.path.exists(path):
        return []
    with open(path) as fh:
        return [json.loads(line) for line in fh if line.strip()]


# The writes that commit a step: format v1's Ledger, a book's stitch and slot
# (blklog_book.py names them by their sectors' heads).
COMMIT_KINDS = ("ledger", "stitch", "slot")


def Outside(w):
    return bool(w["touches"]) and all(t["kind"] == "outside" for t in w["touches"])


def Stitch(w):
    """A write that lays a sector of a book's stitch (blklog_book.py)."""
    return any(t["kind"] == "stitch" for t in w["touches"])


# ---------------------------------------------------------------------------
# step — one thing the machine was asked, as JSON the shell cannot get wrong
# ---------------------------------------------------------------------------


def CmdStep(a):
    step = {"step": a.step, "cmd": a.cmd, "before": a.before, "ack": a.ack, "quiet": a.quiet,
            "line": a.line, "durable": None, "noquiet": a.noquiet}
    if a.durable:
        name, record, lo, hi = a.durable.rsplit(":", 3)
        step["durable"] = {"name": name, "record": record, "from": int(lo), "to": int(hi),
                           "size": RECORDS[record][1] * (int(hi) if record == "log" else 1)}
    with open(a.steps, "a") as fh:
        fh.write(json.dumps(step, sort_keys=True) + "\n")
    return 0


# ---------------------------------------------------------------------------
# plan — which media to build
# ---------------------------------------------------------------------------


class Planner:
    def __init__(self, listing, seed, all_max, sample_n, tear_all_max):
        self.listing, self.seed = listing, seed
        self.all_max, self.sample_n, self.tear_all_max = all_max, sample_n, tear_all_max
        self.by_epoch = {}
        for w in listing["writes"]:
            self.by_epoch.setdefault(w["epoch"], []).append(w)

    def Subsets(self, k, vol):
        """[(label, frozenset of indices, tear or None)], how they were chosen, tears."""
        m, idx = len(vol), [w["index"] for w in vol]
        out = []
        if m <= self.all_max:
            full, width = (1 << m) - 1, max(1, (m + 3) // 4)
            for bits in range(1 << m):
                label = "all" if bits == full else "none" if bits == 0 else "s%0*x" % (width, bits)
                out.append((label, frozenset(idx[j] for j in range(m) if bits >> j & 1), None))
            how = "every subset"
        else:
            for j in range(m + 1):
                out.append(("all" if j == m else "none" if j == 0 else "p%03d" % j,
                            frozenset(idx[:j]), None))
            for j in range(m):
                out.append(("o%03d" % j, frozenset(idx[:j] + idx[j + 1:]), None))
            known = {s for _l, s, _t in out}
            want = min(self.sample_n, (1 << m) - len(known))
            rng, drawn = random.Random(self.seed ^ k), 0
            while drawn < want:
                s = frozenset(i for i in idx if rng.random() < 0.5)
                if s not in known:
                    known.add(s)
                    out.append(("r%03d" % drawn, s, None))
                    drawn += 1
            how = "every prefix %d, every single omission %d, %d drawn with seed %x" % (
                m + 1, m, want, self.seed ^ k)
        tears, n = [], 0
        if vol and vol[-1]["sectors"] >= 2:
            n = vol[-1]["sectors"]
            tears = range(1, n) if n <= self.tear_all_max or Stitch(vol[-1]) else \
                sorted({1, n - 1} | set(range(8, n, 8)))
            for j in tears:
                out.append(("t%03d" % j, frozenset(idx[:-1]), (vol[-1]["index"], (1 << j) - 1)))
        stitched = 0
        for p, w in enumerate(vol[:-1]):
            if Stitch(w):
                for j in range(1, w["sectors"]):
                    out.append(("w%03dt%03d" % (p, j), frozenset(idx[:p]), (w["index"], (1 << j) - 1)))
                    stitched += 1
        return out, how, len(tears) + stitched, max(0, n - 1 - len(tears)), n

    def Run(self):
        prov, seen, cuts, epochs, through_before = {}, {}, [], [], 0
        for e in self.listing["epochs"]:
            k = e["epoch"]
            ws = self.by_epoch.get(k, [])
            vol = [w for w in ws if not Outside(w)]
            always = frozenset(w["index"] for w in ws if Outside(w))
            subsets, how, tears, untorn, tear_sectors = self.Subsets(k, vol)
            built, distinct = set(), 0
            for label, sub, tear in subsets:
                if (sub, tear) in built:
                    continue
                built.add((sub, tear))
                keep = always | sub
                state, through, open_run = dict(prov), through_before, True
                for w in ws:
                    torn = tear is not None and w["index"] == tear[0]
                    if w["index"] in keep or torn:
                        mask = tear[1] if torn else (1 << w["sectors"]) - 1
                        for i in range(w["sectors"]):
                            if mask >> i & 1:
                                state[w["lba"] + i] = w["seq"]
                    open_run = open_run and w["index"] in keep
                    if open_run:
                        through = w["seq"]
                key = hashlib.sha1(repr(sorted(state.items())).encode()).digest()
                cid = "e%d-%s" % (k, label)
                same = seen.get(key)
                if same is None:
                    seen[key] = cid
                    distinct += 1
                cuts.append({"id": cid, "epoch": k, "keep": Ranges(keep),
                             "tear": list(tear) if tear else None, "through": through, "same": same})
            for w in ws:
                for i in range(w["sectors"]):
                    prov[w["lba"] + i] = w["seq"]
                through_before = w["seq"]
            m = len(vol)
            enumerated = sum(1 for (_s, t) in built if t is None)
            epochs.append({"epoch": k, "writes": m, "outside": len(always), "subsets": 1 << m,
                           "enumerated": enumerated, "not_enumerated": (1 << m) - enumerated,
                           "tears": tears, "tears_not_built": untorn, "tear_sectors": tear_sectors,
                           "distinct": distinct, "collapsed": len(built) - distinct,
                           "how": how, "flush_seq": e["flush_seq"]})
        return {"seed": "%x" % self.seed, "all_max": self.all_max, "sample": self.sample_n,
                "tear_all_max": self.tear_all_max, "epochs": epochs, "cuts": cuts}


def CmdPlan(a):
    with open(a.list) as fh:
        listing = json.load(fh)
    json.dump(Planner(listing, int(a.seed, 16), a.all_max, a.sample, a.tear_all_max).Run(),
              sys.stdout, sort_keys=True)
    return 0


# ---------------------------------------------------------------------------
# checks — volume-read asked about every distinct medium, the answers kept short
# ---------------------------------------------------------------------------


def FileKey(f):
    """A file as the judge compares it. A book's locked file is its fid, state
    and why alone (bookcensus.py Json): no name, size or digest to compare."""
    return (f["fid"], f.get("name"), f.get("size"), f.get("sha256"))


def Compact(cid, rc, out, err, base_files, base_names):
    """What the judge needs from one `check --json`: the mount the kernel would
    make, every finding, which chains would be refused and how long each is,
    the base's files where they differ from the base, and every file the base
    did not have."""
    try:
        d = json.loads(out)
    except ValueError:
        return {"id": cid, "rc": rc, "error": ((out.strip() or err.strip() or "no output")
                                               .splitlines()[-1])}
    k = d["kernel"]
    files = {f["fid"]: f for f in d["files"]}
    changed = []
    for key, bf in base_files.items():
        f = files.get(bf["fid"])
        if f is None or not f["visible"] or FileKey(f) != key:
            changed.append("fid %d %s %s" % (bf["fid"], bf["name"], "gone" if f is None else
                           "not visible (%s)" % f["state"] if not f["visible"] else
                           "is %s bytes, sha256 %s" % (f["size"], (f["sha256"] or "none")[:12])))
    out = {"id": cid, "rc": rc, "verdict": d["verdict"], "mounts": k["mounts"],
           "refusal": k["refusal"], "format": d.get("format", "v1")}
    out.update(CompactBook(d) if out["format"] == "v2" else CompactLedger(d))
    out.update({
            "findings": [[x["code"], x["severity"], x["text"]] for x in d["findings"]],
            "base_changed": changed,
            "new_files": [[f["fid"], f.get("name"), f["state"], f["visible"], f.get("size"), f.get("sha256")]
                          for f in d["files"] if (f["fid"], f.get("name")) not in base_names]})
    if out["format"] == "v2":
        # Where the files the base did not have lie, run by run — a book's file
        # may share its ground with another's (powercut_judge_book.py Dedup).
        out["new_runs"] = [[f["fid"], [r[:3] for r in f.get("runs", [])]] for f in d["files"]
                           if f["visible"] and (f["fid"], f.get("name")) not in base_names]
    return out


def CompactLedger(d):
    """Format v1's part: the next file id the mount derives against the one the
    Ledger it stands on counts, the chains — which are refused, how long — and
    what the kernel's fsck will say (a book has no fsck: bookcensus.py KernelBook)."""
    led, fsck = d["ledger"], d["kernel"]["fsck"] or {}
    stood = next((c for c in led["copies"] if c.get("copy") == led["standing_on"]), None)
    return {"says": d["kernel"]["says"], "next_file_id": led.get("next_file_id"),
            "fsck": {x: fsck.get(x) for x in ("orphan", "missing", "corrected", "lost")},
            "ledger_next": stood.get("next_file_id") if stood else None,
            "refused": d["chains"]["refused"] or {},
            "chains": {x: len(d["chains"][x]) for x in ("registry", "table", "pool")}}


def CompactBook(d):
    """A book's part (bookcensus.py Json, `book`): the next file id the mount
    derives against the one the slot it stands on counts — the Ledger's place
    in format v2 (SpineCounts) — the kernel's count line, and the ribbon: how
    many stitches it sews on, the last, how many torn, whether the medium is
    shown to have lied — and the last it sews on, every line the mount says
    of it by its code, and where each sector of each stitch lies on the device
    — on into its next Quire too (stitch.py Place.On). A book has no chains. And every file it shows, as [fid, name, size, sha256]: a book's
    file is the state a stitch left it in, held to every state a flush stood
    behind (powercut_judge_book.py Whole)."""
    k, b = d["kernel"], d["book"]
    slot = next((x for x in b["slots"] if x["index"] == b["slot"]), None)
    r, start = b["ribbon"], (d.get("ground") or {}).get("start", 0)
    return {"says": {"data_blocks": k.get("data_blocks"), "free": k.get("free"), "files": k.get("files")},
            "next_file_id": b["counts"].get("next_fid"),
            "ledger_next": (slot["counts"] or {}).get("next_fid") if slot else None,
            "refused": {}, "chains": {},
            "files": [[f["fid"], f.get("name"), f.get("size"), f.get("sha256")] for f in d["files"] if f["visible"]],
            "book": {"slot": b["slot"], "tally": (b["tally"] or {}).get("seq"),
                     "stitches": len(r["stitches"]), "torn": len(r["torn"]), "lied": r["lied"],
                     "glass": list(b["under_glass"]), "last": k.get("last"), "ribbon": k.get("ribbon") or {},
                     "sewn": [[u["seq"], [start + v for v in u["vlbas"]]] for u in r["stitches"]]}}


def CmdChecks(a):
    with open(a.plan) as fh:
        plan = json.load(fh)
    with open(a.basecheck) as fh:
        base = json.load(fh)
    base_files = {FileKey(f): f for f in base["files"] if f["visible"]}
    base_names = {(f["fid"], f.get("name")) for f in base["files"]}
    # A lie's plan (powercut_lie.py) is judged the same way into a file of its
    # own: its media named by what they lose, the power's cuts by what they keep.
    todo = [c for c in plan["media" if a.lies else "cuts"] if c["same"] is None]
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")

    def One(c, memo=None):
        cmd = [sys.executable, VOLUME_READ, a.base, "cut", a.log, "--epoch", str(c["epoch"]),
               "--keep", c["keep"] or "none"]
        if memo:
            cmd += [memo, a.digests]
        if c.get("tear"):
            cmd += ["--tear", "%d:0x%x" % tuple(c["tear"])]
        if c.get("blot"):
            cmd += ["--blot", c["blot"]]
        if c.get("smudge"):
            cmd += ["--smudge", ",".join("%d+%d" % tuple(x) for x in c["smudge"])]
        r = subprocess.run(cmd + ["--check", "--json"], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True, env=env)
        return Compact(c["id"], r.returncode, r.stdout, r.stderr, base_files, base_names)

    # The block digests a book's census takes are nearly all the same in every
    # cut of one record (bookcensus.py DigestMemoWrite): the final state's
    # check, the cut that holds the most of the record, takes them first and
    # keeps them in --digests, and every other check reads them there.
    first = {}
    if a.digests and not a.lies and plan["cuts"] and not os.path.exists(a.digests):
        final = {c["id"]: c for c in plan["cuts"]}.get("e%d-all" % max(c["epoch"] for c in plan["cuts"]))
        final = final and (final["same"] or final["id"])
        for c in todo:
            if c["id"] == final:
                first[final] = One(c, "--digests-out")
    memo = "--digests" if a.digests and os.path.exists(a.digests) else None

    def Each(c):
        return first.get(c["id"]) or One(c, memo)

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for n, res in enumerate(pool.map(Each, todo), 1):
            print(json.dumps(res, sort_keys=True))
            if n % 500 == 0:
                print("checks: %d of %d media judged" % (n, len(todo)), file=sys.stderr)
    return 0


# ---------------------------------------------------------------------------
# One run, as the judge and the boot selector see it
# ---------------------------------------------------------------------------


class Run:
    def __init__(self, d):
        self.dir = d
        self.plan = {"cuts": [], "epochs": []}
        if os.path.exists(os.path.join(d, "plan.json")):
            with open(os.path.join(d, "plan.json")) as fh:
                self.plan = json.load(fh)
        with open(os.path.join(d, "list.json")) as fh:
            self.listing = json.load(fh)
        self.results = {c["id"]: c for c in LoadLines(os.path.join(d, "cuts.jsonl"))}
        self.steps = LoadLines(os.path.join(d, "steps.jsonl"))
        self.cut = {c["id"]: c for c in self.plan["cuts"]}
        self.canon = [c for c in self.plan["cuts"] if c["same"] is None]
        self.through = {}
        for c in self.plan["cuts"]:
            cid = c["same"] or c["id"]
            self.through[cid] = max(self.through.get(cid, 0), c["through"])
        self.writes = {(w["epoch"], w["index"]): w for w in self.listing["writes"]}
        self.flushes = sorted(e["flush_seq"] for e in self.listing["epochs"] if e["flush_seq"])
        self.closes = {e["flush_seq"]: e["epoch"] for e in self.listing["epochs"] if e["flush_seq"]}
        self.final = self.Canon("e%d-all" % self.listing["epochs"][-1]["epoch"])
        self.kind = self.Kinds()

    def Kinds(self):
        """Every medium's kind, prefix or subset (see the top of this file):
        a cut's kept volume writes are the first n of its epoch's, and a torn
        write is the next one — and a medium two cuts build is a prefix when
        either of them is."""
        order = {}
        for w in self.listing["writes"]:
            if not Outside(w):
                order.setdefault(w["epoch"], []).append(w["index"])
        kind = {}
        for c in self.plan["cuts"]:
            vol = order.get(c["epoch"], [])
            keep = RangeSet(c["keep"]) & set(vol)
            n, cid = len(keep), c["same"] or c["id"]
            if keep == set(vol[:n]) and (c["tear"] is None or (n < len(vol) and c["tear"][0] == vol[n])):
                kind[cid] = "prefix"
            else:
                kind.setdefault(cid, "subset")
        return kind

    def Canon(self, cid):
        c = self.cut.get(cid)
        return None if c is None else c["same"] or c["id"]

    def Kept(self, cid):
        """The entries a cut applies of its own epoch, and what they are."""
        c = self.cut[cid]
        ws = [self.writes[(c["epoch"], i)] for i in sorted(RangeSet(c["keep"]))]
        kinds = sorted({w["touches"][0]["kind"] for w in ws if w["touches"]})
        tear = ""
        if c["tear"]:
            w = self.writes[(c["epoch"], c["tear"][0])]
            tear = ", entry %d (%s) torn to sector mask 0x%x" % (
                w["seq"], w["touches"][0]["kind"] if w["touches"] else "?", c["tear"][1])
        kept = Ranges(w["seq"] for w in ws) or "none"
        if len(kept) > 120:
            everyone = [w["seq"] for (k, _i), w in self.writes.items() if k == c["epoch"]]
            kept = "%d of the epoch's %d (the whole list is %s in plan.json)" % (len(ws), len(everyone),
                                                                                cid)
        return "epoch %d, entries %s kept (%s)%s" % (c["epoch"], kept, ", ".join(kinds) or "nothing",
                                                      tear)

    def CommitWrites(self, step):
        """The step's own writes that commit what it changed: entries in (before,
        ack] that touch format v1's Ledger, or a book's stitch — the unit a
        Settle cycle of format v2 commits by (spec_v2 §3.2), a slot the one a
        Tally does (COMMIT_KINDS)."""
        return [w["seq"] for w in self.listing["writes"] if step["before"] < w["seq"] <= step["ack"]
                and any(t["kind"] in COMMIT_KINDS for t in w["touches"])]

    def AckFlush(self, step):
        """The flush behind the step's acknowledgement: the first after its last
        commit write, and no later than the ack was read. None when the step
        committed nothing, or no flush followed its last commit in time."""
        led = self.CommitWrites(step)
        f = next((s for s in self.flushes if led and s > led[-1]), None)
        return f if f is not None and f <= step["ack"] else None

    def Exhaustive(self, from_epoch=0, prefixes=False):
        """Whether a defect no cut shows is gone from this machine: every medium
        the model has of the epochs from `from_epoch` on was built — no subset
        left out (of the prefixes alone, when only they are asked about: the
        plan builds every one), no prefix of a last write left untorn — and no
        step left a writer behind, whose record is one run's timing."""
        return not any(s.get("noquiet") for s in self.steps) and all(
            (prefixes or not e["not_enumerated"]) and not e.get("tears_not_built", 0)
            for e in self.plan["epochs"] if e["epoch"] >= from_epoch)

    def Unbuilt(self, from_epoch=0, prefixes=False):
        """Why Exhaustive said no."""
        if any(s.get("noquiet") for s in self.steps):
            return "a writer kept writing through the record, so its cuts are one run's timing"
        es = [e for e in self.plan["epochs"] if e["epoch"] >= from_epoch]
        left = 0 if prefixes else sum(e["not_enumerated"] for e in es)
        return "%s subset(s) and %d torn prefix(es) of the record were not built" % (
            left if left < 1 << 40 else "over 2^%d" % (left.bit_length() - 1),
            sum(e.get("tears_not_built", 0) for e in es))

    def SeqCeiling(self, item, held):
        """The last seq a durable item may hold. TO 0 is "as far as the medium
        took the writer": no further than the record holds entries past FROM,
        where each record it writes is a write of its own (format v1), nor than
        HELD, the seq the final state reads back — the final state is every
        write of the record, so no cut of it holds a newer record. On a book a
        writer's 16 bytes ride the stitches as a Slip (spec_v2 §2.8): thousands
        of records between two stitches, the newest stitch carrying the last.
        Read off the medium and never off the writer's log, whose lines reach
        the serial port through the display's lane after its writes — a halt
        ends the programs before the lane is drained (system_halt.c), so the
        last word a log shows may be short of the last record written."""
        return item["to"] or max(item["from"] + self.listing["entries"], held)

    def LastWriteBefore(self, seq):
        return max((w["seq"] for w in self.listing["writes"] if w["seq"] < seq), default=0)

    def Judged(self, cid):
        """A cut volume-read answered for."""
        r = self.results.get(cid)
        return r is not None and "error" not in r

    def Signature(self, cid, chains=False):
        """The damage and leak codes a cut shows; with `chains`, CHAIN_REFUSED
        once per chain it names, as CHAIN_REFUSED:<chain>."""
        r = self.results.get(cid, {})
        harm = {(f[0], f[1]) for f in r.get("findings", []) if f[1] in HARM}
        if chains:
            harm = ({x for x in harm if x[0] != "CHAIN_REFUSED"} |
                    {("CHAIN_REFUSED:" + name, "damage") for name in r.get("refused", {})})
        return tuple(sorted(harm))

    def Step(self, name):
        return next((s for s in self.steps if s["step"] == name), None)


# ---------------------------------------------------------------------------
# boots — which media are booted
# ---------------------------------------------------------------------------


def PrefixPicks(run, name):
    """Every prefix of the epoch the step's ack flush closes, ending at one of
    the step's own writes."""
    step = run.Step(name)
    flush = run.AckFlush(step) if step else None
    if flush is None:
        return []
    k = run.closes[flush]
    vol = [w for w in run.listing["writes"] if w["epoch"] == k and not Outside(w)]
    always = {w["index"] for w in run.listing["writes"] if w["epoch"] == k and Outside(w)}
    out = []
    for j in range(1, len(vol) + 1):
        if vol[j - 1]["seq"] <= step["before"]:
            continue
        want = always | {w["index"] for w in vol[:j]}
        c = next((c for c in run.plan["cuts"] if c["epoch"] == k and c["tear"] is None
                  and RangeSet(c["keep"]) == want), None)
        if c is not None:
            w = vol[j - 1]
            out.append((c["same"] or c["id"], "prefix through entry %d (%s)"
                        % (w["seq"], w["touches"][0]["kind"] if w["touches"] else "?")))
    return out


def CmdBoots(a):
    run = Run(a.dir)
    chosen = {}

    def Add(cid, why):
        if cid is not None:
            chosen.setdefault(cid, []).append(why)

    if a.picks == "final":
        Add(run.final, "the final state")
    elif a.picks.startswith("prefixes:"):
        for cid, why in PrefixPicks(run, a.picks[len("prefixes:"):]):
            Add(cid, why)
    elif a.picks.startswith("dedup:"):
        import powercut_judge_book
        for cid, why in powercut_judge_book.DedupPicks(run, a.picks[len("dedup:"):]):
            Add(cid, why)
    for s in run.steps:
        f = run.AckFlush(s) if s["durable"] else None
        if f is not None:
            Add(run.Canon("e%d-all" % run.closes[f]), "the boundary after the flush of %s" % s["step"])
    # The first medium of every signature is booted — of every kind and
    # every chain refused, too, when the census is kept by kind, so the
    # kernel's own mount is held to check's for each (the judge's reconcile).
    seen = set()
    for c in run.canon:
        kind = run.kind[c["id"]] if a.by_kind else None
        sig = (kind, run.Signature(c["id"], a.by_kind))
        if sig not in seen:
            seen.add(sig)
            Add(c["id"], "the first%s of %s" % (" %s" % kind if kind else "",
                                                " ".join("%s/%s" % x for x in sig[1]) or "the consistent"))
    rest = [c["id"] for c in run.canon if c["id"] not in chosen]
    for cid in random.Random(int(a.seed, 16)).sample(rest, min(a.sample, len(rest))):
        Add(cid, "sampled")
    for cid, why in chosen.items():
        c = run.cut[cid]
        print("\t".join((cid, str(c["epoch"]), c["keep"] or "none",
                         "%d:0x%x" % tuple(c["tear"]) if c["tear"] else "-", "; ".join(why))))
    return 0


# ---------------------------------------------------------------------------
# crowd — the arithmetic that brings chaingrow's base to both edges
# ---------------------------------------------------------------------------


def CmdCrowd(a):
    """Where a volume stands against both edges — the file table one create
    short of the row its blocks run out at, and the tag registry's last block
    with room for exactly --room more of crowd's tags — read off volume-read's
    check of it, as the volume is and not as a count of crowd's files says it
    should be: the machine interns tags of its own (a program's console lane,
    `anchor`) whenever it likes.

    At both edges it prints "edges", how many files the recorded run makes to
    grow both chains, and the numbers. Anywhere else, "more", FILL and AGAIN:
    what one more round of crowd takes to get there. A crowd file adds a table
    row and, when its name is new, a registry record; a name made again — one
    of the --names crowd's earlier rounds made, crowd0000000000 up — adds the
    row alone. The registry is filled a record at a time, one that does not
    fit starting the next block (tag_registry.c flush), so the FILLs that
    leave --room are one every PER_BLOCK records from the first; the table's
    edge is the end of its last block, or of a block past it when the rows
    left are fewer than that FILL. The least FILL whose AGAIN the names made
    so far can cover is taken. Tab-separated; 1 when no round reaches both."""
    with open(a.check) as fh:
        d = json.load(fh)
    ch = d["chains"]
    nfid = next(c["next_file_id"] for c in d["ledger"]["copies"]
                if c["copy"] == d["ledger"]["standing_on"])
    rows = ch["table_rows_per_block"] * len(ch["table"])
    record = ch["registry_record_header"] + a.name_bytes
    per_block = ch["registry_block_data"] // record
    room = (ch["registry_block_data"] - ch["registry_tail_used"]) // record
    said = ("next_file_id %d; the file table %d block(s), %d rows, so fid %d takes its next block; "
            "the registry %d block(s), its last (data block %d) %d of %d bytes used — room for %d "
            "more of crowd's %d-byte records (%d of header, a %d-byte name), %d to a fresh block"
            % (nfid, len(ch["table"]), rows, rows, len(ch["registry"]), ch["registry"][-1],
               ch["registry_tail_used"], ch["registry_block_data"], room, record,
               ch["registry_record_header"], a.name_bytes, per_block))
    if room == a.room and rows - nfid == 1:
        run = max(room, rows - nfid) + 1
        print("edges\t%d\t%s; so %d of crowd's files grow both chains: the last is fid %d, the first "
              "row past the table's blocks, and its tag the record past the registry's room"
              % (run, said, run, nfid + run - 1))
        return 0
    first = (room - a.room) % per_block
    for edge in (rows + ch["table_rows_per_block"] * j for j in range(3)):
        fids = edge - 1 - nfid
        fill = next((t for t in range(first, fids + 1, per_block) if fids - t <= a.names + t), None)
        if fids >= 0 and fill is not None:
            print("more\t%d\t%d\t%s; so crowd makes %d new names and %d of them again: %d file ids, "
                  "next_file_id %d, and the registry's last block left room for %d"
                  % (fill, fids - fill, said, fill, fids - fill, fids, edge - 1, a.room))
            return 0
    print("no FILL and AGAIN of crowd's reach both edges: %s — %d of its names made so far"
          % (said, a.names), file=sys.stderr)
    return 1


def Main():
    ap = argparse.ArgumentParser(description="PowerCut's planner and judge; see tools/powercut.sh")
    sub = ap.add_subparsers(dest="verb")
    s = sub.add_parser("step")
    s.add_argument("steps")
    for x in ("step", "cmd", "line"):
        s.add_argument("--" + x, required=True)
    for x in ("before", "ack", "quiet"):
        s.add_argument("--" + x, type=int, required=True)
    s.add_argument("--durable")
    s.add_argument("--noquiet", action="store_true")
    s = sub.add_parser("plan")
    s.add_argument("list")
    s.add_argument("--seed", required=True)
    s.add_argument("--all-max", type=int, default=10)
    s.add_argument("--sample", type=int, default=64)
    s.add_argument("--tear-all-max", type=int, default=16)
    s = sub.add_parser("checks")
    for x in ("plan", "base", "log", "basecheck"):
        s.add_argument(x)
    s.add_argument("--jobs", type=int, default=4)
    s.add_argument("--lies", action="store_true")
    s.add_argument("--digests")
    s = sub.add_parser("lies")
    s.add_argument("list")
    s = sub.add_parser("lieboots")
    s.add_argument("dir")
    s.add_argument("--sample", type=int, default=2)
    s.add_argument("--seed", required=True)
    s = sub.add_parser("boots")
    s.add_argument("dir")
    s.add_argument("--picks", required=True)
    s.add_argument("--sample", type=int, default=8)
    s.add_argument("--seed", required=True)
    s.add_argument("--by-kind", action="store_true")
    s = sub.add_parser("judge")
    s.add_argument("dir")
    s.add_argument("--path", required=True)
    s.add_argument("--caveat", action="append", default=[])
    s.add_argument("--verify", action="append", default=[])
    s.add_argument("--hogged", action="store_true")
    s.add_argument("--final-consistent", action="store_true")
    s.add_argument("--waited", type=int, default=0)
    s.add_argument("--by-kind", action="store_true")
    s.add_argument("--medium", default="")          # a book's run signed by its medium (powercut.sh pc_sign)
    s = sub.add_parser("settlecut")
    s.add_argument("dir")
    s.add_argument("--path", required=True)
    s.add_argument("--caveat", action="append", default=[])
    s = sub.add_parser("crowd")
    s.add_argument("check")
    s.add_argument("--name-bytes", type=int, required=True)
    s.add_argument("--room", type=int, required=True)
    s.add_argument("--names", type=int, default=0)
    s = sub.add_parser("forge")
    for x in ("list", "log", "out"):
        s.add_argument(x)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if not a.verb:
        ap.error("say what to do: step, plan, checks, lies, lieboots, boots, judge, settlecut, crowd, forge "
                 "or selftest")
    if a.verb in ("selftest", "forge"):
        # The gate judging itself lives beside it, and is handed this very
        # module: the arithmetic tested is the arithmetic the judge runs.
        import powercut_selftest
        return (powercut_selftest.SelfTest(sys.modules[__name__]) if a.verb == "selftest"
                else powercut_selftest.Forge(a))
    if a.verb in ("lies", "lieboots"):
        # The lie lives beside it, and takes the run and Compact from this
        # module, registered under its own name as for the judge (below).
        sys.modules.setdefault("powercut", sys.modules[__name__])
        import powercut_lie
        return (powercut_lie.CmdLies if a.verb == "lies" else powercut_lie.CmdLieBoots)(a)
    if a.verb in ("judge", "settlecut"):
        # The judge lives beside it as well, and takes the run, the records and
        # the severities from this module: registered under its own name
        # first, the import it makes finds this very module and not a copy.
        sys.modules.setdefault("powercut", sys.modules[__name__])
        import powercut_judge
        return (powercut_judge.CmdJudge if a.verb == "judge" else powercut_judge.CmdSettleCut)(a)
    return {"step": CmdStep, "plan": CmdPlan, "checks": CmdChecks, "boots": CmdBoots,
            "crowd": CmdCrowd}[a.verb](a)


if __name__ == "__main__":
    sys.exit(Main())
