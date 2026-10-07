"""mount — what the kernel's mount decides on a volume, step for step.

The decision procedure of MountReadVolume (src/kernel/tagfs/mount/mount.c) and
of what it calls, said once on the host, for `list`, `get` and `check` alike:

    the Ledger       the newest intact copy, and its seal — a copy's seal is
                     one only when it says so (ledger.c ledger_seal_of); with
                     none on the newest copy there is none at all
    the registry     read up to the block the newest seal names, and as far in
                     it as a seal the block bears out; a block that bears none
                     out rolls back (tag_registry_load.c) — whatever the pool
                     then stands on
    the pool         the walk that holds each seal against the chain, and the
                     one it then stands on: the newest seal borne out, the
                     other borne out (fell back), a seal whose closed blocks
                     are borne out and open block is not (its open block rolls
                     back), or none — the whole chain from the Deed's head
                     (metadata_pool/meta_pool_mount.c PoolMount)
    the table        walked from the Deed's head: strictly with no seal, cut
                     short under one (file_table.c TableWalk)
    the files        a file is its newest readable record; its row is
                     corrected to it, or derived under a seal; a row with no
                     record is a cycle that did not settle under a seal and a
                     file lost without one; a record no row names is not taken
                     up without a seal (mount.c MountFile)
    the numbers      the next file id past every id a row or a record shows,
                     the next tag id past every tag a record wears (mount.c
                     MountNextFileId, tag_registry_load.c TagRegistryNumbersFrom)
    the ground       every owner's claim, as GroundCensus hears it
                     (mount.c MountCensus, src/include/ground_owner.h)

It reads nothing but through `read_block` (a block of the data run, None when
it will not read), and writes nothing: the mount itself writes no byte either —
what it repairs in memory the next Settle cycle writes.
"""

import struct

from . import ondisk
from .ondisk import (CHAIN_HEADER, MPOOL_DATA, MPOOL_MAGIC, REGISTRY_DATA, REGISTRY_MAGIC,
                     FILETBL_MAGIC, FTABLE_PER_BLOCK, FILE_ACTIVE, FILE_TRASHED, MAX_TAG_ID,
                     SECTOR, SECTORS_PER_BLOCK, LEDGER_COPIES, U16, U32, Crc32)


# ---------------------------------------------------------------------------
# The Ledger's copies and their seals
# ---------------------------------------------------------------------------


def LedgerChoice(copies):
    """The newest intact copy: the strictly greater seq, a tie to copy 0
    (ledger.c ledger_load). None when neither is intact — the kernel refuses."""
    newest = None
    for c in copies:
        if c["ok"] and (newest is None or c["seq"] > newest["seq"]):
            newest = c
    return newest


def Seals(copies):
    """MountSealsRead (mount.c): [newest copy, the other copy] and their seals
    [s0, s1] — s0 only when the newest copy carries a seal of its own, s1 only
    when s0 does and the other copy is intact and sealed."""
    newest = LedgerChoice(copies)
    if newest is None:
        return [None, None], [None, None]
    other = copies[(newest["copy"] + 1) % LEDGER_COPIES]
    s0 = newest["mark"] if newest["sealed"] else None
    s1 = other["mark"] if s0 is not None and other["ok"] and other["sealed"] else None
    return [newest, other], [s0, s1]


# ---------------------------------------------------------------------------
# The registry
# ---------------------------------------------------------------------------


def RegistrySealedBytes(raw, newest, older, block):
    """How much of the block the newest seal names the registry stands on: the
    first of the two seals naming this block whose bytes the block bears out,
    or None — its records roll back (tag_registry_load.c)."""
    for m in (newest, older):
        if m is None or m["registry_block"] != block or U32(raw, 0) != REGISTRY_MAGIC:
            continue
        n = m["registry_bytes"]
        if (n <= U16(raw, 10) and n <= REGISTRY_DATA and
                Crc32(bytes(raw[CHAIN_HEADER:CHAIN_HEADER + n])) == m["registry_crc32"]):
            return n
    return None


