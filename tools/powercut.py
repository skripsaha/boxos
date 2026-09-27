#!/usr/bin/env python3
"""powercut — the planner, the checker's fan-out and the judge of tools/powercut.sh.

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
    powercut.py checks PLAN BASE LOG BASECHECK [--jobs N] > CUTS
    powercut.py boots DIR --picks final|prefixes:STEP --sample N --seed HEX > BOOTS
    powercut.py judge DIR --path PATH [--caveat KEY=NAME]... [--verify KIND] [--hogged] [--waited S]
    powercut.py settlecut DIR --path PATH [--caveat KEY=NAME]...
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
not tear. An epoch with nothing left unbuilt is enumerated whole, and only
there does a defect's absence say anything: a sample cannot show one is gone —
and neither can a record a writer kept writing (a step --noquiet), whose cuts
are one run's timing and not the machine's.
Two cuts whose every sector came from the same write are one medium and are
judged once: the listing carries no content hash (contract C3), so identity is
by provenance, and two writes that happen to carry the same bytes are not
collapsed — the count errs towards judging more, never fewer.

Every verdict is one tab-separated line on stdout, which the shell says in
stand.sh's words:
    KEY <TAB> caveat|plain|harness|note <TAB> NAME <TAB> defect|behaves|fail|unseen <TAB> WHAT
A note is no verdict: a caveated defect that a sampled census did not meet.

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
import re                            # noqa: E402
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
}


def Mix(a, b):
    """The checksum the three programs share (persist.c fold_checksum)."""
    x = (a ^ (b * 2654435761)) & 0xFFFFFFFF
    x ^= x >> 16
    return (x * 0x85EBCA6B) & 0xFFFFFFFF


def RecordBytes(kind, seq):
    magic, size = RECORDS[kind]
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


def Outside(w):
    return bool(w["touches"]) and all(t["kind"] == "outside" for t in w["touches"])


# ---------------------------------------------------------------------------
# step — one thing the machine was asked, as JSON the shell cannot get wrong
# ---------------------------------------------------------------------------


def CmdStep(a):
    step = {"step": a.step, "cmd": a.cmd, "before": a.before, "ack": a.ack, "quiet": a.quiet,
            "line": a.line, "durable": None, "noquiet": a.noquiet}
    if a.durable:
        name, record, lo, hi = a.durable.rsplit(":", 3)
        step["durable"] = {"name": name, "record": record, "size": RECORDS[record][1],
                           "from": int(lo), "to": int(hi)}
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
            tears = range(1, n) if n <= self.tear_all_max else sorted({1, n - 1} | set(range(8, n, 8)))
            for j in tears:
                out.append(("t%03d" % j, frozenset(idx[:-1]), (vol[-1]["index"], (1 << j) - 1)))
        return out, how, len(tears), max(0, n - 1 - len(tears)), n

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
    return (f["fid"], f["name"], f["size"], f["sha256"])


def Compact(cid, rc, out, err, base_files, base_names):
    """What the judge needs from one `check --json`: the mount the kernel would
    make, every finding, the base's files where they differ from the base, and
    every file the base did not have."""
    try:
        d = json.loads(out)
    except ValueError:
        return {"id": cid, "rc": rc, "error": ((out.strip() or err.strip() or "no output")
                                               .splitlines()[-1])}
    k, fsck = d["kernel"], d["kernel"]["fsck"] or {}
    files = {f["fid"]: f for f in d["files"]}
    changed = []
    for key, bf in base_files.items():
        f = files.get(bf["fid"])
        if f is None or not f["visible"] or FileKey(f) != key:
            changed.append("fid %d %s %s" % (bf["fid"], bf["name"], "gone" if f is None else
                           "not visible (%s)" % f["state"] if not f["visible"] else
                           "is %s bytes, sha256 %s" % (f["size"], (f["sha256"] or "none")[:12])))
    return {"id": cid, "rc": rc, "verdict": d["verdict"], "mounts": k["mounts"],
            "refusal": k["refusal"], "says": k["says"],
            "fsck": {x: fsck.get(x) for x in ("orphan", "missing", "corrected", "lost")},
            "findings": [[x["code"], x["severity"], x["text"]] for x in d["findings"]],
            "base_changed": changed,
            "new_files": [[f["fid"], f["name"], f["state"], f["visible"], f["size"], f["sha256"]]
                          for f in d["files"] if (f["fid"], f["name"]) not in base_names]}


def CmdChecks(a):
    with open(a.plan) as fh:
        plan = json.load(fh)
    with open(a.basecheck) as fh:
        base = json.load(fh)
    base_files = {FileKey(f): f for f in base["files"] if f["visible"]}
    base_names = {(f["fid"], f["name"]) for f in base["files"]}
    todo = [c for c in plan["cuts"] if c["same"] is None]
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")

    def One(c):
        cmd = [sys.executable, VOLUME_READ, a.base, "cut", a.log, "--epoch", str(c["epoch"]),
               "--keep", c["keep"] or "none"]
        if c["tear"]:
            cmd += ["--tear", "%d:0x%x" % tuple(c["tear"])]
        r = subprocess.run(cmd + ["--check", "--json"], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True, env=env)
        return Compact(c["id"], r.returncode, r.stdout, r.stderr, base_files, base_names)

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for n, res in enumerate(pool.map(One, todo), 1):
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

    def LedgerWrites(self, step):
        """The step's own writes that touch the ledger: entries in (before, ack]."""
        return [w["seq"] for w in self.listing["writes"] if step["before"] < w["seq"] <= step["ack"]
                and any(t["kind"] == "ledger" for t in w["touches"])]

    def AckFlush(self, step):
        """The flush behind the step's acknowledgement: the first after its last
        ledger write, and no later than the ack was read. None when the step
        wrote no ledger, or no flush followed its last one in time."""
        led = self.LedgerWrites(step)
        f = next((s for s in self.flushes if led and s > led[-1]), None)
        return f if f is not None and f <= step["ack"] else None

    def Exhaustive(self, from_epoch=0):
        """Whether a defect no cut shows is gone from this machine: every medium
        the model has of the epochs from `from_epoch` on was built — no subset
        left out, no prefix of a last write left untorn — and no step left a
        writer behind, whose record is one run's timing."""
        return not any(s.get("noquiet") for s in self.steps) and all(
            not e["not_enumerated"] and not e.get("tears_not_built", 0)
            for e in self.plan["epochs"] if e["epoch"] >= from_epoch)

    def Unbuilt(self, from_epoch=0):
        """Why Exhaustive said no."""
        if any(s.get("noquiet") for s in self.steps):
            return "a writer kept writing through the record, so its cuts are one run's timing"
        es = [e for e in self.plan["epochs"] if e["epoch"] >= from_epoch]
        left = sum(e["not_enumerated"] for e in es)
        return "%s subset(s) and %d torn prefix(es) of the record were not built" % (
            left if left < 1 << 40 else "over 2^%d" % (left.bit_length() - 1),
            sum(e.get("tears_not_built", 0) for e in es))

    def SeqCeiling(self, item):
        """The last seq a durable item may hold. TO 0 is "as many as the record
        has entries": a writer that starts at FROM cannot have written more
        records than the record holds writes."""
        return item["to"] or item["from"] + self.listing["entries"]

    def LastWriteBefore(self, seq):
        return max((w["seq"] for w in self.listing["writes"] if w["seq"] < seq), default=0)

    def Judged(self, cid):
        """A cut volume-read answered for."""
        r = self.results.get(cid)
        return r is not None and "error" not in r

    def Signature(self, cid):
        return tuple(sorted({(f[0], f[1]) for f in self.results.get(cid, {}).get("findings", [])
                             if f[1] in HARM}))

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
    for s in run.steps:
        f = run.AckFlush(s) if s["durable"] else None
        if f is not None:
            Add(run.Canon("e%d-all" % run.closes[f]), "the boundary after the flush of %s" % s["step"])
    seen = set()
    for c in run.canon:
        sig = run.Signature(c["id"])
        if sig not in seen:
            seen.add(sig)
            Add(c["id"], "the first of %s" % (" ".join("%s/%s" % x for x in sig) or "the consistent"))
    rest = [c["id"] for c in run.canon if c["id"] not in chosen]
    for cid in random.Random(int(a.seed, 16)).sample(rest, min(a.sample, len(rest))):
        Add(cid, "sampled")
    for cid, why in chosen.items():
        c = run.cut[cid]
        print("\t".join((cid, str(c["epoch"]), c["keep"] or "none",
                         "%d:0x%x" % tuple(c["tear"]) if c["tear"] else "-", "; ".join(why))))
    return 0


