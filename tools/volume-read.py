#!/usr/bin/env python3
"""volume-read — read a BoxOS volume from the host, and judge it.

A machine on a bench writes its account onto its own volume, and then the
volume is the only copy. `dd` cannot get it back: a file's contents are a list
of extents scattered through the data run, not a range of sectors, and nothing
on a Mac or a Linux box knows how to follow them.

So this follows them, and it is deliberately READ ONLY. Nothing here writes a
byte to the medium it is pointed at: a tool used to recover evidence must not
be able to damage it. The only files it writes are the ones it is told to
(`get -o`, `cut --out`, `self-test --emit --out`), and never over its input.

    tools/volume-read.py DEVICE list
    tools/volume-read.py DEVICE get NAME [-o OUT]
    tools/volume-read.py DEVICE deed
    tools/volume-read.py DEVICE check [--json]
    tools/volume-read.py DEVICE cut LOG --count [--json]
    tools/volume-read.py DEVICE cut LOG --list [--json]
    tools/volume-read.py DEVICE cut LOG --epoch K (--keep SET | --subset-bits MASK)
                                       [--tear W:MASK]... (--out IMG | --check [--json]
                                       [--digests MEMO] [--digests-out MEMO])
    tools/volume-read.py DEVICE cut LOG --against FINAL
    tools/volume-read.py IMAGE  self-test [--emit CASE --out IMG]

`list` and `get` show the files the mounted kernel would show: each at the
newest record its pool holds, CRC-checked, the way the mount resolves it — or,
on a volume of format v2, as its book holds them once its ribbon is replayed.
`check` is the GroundCensus: every owner of every block against the bitmap,
and a simulation of what the next mount decides (exit 0 consistent, 1 not,
2 unreadable) — on format v2 the book's census (bookcensus.py), every owner
derived from the names that take a block. `cut` reads a QEMU blklogwrites log
of this DEVICE and builds any crash state out of it; the cuts of one record
share a memo of the block digests a book's census takes (bookcensus.py
DigestMemoWrite). `self-test` plants every rule's damage into an overlay over
IMAGE and checks that each is caught.

DEVICE is a raw device or an image file:

    sudo tools/volume-read.py /dev/rdisk4 list
    tools/volume-read.py build/boxos.img get watch.log -o watch.log

On macOS use /dev/rdiskN rather than /dev/diskN — the raw node reads without
the buffer cache, and diskN refuses unaligned reads.

The format itself is said once, in tools/volumeread/ondisk.py (and the book of
format v2 in bookread.py, tally.py and stitch.py), with the C line every number
is copied from; `self-test` holds those numbers against the headers in src/.
This file is only the command line.
"""

import argparse
import json
import os
import sys

# ‼ BEFORE THE PACKAGE IS IMPORTED. This runs from `make logs`, from logcheck
# and from the stand's harnesses, out of a source tree; a __pycache__ left
# beside the sources is build output in a place that holds none.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from volumeread import ondisk                                   # noqa: E402
from volumeread.mount import Volume                             # noqa: E402
from volumeread.medium import (FormatVerdict, Medium, Unreadable,  # noqa: E402
                               find_ground, find_the_stick, permission_advice,
                               read_deed)


def OpenDevice(device):
    if device == "auto":
        device, medium = find_the_stick()
        print(f"[volume-read] {device}", file=sys.stderr)
        return device, medium
    try:
        return device, Medium(device)
    except PermissionError:
        sys.exit(permission_advice(device))
    except OSError as e:
        sys.exit(f"cannot open {device}: {e}\n"
                 f"on macOS a raw device is /dev/rdiskN, not /dev/diskN")