def RegistryMount(read_block, first, newest, older, run):
    """tag_registry_load: the chain from the Deed's head, every block read
    claimed, up to the block the newest seal names — never past it — or to the
    chain's end. {blocks, taken: [(block, records)], rolled, short, refused}:
    `rolled` the sealed block that bore no seal out, `short` the block the
    chain ended at before the seal's, `refused` the loader's sentence. A block
    that refuses the mount is not among `blocks`: the kernel claims nothing of
    a volume it does not mount, and the census's account of it is the chain as
    far as it is one."""
    out = {"blocks": [], "taken": [], "rolled": None, "short": None,
           "refused": None, "past": None}
    if newest is None:
        newest, older = older, None
    block, hops = first, 0
    while True:
        if block >= run:
            out["refused"] = ("the chain points at block %u and this volume's data run is %u "
                              "blocks" % (block, run))
            return out
        raw = read_block(block)
        if raw is None:
            out["refused"] = "block %u of the tag registry would not read" % block
            return out
        sealed_here = newest is not None and block == newest["registry_block"]
        magic = U32(raw, 0)
        if not sealed_here and magic != REGISTRY_MAGIC:
            out["refused"] = "block %u holds 0x%08x where the tag registry should be" % (block, magic)
            return out
        out["blocks"].append(block)
        upto = REGISTRY_DATA
        if sealed_here:
            upto = RegistrySealedBytes(raw, newest, older, block)
            if upto is None:
                out["rolled"], upto = block, 0
        records = [] if magic != REGISTRY_MAGIC else ondisk.ReadRegistryBlock(raw, upto)["records"]
        out["taken"].append((block, records))
        if sealed_here:
            out["past"] = U32(raw, 4) or None
            return out
        nxt = U32(raw, 4)
        if nxt == 0:
            if newest is not None:
                out["short"] = block
            return out
        hops += 1
        if hops >= run:
            out["refused"] = ("this volume's tag registry chain has taken %u hops in a data run "
                              "of %u blocks" % (hops, run))
            return out
        block = nxt


def RegistryAdopt(taken):
    """RegistryAdopt (tag_registry.c) over every record taken, in order: a
    spelling the registry holds keeps its id; an id past the last is dropped;
    an id another spelling holds is dropped. ({id: (key, value)}, [(why,
    block, record)])."""
    by_id, by_kv, dropped = {}, {}, []
    for block, records in taken:
        for r in records:
            kv = (r["key"], r["value"])
            if kv in by_kv:
                if by_kv[kv] != r["tag_id"]:
                    dropped.append(("repeats", block, r, by_kv[kv]))
            elif r["tag_id"] > MAX_TAG_ID:
                dropped.append(("range", block, r, None))
            elif r["tag_id"] in by_id:
                dropped.append(("collides", block, r, None))
            else:
                by_id[r["tag_id"]], by_kv[kv] = kv, r["tag_id"]
    return by_id, dropped


# ---------------------------------------------------------------------------
# The pool
# ---------------------------------------------------------------------------

_STOPPED = object()       # a judging walk the seals' verdicts decide what comes after


class _PoolWalk:
    """One walk's chain, map and counts; the numbers the medium has shown are
    kept across walks (PoolMountOutcome top_fid, top_tag)."""

    def __init__(self, read_block, run):
        self.read, self.run = read_block, run
        self.top_fid = self.top_tag = 0
        self.past, self.past_blocks = [], []
        self.Forget()

    def Forget(self):
        """WalkForget: a walk made again starts from nothing."""
        self.chain, self.spans, self.records = [], [], []
        self.files, self.gone, self.unreadable = {}, set(), []
        self.disagreed, self.tail = 0, None


def _Try(mark):
    return {"mark": mark, "fold": 0, "verdict": "unmet"}


def _Judge(tries, block, raw):
    """WalkJudge: the seals not yet met, held against the block the walk is at
    — raw None when it would not read as a block of the pool, block None when
    the chain ran out before a seal's open block."""
    for t in tries or ():
        if t is None or t["mark"] is None or t["verdict"] != "unmet":
            continue
        m = t["mark"]
        if block != m["pool_to_block"]:
            if raw is not None:
                t["fold"] ^= ondisk.PoolBlockMix(block, raw)
            else:
                t["verdict"] = "broken"
            continue
        to = m["pool_to_bytes"]
        tail = (raw is not None and to <= MPOOL_DATA and U16(raw, 8) >= to and
                Crc32(bytes(raw[CHAIN_HEADER:CHAIN_HEADER + to])) == m["tail_crc32"])
        t["verdict"] = ("broken" if t["fold"] != m["closed_crc32"] else
                        "borne" if tail else "torn")