# ---------------------------------------------------------------------------
# judge — every oracle, one verdict line each
# ---------------------------------------------------------------------------


class Judge:
    def __init__(self, run, base, caveats, path, waited):
        self.run, self.base, self.caveats, self.path, self.waited = run, base, caveats, path, waited
        self.census, self.boots = {}, []

    def Out(self, key, seen, what, caveat=None):
        name = self.caveats.get(caveat or key)
        kind = ("harness" if seen == "harness" else "note" if seen == "unseen" else
                "caveat" if name else "plain")
        print("\t".join((key, kind, name or "-", "fail" if seen == "harness" else seen,
                         (what + " [path: %s]" % self.path).replace("\t", " ").replace("\n", " "))))

    def Acks(self):
        for s in self.run.steps:
            if not s["durable"]:
                continue
            f, led = self.run.AckFlush(s), self.run.LedgerWrites(s)
            if not led:
                self.Out("ack:" + s["step"], "fail", "%s was acknowledged at record entry %d and wrote "
                         "no ledger in entries %d..%d, so no sync of its own stands behind it"
                         % (s["step"], s["ack"], s["before"] + 1, s["ack"]))
            elif f is None:
                self.Out("ack:" + s["step"], "fail", "%s was acknowledged at record entry %d with no "
                         "flush behind its last ledger write, entry %d — %s"
                         % (s["step"], s["ack"], led[-1], "the answer was read with nothing after it"
                            if led[-1] >= s["ack"] else "none in entries %d..%d" % (led[-1] + 1, s["ack"])))
            else:
                self.Out("ack:" + s["step"], "behaves", "%s was acknowledged at record entry %d and "
                         "the flush at entry %d, the first after its last ledger write (entry %d), "
                         "closing epoch %d, stands behind it"
                         % (s["step"], s["ack"], f, led[-1], self.run.closes[f]))

    def BaseFiles(self):
        """No cut the kernel mounts loses or changes a file the base held. A cut
        it refuses to mount holds no file of anybody's, and every refusal is a
        damage code of its own (GROUND_REFUSED … CHAIN_REFUSED), judged there."""
        run = self.run
        judged = [c["id"] for c in run.canon if run.Judged(c["id"])]
        mounted = [cid for cid in judged if run.results[cid]["mounts"]]
        lost = [(cid, run.results[cid]["base_changed"]) for cid in mounted
                if run.results[cid]["base_changed"]]
        count = sum(1 for f in self.base["files"] if f["visible"])
        refused = len(judged) - len(mounted)
        rest = "; the %d cut(s) the kernel refuses to mount are their codes'" % refused if refused else ""
        if lost:
            cid, ch = lost[0]
            self.Out("d0", "fail", "%d of %d mountable cuts lost or changed a file the base held, e.g. "
                     "%s (%s): %s%s" % (len(lost), len(mounted), cid, run.Kept(cid), "; ".join(ch[:3]),
                                        rest))
        else:
            self.Out("d0", "behaves", "all %d files of the base are in every one of %d mountable cuts, "
                     "byte for byte%s" % (count, len(mounted), rest))

    def Durable(self):
        run = self.run
        for s in run.steps:
            item = s["durable"]
            f = run.AckFlush(s) if item else None
            if f is None:
                continue
            hashes = {hashlib.sha256(RecordBytes(item["record"], q)).hexdigest(): q
                      for q in range(item["from"], run.SeqCeiling(item) + 1)}
            need = run.LastWriteBefore(f)
            after = [c["id"] for c in run.canon if run.through[c["id"]] >= need]
            bad = []
            for cid in after:
                mine = [x for x in run.results.get(cid, {}).get("new_files", []) if x[1] == item["name"]]
                if not any(x[3] and x[4] == item["size"] and x[5] in hashes for x in mine):
                    bad.append((cid, "; ".join("fid %d %s (%s), visible %s, %s bytes" % tuple(x[:5])
                                               for x in mine) or "no file of that name"))
            key = "durable:%s@%s" % (item["name"], s["step"])
            if bad:
                self.Out(key, "defect" if self.caveats.get(key) else "fail",
                         "%s, made durable by the flush at entry %d, is missing or wrong "
                         "in %d of the %d cuts after it, e.g. %s (%s): %s"
                         % (item["name"], f, len(bad), len(after), bad[0][0], run.Kept(bad[0][0]),
                            bad[0][1]))
            elif after:
                seqs = sorted({hashes[x[5]] for cid in after for x in run.results[cid]["new_files"]
                               if x[1] == item["name"] and x[5] in hashes})
                whole = run.Exhaustive(run.closes[f])
                self.Out(key, "behaves" if whole or not self.caveats.get(key) else "unseen",
                         "%s is visible with its %d bytes, a whole record (seq %s, %d..%d allowed), in "
                         "every one of the %d cuts after the flush at entry %d%s"
                         % (item["name"], item["size"], Ranges(seqs), item["from"],
                            run.SeqCeiling(item), len(after), f,
                            "" if whole else "; " + run.Unbuilt(run.closes[f])))
            else:
                self.Out(key, "fail", "no cut stands after the flush at entry %d, so the durability "
                         "of %s was never judged" % (f, item["name"]))

    def Codes(self):
        run, broken = self.run, []
        for c in run.canon:
            r = run.results.get(c["id"])
            if r is None or "error" in r:
                broken.append((c["id"], (r or {}).get("error", "no answer")))
                continue
            for code, sev, text in r["findings"]:
                e = self.census.setdefault((code, sev), {"cuts": set(), "first": c["id"], "text": text})
                e["cuts"].add(c["id"])
        if broken:
            self.Out("checker", "harness", "volume-read could not judge %d of %d cuts, e.g. %s: %s"
                     % (len(broken), len(run.canon), broken[0][0], broken[0][1]))
        judged = len(run.canon) - len(broken)
        harm = sorted(k for k in self.census if k[1] in HARM)
        for code, sev in harm:
            e = self.census[(code, sev)]
            what = "%s (%s) in %d of %d cuts, first %s (%s): %s" % (
                code, sev, len(e["cuts"]), judged, e["first"], run.Kept(e["first"]), e["text"])
            if self.caveats.get("code:" + code):
                self.Out("code:" + code, "defect", what)
            else:
                self.Out("code:" + code, "fail", what + " — no caveat names it; its step is the "
                         "owner's to assign")
        whole = run.Exhaustive()
        for key in sorted(self.caveats):
            if key.startswith("code:") and key[5:] not in {c for c, _s in harm}:
                self.Out(key, "behaves" if whole else "unseen", "no cut of %d shows %s%s"
                         % (judged, key[5:], "" if whole else "; " + run.Unbuilt()))
        if not harm:
            self.Out("codes", "behaves", "every one of %d cuts is consistent" % judged)

    def Boundaries(self):
        """Every state a sync stood behind — an epoch its flush closed, holding the ledger HEAD's sync
        writes last, all applied — is consistent; a WriteJob's flush of data promises nothing."""
        synced = {w["epoch"] for w in self.run.listing["writes"]
                  if any(t["kind"] == "ledger" for t in w["touches"])}
        ends = [(self.run.Canon("e%d-all" % e["epoch"]), e["flush_seq"])
                for e in self.run.listing["epochs"] if e["flush_seq"] and e["epoch"] in synced]
        blind = ["%s (flush at entry %d)" % (c, f) for c, f in ends if not self.run.Judged(c)]
        bad = ["%s (flush at entry %d): %s" % (c, f, " ".join("%s/%s" % s for s in self.run.Signature(c)))
               for c, f in ends if self.run.Judged(c) and self.run.Signature(c)]
        if blind:
            self.Out("boundaries", "harness", "volume-read could not judge %d of %d syncs' flushes: %s"
                     % (len(blind), len(ends), "; ".join(blind[:3])))
        elif ends:
            self.Out("boundaries", "fail" if bad else "behaves", "%d of %d syncs' flushes stand behind an "
                     "inconsistent volume: %s" % (len(bad), len(ends), "; ".join(bad[:3])) if bad else
                     "the volume is consistent at every one of %d syncs' flushes" % len(ends))

    def Boots(self):
        path = os.path.join(self.run.dir, "boots.txt")
        for line in (open(path).read().splitlines() if os.path.exists(path) else []):
            cid, _k, _keep, _tear, why = line.split("\t")
            lp = os.path.join(self.run.dir, "boots", cid + ".serial.log")
            log = open(lp, errors="replace").read().replace("\r", "") if os.path.exists(lp) else ""
            self.boots.append({"id": cid, "why": why, "log": log})
        if not self.boots:
            self.Out("boots", "fail", "no cut was booted")
            return
        dead, wrong = [], []
        for b in self.boots:
            r = self.run.results.get(b["id"], {})
            if "KERNEL PANIC" in b["log"] or "BoxOS Shell" not in b["log"]:
                dead.append("%s: %s" % (b["id"], "KERNEL PANIC" if "KERNEL PANIC" in b["log"]
                                        else "no shell"))
            elif r and "error" not in r:
                wrong += ["%s: %s" % (b["id"], w) for w in Reconcile(r, b["log"])]
        self.Out("boots", "fail" if dead else "behaves", "%d of %d booted cuts panicked or never "
                 "reached a shell: %s" % (len(dead), len(self.boots), "; ".join(dead))
                 if dead else "all %d booted cuts reached a shell with no panic" % len(self.boots))
        self.Out("reconcile", "fail" if wrong else "behaves", "the kernel's mount disagrees with "
                 "check in %d place(s): %s" % (len(wrong), "; ".join(wrong[:4])) if wrong else
                 "on all %d boots the kernel mounted, counted and repaired as check predicted"
                 % len(self.boots))

    def Final(self):
        return next((b for b in self.boots if b["id"] == self.run.final), None)

    def Verify(self, kind):
        """The scenario's own question, asked of the booted media."""
        what, _sep, arg = kind.partition(":")
        if what == "reissue":
            return self.Reissue(arg)
        final = self.Final()
        if final is None:
            return self.Out("verify", "fail", "the final state was not booted")
        if what == "persist":
            return self.Persist(final["log"])
        name, _sep, step_name = arg.partition(":")
        return self.Quill(final["log"], name, what == "quill-exact", self.run.Step(step_name))

    def Persist(self, log):
        step = self.run.Step("persist")
        want = re.search(r"WROTE fid=(\d+)", step["line"]) if step else None
        red = re.search(r"\[PERSIST\] FAIL: file '_persist_test' not found after reboot \(rc=0\)", log)
        green = re.search(r"\[PERSIST\] READ  fid=(\d+) rrc=16 seq=1 .*\n.*\[PERSIST\] PASS", log)
        if red:
            self.Out("verify", "defect", "persist wrote fid=%s, the power went %d s later, and on the "
                     "final state %s" % (want.group(1) if want else "?", self.waited, red.group(0)))
        elif green and want and green.group(1) == want.group(1):
            self.Out("verify", "behaves", "persist verify read fid=%s back whole on the final state"
                     % green.group(1))
        else:
            self.Out("verify", "fail", "persist verify on the final state: %s"
                     % ("; ".join(re.findall(r"\[PERSIST\] [^\n]*", log)) or "no answer"))

    def Quill(self, log, name, exact, step):
        if step is None:
            return self.Out("verify", "fail", "no step says what %s must hold" % name)
        want = re.search(r"seq=(\d+)", step["line"])
        floor = step["durable"]["from"] if step["durable"] else 1
        red = re.search(r"\[QUILL\] verify: FAIL — the volume has 0 file\(s\) called %s[^\n]*"
                        % re.escape(name), log)
        ok = re.search(r"\[QUILL\] verify: PASS — %s fid=(\d+) seq=(\d+) bytes=16" % re.escape(name), log)
        held = ok and (int(ok.group(2)) == int(want.group(1)) if exact and want
                       else int(ok.group(2)) >= floor)
        if red:
            self.Out("verify", "defect", "%s was written (seq %s), the power went %d s later, and on "
                     "the final state %s" % (name, want.group(1) if want else "?", self.waited,
                                             red.group(0)))
        elif held:
            self.Out("verify", "behaves", "quilltest verify read %s fid=%s seq=%s back whole on the "
                     "final state (%s)" % (name, ok.group(1), ok.group(2), "seq %s was written"
                                           % want.group(1) if exact and want
                                           else "at least seq %d was due" % floor))
        else:
            self.Out("verify", "fail", "quilltest verify on the final state: %s"
                     % ("; ".join(re.findall(r"\[QUILL\] verify: [^\n]*", log)) or "no answer"))

    def Reissue(self, step_name):
        step = self.run.Step(step_name)
        m = re.search(r"fid=(\d+)", step["line"]) if step else None
        fid = m.group(1) if m else None
        seen, judged = [], 0
        for b in self.boots:
            if not b["why"].startswith("prefix"):
                continue
            a = re.search(r"\[ANCHOR\] (?:WROTE\+ANCHORED fid=(\d+)|PASS)", b["log"])
            if not a:
                continue
            judged += 1
            named = any(x[0] == "FID_PAST_NEXT" and x[2].startswith("fid %s '" % fid)
                        for x in self.run.results.get(b["id"], {}).get("findings", []))
            if a.group(1) == fid and named:
                seen.append("%s (%s): its table names fid %s and anchor_test was issued fid %s again"
                            % (b["id"], b["why"], fid, a.group(1)))
        if not judged:
            self.Out("reissue", "fail", "no prefix cut of the anchor's epoch answered anchor_test")
        elif seen:
            self.Out("reissue", "defect", "%d of %d prefix cuts issued a file id their table already "
                     "names: %s" % (len(seen), judged, "; ".join(seen[:2])))
        else:
            self.Out("reissue", "behaves", "none of %d prefix cuts issued fid %s twice" % (judged, fid))

    def FinalState(self):
        """The state an orderly halt left: consistent, and booted, nothing lost."""
        final, r = self.Final(), self.run.results.get(self.run.final or "", {})
        log = final["log"] if final else ""
        gone = sorted(set(re.findall(r"\[TagFS FSCK\] file (\d+) is named at block \d+\+\d+ and there "
                                     r"is no readable metadata", log)))
        lost = re.search(r"(\d+) file\(s\) lost", log)
        harm = sorted({"%s/%s" % (x[0], x[1]) for x in r.get("findings", []) if x[1] in HARM})
        held = final is not None and r.get("verdict") == "consistent" and not gone and \
            not (lost and int(lost.group(1)))
        self.Out("final", "behaves" if held else "fail", "the state the halt left (%s): check says %s%s; "
                 "booted, %s file(s) gone and %s lost" % (
                     self.run.final, r.get("verdict", "nothing"), " — " + " ".join(harm) if harm else "",
                     len(gone), lost.group(1) if lost else "no"))

    def Hogged(self):
        final = self.Final()
        m = re.search(r"\[QUILL\] hogged: (PASS|FAIL) — [^\n]*", final["log"]) if final else None
        self.Out("hoganchor", "behaves" if m and m.group(1) == "PASS" else "fail",
                 m.group(0) if m else "quilltest hogged anchor_test never answered beside the hog on "
                 "one core — an anchor nobody answers is a defect of its own, not this caveat")


