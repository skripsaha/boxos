"""powercut_selftest — the PowerCut gate judging itself: `tools/powercut.py
selftest`, the planner's arithmetic and the judge's reading of a record on
listings made up here, and `forge`, a real record with one byte flipped for
the record's self-check to refuse.

Not a program of its own: powercut.py hands it the module it was loaded as,
so the arithmetic tested is the arithmetic the judge runs, not a copy of it.
"""

import json
import os
import struct
import tempfile


def Synthetic(shape):
    """A listing from [[write...] per epoch]; a write is its sector count,
    (sectors, lba) to put it somewhere on purpose — lba 0 is outside the
    volume, None the next place — or (sectors, lba, kind) to say what it
    touched. Every epoch but the last is closed by a flush."""
    writes, epochs, seq, lba = [], [], 0, 1000
    for k, ws in enumerate(shape):
        for i, w in enumerate(ws):
            n, at, kind = (tuple(w) + (None, None))[:3] if isinstance(w, tuple) else (w, None, None)
            seq += 1
            writes.append({"epoch": k, "index": i, "seq": seq, "lba": lba if at is None else at,
                           "sectors": n, "discard": False,
                           "touches": [{"kind": kind or ("outside" if at == 0 else "file"),
                                        "first_lba": lba, "sectors": n, "what": "w"}]})
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

    ledger = (1, None, "ledger")
    run = Recorded([[ledger], [1], [1]], 0, 4)
    Want(run.AckFlush(run.steps[0]) == 2, "the flush behind a step is the first after its ledger, not a "
         "writer's later one (%s)" % run.AckFlush(run.steps[0]))
    run = Recorded([[1], [ledger]], 0, 3)
    Want(run.AckFlush(run.steps[0]) is None, "a data writer's flush before the step's ledger stands behind "
         "nothing of the step's (%s)" % run.AckFlush(run.steps[0]))
    run = Recorded([[1], [1]], 0, 3)
    Want(not run.LedgerWrites(run.steps[0]) and run.AckFlush(run.steps[0]) is None,
         "a step that wrote no ledger has no flush of its own")
    Want(Recorded([[1, 1], [1]], 0, 3).Exhaustive() and not Recorded([[1] * 14], 0, 0).Exhaustive()
         and not Recorded([[40]], 0, 0).Exhaustive(),
         "a record is enumerated whole only with no subset and no torn prefix left out")
    print("selftest: %d failed" % len(fails))
    return 1 if fails else 0
