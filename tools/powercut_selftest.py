"""powercut_selftest — the PowerCut gate judging itself: `tools/powercut.py
selftest`, the planner's arithmetic and the judge's reading of a record on
listings made up here, and `forge`, a real record with one byte flipped for
the record's self-check to refuse.

Not a program of its own: powercut.py hands it the module it was loaded as,
so the arithmetic tested is the arithmetic the judge runs, not a copy of it.
"""

import argparse
import contextlib
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile


def Synthetic(shape):
    """A listing from [[write...] per epoch]; a write is its sector count,
    (sectors, lba) to put it somewhere on purpose — lba 0 is outside the
    volume, None the next place — or (sectors, lba, kind[, what]) to say what
    it touched. Every epoch but the last is closed by a flush."""
    writes, epochs, seq, lba = [], [], 0, 1000
    for k, ws in enumerate(shape):
        for i, w in enumerate(ws):
            n, at, kind, what = (tuple(w) + (None, None, None))[:4] if isinstance(w, tuple) else (w, None, None, None)
            seq += 1
            writes.append({"epoch": k, "index": i, "seq": seq, "lba": lba if at is None else at,
                           "sectors": n, "discard": False,
                           "touches": [{"kind": kind or ("outside" if at == 0 else "file"),
                                        "first_lba": lba if at is None else at, "sectors": n,
                                        "what": what or "w"}]})
            lba += n
        seq += 1
        epochs.append({"epoch": k, "writes": len(ws), "flush_seq": seq if k < len(shape) - 1 else None,
                       "maybe_qemu_exit_flush": False})
    return {"writes": writes, "epochs": epochs, "entries": seq}