def Reconcile(r, log):
    """Where the kernel's own mount lines disagree with what check predicted."""
    mounted = re.search(r"is MOUNTED — (\d+) file", log)
    refused = re.search(r"\[TagFS\] this volume is NOT mounted: ([^\n]*)", log)
    if not r["mounts"]:
        return [] if refused else ["check predicted the refusal '%s' and the kernel %s"
                                   % (r["refusal"], "mounted" if mounted else "said neither")]
    if not mounted:
        return ["check predicted a mount and the kernel %s" % ("refused: " + refused.group(1)
                                                              if refused else "said neither")]
    wrong = []
    says = re.search(r"\[TagFS\] (\d+) data blocks, (\d+) free, (\d+) files", log)
    have = tuple(map(int, says.groups())) if says else None
    want = (r["says"]["data_blocks"], r["says"]["free"], r["says"]["files"])
    if have != want or int(mounted.group(1)) != want[2]:
        wrong.append("the kernel says %s blocks, free, files and MOUNTED %s; check predicted %s"
                     % (have, mounted.group(1), want))
    for pat, a, b in ((r"\[TagFS FSCK\] Bitmap mismatch: (\d+) orphan, (\d+) missing", "orphan", "missing"),
                      (r"\[TagFS FSCK\] (\d+) file table entry\(s\) corrected from this volume's "
                       r"metadata pool, (\d+) file\(s\) lost", "corrected", "lost")):
        m = re.search(pat, log)
        got, due = (tuple(map(int, m.groups())) if m else (0, 0)), (r["fsck"][a], r["fsck"][b])
        if got != due:
            wrong.append("fsck says %u %s, %u %s; check predicted %u and %u"
                         % (got[0], a, got[1], b, due[0], due[1]))
    return wrong