def _Plan(mark=None, torn=False, head=None):
    if mark is None:
        return {"start": head, "sealed": False, "torn": False, "to_block": None, "to_bytes": 0}
    return {"start": mark["chain_first"], "sealed": True, "torn": torn,
            "to_block": mark["pool_to_block"], "to_bytes": mark["pool_to_bytes"]}


def _Records(w, raw, block, upto):
    """WalkRecords + WalkPut: the block's records up to `upto` into the map, in
    order. A record that does not unpack is passed over — the file keeps the
    newest of its records that does (the rule a rotten newest record meets
    today); an ACTIVE one becomes its file's, a Quitclaim takes the file out."""
    walk, _stop = ondisk.PoolRecords(raw, upto)
    for at, _len, fid in walk:
        w.top_fid = max(w.top_fid, fid)
        rec = ondisk.ParseRecord(raw, at, block)
        w.records.append(rec)
        if not fid:
            continue
        if not rec.unpack_ok:
            w.unreadable.append(rec)
            continue
        w.top_tag = max([w.top_tag] + rec.tags)
        if rec.flags & FILE_ACTIVE:
            w.files[fid] = rec
            w.gone.discard(fid)
        else:
            w.files.pop(fid, None)
            w.gone.add(fid)


def _FidsPast(w, raw, block, start):
    """WalkFidsPast: the ids of the records a seal does not hold."""
    walk, _stop = ondisk.PoolRecords(raw, ondisk.PoolUsed(raw))
    for at, _len, fid in walk:
        if at - CHAIN_HEADER >= start:
            w.top_fid = max(w.top_fid, fid)
            w.past.append(ondisk.ParseRecord(raw, at, block))


def _PastSpan(w, nxt):
    """WalkPastSpan: the blocks a cycle that did not settle linked on, read for
    their records' ids only, and left at the first that is not one."""
    hops = 0
    while nxt != 0 and nxt < w.run and hops < w.run:
        raw = w.read(nxt)
        if raw is None or U32(raw, 0) != MPOOL_MAGIC:
            break
        _FidsPast(w, raw, nxt, 0)
        w.past_blocks.append(nxt)
        nxt = U32(raw, 4)
        hops += 1


def _WalkOnce(w, plan, judge):
    """WalkOnce: None with the tail laid, _STOPPED for a judging walk that could
    go no further, or the sentence of a walk that does not mount."""
    block, hops = plan["start"], 0
    while True:
        at_to = plan["sealed"] and block == plan["to_block"]
        if at_to and plan["torn"]:
            w.chain.append(block)
            w.tail = (block, 0)
            return None
        why, raw = None, None
        if block >= w.run:
            why = "lies outside the volume's data run"
        else:
            raw = w.read(block)
            if raw is None:
                why = "would not read"
            elif U32(raw, 0) != MPOOL_MAGIC:
                why = "holds no metadata pool"
        _Judge(judge, block, None if why else raw)
        if why:
            if judge is not None:
                return _STOPPED
            return ("block %u of this volume's metadata chain %s — the records of its files "
                    "cannot all be reached" % (block, why))
        if ondisk.ReadPoolBlock(raw)["kept_counted"] != U16(raw, 12):
            w.disagreed += 1
        w.chain.append(block)
        used, nxt = ondisk.PoolUsed(raw), U32(raw, 4)
        if at_to or (not plan["sealed"] and nxt == 0):
            upto = min(plan["to_bytes"], used) if at_to else used
            w.spans.append((block, raw, upto))
            _Records(w, raw, block, upto)
            if at_to:
                _FidsPast(w, raw, block, upto)
                _PastSpan(w, nxt)
            w.tail = (block, upto)
            return None
        w.spans.append((block, raw, used))
        _Records(w, raw, block, used)
        stop = ("ends" if nxt == 0 else
                "goes round in a circle" if hops + 1 >= w.run else None)
        if stop:
            _Judge(judge, None, None)
            if judge is not None:
                return _STOPPED
            return ("this volume's metadata chain %s at block %u%s" %
                    (stop, block, ", short of block %u the seal it stands on names" % plan["to_block"]
                     if plan["sealed"] else ""))
        block = nxt
        hops += 1