def Forge(a):
    """The record with one byte flipped in the data of its last write — the one write no later write
    covers, so the disk QEMU left must show the byte. The one piece of the record's layout this file
    knows, for this alone: after the superblock's sector, an entry is a header sector and then its data
    sectors, a flush a header only (blklogwrites.c 486-496)."""
    with open(a.list) as fh:
        listing = json.load(fh)
    ss = listing["sector_bytes"]
    writes = [w for w in listing["writes"] if not w["discard"]]
    if not writes:
        return 1
    last = max(writes, key=lambda w: w["seq"])
    flushes = sum(1 for e in listing["epochs"] if e["flush_seq"] and e["flush_seq"] < last["seq"])
    before = sum(1 + (0 if w["discard"] else w["sectors"] * 512 // ss)
                 for w in listing["writes"] if w["seq"] < last["seq"])
    at = (1 + flushes + before + 1) * ss + ss // 2
    with open(a.log, "rb") as fh:
        data = bytearray(fh.read())
    data[at] ^= 0xFF
    with open(a.out, "wb") as fh:
        fh.write(data)
    print("byte %d of the record, inside entry %d's data (LBA %d)" % (at, last["seq"], last["lba"]))
    return 0


def SelfTest(pc):
    """pc is powercut.py's own module."""
    fails = []

    def Want(held, what):
        print("  %s %s" % ("ok  " if held else "FAIL", what))
        if not held:
            fails.append(what)

    def Plan(shape, seed=7):
        return pc.Planner(Synthetic(shape), seed, 10, 64, 16).Run()

    def Recorded(shape, before, ack):
        """A Run over a record of `shape`, planned, with one durable step."""
        with tempfile.TemporaryDirectory() as d:
            listing = Synthetic(shape)
            for name, body in (("list.json", listing), ("plan.json", pc.Planner(listing, 7, 10, 64, 16).Run())):
                with open(os.path.join(d, name), "w") as fh:
                    json.dump(body, fh)
            with open(os.path.join(d, "steps.jsonl"), "w") as fh:
                fh.write(json.dumps({"step": "s", "cmd": "c", "before": before, "ack": ack, "quiet": ack,
                                     "line": "", "durable": None}) + "\n")
            return pc.Run(d)

    PlanTests(pc, Want, Plan)
    RecordTests(Want, Recorded)
    CrowdTests(pc, Want)
    try:
        CompactTests(pc, Want)
        BookTests(pc, Want, Plan, Synthetic)
        LieTests(pc, Want, Synthetic)
        LieRowTests(pc, Want, Synthetic)
        BookRowTests(pc, Want, Synthetic)
    finally:
        while RUNS:
            shutil.rmtree(RUNS.pop(), ignore_errors=True)
    print("selftest: %d failed" % len(fails))
    return 1 if fails else 0


def PlanTests(pc, Want, Plan):
    """The planner: subsets, prefixes, tears, seeds, and the records it knows."""
    Want(pc.Mix(0x42504552, 1) == 0xC3BEFF2C, "persist's checksum of seq 1 is the runtime's 0xc3beff2c")
    e = Plan([[1, 1, 1]])["epochs"][0]
    Want((e["subsets"], e["enumerated"], e["not_enumerated"], e["distinct"]) == (8, 8, 0, 8),
         "three one-sector writes: 8 subsets, all built, 8 distinct media")
    e = Plan([[1, (1, 1000)]])["epochs"][0]
    Want(e["distinct"] == 3 and e["collapsed"] == 1,
         "a write the next one covers collapses: 3 distinct media of 4 (%d)" % e["distinct"])
    cuts = {c["id"]: c for c in Plan([[1, 1], [1]])["cuts"]}
    Want(cuts["e1-none"]["same"] == "e0-all", "a flush boundary is one medium: e1-none is e0-all")
    Want(Plan([[1], []])["cuts"][-1]["id"] == "e1-all", "an epoch with no writes has its one cut, all")
    e = Plan([[8, 8]])["epochs"][0]
    Want((e["tears"], e["tears_not_built"]) == (7, 0), "an 8-sector last write is torn 7 ways, none left out")
    e = Plan([[40]])["epochs"][0]
    Want((e["tears"], e["tears_not_built"], e["tear_sectors"]) == (6, 33, 40),
         "a 40-sector last write: 1, 39 and every 8th — 6 tears, and the 33 other prefixes counted as "
         "not built (%d)" % e["tears_not_built"])
    torn = next(c for c in Plan([[1, 8]])["cuts"] if c["id"] == "e0-t003")
    Want((torn["keep"], torn["tear"]) == ("0", [1, 7]), "tear 3 keeps the first write and lands "
         "sectors 0-2 of the last (%s %s)" % (torn["keep"], torn["tear"]))
    a, b = Plan([[1] * 14], 0xABC), Plan([[1] * 14], 0xABC)
    e = a["epochs"][0]
    Want(a == b, "the same seed gives the same plan")
    Want(e["enumerated"] == 15 + 13 + 64 and e["not_enumerated"] == (1 << 14) - 92,
         "14 writes: 15 prefixes, 13 more omissions (leaving out the last is a prefix), 64 drawn "
         "— exactly %d not built (%d)" % ((1 << 14) - 92, e["not_enumerated"]))
    Want(Plan([[1] * 14], 0xABD) != a, "another seed draws another sample")
    p = Plan([[(1, 0), 1, 1]])
    Want(p["epochs"][0]["outside"] == 1 and all(0 in pc.RangeSet(c["keep"]) for c in p["cuts"])
         and p["epochs"][0]["subsets"] == 4, "a write outside the volume is in every cut and is not "
         "enumerated")
    th = {c["id"]: c["through"] for c in Plan([[1, 1, 1]])["cuts"]}
    Want((th["e0-all"], th["e0-s3"], th["e0-s5"], th["e0-none"])
         == (3, 2, 1, 0), "`through` is the longest run applied from the start: 3, 2, 1, 0")
    Want(pc.Ranges([1, 2, 3, 5, 7, 8]) == "1-3,5,7-8" and pc.RangeSet("1-3,5,7-8") == {1, 2, 3, 5, 7, 8},
         "entry sets are written and read back as ranges")
    rec = pc.RecordBytes("anchor", 1)
    Want(len(rec) == 128 and rec[:12] == struct.pack("<III", 0x414E4348, 1, 0xC389EC80),
         "anchor_test's record: 128 bytes, magic, seq 1, sum 0xc389ec80, zeros")


def RecordTests(Want, Recorded):
    """A run of a record: the flush behind a step, Exhaustive, prefixes and subsets."""
    ledger = (1, None, "ledger")
    run = Recorded([[ledger], [1], [1]], 0, 4)
    Want(run.AckFlush(run.steps[0]) == 2, "the flush behind a step is the first after its ledger, not a "
         "writer's later one (%s)" % run.AckFlush(run.steps[0]))
    run = Recorded([[1], [ledger]], 0, 3)
    Want(run.AckFlush(run.steps[0]) is None, "a data writer's flush before the step's ledger stands behind "
         "nothing of the step's (%s)" % run.AckFlush(run.steps[0]))
    run = Recorded([[1], [1]], 0, 3)
    Want(not run.CommitWrites(run.steps[0]) and run.AckFlush(run.steps[0]) is None,
         "a step that wrote no ledger has no flush of its own")
    sewn = (1, None, "stitch")
    run = Recorded([[1, sewn], [1]], 0, 4)
    Want(run.AckFlush(run.steps[0]) == 3, "on a book the flush behind a step is the first after its "
         "stitch, the unit a cycle of format v2 commits by (%s)" % run.AckFlush(run.steps[0]))
    Want(Recorded([[1, 1], [1]], 0, 3).Exhaustive() and not Recorded([[1] * 14], 0, 0).Exhaustive()
         and not Recorded([[40]], 0, 0).Exhaustive(),
         "a record is enumerated whole only with no subset and no torn prefix left out")
    Want(Recorded([[1] * 14], 0, 0).Exhaustive(prefixes=True) and
         not Recorded([[40]], 0, 0).Exhaustive(prefixes=True),
         "its prefixes are built whole however many subsets are left out, and not with a torn one left")

    kind = Recorded([[1, 1, 1]], 0, 3).kind
    Want([kind[c] for c in ("e0-none", "e0-s1", "e0-s3", "e0-all")] == ["prefix"] * 4 and
         [kind[c] for c in ("e0-s2", "e0-s4", "e0-s5", "e0-s6")] == ["subset"] * 4,
         "of three writes the cuts that keep the first 0, 1, 2 or 3 are prefixes and the four "
         "others subsets")
    Want(Recorded([[1, 8]], 0, 3).kind["e0-t003"] == "prefix", "a torn last write is a prefix")
    kind = Recorded([[(1, 0), 1, 1]], 0, 3).kind
    Want((kind["e0-s1"], kind["e0-s2"]) == ("prefix", "subset"),
         "a write outside the volume, kept by every cut, makes no prefix a subset (%s %s)"
         % (kind["e0-s1"], kind["e0-s2"]))
    run = Recorded([[1, (1, 1000)]], 0, 3)
    Want(run.Canon("e0-all") == "e0-s2" and run.kind["e0-s2"] == "prefix",
         "a medium a subset and a prefix both build is a prefix (%s, %s)"
         % (run.Canon("e0-all"), run.kind.get("e0-s2")))


def CrowdTests(pc, Want):
    """crowd: the names a recorded run makes at the registry's and the table's edges."""
    def Crowd(nfid, used, names=0):
        check = {"ledger": {"standing_on": 0, "copies": [{"copy": 0, "next_file_id": nfid}]},
                 "chains": {"registry": [0], "table": [1], "registry_tail_used": used,
                            "registry_block_data": 4080, "registry_record_header": 6,
                            "table_rows_per_block": 510}}
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "check.json")
            with open(path, "w") as fh:
                json.dump(check, fh)
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                rc = pc.CmdCrowd(argparse.Namespace(check=path, name_bytes=15, room=1, names=names))
        said = out.getvalue().split("\t")
        return rc, said[:2] if said[0] == "edges" else said[:3]

    rc, said = Crowd(71, 1202)
    Want(rc == 0 and said == ["more", "330", "108"],
         "fid 71 and 1202 registry bytes: 330 new names and 108 made again — 438 ids, and 137 - 1 + "
         "194 records leave room for one (%s %s)" % (rc, said))
    rc, said = Crowd(509, 4053)
    Want(rc == 0 and said == ["edges", "2"], "at both edges the recorded run makes 2 files (%s %s)"
         % (rc, said))
    Want(Crowd(508, 4053)[1][0] == "more" and Crowd(509, 4032)[1][0] == "more",
         "one row or one record off either edge is not at both edges")
    rc, said = Crowd(509, 12, names=326)
    Want(rc == 0 and said == ["more", "192", "318"],
         "a round the machine's own tags carried past the registry's edge — fid 509, 12 bytes in a "
         "fresh block, 326 names made — goes on to the table's next block: 192 new names and 318 "
         "made again (%s %s)" % (rc, said))
    rc, said = Crowd(508, 4053, names=10)
    Want(rc == 0 and said == ["more", "0", "1"],
         "one row short with the registry at its edge: one name made again (%s %s)" % (rc, said))