def CmdFiles(args, medium):
    try:
        ground, how = find_ground(medium, args.start)
        deed = read_deed(medium, ground)
    except (ValueError, IOError) as e:
        sys.exit(f"{e}")

    if args.command == "deed":
        uuid = deed["uuid"].hex()
        lay, frame = deed["layout"], deed["frame"]
        print(f"ground      : sector {ground} ({how})")
        print(f"volume      : {uuid}")
        print(f"sectors     : {deed['sectors']}")
        print(f"role        : {deed['role']} (1=head, 2=tail)")
        print(f"format      : {deed['format']} (v1 = LAYOUT, v2 = FRAME)")
        if lay:
            print(f"blocks      : {lay['total_blocks']}")
            print(f"metadata at : block {lay['metadata_pool_block']}")
            print(f"data run    : block {lay['data_block']}, "
                  f"{lay['data_blocks']} block(s)")
        if frame:
            print(f"spine       : {frame['spine_slots']} slot(s) from block "
                  f"{frame['spine_block']}, {frame['spine_stride']} block(s) apart")
            print(f"data run    : block {frame['data_block']} to the far copy at block "
                  f"{deed['tail_sector'] // ondisk.SECTORS_PER_BLOCK}")
        if deed["format"] == "v2":
            return BookDeed(medium, args.start)
        verdict = FormatVerdict(deed)
        if verdict:
            print(f"verdict     : {verdict}")
        return 0

    if deed["format"] == "v2":
        return BookFiles(args, medium, ground, how)
    verdict = FormatVerdict(deed)
    if verdict:
        sys.exit(verdict)

    vol = Volume(medium, ground, deed)
    if vol.ledger() is None:
        print("[volume-read] neither ledger copy is readable, so the kernel would not "
              "mount this volume; every file its table names is listed",
              file=sys.stderr)
    files = vol.files()

    if args.command == "list":
        print(f"ground sector {ground} ({how}), "
              f"volume {deed['uuid'].hex()[:16]}…")
        if not files:
            print("no active files")
            return 0
        width = max(len(f["name"]) for f in files)
        for f in sorted(files, key=lambda x: x["name"]):
            blocks = sum(c for _s, c in f["extents"])
            print(f"  {f['name']:<{width}}  {f['size']:>10} bytes  "
                  f"{len(f['extents'])} extent(s), {blocks} block(s)")
        return 0

    match = [f for f in files if f["name"] == args.name]
    if not match:
        have = ", ".join(sorted(f["name"] for f in files)) or "(none)"
        sys.exit(f"no active file named {args.name!r} on this volume.\n"
                 f"what is here: {have}")

    data = vol.contents(match[0])
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(data)
        print(f"{len(data)} byte(s) of {args.name} written to {args.out}",
              file=sys.stderr)
    else:
        sys.stdout.buffer.write(data)
    return 0


def BookDeed(medium, start):
    """What the Spine of a format-v2 volume says, slot by slot, and whether
    its book mounts — or, when the volume cannot be stood on at all (neither
    copy of the Deed answers as the strict reader asks), why, beside what the
    forgiving read of the Deed above already said."""
    from volumeread import tally
    from volumeread.bookmount import BookMount
    from volumeread.bookground import BookGround
    try:
        m = BookMount(BookGround(medium, start))
    except (Unreadable, IOError) as e:
        print(f"book        : not read — {e}")
        return 0
    for s in m.slots:
        said = s.state if s.state != "valid" else (
            f"seq {s.seq} — Tally {s.tally_seq} ("
            + ", ".join(tally.TREE_NAMES.get(k, str(k)) for k in sorted(s.roots))
            + f"), ribbon from block {s.ribbon['at']}, stitch {s.stitch_seq}")
        print(f"slot {s.index}      : block {s.block}, {said}{' — ' + s.why if s.why else ''}")
    print("book        : " + (f"does not mount — {m.refusal}" if m.refusal else
                              f"mounts on slot {m.slot.index}, {len(m.files)} file(s)"
                              + (", read only" if m.glass else "")))
    return 0


def GroundRuns(runs):
    """The runs of ground a book's file lies in. Its record keeps a run a
    cycle on each run of ground — a run a settled stitch named is never grown
    (spec_v2 §2.8: a run's birth is the stitch that put it in) — so its runs
    count the cycles that grew it as well; one that begins where the one
    before it ended goes on the same run of ground, a hole between them or
    none, as the kernel's stand counts them (stand_fresh_ground_tests.c
    StandFreshForcedRuns)."""
    n, end = 0, None
    for r in runs:
        if not r.hole:
            n += r.start != end
            end = r.start + r.count
    return n