def _SealsDecide(tries):
    """SealsDecide: (stood, index, fell_back)."""
    t0, t1 = tries
    if t0 and t0["mark"] and t0["verdict"] == "borne":
        return "seal", 0, False
    if t1 and t1["mark"] and t1["verdict"] == "borne":
        return "seal", 1, True
    for i, t in enumerate(tries):
        if t and t["mark"] and t["verdict"] == "torn":
            return "torn", i, False
    return "none", None, False


def PoolMount(read_block, head, seals, run):
    """PoolMount (meta_pool_mount.c) on the seals [s0, s1]: the walks it makes,
    and what the last of them stood on. A dict — stood ('seal' | 'torn' |
    'none'), seal (0 | 1 | None), fell_back, verdicts, chain, spans [(block,
    raw, upto)], records (every record the standing walk met, in order), files
    {fid: Record} (the map: the newest readable ACTIVE record of each), gone
    (ids a Quitclaim took out), unreadable, top_fid, top_tag, disagreed, tail,
    past (records no seal holds), past_blocks, refused."""
    w = _PoolWalk(read_block, run)
    s0, s1 = seals
    out = {"stood": "none", "seal": None, "fell_back": False, "refused": None,
           "verdicts": [None, None]}
    if run == 0:
        out["refused"] = "this volume states a data run of no blocks at all"
        return _PoolOut(w, out)
    if s0 is None:
        rc = _WalkOnce(w, _Plan(head=head), None)
    else:
        apart = s1 is not None and s1["chain_first"] != s0["chain_first"]
        tries = [_Try(s0), _Try(None if apart else s1)]
        rc = _WalkOnce(w, _Plan(s0), tries)
        judged = rc is None or rc is _STOPPED
        if judged and tries[0]["verdict"] == "borne":
            out.update(stood="seal", seal=0)
            rc = None
        elif judged:
            rc = _AfterNewest(w, head, s0, s1, tries, out, apart)
        out["verdicts"] = [t["verdict"] if t and t["mark"] else None for t in tries]
    if rc is _STOPPED:
        rc = "a walk that judges a seal could go no further"
    if rc is not None:
        out["refused"] = rc
    return _PoolOut(w, out)


def _AfterNewest(w, head, s0, s1, tries, out, apart):
    """WalkAfterNewest: the other seal judged — by a walk of its own when its
    chain begins elsewhere — and the walk the verdicts choose, made again."""
    w.Forget()
    if apart:
        own = [None, _Try(s1)]
        rc = _WalkOnce(w, _Plan(s1), own)
        tries[1] = own[1]
        if not (rc is None or rc is _STOPPED):
            return rc
        if own[1]["verdict"] == "borne":
            out.update(stood="seal", seal=1, fell_back=True)
            return None
        w.Forget()
    stood, at, fell_back = _SealsDecide(tries)
    out.update(stood=stood, seal=at, fell_back=fell_back)
    if stood == "seal":
        return _WalkOnce(w, _Plan(s1), None)
    if stood == "torn":
        mark = s0 if at == 0 else s1
        out["torn_bytes"] = mark["pool_to_bytes"]
        return _WalkOnce(w, _Plan(mark, torn=True), None)
    return _WalkOnce(w, _Plan(head=head), None)


def _PoolOut(w, out):
    out.update(chain=w.chain, spans=w.spans, records=w.records, files=w.files, gone=w.gone,
               unreadable=w.unreadable, top_fid=w.top_fid, top_tag=w.top_tag,
               disagreed=w.disagreed, tail=w.tail, past=w.past, past_blocks=w.past_blocks)
    return out