def CompactTests(pc, Want):
    # A book's `check --json` (bookcensus.py Json): its kernel has no fsck, and
    # a locked file is its fid, state and why alone — the shape volume-read
    # gives landing B4's fid 87.
    live = {"fid": 1, "state": "live", "name": "a", "size": 3, "sha256": "ab" * 32, "visible": True}
    book = {"format": "v2", "verdict": "clean", "findings": [],
            "kernel": {"mounts": True, "refusal": None, "data_blocks": 9, "free": 5, "files": 1,
                       "slot": 0, "locked": [87]},
            "book": {"slot": 0, "slots": [{"index": 0, "counts": {"next_fid": 120}}],
                     "counts": {"next_fid": 120}, "tally": {"seq": 4}, "under_glass": [],
                     "ribbon": {"stitches": [{"seq": 3, "vlba": 70, "vlbas": [70], "of": 1}], "torn": [],
                                "lied": False}},
            "files": [live, {"fid": 87, "state": "locked", "why": "w", "visible": False}]}
    try:
        got = pc.Compact("c0", 0, json.dumps(book), "", {pc.FileKey(live): live}, {(1, "a")})
    except KeyError as e:
        got = {"error": "KeyError %s" % e}
    Want(got.get("new_files") == [[87, None, "locked", False, None, None]] and not got["base_changed"]
         and "fsck" not in got and (got["next_file_id"], got["ledger_next"]) == (120, 120),
         "a book's check, a locked file in it, compacts: no fsck, the locked fid new and nameless, "
         "the next fid against its slot's (%s)" % {x: got.get(x) for x in ("error", "new_files", "fsck")})


# The runs Judged lays down: a Judge reads its run's files as it judges, so
# they stand until the self-test is done, and go with it.
RUNS = []


