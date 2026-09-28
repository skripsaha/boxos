"""powercut_judge — PowerCut's judge: every oracle held against one recorded run
of tools/powercut.sh, one verdict line each, the settlecut cross-check, and the
report of what was enumerated, found and booted.

Not a program of its own: tools/powercut.py is the way in (`judge`,
`settlecut`), and the run, the records a surviving file must hold and the
severities that make volume-read say "inconsistent" are that module's, taken
from it by name — powercut.py registers itself under its own name before it
imports this, so the import below finds the module that is running and not a
second copy of it.

Every verdict is one tab-separated line on stdout, which the shell says in
stand.sh's words:
    KEY <TAB> caveat|plain|harness|note <TAB> NAME <TAB> defect|behaves|fail|unseen <TAB> WHAT
A note is no verdict: a caveated defect that a sampled census did not meet.
"""

import hashlib
import json
import os
import re
import subprocess
import sys

from powercut import HARM, VOLUME_READ, Outside, Ranges, RecordBytes, Run


# ---------------------------------------------------------------------------
# judge — every oracle, one verdict line each
# ---------------------------------------------------------------------------


def BootLogs(run):
    """Every booted cut, why it was chosen, and what its machine said."""
    path, out = os.path.join(run.dir, "boots.txt"), []
    for line in (open(path).read().splitlines() if os.path.exists(path) else []):
        cid, _k, _keep, _tear, why = line.split("\t")
        lp = os.path.join(run.dir, "boots", cid + ".serial.log")
        log = open(lp, errors="replace").read().replace("\r", "") if os.path.exists(lp) else ""
        out.append({"id": cid, "why": why, "log": log})
    return out