def HeadPrefix(read_block, head, first, chain, run):
    """GleanMount + GleanWalkPrefix (glean.c): once the chain begins past the
    Deed's head, what the head still names before the chain's first block,
    followed from its next_block and no further than the first block, a block
    outside the run, the head, a block of the chain, one the walk met already,
    or one that does not read as a block of the pool."""
    if first == head:
        return []
    raw = read_block(head) if head < run else None
    if raw is None or U32(raw, 0) != MPOOL_MAGIC:
        return []
    out, block, on_chain = [], U32(raw, 4), set(chain)
    for _hops in range(run):
        if block in (first, 0) or block >= run or block == head or block in on_chain or block in out:
            break
        raw = read_block(block)
        if raw is None or U32(raw, 0) != MPOOL_MAGIC:
            break
        out.append(block)
        block = U32(raw, 4)
    return out


# ---------------------------------------------------------------------------
# The table
# ---------------------------------------------------------------------------


def TableMount(read_block, first, sealed, run):
    """file_table_init (file_table.c TableWalk) from the Deed's head. With no
    seal a block that is not the table refuses the mount; under one the chain
    ends there — a block reached is kept as its last link, rowless. {links,
    rows {fid: (meta_block, meta_offset)}, cut, refused}."""
    out = {"links": [], "rows": {}, "cut": False, "refused": None}
    block, hops = first, 0
    while True:
        if block >= run:
            if not sealed:
                out["refused"] = "block %u lies outside the volume's data run" % block
            out["cut"] = True
            return out
        raw = read_block(block)
        if raw is None or U32(raw, 0) != FILETBL_MAGIC:
            if not sealed:
                out["refused"] = "block %u %s" % (block, "would not read" if raw is None
                                                  else "holds no file table")
                return out
            out["links"].append(block)
            out["cut"] = True
            return out
        link = len(out["links"])
        out["links"].append(block)
        for i, (mb, mo) in enumerate(ondisk.ReadTableBlock(raw)["rows"]):
            fid = link * FTABLE_PER_BLOCK + i
            if mb != 0 and fid != 0:
                out["rows"][fid] = (mb, mo)
        nxt = U32(raw, 4)
        if nxt == 0:
            return out
        if hops + 1 >= run:
            if not sealed:
                out["refused"] = "block %u links a chain that goes round in a circle" % block
            out["cut"] = True
            return out
        block = nxt
        hops += 1


# ---------------------------------------------------------------------------
# The files and the numbers
# ---------------------------------------------------------------------------


def MountFiles(files, rows, sealed):
    """MountWalkFiles / MountFile (mount.c): every id the map holds — a record,
    a row, or both — judged. ({fid: verdict}, counts, the highest id met). A
    verdict is {state, record, row, corrected, derived}, state one of
    'mounted', 'unsettled' (sealed, a row with no record), 'lost' (no seal, a
    row with no record) and 'unnamed' (no seal, a record no row names)."""
    out, top = {}, 0
    n = dict(checked=0, corrected=0, derived=0, unsettled=0, lost=0, unnamed=0)
    for fid in sorted(set(files) | set(rows)):
        top = max(top, fid)
        rec, row = files.get(fid), rows.get(fid)
        v = {"state": None, "record": rec, "row": row, "corrected": False, "derived": False}
        if rec is None:
            v["state"] = "unsettled" if sealed else "lost"
        elif row is None and not sealed:
            v["state"] = "unnamed"
        else:
            v["state"] = "mounted"
            v["derived"] = row is None
            v["corrected"] = row is not None and row != (rec.block, rec.at)
            n["checked"] += 1
            n["corrected"] += v["corrected"]
            n["derived"] += v["derived"]
        if v["state"] != "mounted":
            n[v["state"]] += 1
        out[fid] = v
    return out, n, top


def NextFileId(ledger_next, top_row, top_record):
    """MountNextFileId (mount.c): past every id a row or a record shows, never
    short of the Ledger's count; a wrapped count (0) stays."""
    nxt = min(max(top_row, top_record) + 1, 0xFFFFFFFF)
    return ledger_next if ledger_next == 0 or nxt <= ledger_next else nxt


def NextTagId(by_id, top_tag):
    """The registry's next number once the mount is read: past every id it
    adopted (RegistryEnter), and past every tag a record of the pool wears
    (TagRegistryNumbersFrom), never past the last id + 1. The Ledger's
    next_tag_id is read by nobody."""
    nxt = max(by_id) + 1 if by_id else 0
    return max(nxt, min(top_tag + 1, MAX_TAG_ID + 1))