def Said(fn, *args):
    """What fn printed — the judge's verdict lines — as {key: (seen, what)}."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        fn(*args)
    got = {}
    for line in out.getvalue().splitlines():
        cols = line.split("\t")
        if len(cols) == 5:
            got[cols[0]] = (cols[3], cols[4])
    return got


def Judged(pc, listing, results, lies=None, lie_results=None):
    """A Judge over a run of `listing`, planned, its cuts answered by
    results(cid) — None for a cut volume-read could not judge — and its lie
    media by lie_results(mid) when `lies` is a lie plan. The judge is
    powercut_judge's own, on this very powercut module."""
    sys.modules.setdefault("powercut", pc)
    import powercut_judge
    d = tempfile.mkdtemp()
    RUNS.append(d)
    plan = pc.Planner(listing, 7, 10, 64, 16).Run()
    for name, body in (("list.json", listing), ("plan.json", plan)):
        with open(os.path.join(d, name), "w") as fh:
            json.dump(body, fh)
    with open(os.path.join(d, "steps.jsonl"), "w") as fh:
        fh.write(json.dumps({"step": "s", "cmd": "c", "before": 0, "ack": 0, "quiet": 0, "line": "",
                             "durable": None}) + "\n")
    with open(os.path.join(d, "cuts.jsonl"), "w") as fh:
        for c in plan["cuts"]:
            if c["same"] is None and results(c["id"]) is not None:
                fh.write(json.dumps(dict(results(c["id"]), id=c["id"])) + "\n")
    if lies is not None:
        with open(os.path.join(d, "lies.json"), "w") as fh:
            json.dump(lies, fh)
        with open(os.path.join(d, "lies.jsonl"), "w") as fh:
            for m in lies["media"]:
                if m["same"] is None:
                    fh.write(json.dumps(dict(lie_results(m), id=m["id"])) + "\n")
    base = {"format": "v2", "files": [{"fid": 1, "name": "a", "size": 3, "sha256": "aa", "visible": True,
                                       "state": "live"}]}
    return powercut_judge.Judge(pc.Run(d), base, {}, "p", 0)


def Book(files, findings=(), last=3, lied=False, sewn=()):
    """One cut of a book as Compact keeps it: every file as [fid, name, size, sha256]."""
    return {"rc": 0, "verdict": "inconsistent" if any(f[1] in ("damage", "leak") for f in findings)
            else "consistent", "mounts": True, "refusal": None, "format": "v2",
            "says": {"data_blocks": 9, "free": 5, "files": len(files)}, "next_file_id": 9, "ledger_next": 9,
            "refused": {}, "chains": {}, "findings": [list(f) for f in findings], "base_changed": [],
            "new_files": [], "files": [list(f) for f in files],
            "book": {"slot": 0, "tally": 1, "stitches": len(sewn), "torn": 0, "lied": lied, "glass": [],
                     "last": last, "ribbon": {}, "sewn": [list(x) for x in sewn], "start": 0}}


def BookTests(pc, Want, Plan, Synthetic):
    """A book's matrix: a stitch torn at every sector it has (C2), the records
    of a logger, and the oracles a book's run is held to beside format v1's."""
    e = Plan([[(40, None, "stitch")]])["epochs"][0]
    Want((e["tears"], e["tears_not_built"]) == (39, 0),
         "a stitch of 40 sectors, the epoch's last write, is torn after every one of them whatever "
         "--tear-all-max says — 39 tears, none left out (%d, %d)" % (e["tears"], e["tears_not_built"]))
    cuts = [c for c in Plan([[(4, None, "stitch"), 1]])["cuts"] if c["tear"] and c["tear"][0] == 0]
    Want(sorted(c["tear"][1] for c in cuts) == [1, 3, 7] and all(c["keep"] == "" for c in cuts),
         "a stitch before its epoch's last write is torn too, at each of its sectors, after the writes "
         "before it (%s)" % [(c["keep"], c["tear"]) for c in cuts])
    run = Judged(pc, Synthetic([[(4, None, "stitch"), 1]]), lambda cid: Book([]))
    torn = [c["id"] for c in run.run.plan["cuts"] if c["tear"] and c["tear"][0] == 0]
    Want(torn and all(run.run.kind[c] == "prefix" for c in torn),
         "and every such cut is a prefix: what a device that writes in order leaves (%s)"
         % {c: run.run.kind.get(c) for c in torn})
    e = Plan([[8, 8]])["epochs"][0]
    Want((e["tears"], e["tears_not_built"]) == (7, 0), "a write of data is torn as before: 7 ways of 8 sectors")
    try:
        lines = pc.RecordBytes("log", 3)
    except (KeyError, AttributeError) as x:
        lines = repr(x).encode()
    Want(lines == b"".join(b"%08d" % n + b"." * 39 + b"\n" for n in range(3)),
         "a logger's record of 3 is its first three lines, each 48 bytes as quilltest log writes them "
         "(quilltest_book.c QuillLogLine): %r" % lines[:60])

    try:
        import powercut_judge_book as pjb
    except ImportError as x:
        Want(False, "the book's oracles are there to ask: %s" % x)
        return
    shape = Synthetic([[1, (1, None, "stitch")], [1, (1, None, "stitch")], [1]])
    held = {"e0-all": [[1, "a", 3, "aa"], [5, "f", 4, "f1"]], "e1-all": [[1, "a", 3, "aa"], [5, "f", 8, "f2"]],
            "e2-all": [[1, "a", 3, "aa"], [5, "f", 8, "f2"]]}

    def Mixed(bad):
        return lambda cid: Book(held.get(cid, bad if cid == "e1-s1" else [[1, "a", 3, "aa"], [5, "f", 4, "f1"]]))

    got = Said(pjb.Whole, Judged(pc, shape, Mixed([[1, "a", 3, "aa"], [5, "f", 4, "f1"]])))
    Want(got.get("files:whole", ("",))[0] == "behaves",
         "every cut shows each file as one state a flush stood behind left it: whole (%s)" % (got,))
    got = Said(pjb.Whole, Judged(pc, shape, Mixed([[1, "a", 3, "aa"], [5, "f", 6, "mix"]])))
    Want(got.get("files:whole", ("",))[0] == "fail" and "e1-s1" in got["files:whole"][1],
         "a cut showing a file as no flush ever left it — its old bytes and new mixed — fails, and names "
         "the cut (%s)" % (got,))
    got = Said(pjb.Whole, Judged(pc, shape, Mixed([[1, "a", 3, "zz"], [5, "f", 4, "f1"]])))
    Want(got.get("files:whole", ("",))[0] == "fail",
         "so does a base file read as other bytes than the base's (%s)" % (got,))
    log = pc.RecordBytes("log", 4) if isinstance(lines, bytes) and len(lines) == 144 else b""
    sha = {n: hashlib.sha256(log[:n * 48]).hexdigest() for n in range(5)}
    ok = Judged(pc, shape, lambda cid: Book([[7, "pc-l.log", 96 if cid != "e2-all" else 192,
                                              sha[2] if cid != "e2-all" else sha[4]]]))
    got = Said(pjb.Lines, ok, "pc-l.log", 4)
    Want(got.get("lines:pc-l.log", ("",))[0] == "behaves",
         "a logger's file is whole lines in every cut, as many as its last flush left or more (%s)" % (got,))
    bad = Judged(pc, shape, lambda cid: Book([[7, "pc-l.log", 100, sha[2]]]))
    got = Said(pjb.Lines, bad, "pc-l.log", 4)
    Want(got.get("lines:pc-l.log", ("",))[0] == "fail",
         "and a logger's file cut inside a line fails (%s)" % (got,))
    blocks = Synthetic([[(8, None, "file", "fid 7 data, named by stitch seq %d" % n)
                         for n in range(6)], [(1, None, "slot", "Spine slot of seq 4")]])
    got = Said(pjb.SlipBlocks, Judged(pc, blocks, lambda cid: Book([])), 7, 40)
    Want(got.get("slip:blocks", ("",))[0] == "fail",
         "a logger of 40 lines (1920 bytes, under a block) whose file took 6 blocks of data over one Tally "
         "wrote its lines as blocks, not as Slips (%s)" % (got,))
    got = Said(pjb.SlipBlocks, Judged(pc, Synthetic([[(8, None, "file", "fid 7 data, named by stitch seq 2")],
                                                     [(1, None, "slot", "Spine slot of seq 4")]]),
                                      lambda cid: Book([])), 7, 40)
    Want(got.get("slip:blocks", ("",))[0] == "behaves",
         "one block of it, its tail engrossed for the one Tally, is what a Slip writer owes (%s)" % (got,))


