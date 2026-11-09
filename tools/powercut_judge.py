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

from powercut import COMMIT_KINDS, HARM, VOLUME_READ, Outside, Ranges, RecordBytes, Run

# What volume-read says of a mount that does not stand on the newest seal borne
# out (census_claims.py _Seal): no harm on a cut before its sync's flush — a
# cut there rolls the cycle back — and a record the seal says landed that the
# medium does not hold on a state the flush stood behind.
SEAL_NOT_STOOD = ("SEAL_FELL_BACK", "SEAL_TAIL_TORN", "SEAL_ALONE_TORN", "SEAL_UNBORNE")
# And of a book (bookmount.py, stitch_ribbon.py): data a stitch the flush stood
# behind names that did not land, a slot fallen back, the ribbon's end or a
# closed stitch torn — none of which a state behind a flush may show. But a
# torn tail past the ribbon's end said as a note: a stitch cut while it was
# written, its own retry gone on past it (M-6, away.c AwayBookTakenUp) — the
# flush that closes the retry's epoch stands behind the retry, not the cut.
BOOK_NOT_STOOD = ("NOT_LANDED", "SLOT_FELL_BACK", "RIBBON_TORN", "STITCH_TORN")


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


def QuillVerified(log, name):
    """quilltest verify's PASS of NAME in a boot's LOG: (fid, seq) as matched."""
    return re.search(r"\[QUILL\] verify: PASS — %s fid=(\d+) seq=(\d+) bytes=16" % re.escape(name), log)