def CmdJudge(a):
    run = Run(a.dir)
    with open(os.path.join(a.dir, "base.check.json")) as fh:
        base = json.load(fh)
    j = Judge(run, base, dict(x.split("=", 1) for x in a.caveat), a.path, a.waited)
    j.Acks()
    j.BaseFiles()
    j.Durable()
    j.Codes()
    j.Boundaries()
    j.Boots()
    if a.verify:
        j.Verify(a.verify)
    if a.final_consistent:
        j.FinalState()
    if a.hogged:
        j.Hogged()
    with open(os.path.join(a.dir, "report.txt"), "w") as fh:
        fh.write(Report(run, j))
    return 0


# ---------------------------------------------------------------------------
# settlecut — the second oracle, held against the first
# ---------------------------------------------------------------------------


def Harm(d):
    return sorted({(x["code"], x["severity"]) for x in d["findings"] if x["severity"] in HARM})


def Codes(codes):
    return " ".join("%s/%s" % c for c in codes) or "consistent"


def CmdSettleCut(a):
    """A kernel built SETTLECUT=ledger stopped its first sync before the ledger (DIR/ledger.check.json);
    one built SETTLECUT=done stopped it after the flush (DIR/done.check.json). The recorded plain run of
    the same anchor_test is cut at two points of its own sync — through the file table, and everything
    before the ledger — and each is judged by volume-read: the SETTLECUT medium and PowerCut's cut
    before the ledger must name the same codes. The SETTLECUT=ledger medium is judged code by code, as
    Codes judges a census: each harm code is held under its own caveat or is a FAIL, and a caveated code
    it does not show is lifted early — one medium is its own whole census."""
    run = Run(a.dir)
    judge = Judge(run, None, dict(x.split("=", 1) for x in a.caveat), a.path, 0)
    checks = {}
    for point in ("ledger", "done"):
        with open(os.path.join(a.dir, point + ".check.json")) as fh:
            checks[point] = json.load(fh)
    ledger, done = checks["ledger"], checks["done"]
    harm, first = Harm(ledger), {}
    for x in ledger["findings"]:
        first.setdefault(x["code"], x["text"])
    for code, sev in harm:
        what = "SETTLECUT=ledger left %s/%s — %s" % (code, sev, first[code])
        if judge.caveats.get("code:" + code):
            judge.Out("code:" + code, "defect", what)
        else:
            judge.Out("code:" + code, "fail", what + " — no caveat names it; its step is the owner's to "
                      "assign")
    for key in sorted(judge.caveats):
        if key.startswith("code:") and key[5:] not in {c for c, _s in harm}:
            judge.Out(key, "behaves", "SETTLECUT=ledger left %s, and no %s" % (Codes(harm), key[5:]))
    step = run.Step("anchor_test")
    flush = run.AckFlush(step) if step else None
    if flush is None:
        judge.Out("cross", "fail", "the recorded anchor_test has no flush behind it, so its sync has no "
                  "points to cut at")
    else:
        k = run.closes[flush]
        ws = [w for w in run.listing["writes"] if w["epoch"] == k and not Outside(w)]
        kind = [w["touches"][0]["kind"] if w["touches"] else "?" for w in ws]
        mine = [i for i, w in enumerate(ws) if w["seq"] > step["before"]]
        table = next((i for i in mine if kind[i] == "table"), None)
        before = next((i for i in mine if kind[i] == "ledger"), None)
        if table is None or before is None:
            judge.Out("cross", "fail", "the anchor's sync in epoch %d wrote no %s"
                      % (k, "file table" if table is None else "ledger"))
        else:
            said = []
            for name, n in (("through the file table", table + 1), ("before the ledger", before)):
                r = subprocess.run([sys.executable, VOLUME_READ, os.path.join(a.dir, "base.img"), "cut",
                                    os.path.join(a.dir, "run.log"), "--epoch", str(k), "--keep",
                                    Ranges(w["index"] for w in ws[:n]) or "none", "--check", "--json"],
                                   stdout=subprocess.PIPE, universal_newlines=True)
                said.append((name, ws[n - 1]["seq"], Harm(json.loads(r.stdout))))
            judge.Out("cross", "behaves" if said[1][2] == Harm(ledger) else "fail",
                      "SETTLECUT=ledger names %s; PowerCut's cut of the same sync %s" % (
                          Codes(Harm(ledger)), "; ".join("%s (entries to %d) names %s" % (
                              name, seq, Codes(codes)) for name, seq, codes in said)))
    anchored = [f for f in done["files"] if f["name"] == "_anchor_test"]
    whole = [f for f in anchored if f["visible"] and f["size"] == 128 and
             f["sha256"] == hashlib.sha256(RecordBytes("anchor", 1)).hexdigest()]
    judge.Out("done", "behaves" if whole and done["verdict"] == "consistent" else "fail",
              "SETTLECUT=done left a %s volume, and _anchor_test %s" % (
                  done["verdict"], "whole: 128 bytes, seq 1" if whole else
                  "; ".join("%s, visible %s, %s bytes" % (f["state"], f["visible"], f["size"])
                            for f in anchored) or "absent"))
    return 0