def LieTests(pc, Want, Synthetic):
    """The medium that lied (§16.5 п.21): Blot — writes a kept flush stood
    behind, lost — and Smudge — the sectors of a 16 KiB page around a write,
    spoilt — planned apart from the power cuts and counted apart."""
    try:
        import powercut_lie as lie
        from volumeread import blklog, medium
    except ImportError as x:
        Want(False, "the lie's planner and volume-read's Blot and Smudge are there to ask: %s" % x)
        return
    shape = Synthetic([[(1, None, "file", "fid 3 data, named by stitch seq 1"), (1, None, "stitch", "stitch seq 1")],
                       [(1, None, "file", "fid 3 data, named by stitch seq 2"), (1, None, "stitch", "stitch seq 2")],
                       [(2, 1003, "stitch", "stitch seq 3, 2 sector(s)")]])
    plan = lie.LiePlanner(shape).Run()
    blots = {m["id"]: m for m in plan["media"] if m["kind"] == "blot"}
    smudges = {m["id"]: m for m in plan["media"] if m["kind"] == "smudge"}
    Want(blots.get("b2-d2-all", {}).get("blot") == "1-2,4-5" and blots["b2-d2-all"]["keep"] == "0",
         "Blot of depth 2 at epoch 2: entries 1-2 and 4-5 of epochs 0 and 1 lost, epoch 2 whole (%s)"
         % blots.get("b2-d2-all"))
    Want(blots.get("b1-d1-data", {}).get("blot") == "1" and blots.get("b1-d1-none", {}).get("keep") == "",
         "Blot of depth 1 loses epoch 0's data alone, or all of it before nothing of epoch 1 (%s)"
         % sorted(blots))
    m = smudges.get("m2-0", {})
    Want(m.get("smudge") == [[992, 11], [1005, 19]] and m.get("keep") == "",
         "Smudge of the 2-sector stitch at LBA 1003: the rest of its 16 KiB page, 992..1023 but its own, "
         "spoilt, the stitch itself never landed (%s)" % m)
    Want(plan["counts"]["blot"] == len(blots) and plan["counts"]["smudge"] == len(smudges)
         and all(c["id"][0] == "e" for c in pc.Planner(shape, 7, 10, 64, 16).Run()["cuts"]),
         "the lie's media are counted apart, and none is a cut of the power's plan (%s)" % plan["counts"])

    with tempfile.TemporaryDirectory() as d:
        img = os.path.join(d, "base.img")
        with open(img, "wb") as fh:
            fh.write(bytes(64 * 512))
        rec = bytearray(512)
        struct.pack_into("<QQQI", rec, 0, blklog.LOG_MAGIC, blklog.LOG_VERSION, 4, 512)
        for lba, fill in ((4, 0x11), (8, 0x22)):
            rec += struct.pack("<QQQQ", lba, 1, 0, 512).ljust(512, b"\0") + bytes([fill]) * 512
            rec += struct.pack("<QQQQ", 0, 0, blklog.LOG_FLUSH, 0).ljust(512, b"\0")
        log = blklog.BlkLog(bytes(rec), 64)
        try:
            over = log.Cut(medium.Medium(img), 2, set(), {}, blot={1}, smudge=[(10, 2)])
            held = (4 not in over.over and bytes(over.over[8]) == b"\x22" * 512
                    and over.over[10] != bytes(512) and over.over[10] != over.over[11]
                    and over.over[10] == blklog.Smudged(10))
            said = "entry 1 %s, entry 3 %s, sector 10 %s" % (
                "lost" if 4 not in over.over else "laid", "laid" if 8 in over.over else "lost",
                "spoilt" if 10 in over.over else "untouched")
        except (TypeError, AttributeError) as x:
            held, said = False, repr(x)
        Want(held, "volume-read's cut lays every epoch before K but the entries Blot names, and sectors "
             "Smudge names as bytes no writer laid, one pattern a sector (%s)" % said)

    results = {"b": [["DOUBLE_OWNER", "damage", "two owners"]]}
    j = Judged(pc, shape, lambda cid: Book([[1, "a", 3, "aa"]]), plan,
               lambda m: Book([[1, "a", 3, "aa"]], results.get(m["id"][0], [])))
    got = Said(j.Codes)
    got.update(Said(lie.Judge, j))
    Want(got.get("codes", ("",))[0] == "behaves" and got.get("blot:owners", ("",))[0] == "fail"
         and got.get("smudge:owners", ("",))[0] == "behaves",
         "two owners of a block a Blot left fail Blot's own count, and the power cut's census stays "
         "consistent beside it (%s)" % {k: v[0] for k, v in got.items()})
    Want(j.run.Exhaustive(), "and the power's plan is still built whole: the lie's media are no subset of "
         "its epochs")
    proven = Judged(pc, shape, lambda cid: Book([[1, "a", 3, "aa"]], last=5), plan,
                    lambda m: Book([[1, "a", 3, "aa"]], last=1))
    got = Said(lie.Judge, proven)
    Want(got.get("blot:proven", ("",))[0] == "fail",
         "a Blot medium holding a whole stitch past the last its census sews on, saying nothing of it, is "
         "a lie gone unsaid (%s)" % (got.get("blot:proven"),))
    with open(os.path.join(proven.run.dir, "lieboots.txt"), "w") as fh:
        fh.write("m2-0\t2\tnone\t-\t-\t992+11,1005+19\tsampled\n")
    got = Said(lie.Judge, proven)
    Want(got.get("smudge:boots", ("",))[0] == "fail" and "blot:boots" not in got,
         "a Smudge medium booted twice is judged under Smudge's own count, and one that never reached a shell "
         "fails it (%s)" % (got.get("smudge:boots"),))