# ---------------------------------------------------------------------------
# The whole read
# ---------------------------------------------------------------------------


class MountModel:
    """MountReadVolume on one image. `layout` is the Deed's (as the mount takes
    it: after a growth it would make), `copies` the two Ledger copies as
    ondisk.ReadLedgerCopy reads them — with `use_chain_read` where the census
    has read the chains they name — and `boot` the Deed's BOOT stamp or None.

    `refusal` is (step, sentence) of the first refusal in the kernel's order
    (ledger, registry, pool, table), or None. Everything else is filled as far
    as the image can be read, refused or not, for the census to report."""

    def __init__(self, read_block, layout, copies, boot=None):
        self.read_block, self.layout, self.copies = read_block, layout, copies
        self.run = D = layout["data_blocks"]
        self.refusal = None
        base = layout["data_block"]
        self.head = {k: (layout[k] - base) & 0xFFFFFFFF for k in
                     ("tag_registry_block", "file_table_block", "metadata_pool_block")}

        (self.newest, self.other), self.seals = Seals(copies)
        if self.newest is None:
            self.refusal = ("ledger", "[Ledger] neither copy is readable — this volume cannot "
                            "say what is on it")
        s0, s1 = self.seals

        self.registry = RegistryMount(read_block, self.head["tag_registry_block"], s0, s1, D)
        self.by_id, self.dropped = RegistryAdopt(self.registry["taken"])

        self.pool = PoolMount(read_block, self.head["metadata_pool_block"], self.seals, D)
        self.sealed = self.pool["stood"] != "none"
        at = self.pool["seal"]
        self.stood = (self.newest if at is None else self.newest if at == 0 else self.other)
        chain = self.pool["chain"]
        first = chain[0] if chain else self.head["metadata_pool_block"]
        self.prefix = (HeadPrefix(read_block, self.head["metadata_pool_block"], first, chain, D)
                       if not self.pool["refused"] else [])

        self.table = TableMount(read_block, self.head["file_table_block"], self.sealed, D)

        self.files, self.counts, top = MountFiles(self.pool["files"], self.table["rows"],
                                                  self.sealed)
        ledger_next = self.stood["next_file_id"] if self.stood else 0
        self.next_file_id = NextFileId(ledger_next, top, self.pool["top_fid"])
        self.next_tag_id = NextTagId(self.by_id, self.pool["top_tag"])

        if self.refusal is None:
            for step, sentence, why in (
                    ("registry", "its tag registry would not read", self.registry["refused"]),
                    ("pool", "its metadata pool would not read", self.pool["refused"]),
                    ("table", "its file table would not read", self.table["refused"])):
                if why:
                    self.refusal = (step, "%s — %s" % (sentence, why))
                    break
        self.claims = self._Ground(boot)

    def Refused(self):
        """{chain: why} for every chain the kernel's walk would refuse."""
        return {name: part["refused"] for name, part in (("registry", self.registry),
                                                         ("pool", self.pool),
                                                         ("table", self.table))
                if part["refused"]}

    def Mounted(self):
        """{fid: Record} of every file the mount takes up."""
        return {fid: v["record"] for fid, v in self.files.items() if v["state"] == "mounted"}

    def _Ground(self, boot):
        """MountCensus: every owner's claim, [(kind, label, runs, partner)] —
        the kinds GroundOwner names (src/include/ground_owner.h)."""
        D, lay, out = self.run, self.layout, []
        for i, b in enumerate(self.registry["blocks"]):
            out.append(("registry", "registry[%u]" % i, [(b, 1)], None))
        for i, b in enumerate(self.table["links"]):
            out.append(("table", "table[%u]" % i, [(b, 1)], None))
        for i, b in enumerate(self.pool["chain"]):
            out.append(("pool", "pool[%u]" % i, [(b, 1)], None))
        head = self.head["metadata_pool_block"]
        if head not in self.pool["chain"]:
            out.append(("pool", "pool head", [(head, 1)], None))
        for i, b in enumerate(self.prefix):
            out.append(("pool", "pool before its first[%u]" % i, [(b, 1)], None))
        for fid, rec in sorted(self.Mounted().items()):
            out.append(("file", Label(fid, rec), list(rec.extents_at_mount), None))
        led = self.stood or {}
        if led.get("integrity_map_block"):
            out.append(("integrity", "integrity map",
                        [(led["integrity_map_block"], led["integrity_map_blocks"])], None))
        if led.get("cow_manifest_block"):
            out.append(("manifest", "snapshot manifest", [(led["cow_manifest_block"], 1)], None))
        for first, blocks in UseChainsClaimed((self.newest, self.other)):
            out.append(("use_chain", "Use Context chain at data block %u" % first,
                        [(b, 1) for b in blocks], None))
        if boot and boot["kernel_blocks"] and boot["kernel_block"] >= lay["data_block"]:
            out.append(("boot", "BOOT", [(boot["kernel_block"] - lay["data_block"],
                                          boot["kernel_blocks"])], None))
        return out