def BookFiles(args, medium, ground, how):
    """`list` and `get` of a format-v2 volume: the files its book holds once
    its ribbon is replayed, each read through its runs and Slips."""
    from volumeread.bookcensus import BookCensus
    try:
        census = BookCensus(medium, args.start)
    except (Unreadable, IOError) as e:
        sys.exit(f"{e}")
    m = census.mount
    if m.refusal:
        sys.exit(f"the book does not mount: {m.refusal}")
    named = {fs.name.decode("utf-8", "replace"): fid for fid, fs in m.files.items()}
    if args.command == "list":
        print(f"ground sector {ground} ({how}), volume {census.g.uuid.hex()[:16]}… — format v2")
        if not named:
            print("no active files")
            return 0
        width = max(len(n) for n in named)
        for name in sorted(named):
            fs = m.files[named[name]]
            print(f"  {name:<{width}}  {fs.size:>10} bytes  {len(fs.runs)} run(s), {fs.Blocks()} block(s), "
                  f"{GroundRuns(fs.runs)} run(s) of ground" + (f", {len(fs.slips)} Slip(s)" if fs.slips else ""))
        return 0
    if args.name not in named:
        have = ", ".join(sorted(named)) or "(none)"
        sys.exit(f"no active file named {args.name!r} on this volume.\nwhat is here: {have}")
    data = census.Content(named[args.name])
    if data is None:
        sys.exit(f"{args.name} names a block the medium will not give")
    bad = [x.text for x in census.findings if x.code == "DIGEST_MISMATCH" and
           x.text.startswith(census.Label(named[args.name]))]
    for text in bad:
        print(f"[volume-read] {text}", file=sys.stderr)
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(data)
        print(f"{len(data)} byte(s) of {args.name} written to {args.out}", file=sys.stderr)
    else:
        sys.stdout.buffer.write(data)
    return 0


def AnyCensus(medium, start):
    """The census of the volume's own format: the book's for format v2,
    GroundCensus for v1 (which refuses every other Deed as it always has)."""
    from volumeread.bookcensus import BookCensus
    from volumeread.census import GroundCensus
    from volumeread.medium import StandOnDeed
    if StandOnDeed(medium, start).format == "v2":
        return BookCensus(medium, start)
    return GroundCensus(medium, start)


def SayCensus(census, as_json):
    if as_json:
        print(json.dumps(census.Json(), sort_keys=True))
    else:
        for line in census.HumanLines():
            print("check: " + line)
    return census.ExitCode()


def CmdCheck(args, medium):
    try:
        census = AnyCensus(medium, args.start)
    except (Unreadable, IOError) as e:
        print(f"check: unreadable — {e}")
        return 2
    return SayCensus(census, args.json)


def CmdCut(args, medium):
    from volumeread import blklog
    from volumeread.bookcensus import DigestMemoRead, DigestMemoWrite
    try:
        log = blklog.BlkLog(args.log, medium.SizeSectors())
        if args.against:
            diff = blklog.Against(log, medium, args.against)
            if diff:
                print(f"cut: {diff}")
                return 1
            print(f"cut: base + {log.entries} entries of the log equal {args.against}, "
                  f"sector for sector")
            return 0
        facts = log.EpochFacts()
        if args.count:
            return CutCount(log, facts, args.json)
        classifier = blklog.Classifier(log, medium, args.start)
        if args.list:
            return CutList(log, facts, classifier, args.json)
        epoch = log.Epoch(args.epoch)
        if args.subset_bits is not None:
            keep = blklog.ParseSubsetBits(args.subset_bits, epoch)
        else:
            keep = blklog.ParseKeep(args.keep, epoch, classifier.Classify)
        tears = blklog.ParseTears(args.tear or [], epoch, keep)
        over = log.Cut(medium, args.epoch, keep, tears, blklog.ParseBlot(args.blot),
                       blklog.ParseSmudge(args.smudge))
        if args.out:
            cloned = over.WriteOut(args.out, [(args.log, "the log")])
            print(f"cut: epoch {args.epoch}, {len(keep)} of {len(epoch)} write(s) kept, "
                  f"{len(tears)} torn — {len(over.over)} sector(s) laid over "
                  f"{'a clone' if cloned else 'a copy'} of the base in {args.out}")
            return 0
        if args.digests:
            try:
                DigestMemoRead(args.digests)
            except (OSError, ValueError, TypeError) as e:
                print(f"cut: the memo of digests {args.digests} does not read — {e}", file=sys.stderr)
                return 2
        try:
            census = AnyCensus(over, args.start)
        except (Unreadable, IOError) as e:
            print(f"check: unreadable — {e}")
            return 2
        if args.digests_out:
            DigestMemoWrite(args.digests_out)
        return SayCensus(census, args.json)
    except (blklog.LogUnreadable, ValueError) as e:
        print(f"cut: {e}", file=sys.stderr)
        return 2