def LieRowTests(pc, Want, Synthetic):
    """The lie's media each held to the line its row of §3.10 owes: Blot's
    torn form (C5) and Smudge's (C13, C18) planned; a Blot holding a whole
    stitch past its census's last says the lie proven or what settled lost,
    and MEDIUM_LIED — a rot is no answer to it — and one holding none never
    says the lie proven (M-2); a stitch a Smudge spoilt that the medium still
    shows said by the ribbon, not by a rot, and one wiped whole with nothing
    past it a FAIL of its own (C18); a stitch's sectors where they lie, on into
    the next Quire too."""
    try:
        import powercut_lie as lie
    except ImportError as x:
        Want(False, "the lie's planner is there to ask: %s" % x)
        return
    shape = Synthetic([[(1, None, "file", "fid 3 data, named by stitch seq 1"), (1, None, "stitch", "stitch seq 1")],
                       [(1, None, "file", "fid 3 data, named by stitch seq 2"), (1, None, "stitch", "stitch seq 2")],
                       [(2, 1003, "stitch", "stitch seq 3, 2 sector(s)")]])
    plan = lie.LiePlanner(shape).Run()
    media = {m["id"]: m for m in plan["media"]}
    m = media.get("b2-d1-data-torn", {})
    Want((m.get("blot"), m.get("keep"), m.get("tear")) == ("4", "", [0, 1]),
         "Blot's torn form at epoch 2: epoch 1's data lost behind its flush, the first stitch of epoch 2 torn "
         "after every sector but its last — C5's medium (%s)" % m)
    m = media.get("m2-0-t", {})
    Want((m.get("tear"), m.get("smudge"), m.get("keep")) == ([0, 1], [[992, 11], [1005, 19]], ""),
         "Smudge's torn form: the stitch in flight laid but its last sector, its StitchHead with it, the rest "
         "of its 16 KiB page spoilt — the paired page of C13 and C18's proof (%s)" % m)

    def Blot(mid, findings, last, lied):
        j = Judged(pc, shape, lambda cid: Book([[1, "a", 3, "aa"]], last=5), plan,
                   lambda m: Book([[1, "a", 3, "aa"]], findings if m["id"] == mid else [],
                                  last if m["id"] == mid else 5, lied and m["id"] == mid))
        return Said(lie.Judge, j).get("blot:proven", ("",))[0]

    proven = [("DEAD_BRANCH", "damage", "the lie is proven: stitch seq 3 lies past the end of the ribbon")]
    Want(Blot("b2-d2-all", [("DIGEST_MISMATCH", "damage", "block 9 does not match")], 1, False) == "fail",
         "a Blot holding stitch 3 whole past its census's last, 1, answered by a rot alone fails: C17 says "
         "the lie proven")
    Want(Blot("b2-d2-all", proven, 1, True) == "behaves" and Blot("b2-d2-all", proven, 1, False) == "fail",
         "said proven with MEDIUM_LIED it behaves; proven without MEDIUM_LIED fails")
    Want(Blot("b2-d1-none", proven, 1, True) == "fail",
         "a Blot holding no whole stitch past its census's last that says the lie proven fails (C5, M-2)")
    Want(Blot("b2-d1-data-torn", [("NOT_LANDED", "note", "fid 3's data in stitch 2 did not land")], 2, False)
         == "fail", "C5: data a stitch named not landed with that stitch's successor torn, and no MEDIUM_LIED, fails")
    SmudgeRowTests(pc, Want, Synthetic, lie)