class Judge:
    def __init__(self, run, base, caveats, path, waited, by_kind=False, medium=""):
        self.run, self.base, self.caveats, self.path, self.waited = run, base, caveats, path, waited
        self.by_kind, self.census, self.boots, self.medium = by_kind, {}, BootLogs(run), medium

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
            f, led, unit = self.run.AckFlush(s), self.run.CommitWrites(s), self.Unit()
            if not led:
                self.Out("ack:" + s["step"], "fail", "%s was acknowledged at record entry %d and wrote "
                         "no %s in entries %d..%d, so no sync of its own stands behind it"
                         % (s["step"], s["ack"], unit, s["before"] + 1, s["ack"]))
            elif f is None:
                self.Out("ack:" + s["step"], "fail", "%s was acknowledged at record entry %d with no "
                         "flush behind its last %s write, entry %d — %s"
                         % (s["step"], s["ack"], unit, led[-1], "the answer was read with nothing after it"
                            if led[-1] >= s["ack"] else "none in entries %d..%d" % (led[-1] + 1, s["ack"])))
            else:
                self.Out("ack:" + s["step"], "behaves", "%s was acknowledged at record entry %d and "
                         "the flush at entry %d, the first after its last %s write (entry %d), "
                         "closing epoch %d, stands behind it"
                         % (s["step"], s["ack"], f, unit, led[-1], self.run.closes[f]))

    def Unit(self):
        """What commits a step on the volume of this run: format v1's Ledger, a
        book's stitch (powercut.py COMMIT_KINDS)."""
        return "stitch" if self.base.get("format") == "v2" else "ledger"

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
            ceiling = run.SeqCeiling(item, self.FinalSeq(item["name"]))
            hashes = {hashlib.sha256(RecordBytes(item["record"], q)).hexdigest(): q
                      for q in range(item["from"], ceiling + 1)}
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
                         "in %d of the %d cuts after it (a whole record of seq %d..%d allowed), e.g. %s (%s): %s"
                         % (item["name"], f, len(bad), len(after), item["from"], ceiling, bad[0][0],
                            run.Kept(bad[0][0]), bad[0][1]))
            elif after:
                seqs = sorted({hashes[x[5]] for cid in after for x in run.results[cid]["new_files"]
                               if x[1] == item["name"] and x[5] in hashes})
                whole = run.Exhaustive(run.closes[f])
                held = ("empty, as it was made" if not item["size"] else
                        "with its %d bytes, a whole record (seq %s, %d..%d allowed)"
                        % (item["size"], Ranges(seqs), item["from"], ceiling))
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
            if self.caveats.get("code:" + code):
                self.Out("code:" + code, "defect", what)
            else:
                self.Out("code:" + code, "fail", what + " — no caveat names it; its step is the "
                         "owner's to assign")
        for key in sorted(self.caveats):
            if key.startswith("code:") and key[5:] not in {c for c, _s in harm}:
                prefixes = self.by_kind and key[5:].startswith("prefix:")
                whole = run.Exhaustive(prefixes=prefixes)
                n, kind = Of(key[5:])
                # A code check says as a note — one the next mount repairs — is
                # no harm, and is said, so "no cut shows it" is never read as
                # "no cut has it".
                noted = self.census.get((key[5:], "note"))
                self.Out(key, "behaves" if whole else "unseen", "no %scut of %d shows %s as damage or a "
                         "leak%s%s" % (kind, n, key[5:], "; %d show it as a note, first %s: %s" % (
                             len(noted["cuts"]), noted["first"], noted["text"]) if noted else "",
                             "" if whole else "; " + run.Unbuilt(prefixes=prefixes)))
        if not harm:
            self.Out("codes", "behaves", "every one of %d cuts is consistent" % sum(judged.values()))

    def Behind(self, cid):
        """What a state a sync's flush stood behind shows that it must not: a
        damage or a leak code, and a mount that does not stand on the newest
        seal borne out — each in the census's words. Empty when nothing."""
        said = ["%s/%s" % x for x in self.run.Signature(cid)]
        said += ["%s: %s" % (code, text) for code, sev, text in
                 self.run.results.get(cid, {}).get("findings", []) if code in SEAL_NOT_STOOD + BOOK_NOT_STOOD
                 and (code, sev) != ("RIBBON_TORN", "note")]
        return " ".join(said)

    def Boundaries(self):
        """Every state a sync stood behind — an epoch its flush closed, holding the ledger HEAD's sync
        writes last, all applied — is consistent and stands on the seal that sync wrote, borne
        out; a WriteJob's flush of data promises nothing."""
        synced = {w["epoch"] for w in self.run.listing["writes"]
                  if any(t["kind"] in COMMIT_KINDS for t in w["touches"])}
        ends = [(self.run.Canon("e%d-all" % e["epoch"]), e["flush_seq"])
                for e in self.run.listing["epochs"] if e["flush_seq"] and e["epoch"] in synced]
        blind = ["%s (flush at entry %d)" % (c, f) for c, f in ends if not self.run.Judged(c)]
        bad = ["%s (flush at entry %d): %s" % (c, f, self.Behind(c))
               for c, f in ends if self.run.Judged(c) and self.Behind(c)]
        if blind:
            self.Out("boundaries", "harness", "volume-read could not judge %d of %d syncs' flushes: %s"
                     % (len(blind), len(ends), "; ".join(blind[:3])))
        elif ends:
            self.Out("boundaries", "fail" if bad else "behaves", "%d of %d syncs' flushes stand behind an "
                     "inconsistent volume, or one that does not stand on its newest seal: %s"
                     % (len(bad), len(ends), "; ".join(bad[:3])) if bad else
                     "the volume is consistent, and stands on its newest seal, at every one of %d "
                     "syncs' flushes" % len(ends))

    def Boots(self):
        if not self.boots:
            self.Out("boots", "fail", "no cut was booted")
            return
        dead, wrong = [], []
        for b in self.boots:
            r = self.run.results.get(b["id"], {})
            if "KERNEL PANIC" in b["log"] or "BoxOS Hub" not in b["log"]:
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

    def FinalSeq(self, name):
        """The seq quilltest verify read of NAME back whole on the final state:
        the newest record of NAME the medium took; 0 where none was read."""
        final = self.Final()
        ok = QuillVerified(final["log"], name) if final else None
        return int(ok.group(2)) if ok else 0

    def Verify(self, kind):
        """The scenario's own question, asked of the booted media."""
        what, _sep, arg = kind.partition(":")
        if what == "reissue":
            return self.Reissue(arg)
        if what == "grown":
            return self.Grown()
        if what in ("logger", "lines", "tipin", "spared", "engross", "chain"):
            import powercut_judge_book as book
            name, _sep, most = arg.partition(":")
            return {"logger": lambda: book.Logger(self, arg), "lines": lambda: book.Lines(self, name, int(most or 0)),
                    "tipin": lambda: book.Tipin(self),
                    "spared": lambda: book.Spared(self), "engross": lambda: book.Engross(self),
                    "chain": lambda: book.Chain(self, arg)}[what]()
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
        ok = QuillVerified(log, name)
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
        """No booted prefix cut hands its first create an id its medium already
        shows. The id due is volume-read's: past every id a row or a record of
        the cut names, and past the Ledger's count (mount/mount.c); a create
        given less is given an id that was issued before. Asked only where it
        means something — a cut whose medium names more than its Ledger counts
        — and at least one such cut must have been booted. On format v1 the
        create is anchor_test's own, where its file is not found. A book's cut
        that asks it bears the anchor's file on its ribbon ahead of the slot,
        so anchor_test finds the file there and creates nothing: the boot is
        asked a create of a file no medium of the run held (pc_nextfid,
        persist write), and its id is the answer."""
        step = self.run.Step(step_name)
        m = re.search(r"fid=(\d+)", step["line"]) if step else None
        fid = m.group(1) if m else None
        seen, judged, ahead, blind = [], 0, 0, []
        book = self.base.get("format") == "v2"
        counter = "the slot it stands on" if book else "its Ledger"
        asker = "persist write" if book else "anchor_test"
        made = (r"\[PERSIST\] WROTE fid=(\d+)" if book else r"\[ANCHOR\] (?:WROTE\+ANCHORED fid=(\d+)|PASS)")
        for b in self.boots:
            if not b["why"].startswith("prefix"):
                continue
            a = re.search(made, b["log"])
            if not a:
                continue
            judged += 1
            r = self.run.results.get(b["id"], {})
            due, counted = r.get("next_file_id"), r.get("ledger_next")
            if due is None or counted is None:
                blind.append(b["id"])
                continue
            if due > counted:
                ahead += 1
            if a.group(1) and int(a.group(1)) < due:
                seen.append("%s (%s): its medium names ids below %d, %s counts %d, and "
                            "%s was issued fid %s" % (b["id"], b["why"], due, counter, counted, asker,
                                                      a.group(1)))
        if blind:
            self.Out("reissue", "harness", "volume-read gave no next file id for %d booted cut(s), "
                     "e.g. %s" % (len(blind), blind[0]))
        elif not judged:
            self.Out("reissue", "fail", "no prefix cut of the anchor's epoch answered %s with a file it made"
                     % asker if book else "no prefix cut of the anchor's epoch answered anchor_test")
        elif seen:
            self.Out("reissue", "defect", "%d of %d prefix cuts issued a file id their medium already "
                     "shows: %s" % (len(seen), judged, "; ".join(seen[:2])))
        elif not ahead:
            self.Out("reissue", "fail", "none of %d prefix cuts has a medium naming more files than "
                     "%s counts, so no cut asked the question" % (judged, counter))
        else:
            self.Out("reissue", "behaves", "none of %d prefix cuts issued an id its medium shows — "
                     "%d of them name the anchor's fid %s ahead of %s"
                     % (judged, ahead, fid, "the slots they stand on" if book else "their Ledger"))

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
    if r.get("format") == "v2":
        return wrong      # a book keeps no bitmap and no pool: its census is its mount's (bookcensus.py)
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
    j = Judge(run, base, dict(x.split("=", 1) for x in a.caveat), a.path, a.waited, a.by_kind, a.medium)
    j.Acks()
    j.BaseFiles()
    j.Durable()
    j.Codes()
    j.Boundaries()
    if base.get("format") == "v2":
        import powercut_judge_book
        powercut_judge_book.Whole(j)
        powercut_judge_book.Order(j)
    j.Boots()
    for kind in a.verify:
        j.Verify(kind)
    if a.final_consistent:
        j.FinalState()
    if a.hogged:
        j.Hogged()
    import powercut_lie
    powercut_lie.Judge(j)
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
    """A kernel built SETTLECUT=ledger stopped the cycle that settles anchor_test's change before the
    ledger (DIR/ledger.check.json); one built SETTLECUT=done stopped it after the flush
    (DIR/done.check.json). The recorded plain run of the same anchor_test is cut at two points of its
    own cycle — through the file table, and everything before the ledger — and each is judged by
    volume-read: the SETTLECUT medium and PowerCut's cut before the ledger must name the same codes. The SETTLECUT=ledger medium is judged code by code, as
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
        judge.Out("cross", "fail", "the recorded anchor_test has no flush behind it, so its cycle has "
                  "no points to cut at")
    else:
        # The anchor's cycle is every volume write from its first to the flush its ack stands on,
        # across the epochs its FLUSH#1 parts. A cut at a point is the point's own epoch: every
        # epoch before it whole, and of it the writes before the point — and those outside the
        # volume, which every cut applies.
        cycle = [w for w in run.listing["writes"] if step["before"] < w["seq"] < flush and not Outside(w)]
        table_at = next((w for w in cycle if w["touches"] and w["touches"][0]["kind"] == "table"), None)
        ledger_at = next((w for w in cycle if w["touches"] and w["touches"][0]["kind"] == "ledger"), None)
        if table_at is None or ledger_at is None:
            judge.Out("cross", "fail", "the anchor's cycle, entries %d to %d, wrote no %s"
                      % (step["before"] + 1, flush, "file table" if table_at is None else "ledger"))
        else:
            said = []
            for name, at, last in (("through the file table", table_at, table_at["seq"]),
                                   ("before the ledger", ledger_at, ledger_at["seq"] - 1)):
                keep = [w["index"] for w in run.listing["writes"]
                        if w["epoch"] == at["epoch"] and (w["seq"] <= last or Outside(w))]
                r = subprocess.run([sys.executable, VOLUME_READ, os.path.join(a.dir, "base.img"), "cut",
                                    os.path.join(a.dir, "run.log"), "--epoch", str(at["epoch"]), "--keep",
                                    Ranges(keep) or "none", "--check", "--json"],
                                   stdout=subprocess.PIPE, universal_newlines=True)
                said.append((name, run.LastWriteBefore(last + 1), Harm(json.loads(r.stdout))))
            judge.Out("cross", "behaves" if said[1][2] == Harm(ledger) else "fail",
                      "SETTLECUT=ledger names %s; PowerCut's cut of the same cycle %s" % (
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
             "(every sector up to %d)%s" % (p["seed"], p["all_max"], p["sample"], p["tear_all_max"],
                                            ", and a book's every stitch at each of its sectors"
                                            if judge.base.get("format") == "v2" else ""))
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
    lies, _results = __import__("powercut_lie").Plan(run.dir)
    if lies is not None:
        n = lies["counts"]
        L.append("lies, counted apart (powercut_lie.py): %d Blot and %d Smudge media, %d distinct, %d collapsed "
                 "into an earlier one; their verdicts are keyed blot: and smudge:"
                 % (n["blot"], n["smudge"], n["distinct"], n["collapsed"]))
    booted = {b["id"] for b in judge.boots}
    alone = [c["id"] for c in run.canon if c["id"] not in booted]
    L.append("judged by check alone: %d media, named on the next line of report.txt" % len(alone))
    L.append("  " + " ".join(alone))
    return "\n".join(L) + "\n"