def CutCount(log, facts, as_json):
    if as_json:
        print(json.dumps({"entries": log.entries, "super_entries": log.super_entries,
                          "flushes": len(log.flush_seqs), "sector_bytes": log.sector_bytes,
                          "stop": log.stop, "notes": log.notes,
                          "epochs": [e["writes"] for e in facts]}, sort_keys=True))
        return 0
    print(f"cut: {log.entries} entries, {len(log.flush_seqs)} of them flushes; "
          f"the walk stopped at {log.stop}")
    for note in log.notes:
        print(f"cut: {note}")
    print("cut: writes per epoch: " + " ".join(str(e["writes"]) for e in facts))
    return 0


def CutList(log, facts, classifier, as_json):
    writes = []
    for w in log.Writes():
        writes.append({"epoch": w.epoch, "index": w.index, "seq": w.seq, "lba": w.lba,
                       "sectors": w.sectors, "discard": w.discard,
                       "touches": [{"first_lba": t.first_lba, "sectors": t.sectors,
                                    "kind": t.kind, "what": t.what}
                                   for t in classifier.Classify(w)]})
    if as_json:
        print(json.dumps({"entries": log.entries, "super_entries": log.super_entries,
                          "sector_bytes": log.sector_bytes, "stop": log.stop,
                          "notes": log.notes, "epochs": facts, "writes": writes},
                         sort_keys=True))
        return 0
    for note in log.notes:
        print(f"cut: {note}")
    for e in facts:
        mine = [w for w in writes if w["epoch"] == e["epoch"]]
        kinds = {}
        for w in mine:
            for t in w["touches"]:
                kinds[t["kind"]] = kinds.get(t["kind"], 0) + 1
        closed = ("still open when the log ends" if e["flush_seq"] is None else
                  "closed by flush at entry %u%s" % (e["flush_seq"],
                                                     ", which may be QEMU's own at exit"
                                                     if e["maybe_qemu_exit_flush"] else ""))
        print("epoch %u — %u write(s): %s; %s" % (
            e["epoch"], e["writes"],
            ", ".join("%s %u" % kv for kv in sorted(kinds.items())) or "nothing", closed))
        for w in mine:
            print("  %u.%u  entry %u  LBA %u+%u%s  %s" % (
                w["epoch"], w["index"], w["seq"], w["lba"], w["sectors"],
                "  DISCARD" if w["discard"] else "",
                "; ".join("%s %s" % (t["kind"], t["what"]) for t in w["touches"])))
    return 0


def CmdBound(args, medium):
    """A format-v2 book's newest Tally held to what a writer of one lays
    (binder.BindCheck): bound again where it lies, and the state it is of
    read off the slot before it and its ribbon."""
    from volumeread import binder
    from volumeread.bookground import BookGround
    from volumeread.bookmount import BookMount
    wrong = binder.BindCheck(medium, args.start)
    m = BookMount(BookGround(medium, args.start))
    if m.refusal is None:
        blocks, cost, whole, useful = binder.BindSaid(m)
        print("bound: the newest slot %u (seq %u) names Tally %u, after stitch %u: %u block(s) a copy, the next "
              "reckoned at %u byte(s) of a whole %u; %u useful byte(s) of ribbon past its at, %u stitch(es)"
              % (m.slot.index, m.slot.seq, m.slot.tally_seq, m.slot.stitch_seq, blocks, cost, whole, useful,
                 len(m.ribbon.stitches)))
    for w in wrong:
        print("bound: %s" % w)
    print("bound: %s" % ("HELD" if not wrong else "%u difference(s)" % len(wrong)))
    return 0 if not wrong else 1