def SmudgeRowTests(pc, Want, Synthetic, lie):
    shape = Synthetic([[(1, 900, "file", "fid 3 data, named by stitch seq 1"), (2, 991, "stitch", "stitch seq 1, 2 sector(s)")],
                       [(2, 1001, "stitch", "stitch seq 2, 2 sector(s)")]])
    plan = lie.LiePlanner(shape).Run()

    def Smudge(sewn, said):
        # Stitch 1 is sewn on from e0-all — epoch 1's cuts, e1-none among
        # them, are that medium and more.
        j = Judged(pc, shape, lambda cid: Book([[1, "a", 3, "aa"]], last=1, sewn=sewn if cid == "e0-all" or
                                               cid.startswith("e1") else ()), plan,
                   lambda m: Book([[1, "a", 3, "aa"]], said.get(m["id"], ([], False))[0], 0,
                                  said.get(m["id"], ([], False))[1]))
        got = Said(lie.Judge, j)
        return got.get("smudge:said", ("",))[0], got.get("smudge:wiped", ("",))[0]

    torn = [("RIBBON_TORN", "note", "the ribbon ends on stitch 0; seq 1 past it is torn")]
    lost = ([("DEAD_BRANCH", "damage", "what settled is lost: seq 1, 2 past the end of the ribbon")], True)
    part = [[1, [991, 992]]]
    Want(Smudge(part, {"m1-0-t": lost})[0] == "fail",
         "stitch 1 at 991-992, its sector 992 spoilt by the page of stitch 2 in flight, 991 still showing it, "
         "and the census silent: smudge:said fails")
    Want(Smudge(part, {"m1-0": ([("DIGEST_MISMATCH", "damage", "x")], False), "m1-0-t": lost})[0] == "fail",
         "a rot is no answer to a spoilt stitch: C18 keeps X's own bytes, 0 ERR_CHECKSUM")
    Want(Smudge(part, {"m1-0": (torn, False), "m1-0-t": lost}) == ("behaves", "behaves"),
         "the torn tail said for the one, what settled is lost and MEDIUM_LIED for the torn form: both behave")
    Want(Smudge(part, {"m1-0": (torn, False), "m1-0-t": (torn, False)})[0] == "fail",
         "the torn form's StitchHead names stitch 1, spoilt: a torn tail said and not what settled is lost "
         "fails (C18)")
    Want(Smudge([[1, [994, 995]]], {"m1-0-t": lost})[1] == "fail",
         "stitch 1 at 994-995, wholly inside the page of stitch 2 in flight, nothing of either left: a settled "
         "stitch lost with no word — smudge:wiped fails, the format's answer the owner's to give")
    Want(Smudge([[1, [990, 991, 5000]]], {}) == ("behaves", "behaves"),
         "stitch 1 at 990, 991 and on into the next Quire at 5000: sector 992 the page spoils is not its own, "
         "and nothing is owed")