def UseChainsClaimed(copies):
    """UseRollCall (use_memory_mount.c): the chain the memory stands on and
    every loose one — which together are every chain an intact copy names at a
    block, each block of it read back as the chain's own. `copies` is [newest,
    other]: the newest copy's chain is read first, and the other's only when it
    begins at another block (UseMemoryJudge). [(first, blocks)]."""
    out, seen = [], set()
    for c in copies:
        got = c.get("use_chain_read") if c else None
        if not got or not c["ok"]:
            continue
        first = c["use_chain"]["first"]
        if first not in seen:
            seen.add(first)
            out.append((first, got["blocks"]))
    return out


def Label(fid, rec):
    return "fid %u '%s'" % (fid, rec.name) if rec is not None else "fid %u" % fid


# ---------------------------------------------------------------------------
# Volume — list and get
# ---------------------------------------------------------------------------


class Volume:
    """A volume on a medium, for reading files out of it the way the mounted
    kernel would see them."""

    def __init__(self, medium, ground, deed):
        self.medium = medium
        self.ground = ground
        self.layout = deed["layout"]
        self.deed = deed

    def data_block(self, rel):
        """One 4 KiB block of the DATA RUN, counted from the run's start —
        block_to_vlba (tagfs.c)."""
        lba = self.ground + (self.layout["data_block"] + rel) * SECTORS_PER_BLOCK
        return self.medium.sectors(lba, SECTORS_PER_BLOCK)

    def volume_block(self, abs_b):
        """One 4 KiB block counted from the START of the volume."""
        lba = self.ground + abs_b * SECTORS_PER_BLOCK
        return self.medium.sectors(lba, SECTORS_PER_BLOCK)

    def read_block(self, rel):
        """data_block, answering None where the medium ends — a failed read."""
        try:
            return self.data_block(rel)
        except IOError:
            return None

    def ledgers(self):
        """Both copies of the Ledger, as read."""
        copies = []
        for c in range(LEDGER_COPIES):
            try:
                sector = self.volume_block(self.layout["state_block"] + c)[:SECTOR]
            except IOError:
                sector = b""
            copies.append(ondisk.ReadLedgerCopy(sector, c))
        return copies

    def ledger(self):
        """The newest intact Ledger copy, or None when neither reads."""
        return LedgerChoice(self.ledgers())

    def files(self):
        """Every file the mounted kernel would show, resolved the way its mount
        resolves them (MountModel): each at the newest record the pool's span
        holds for it — not at whatever place its row names — CRC-checked and
        ACTIVE, and not in the trash.

        A volume with no readable Ledger does not mount at all; its files are
        still listed — as a mount with no seal reads them — because this is the
        tool that gets evidence off a volume that will not mount."""
        model = MountModel(self.read_block, self.layout, self.ledgers())
        out = []
        for fid, r in sorted(model.Mounted().items()):
            if r.flags & FILE_TRASHED:
                continue
            out.append({"fid": fid, "name": r.name, "size": r.size, "flags": r.flags,
                        "extents": r.extents, "extents_at_mount": r.extents_at_mount})
        return out

    def contents(self, entry):
        """The file's bytes, following the extents the mount keeps and stopping
        at its size."""
        out = bytearray()
        for start, count in entry["extents_at_mount"]:
            for i in range(count):
                if len(out) >= entry["size"]:
                    break
                out += self.data_block(start + i)
            if len(out) >= entry["size"]:
                break
        return bytes(out[:entry["size"]])