def CmdSelfTest(args, medium):
    from volumeread import selftest
    if args.emit:
        if not args.out:
            print("self-test: --emit needs --out", file=sys.stderr)
            return 2
        return selftest.Emit(medium, args.device, args.emit, args.out)
    return selftest.Run(medium, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "src"))


def Main():
    start = argparse.ArgumentParser(add_help=False)
    start.add_argument("--start", type=int, default=argparse.SUPPRESS,
                       help="ground start in 512-byte sectors, when the "
                            "partition table cannot be trusted")
    ap = argparse.ArgumentParser(
        description="Read a BoxOS volume from the host, and judge it. Read only.",
        parents=[start])
    ap.add_argument("device",
                    help="raw device, image file, or 'auto' to find the "
                         "one BoxOS stick that is plugged in")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("list", parents=[start], help="the files the mounted kernel would show")
    get = sub.add_parser("get", parents=[start], help="one file's bytes")
    get.add_argument("name", help="file to get")
    get.add_argument("-o", "--out", help="write to this path instead of stdout")
    sub.add_parser("deed", parents=[start], help="what the Deed says")
    check = sub.add_parser("check", parents=[start], help="the GroundCensus")
    check.add_argument("--json", action="store_true")
    cut = sub.add_parser("cut", parents=[start], help="read a blklogwrites log; build a crash state")
    cut.add_argument("log")
    mode = cut.add_mutually_exclusive_group(required=True)
    mode.add_argument("--count", action="store_true")
    mode.add_argument("--list", action="store_true")
    mode.add_argument("--against", metavar="FINAL")
    mode.add_argument("--epoch", type=int, metavar="K")
    cut.add_argument("--keep", metavar="SET")
    cut.add_argument("--subset-bits", metavar="MASK")
    cut.add_argument("--tear", action="append", metavar="W:MASK")
    cut.add_argument("--blot", metavar="SEQS", help="entries of epochs before K the medium did not keep")
    cut.add_argument("--smudge", metavar="LBA+N,...", help="sectors laid last as bytes no writer laid")
    cut.add_argument("--out", metavar="IMG")
    cut.add_argument("--check", action="store_true")
    cut.add_argument("--json", action="store_true")
    cut.add_argument("--digests", metavar="MEMO", help="a book's block digests a check of the same run took")
    cut.add_argument("--digests-out", metavar="MEMO", help="the block digests this check took, kept")
    sub.add_parser("bound", parents=[start], help="the newest Tally held to the one rule of binding")
    st = sub.add_parser("self-test", parents=[start], help="every census rule, planted and caught")
    st.add_argument("--emit", metavar="CASE")
    st.add_argument("--out", metavar="IMG")
    args = ap.parse_args()
    args.start = getattr(args, "start", None)

    if args.command == "cut" and args.epoch is not None:
        if (args.keep is None) == (args.subset_bits is None):
            ap.error("cut --epoch wants exactly one of --keep and --subset-bits")
        if bool(args.out) == bool(args.check):
            ap.error("cut --epoch wants exactly one of --out and --check")
    elif args.command == "cut" and (args.keep or args.subset_bits or args.tear or args.out
                                    or args.check):
        ap.error("--keep, --subset-bits, --tear, --out and --check go with --epoch")

    args.device, medium = OpenDevice(args.device)
    if args.command in ("list", "get", "deed"):
        return CmdFiles(args, medium)
    if args.command == "check":
        return CmdCheck(args, medium)
    if args.command == "cut":
        return CmdCut(args, medium)
    if args.command == "bound":
        return CmdBound(args, medium)
    return CmdSelfTest(args, medium)


if __name__ == "__main__":
    sys.exit(Main())