def BookRowTests(pc, Want, Synthetic):
    """A book's oracles held to their rows: a logger's file never fewer lines
    than the flush before left (C21); a Tally laid in said by the kernel
    (C7–C10); the chain's two boots judged, and the file the verdict rolled
    back wearing settle:undone (C16); a torn tail a retry leaves past the
    ribbon's end no boundary not stood (C14)."""
    try:
        import powercut_judge_book as pjb
    except ImportError as x:
        Want(False, "the book's oracles are there to ask: %s" % x)
        return
    shape = Synthetic([[1, (1, None, "stitch", "stitch seq 1")], [1, (1, None, "stitch", "stitch seq 2")], [1]])
    log = pc.RecordBytes("log", 4)
    sha = {n: hashlib.sha256(log[:n * 48]).hexdigest() for n in range(5)}

    def Logger(cid):
        n = {"e0-all": 2, "e2-all": 1}.get(cid, 2 if cid.startswith("e0") else 4)
        return Book([[7, "pc-l.log", n * 48, sha[n]]])

    got = Said(pjb.Lines, Judged(pc, shape, Logger), "pc-l.log", 4).get("lines:pc-l.log", ("", ""))
    Want(got[0] == "fail" and "e2-all" in got[1],
         "a cut with fewer whole lines than the flush before its epoch left — 1 where e1-all left 4 — fails "
         "and is named (%s)" % (got,))
    free = Synthetic([[(8, None, "free", "data no stitch names")] * 4, [(1, None, "slot", "Spine slot of seq 4")]])
    got = Said(pjb.SlipBlocks, Judged(pc, free, lambda cid: Book([])), 7, 40).get("slip:blocks", ("",))[0]
    Want(got == "fail", "data no stitch of the record names counts against the logger's file too: 4 blocks of it "
         "over one Tally, where two a Tally and one for the end are the most (%s)" % got)
    slots = Synthetic([[(1, None, "slot", "Spine slot of seq 4")], [(1, None, "slot", "Spine slot of seq 9")]])
    bound, said = getattr(pjb, "Bound", None), {}
    pjb.Bound = lambda img: (0, ["the Tally binds"])
    try:
        for laid in ("", " (laid in)"):
            j = Judged(pc, slots, lambda cid: Book([]))
            with open(os.path.join(j.run.dir, "run.serial.log"), "w") as fh:
                fh.write("[TagFS] Tally 9 is written over slot 1: the state after stitch 8, 3 node(s) laid in "
                         "2 block(s) a copy, 5 named as they lie%s; 0 block(s) no slot holds any more rest in "
                         "Fallow\n" % laid)
            said[laid] = Said(pjb.Tipin, j).get("tipin", ("",))[0]
    finally:
        pjb.Bound = bound
    Want(said == {"": "fail", " (laid in)": "behaves"},
         "tipin owes a Tally the kernel laid in among the nodes of the one before: whole ones alone fail (%s)"
         % said)
    ChainTests(pc, Want, Synthetic, pjb)
    for sev, want in (("note", "behaves"), ("damage", "fail")):
        j = Judged(pc, shape, lambda cid: Book([[1, "a", 3, "aa"]], [("RIBBON_TORN", sev, "seq 3 past it is torn")]))
        got = Said(j.Boundaries).get("boundaries", ("",))[0]
        Want(got == want, "a flush that stands behind a ribbon whose torn tail is a %s %s (%s)" % (
            sev, "— a retry's cut, M-6 — stands" if sev == "note" else "does not stand", got))


def ChainTests(pc, Want, Synthetic, pjb):
    shape = Synthetic([[1, (1, None, "stitch", "stitch seq 1")], [1, (1, None, "stitch", "stitch seq 2")], [1]])
    landed = [("NOT_LANDED", "note", "fid 1's data in stitch 2 did not land (block 9 of 1 checked does not "
               "match its digest): its record is rolled back by its splice — ERR_TAGFS_NOT_LANDED")]
    mount = "[TagFS] 9 data blocks, 5 free, 1 files\n[TagFS] the volume is MOUNTED — 1 file(s)\nBoxOS Shell\n"

    def Chain(second=mount, tags=("settle:undone",)):
        j = Judged(pc, shape, lambda cid: Book([[1, "a", 3, "aa"]], landed if cid == "e1-s1" else []))
        boots = os.path.join(j.run.dir, "boots")
        os.makedirs(boots)
        with open(os.path.join(j.run.dir, "chain.txt"), "w") as fh:
            fh.write("e1-s1\t1\t0\t-\tthe first cut whose stitch landed and some of its data did not\n")
        after = {"format": "v2", "verdict": "clean", "findings": [],
                 "kernel": {"mounts": True, "refusal": None, "data_blocks": 9, "free": 5, "files": 1, "slot": 0},
                 "book": {"slot": 0, "slots": [{"index": 0, "counts": {"next_fid": 9}}], "counts": {"next_fid": 9},
                          "tally": {"seq": 4}, "under_glass": [],
                          "ribbon": {"stitches": [], "torn": [], "lied": False}},
                 "files": [{"fid": 1, "state": "live", "name": "a", "size": 3, "sha256": "aa", "visible": True,
                            "tag_text": list(tags)}]}
        for name, text in (("e1-s1.serial.log", mount), ("e1-s1.again.serial.log", second),
                           ("e1-s1.after.check.json", json.dumps(after))):
            with open(os.path.join(boots, name), "w") as fh:
                fh.write(text)
        return Said(pjb.Chain, j, "laid").get("chain", ("", ""))

    got = Chain()
    Want(got[0] == "behaves", "C16: both boots mount as predicted, fid 1 rolled back wears settle:undone, no "
         "block of two owners, nothing NOT_LANDED again (%s)" % (got,))
    got = Chain(second="[TagFS] 9 data blocks, 5 free, 1 files\nKERNEL PANIC\n")
    Want(got[0] == "fail" and "second" in got[1], "the second mount panicking fails the chain (%s)" % (got,))
    got = Chain(second=mount.replace("5 free", "6 free"))
    Want(got[0] == "fail" and "second boot" in got[1],
         "the second mount saying other counts than the census of what the first left fails it (%s)" % (got,))
    got = Chain(tags=())
    Want(got[0] == "fail" and "settle:undone" in got[1],
         "fid 1 rolled back without settle:undone fails it (%s)" % (got,))