# ---------------------------------------------------------------------------
# report — what was enumerated, found and booted
# ---------------------------------------------------------------------------


def Report(run, judge):
    L, p = [], run.plan
    facts = os.path.join(run.dir, "facts.txt")
    if os.path.exists(facts):
        L += open(facts).read().splitlines()
    L.append("record: %d entries walked (%s counted by its superblock), %d flushes; the walk stopped "
             "at %s%s" % (run.listing["entries"], run.listing["super_entries"], len(run.flushes),
                          run.listing["stop"], "".join("; " + n for n in run.listing["notes"])))
    L.append("plan: seed %s — every subset of an epoch of up to %d volume writes, else every prefix, "
             "every single omission and %d drawn; the last write of each epoch torn by sectors "
             "(every sector up to %d)" % (p["seed"], p["all_max"], p["sample"], p["tear_all_max"]))
    groups = []
    for e in p["epochs"]:
        shape = tuple(e.get(k, 0) for k in ("writes", "outside", "subsets", "enumerated", "not_enumerated",
                                            "tears", "tears_not_built", "tear_sectors", "distinct",
                                            "collapsed", "how"))
        if groups and groups[-1][2] == shape and groups[-1][1] == e["epoch"] - 1:
            groups[-1][1] = e["epoch"]
        else:
            groups.append([e["epoch"], e["epoch"], shape])
    for a, b, (m, o, sub, en, ne, t, tn, ts, d, c, how) in groups:
        exact = sub < 1 << 40      # past that the count is written as the power it is
        L.append("  %s: %d volume write(s)%s — %s subset(s), %d built (%s), %s NOT built; %d torn%s; "
                 "%d distinct media, %d collapsed into an earlier one"
                 % ("epoch %d" % a if a == b else "epochs %d..%d (%d, each)" % (a, b, b - a + 1), m,
                    " + %d outside the volume, always applied" % o if o else "",
                    sub if exact else "2^%d" % m, en, how, ne if exact else "exactly 2^%d - %d" % (m, en),
                    t, " of a %d-sector last write, %d prefix tear(s) NOT built" % (ts, tn) if tn else "",
                    d, c))
    for s in run.steps:
        f = run.AckFlush(s)
        L.append("step %s: `%s` from entry %d, acknowledged at %d, the record quiet at %d%s — %s"
                 % (s["step"], s["cmd"], s["before"], s["ack"], s["quiet"],
                    "; %s durable behind the flush at entry %d" % (s["durable"]["name"], f)
                    if s["durable"] and f else "", s["line"].strip()[:160]))
    L.append("check census, %d distinct media:" % len(run.canon))
    for (code, sev), e in sorted(judge.census.items(), key=lambda kv: (HARM + ("note",)).index(kv[0][1])):
        L.append("  %-6s %-22s %6d cut(s), first %s — %s" % (sev, code, len(e["cuts"]), e["first"],
                                                            run.Kept(e["first"])))
    L.append("boots: %d of %d media" % (len(judge.boots), len(run.canon)))
    for b in judge.boots:
        m = re.search(r"is MOUNTED — (\d+) file", b["log"])
        L.append("  %s — %s: %s" % (b["id"], b["why"], "KERNEL PANIC" if "KERNEL PANIC" in b["log"]
                                    else "mounted, %s file(s)" % m.group(1) if m else
                                    "NOT mounted" if "is NOT mounted" in b["log"] else "no mount line"))
    booted = {b["id"] for b in judge.boots}
    alone = [c["id"] for c in run.canon if c["id"] not in booted]
    L.append("judged by check alone: %d media, named on the next line of report.txt" % len(alone))
    L.append("  " + " ".join(alone))
    return "\n".join(L) + "\n"


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
    s = sub.add_parser("boots")
    s.add_argument("dir")
    s.add_argument("--picks", required=True)
    s.add_argument("--sample", type=int, default=8)
    s.add_argument("--seed", required=True)
    s = sub.add_parser("judge")
    s.add_argument("dir")
    s.add_argument("--path", required=True)
    s.add_argument("--caveat", action="append", default=[])
    s.add_argument("--verify")
    s.add_argument("--hogged", action="store_true")
    s.add_argument("--final-consistent", action="store_true")
    s.add_argument("--waited", type=int, default=0)
    s = sub.add_parser("settlecut")
    s.add_argument("dir")
    s.add_argument("--path", required=True)
    s.add_argument("--caveat", action="append", default=[])
    s = sub.add_parser("forge")
    for x in ("list", "log", "out"):
        s.add_argument(x)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if not a.verb:
        ap.error("say what to do: step, plan, checks, boots, judge, settlecut, forge or selftest")
    if a.verb in ("selftest", "forge"):
        # The gate judging itself lives beside it, and is handed this very
        # module: the arithmetic tested is the arithmetic the judge runs.
        import powercut_selftest
        return (powercut_selftest.SelfTest(sys.modules[__name__]) if a.verb == "selftest"
                else powercut_selftest.Forge(a))
    return {"step": CmdStep, "plan": CmdPlan, "checks": CmdChecks, "boots": CmdBoots,
            "judge": CmdJudge, "settlecut": CmdSettleCut}[a.verb](a)


if __name__ == "__main__":
    sys.exit(Main())