class Judge:
    def __init__(self, run, base, caveats, path, waited, by_kind=False):
        self.run, self.base, self.caveats, self.path, self.waited = run, base, caveats, path, waited
        self.by_kind, self.census, self.boots = by_kind, {}, BootLogs(run)

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
                held = ("empty, as it was made" if not item["size"] else
                        "with its %d bytes, a whole record (seq %s, %d..%d allowed)"
                        % (item["size"], Ranges(seqs), item["from"], run.SeqCeiling(item)))
                self.Out(key, "behaves" if whole or not self.caveats.get(key) else "unseen",
                         "%s is visible %s, in every one of the %d cuts after the flush at entry %d%s"
                         % (item["name"], held, len(after), f,
                            "" if whole else "; " + run.Unbuilt(run.closes[f])))
            else:
                self.Out(key, "fail", "no cut stands after the flush at entry %d, so the durability "
                         "of %s was never judged" % (f, item["name"]))

    def Findings(self, cid, r):
        """A cut's findings as the census keeps them: each code as it stands,
        or — kept by kind — the cut's kind before it, and CHAIN_REFUSED once
        for every chain the kernel would refuse, by name, so a refusal of the
        table and one of the registry are two verdicts and not one."""
        if not self.by_kind:
            return r["findings"]
        kind = self.run.kind[cid]
        return ([("%s:%s" % (kind, code), sev, text) for code, sev, text in r["findings"]
                 if code != "CHAIN_REFUSED"] +
                [("%s:CHAIN_REFUSED:%s" % (kind, name), "damage", "the %s: %s" % (name, why))
                 for name, why in sorted(r["refused"].items())])

    def Codes(self):
        run, broken, judged = self.run, [], {}
        for c in run.canon:
            r = run.results.get(c["id"])
            if r is None or "error" in r:
                broken.append((c["id"], (r or {}).get("error", "no answer")))
                continue
            kind = run.kind[c["id"]] if self.by_kind else ""
            judged[kind] = judged.get(kind, 0) + 1
            for code, sev, text in self.Findings(c["id"], r):
                e = self.census.setdefault((code, sev), {"cuts": set(), "first": c["id"], "text": text})
                e["cuts"].add(c["id"])
        if broken:
            self.Out("checker", "harness", "volume-read could not judge %d of %d cuts, e.g. %s: %s"
                     % (len(broken), len(run.canon), broken[0][0], broken[0][1]))

        def Of(code):
            """How many cuts a code was looked for in, and the word for their kind."""
            kind = code.split(":")[0] if self.by_kind else ""
            return judged.get(kind, 0), kind + " " if kind else ""

        harm = sorted(k for k in self.census if k[1] in HARM)
        for code, sev in harm:
            e = self.census[(code, sev)]
            what = "%s (%s) in %d of %d %scuts, first %s (%s): %s" % (
                (code, sev, len(e["cuts"])) + Of(code) + (e["first"], run.Kept(e["first"]), e["text"]))
            held = True
            if self.by_kind and code.startswith("prefix:CHAIN_REFUSED:"):
                held, proof = self.Refusal(e["cuts"], code.split(":")[2])
                what += "; " + proof
            if self.caveats.get("code:" + code):
                self.Out("code:" + code, "defect" if held else "fail", what)
            else:
                self.Out("code:" + code, "fail", what + " — no caveat names it; its step is the "
                         "owner's to assign")
        for key in sorted(self.caveats):
            if key.startswith("code:") and key[5:] not in {c for c, _s in harm}:
                prefixes = self.by_kind and key[5:].startswith("prefix:")
                whole = run.Exhaustive(prefixes=prefixes)
                n, kind = Of(key[5:])
                self.Out(key, "behaves" if whole else "unseen", "no %scut of %d shows %s%s"
                         % (kind, n, key[5:], "" if whole else "; " + run.Unbuilt(prefixes=prefixes)))
        if not harm:
            self.Out("codes", "behaves", "every one of %d cuts is consistent" % sum(judged.values()))

    def Refusal(self, cuts, chain):
        """The kernel's own word on a refusal check predicts over `chain`: every
        booted cut among `cuts` whose predicted refusal is that chain's, held
        to the sentence its mount said — and, for the file table, to the block
        its loader names. (held, what was seen)."""
        agree, differ = [], []
        for b in self.boots:
            r = self.run.results.get(b["id"], {})
            why = r.get("refused", {}).get(chain)
            if b["id"] not in cuts or not why or not (r.get("refusal") or "").endswith(" — " + why):
                continue
            due = r["refusal"][:-len(" — " + why)]
            said = re.search(r"\[TagFS\] this volume is NOT mounted: (.*) \(error \d+\)", b["log"])
            named = re.search(r"\[FileTable\] (block \d+ holds 0x[0-9a-f]+) where", b["log"])
            if said and said.group(1) == due and (not named or why.startswith(named.group(1) + " ")):
                agree.append("%s, whose mount said '%s'%s" % (
                    b["id"], said.group(1), " and '[FileTable] %s'" % named.group(1) if named else ""))
            else:
                differ.append("%s, where check predicted '%s' (%s) and the kernel said %s" % (
                    b["id"], due, why, "'%s'" % said.group(1) if said else "no refusal"))
        if differ:
            return False, "booted " + "; ".join(differ[:2])
        if not agree:
            return False, "none of them was booted, so the kernel's own refusal was never seen"
        return True, "booted %d, the kernel refused as check predicted: %s" % (len(agree),
                                                                              "; ".join(agree[:2]))

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
        if what == "grown":
            return self.Grown()
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

    def Grown(self):
        """chaingrow's own question: did the recorded run give the tag registry
        and the file table a block each? A run that grew neither has no cut
        that tests how a chain grows, however green its census."""
        base = {name: len(self.base["chains"][name]) for name in ("registry", "table")}
        have = self.run.results.get(self.run.final or "", {}).get("chains")
        if not have:
            return self.Out("grown", "fail", "the final state %s was not judged, so nothing says "
                            "whether the chains grew" % self.run.final)
        grew = all(have[name] > base[name] for name in base)
        self.Out("grown", "behaves" if grew else "fail", "the recorded run took the tag registry from "
                 "%d to %d block(s) and the file table from %d to %d (the final state %s)"
                 % (base["registry"], have["registry"], base["table"], have["table"], self.run.final))

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
    j = Judge(run, base, dict(x.split("=", 1) for x in a.caveat), a.path, a.waited, a.by_kind)
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
