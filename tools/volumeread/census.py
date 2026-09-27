"""census — GroundCensus: every owner of every block, and what the mount makes of it.

Two roll calls over the same image, held against each other.

The CENSUS is the truth: every structure on the volume that owns a block of
the data run, found by reading the volume the way its writers laid it down,
set against the bitmap. A block with two owners, a block used with no owner
(an orphan) and a block owned and free (missing) are each named, as runs.

The KERNELVIEW is the mount: a step-by-step simulation of what tagfs_init
decides on this image — the refusals, the fsck's orphan and missing runs, the
table entries it corrects, the files it loses, the free count it derives, the
line it prints and the writes the mount itself makes. It models the kernel as
it is in this tree, and a change to the mount or its roll call has to change
this file in the same commit: logcheck's `census` scenario boots the image
and holds the kernel's own named runs against this model, so a drift is a red
line, not a surprise.

Where the two disagree — a block the kernel would call an orphan that the
census knows an owner for — that is a KERNEL_BLIND finding.
"""

import hashlib
import struct

from . import ondisk
from .ondisk import (SECTOR, BLOCK, SECTORS_PER_BLOCK, REGISTRY_MAGIC,
                     FILETBL_MAGIC, MPOOL_MAGIC, TUSE_MAGIC, COW_MANIFEST_MAGIC,
                     FILE_ACTIVE, MAX_TAG_ID, INTEGRITY_PER_BLOCK, MIRROR_SLACK,
                     LEDGER_COPIES, DISKBOOK_BLOCKS, TUSE_TEXT,
                     RunsFromBits, RunsNormal, RunsIntersect, RunsSubtract,
                     RunsCount, RunsText)
from .medium import VolumeFacts

SEVERITY = ("damage", "leak", "note")        # exit 1 on any damage or leak


class Finding:
    """One statement about the volume: a stable code, a severity, one sentence
    with the runtime numbers in it, and the blocks it is about as runs."""

    __slots__ = ("code", "severity", "text", "runs")

    def __init__(self, code, severity, text, runs=()):
        self.code, self.severity, self.text, self.runs = code, severity, text, list(runs)


class GroundClaim:
    """One owner's claim on the data run. An alias (the BOOT run, a redirect's
    live block) is not exclusive: it may coincide with its partner — the one
    file it is — and with nothing else."""

    __slots__ = ("kind", "label", "runs", "exclusive", "kernel_sees", "partner")

    def __init__(self, kind, label, runs, exclusive=True, partner=None):
        self.kind, self.label, self.runs = kind, label, RunsNormal(runs)
        self.exclusive, self.partner, self.kernel_sees = exclusive, partner, True

    def Allows(self, other):
        if self.partner is not None:
            return other.label == self.partner
        return other.kind == "file"


# ---------------------------------------------------------------------------
# Who owns what, piece by piece
# ---------------------------------------------------------------------------


def Sweep(claims):
    """[(start, count, [claims])]: the data run cut where the set of claims
    changes, keeping the pieces that somebody claims."""
    events = []
    for c in claims:
        for s, n in c.runs:
            events.append((s, 1, c))
            events.append((s + n, -1, c))
    events.sort(key=lambda e: (e[0], e[1]))
    active, segs, prev = {}, [], None
    for pos, delta, c in events:
        if prev is not None and pos > prev and active:
            segs.append((prev, pos - prev, list(active)))
        active[c] = active.get(c, 0) + delta
        if not active[c]:
            del active[c]
        prev = pos
    return segs


# ---------------------------------------------------------------------------
# KernelView — what tagfs_init decides on this image
# ---------------------------------------------------------------------------


class KernelView:
    """A pure simulation of the mount in this tree. It reads nothing the facts
    have not read, except the blocks the mount itself would read."""

    def __init__(self, facts):
        self.f = facts
        self.refusal = self.step = None
        self.fsck = None
        self.says = None
        self.writes = []                 # [(sentence, runs of data blocks written)]
        self.kept_line = None
        self.verdicts = {}               # fid -> ondisk.FileVerdict
        self.marked = []
        self.union = []
        self.current = None              # Ledger copy the mount last wrote or stands on
        self._Mount()

    def Refuse(self, step, sentence):
        self.step, self.refusal = step, sentence

    def _Mount(self):
        f, lay, D = self.f, self.f.layout, self.f.D
        st = f.stand
        if not st.kernels:
            return self.Refuse("ground", "%s, and this volume was found at %s" % (st.kernel_why, st.how))
        blocks_in_volume = f.deed["sectors"] // SECTORS_PER_BLOCK
        dl = self.f.deed["layout"]
        if (dl["data_block"] == 0 or dl["data_blocks"] == 0 or
                dl["data_block"] + dl["data_blocks"] > blocks_in_volume or
                dl["state_block"] + dl["state_blocks"] > blocks_in_volume or
                dl["state_blocks"] < LEDGER_COPIES):
            return self.Refuse("layout", "the deed lays out %u data blocks at %u inside a volume of "
                               "%u — that does not fit" % (dl["data_blocks"], dl["data_block"],
                                                          blocks_in_volume))
        block_bytes = f.geometry["block_bytes"] if f.geometry else 0
        if block_bytes != BLOCK:
            return self.Refuse("layout", "the volume was laid out in %u-byte blocks and this kernel "
                               "speaks %u" % (block_bytes, BLOCK))
        g = f.growth
        if g:
            self.writes.append(("the far copy at sector %u, the head deed, and zeros over "
                                "the old far copy at sector %u — the volume takes %u more "
                                "data blocks" % (g["new_tail"], g["old_tail"], g["more"]), []))
        if f.ledger is None:
            return self.Refuse("ledger", "[Ledger] neither copy is readable — this volume cannot say "
                               "what is on it")
        self.current = f.ledger["copy"]
        self.seq = f.ledger["seq"]
        if not f.book_ok:
            self.writes.append(("the DiskBook head and its copy, reformatted empty "
                                "(disk_book.c:146-151)", []))
        for name, sentence in (("registry", "its tag registry would not read"),
                               ("table", "its file table would not read"),
                               ("pool", "its metadata pool would not read")):
            if name in f.refused:
                return self.Refuse(name, "%s — %s" % (sentence, f.refused[name]))
        need = ((D + 7) // 8 + SECTOR - 1) // SECTOR
        if need > lay["block_bitmap_blocks"] * SECTORS_PER_BLOCK:
            return self.Refuse("layout", "its block bitmap region is too small to hold a bit for every "
                               "block of its data run")
        if len(f.region) < need * SECTOR:
            return self.Refuse("bitmap", "its block bitmap would not read, and a missing bitmap reads "
                               "as a volume with nothing on it")
        self._Fsck()
        self._Integrity()
        disagreed = sum(1 for _b, blk, _r in f.pool if blk["kept_counted"] != blk["kept"])
        if disagreed:
            self.kept_line = (disagreed, len(f.pool))

    def MarkChain(self, head, magic):
        """MARK_CHAIN_BLOCKS as it stands in tagfs.c's mount fsck: the head is
        marked and read, the chain is followed until a block will not read, does
        not carry the magic, or says next_block 0 — at most data_blocks hops.
        The head is walked even when it is data block 0, which the tag registry's
        always is; the kernel before this tree's fix never entered that loop
        (K1), and its registry continuations were orphans on every mount."""
        D, cb, marked = self.f.D, head, []
        for _ in range(D):
            if cb < D:
                marked.append((cb, 1))
            raw = self.f.read_block(cb)
            if raw is None:
                break
            magic_found, nxt = struct.unpack_from("<II", raw, 0)
            if magic_found != magic or nxt == 0:
                break
            cb = nxt
        return marked

    def _Fsck(self):
        """The mount's roll call (tagfs_init's fsck, after the ledger and the chains)."""
        f, lay, D = self.f, self.f.layout, self.f.D
        led = f.ledger
        mirror = ondisk.MirrorWalk([b for b, _blk, _r in f.pool], [r for _b, _blk, r in f.pool],
                                   led["next_file_id"] + MIRROR_SLACK + 1)
        marks = [(0, min(3, D))]
        corrected = lost = quits = 0
        for v in ondisk.KernelRollCall(f.read_block, led["next_file_id"], f.rows, mirror):
            self.verdicts[v.fid] = v
            corrected += v.corrected
            if v.outcome == "lost":
                lost += 1
            elif v.outcome == "quitclaim":
                quits += 1
            else:
                marks += RunsIntersect(RunsNormal(v.record.extents_at_mount), [(0, D)])
        marks += self.MarkChain(f.Head(lay["tag_registry_block"]), REGISTRY_MAGIC)
        marks += self.MarkChain(f.Head(lay["file_table_block"]), FILETBL_MAGIC)
        marks += self.MarkChain(f.Head(lay["metadata_pool_block"]), MPOOL_MAGIC)
        first, count = led["integrity_map_block"], led["integrity_map_blocks"]
        if first:
            marks += RunsIntersect([(first, count)], [(0, D)])
        for b in (led["cow_manifest_block"], led["cow_manifest_backup_block"]):
            if b and b < D:
                marks.append((b, 1))
        for _snap, old, new in f.redirects:
            marks += [(b, 1) for b in (old, new) if b < D]
        self.marked = RunsNormal(marks)
        orphans = RunsSubtract(f.used, self.marked)
        missing = RunsSubtract(self.marked, f.used)
        self.union = RunsNormal(f.used + self.marked)
        self.free = D - RunsCount(self.union)
        self.fsck = {"orphan": RunsCount(orphans), "missing": RunsCount(missing),
                     "corrected": corrected, "lost": lost, "quitclaims": quits,
                     "orphan_runs": orphans, "missing_runs": missing}
        self.says = (D, self.free, led["total_files"])
        if self.free != led["free_blocks"]:
            self._LedgerWrite("the ledger's free count, %u where the bitmap says %u"
                              % (led["free_blocks"], self.free))

    def _LedgerWrite(self, why):
        """ledger_write_locked (tagfs.c): the older copy, seq + 1."""
        self.current = (self.current + 1) % LEDGER_COPIES
        self.seq += 1
        self.writes.append(("ledger copy %u at seq %u (%s)" % (self.current, self.seq, why), []))

    def _Alloc(self, count):
        """tagfs_alloc_blocks_internal (tagfs.c): first fit in the free extents,
        ascending, built from the bitmap the roll call left."""
        for s, n in RunsSubtract([(0, self.f.D)], self.union):
            if n >= count:
                self.union = RunsNormal(self.union + [(s, count)])
                return s
        return None

    def _Integrity(self):
        """IntegrityInit (integrity.c:87-258): the first mount lays a map down;
        a volume that grew past its map moves it."""
        led, D = self.f.ledger, self.f.D
        need = (D + INTEGRITY_PER_BLOCK - 1) // INTEGRITY_PER_BLOCK
        first, stored = led["integrity_map_block"], led["integrity_map_blocks"]
        if first != 0 and not (stored != 0 and stored < need):
            return
        start = self._Alloc(need)
        if start is None or start == 0:
            self.writes.append(("nothing for an integrity map — no free run of %u blocks" % need, []))
            return
        # MapOwedMark/MapOwedSettle (tagfs.c): every bitmap sector the run's
        # bits live in is written before the map is.
        lo, hi = (start // 8) // SECTOR, ((start + need - 1) // 8) // SECTOR
        what = "bitmap sector %u" % lo if lo == hi else "bitmap sectors %u..%u" % (lo, hi)
        if first == 0:
            self.writes.append(("%s, a %u-block integrity map at data block %u"
                                % (what, need, start), [(start, need)]))
            self._LedgerWrite("naming the map")
        else:
            self.writes.append(("%s, a %u-block integrity map at data block %u carrying the "
                                "%u blocks at %u" % (what, need, start, stored, first),
                                [(start, need)]))
            self._LedgerWrite("naming the moved map")

    def Json(self):
        fsck = None
        if self.fsck is not None:
            fsck = dict(self.fsck)
            fsck["orphan_runs"] = [list(r) for r in self.fsck["orphan_runs"]]
            fsck["missing_runs"] = [list(r) for r in self.fsck["missing_runs"]]
        g = self.f.growth
        return {"mounts": self.refusal is None, "refusal": self.refusal, "fsck": fsck,
                "says": (None if self.says is None else
                         {"data_blocks": self.says[0], "free": self.says[1], "files": self.says[2]}),
                "writes_at_mount": [w for w, _r in self.writes],
                "kept_line": (None if self.kept_line is None else
                              {"disagreed": self.kept_line[0], "blocks": self.kept_line[1]}),
                "grows": None if g is None else {k: g[k] for k in sorted(g)}}


# ---------------------------------------------------------------------------
# GroundCensus — the truth, and the findings
# ---------------------------------------------------------------------------


def _Label(fid, rec):
    return "fid %u '%s'" % (fid, rec.name) if rec is not None else "fid %u" % fid


class GroundCensus:
    """Every owner of every block of the data run, against the bitmap and
    against the KernelView of the same image."""

    def __init__(self, medium, forced_start=None):
        self.f = f = VolumeFacts(medium, forced_start)
        self.medium = medium
        self.findings, self.claims, self.files, self.by_id = [], [], {}, {}
        self.kernel = KernelView(f)
        self.swept, self.boot_file = False, None
        self._Deed()
        if self.kernel.step == "layout" or not self._Regions():
            return
        self._Ledger()
        self._DiskBook()
        self._Cow()
        self._Chains()
        self._Registry()
        self._Records()
        self._Files()
        self._Boot()
        if f.ledger is not None and self.kernel.step != "bitmap":
            self._Sweep()
        self._Drift()

    def Say(self, code, severity, text, runs=()):
        self.findings.append(Finding(code, severity, text, runs))

    def Claim(self, kind, label, runs, exclusive=True, partner=None):
        c = GroundClaim(kind, label, RunsIntersect(RunsNormal(runs), [(0, self.f.D)]),
                        exclusive, partner)
        c.kernel_sees = not RunsSubtract(c.runs, self.kernel.marked)
        self.claims.append(c)
        return c

    # --- the ground ------------------------------------------------------

    def _Deed(self):
        st, g = self.f.stand, self.f.growth
        if st.stood_on == "tail":
            self.Say("DEED_HEAD_BAD", "damage",
                     "the deed at the head of the ground at sector %u does not answer (%s) — "
                     "the kernel stands on the far copy at sector %u (tagfs.c volume_take_ground)"
                     % (st.start, st.head_why, st.start + st.deed["at_sector"]))
        elif st.tail is None and st.far_end is None:
            self.Say("DEED_TAIL_BAD", "damage",
                     "the far copy the head names at sector %u: %s — the kernel mounts on one "
                     "deed (tagfs.c volume_take_ground)" % (st.deed["tail_sector"], st.tail_why))
        if st.far_end is not None:
            self.Say("GROWTH_INTERRUPTED", "note",
                     "a valid far copy of this volume at sector %u claims %u sectors against "
                     "the head's %u — a growth caught between its two writes (deed.c:177-198); "
                     "the head is what the machine stands on" % (
                         st.far_end["at_sector"], st.far_end["sectors"], st.deed["sectors"]))
        if g:
            self.Say("GROWTH_PENDING", "note",
                     "the kernel takes the ground behind this volume at its next mount: %u "
                     "sectors to %u, %u more data blocks%s (tagfs.c volume_take_more_ground)" % (
                         g["from_sectors"], g["to_sectors"], g["more"],
                         ", clamped by what its bitmap can account for" if g["clamped"] else ""))
        # The kernel's refusals that no later section would name: its ground,
        # its layout and its bitmap. The ledger's and the chains' are named
        # where those are read (LEDGER_NONE, CHAIN_REFUSED).
        step, refusal = self.kernel.step, self.kernel.refusal
        if step == "ground":
            self.Say("GROUND_REFUSED", "damage", "%s — the kernel never mounts it (ground.c "
                     "GroundSurvey, tagfs.c volume_take_ground)" % refusal)
        elif step == "layout":
            self.Say("LAYOUT_UNFIT", "damage", "%s — the kernel refuses the mount (tagfs.c "
                     "volume_take_ground, tagfs_init's bitmap region check)" % refusal)
        elif step == "bitmap":
            self.Say("BITMAP_UNREADABLE", "damage", "%s — the kernel refuses the mount (tagfs_init's "
                     "bitmap read), and the ownership sweep is not run: an unread bitmap is not an "
                     "empty one" % refusal)

    def Regions(self):
        """The Deed's regions as the kernel uses them, in volume blocks: the
        DiskBook is as long as the 130 blocks it writes, whatever it states."""
        lay, deed = self.f.deed["layout"], self.f.deed
        return [("deed", 0, 1), ("ledger", lay["state_block"], lay["state_blocks"]),
                ("diskbook", lay["disk_book_block"], max(lay["disk_book_blocks"], DISKBOOK_BLOCKS)),
                ("bitmap", lay["block_bitmap_block"], lay["block_bitmap_blocks"]),
                ("data", lay["data_block"], lay["data_blocks"]),
                ("far copy", deed["tail_sector"] // SECTORS_PER_BLOCK, 1)]

    def _Regions(self):
        lay, deed, regions, bad = self.f.deed["layout"], self.f.deed, self.Regions(), []
        for name, first, count in regions:
            if first + count > lay["total_blocks"]:
                bad.append("%s %u+%u runs past the volume's %u blocks"
                           % (name, first, count, lay["total_blocks"]))
        for i, (n1, a1, c1) in enumerate(regions):
            for n2, a2, c2 in regions[i + 1:]:
                if a1 < a2 + c2 and a2 < a1 + c1:
                    bad.append("%s %u+%u and %s %u+%u overlap" % (n1, a1, c1, n2, a2, c2))
        if deed["tail_sector"] % SECTORS_PER_BLOCK or \
                deed["tail_sector"] // SECTORS_PER_BLOCK != lay["total_blocks"] - 1:
            bad.append("the far copy at sector %u is not the volume's last block (%u)"
                       % (deed["tail_sector"], lay["total_blocks"] - 1))
        if lay["total_blocks"] * SECTORS_PER_BLOCK > deed["sectors"]:
            bad.append("%u blocks do not fit the %u sectors the deed claims"
                       % (lay["total_blocks"], deed["sectors"]))
        for b in bad:
            self.Say("REGION_OVERLAP", "damage", "the deed's layout: " + b)
        return not bad

    def _Ledger(self):
        f = self.f
        bad = [c for c in f.ledgers if not c["ok"]]
        if len(bad) == LEDGER_COPIES:
            self.Say("LEDGER_NONE", "damage", "neither ledger copy is readable — copy 0 %s, copy 1 "
                     "%s — and the kernel refuses the mount (tagfs.c ledger_load); the ownership "
                     "sweep is not run, because the ledger names blocks nothing else can"
                     % (bad[0]["why"], bad[1]["why"]))
        elif bad:
            self.Say("LEDGER_TORN", "note", "ledger copy %u %s — the kernel runs on copy %u "
                     "(seq %u) alone (tagfs.c ledger_load)" % (bad[0]["copy"], bad[0]["why"],
                                                            f.ledger["copy"], f.ledger["seq"]))
        for c in f.ledgers:
            if c["ok"] and c["use_misplaced"]:
                self.Say("LEDGER_USE_MISPLACED", "note", "ledger copy %u places its Use Context at "
                         "%u for %u bytes, outside its own record — it remembers none "
                         "(tagfs.c ledger_read_copy)" % (c["copy"], c["use_context_offset"],
                                                  c["use_context_bytes"]))

    def _DiskBook(self):
        f = self.f
        if not f.book_ok:
            why = ("its head and the copy both fail their checksum" if f.book is None else
                   "its head reads version %u, capacity %u, count %u, records at sector %u"
                   % (f.book["version"], f.book["capacity"], f.book["count"], f.book["start_sector"]))
            self.Say("DISKBOOK_INVALID", "damage" if f.snapshots else "note",
                     "the DiskBook is not one the mount accepts — %s — so it is reformatted empty "
                     "at mount and no redirect is replayed (disk_book.c:139-153)%s"
                     % (why, "; the manifest names %u snapshot(s) whose frozen blocks are then "
                        "forgotten" % len(f.snapshots) if f.snapshots else ""))
        bad = [i for i, e in enumerate(f.book_records) if not e["valid"]]
        if bad:
            self.Say("DISKBOOK_RECORD_BAD", "note", "DiskBook record(s) %s fail their magic or "
                     "checksum and are skipped at replay (disk_book.c:175-182)"
                     % ", ".join(map(str, bad)))

    def _Cow(self):
        f, led = self.f, self.f.ledger
        for which, b, m in f.manifests:
            label = "snapshot manifest" + (" backup" if which == "cow_manifest_backup_block" else "")
            if b >= f.D or m is None or not m["ok"]:
                self.Say("MANIFEST_BAD", "damage", "the %s the ledger names at data block %u %s "
                         "(tagfs_init's manifest check)" % (label, b, "lies past the data run" if b >= f.D
                                                  else "will not read" if m is None else
                                                  "reads magic 0x%08x, count %u"
                                                  % (m["magic"], m["count"])), [(b, 1)])
            self.Claim("manifest" if "backup" not in label else "manifest_backup", label, [(b, 1)])
        for snap, old, new in f.redirects:
            self.Claim("frozen", "frozen block of snapshot %u" % snap, [(old, 1)])
            self.Claim("redirect_new", "live block of snapshot %u's redirect" % snap, [(new, 1)],
                       exclusive=False)
        if f.stray:
            self.Say("REDIRECT_STRAY", "note", "%u DiskBook redirect(s) name snapshots the manifest "
                     "does not carry (%s) — the kernel ignores them (cow.c:593-602)"
                     % (len(f.stray), ", ".join(str(s[0]) for s in f.stray)))
        if led and led["integrity_map_block"]:
            self.Claim("integrity", "integrity map",
                       [(led["integrity_map_block"], led["integrity_map_blocks"])])

    # --- the chains, the registry and the records -------------------------

    def _Chains(self):
        f = self.f
        region_bits = len(f.region) * 8
        past = RunsIntersect(RunsFromBits(f.region, region_bits), [(f.D, max(0, region_bits - f.D))])
        if past:
            self.Say("BITMAP_PAST_RUN", "damage", "%u bit(s) are set past the data run's %u blocks: "
                     "%s — ground the volume would take on growth is not blank (tagfs.c "
                     "volume_take_more_ground)"
                     % (RunsCount(past), f.D, RunsText(past)))
        for name in ("registry", "table", "pool"):
            for i, b in enumerate(f.chains[name][0]):
                self.Claim(name, "%s[%u]" % (name, i), [(b, 1)])
            if name in f.refused:
                self.Say("CHAIN_REFUSED", "damage", "the %s: %s — the kernel refuses the mount: %s"
                         % (name, f.refused[name], self.kernel.refusal or "(an earlier refusal)"))
        drift = [(b, blk["kept"], blk["kept_counted"]) for b, blk, _r in f.pool
                 if blk["kept"] != blk["kept_counted"]]
        if drift:
            self.Say("POOL_KEPT_DRIFT", "note", "%u of %u pool block(s) state a kept count the records "
                     "do not bear out (%s) — the mount says so and counts (meta_pool.c:395-400)"
                     % (len(drift), len(f.pool), ", ".join("block %u says %u, holds %u" % d
                                                           for d in drift)))
        for b, blk, _r in f.pool:
            if blk["walk_stop"] is not None:
                at, ln = blk["walk_stop"]
                self.Say("RECORD_SHAPE", "damage", "pool block %u: the walk stops at byte %u, where a "
                         "record says it is %u long, with %u used bytes stated — the records behind "
                         "it are never read (meta_pool.c:764-767)" % (b, at, ln, blk["used_bytes"]))

    def _Registry(self):
        """intern_with_id's rules (tag_registry.c:171-218) over the chain."""
        by_kv, dup, rng = {}, [], []
        for b, blk in self.f.registry:
            for r in blk["records"]:
                where, kv = "block %u +%u" % (b, r["at"]), (r["key"], r["value"])
                if r["skipped"]:
                    rng.append("id %u at %s carries a %u-byte value the loader steps over"
                               % (r["tag_id"], where, r["value_len"]))
                elif kv in by_kv:
                    if by_kv[kv] != r["tag_id"]:
                        dup.append("id %u at %s repeats id %u's key/value and is dropped"
                                   % (r["tag_id"], where, by_kv[kv]))
                elif r["tag_id"] > MAX_TAG_ID:
                    rng.append("id %u at %s is past %u and is dropped" % (r["tag_id"], where, MAX_TAG_ID))
                elif r["tag_id"] in self.by_id:
                    dup.append("id %u at %s collides with an earlier record of that id and is "
                               "dropped" % (r["tag_id"], where))
                else:
                    self.by_id[r["tag_id"]], by_kv[kv] = kv, r["tag_id"]
        if dup:
            self.Say("TAG_DUPLICATE", "damage", "tag registry: " + "; ".join(dup))
        if rng:
            self.Say("TAG_ID_RANGE", "damage", "tag registry: " + "; ".join(rng))

    def _Records(self):
        bad, shape = [], []
        for r in self.f.records:
            # A retired record has its file_id zeroed with its CRC left as it
            # was (meta_pool.c:225-234): a fid-0 record fails its sum by
            # construction, and is not damage.
            if r.fid != 0 and not r.unpack_ok:
                bad.append("fid %u at block %u +%u" % (r.fid, r.block, r.at))
            elif r.fid != 0 and (not r.shape_ok or (r.extents and not r.extents_at_mount)):
                shape.append("fid %u at block %u +%u (%s)" % (
                    r.fid, r.block, r.at, "length %u against a shape of %u" % (
                        r.len, 42 + 2 * r.tag_count + 6 * r.extent_count + r.name_len)
                    if not r.shape_ok else "an extent the mount drops, and with it every extent"))
        if bad:
            self.Say("RECORD_CRC", "damage", "%u pool record(s) fail their CRC16: %s — the mount "
                     "never reads them (meta_pool.c:569-580)" % (len(bad), ", ".join(bad)))
        if shape:
            self.Say("RECORD_SHAPE", "damage", "record(s) unpack with the wrong shape: " + ", ".join(shape))

    # --- files -------------------------------------------------------------

    def _File(self, fid, state, rec, row):
        self.files[fid] = {"fid": fid, "state": state, "record": rec, "row": row}

    def _Owns(self, fid, rec):
        runs = RunsNormal(rec.extents_at_mount)
        outside = RunsSubtract(runs, [(0, self.f.D)])
        if outside:
            self.Say("EXTENT_OUTSIDE", "damage", "%s names block(s) %s past the data run's %u "
                     "blocks — the mount keeps them and every later read drops the whole list "
                     "(K4, meta_pool.c:625-641)" % (_Label(fid, rec), RunsText(outside), self.f.D))
        for tag in rec.tags:
            if tag not in self.by_id:
                self.absent.setdefault(tag, []).append(_Label(fid, rec))
        self.Claim("file", _Label(fid, rec), runs)

    def _AtRow(self, fid, row):
        """A named file with no record in the pool's walk: what its row's place
        holds. Returns the record to go on with, or None when it is judged."""
        f = self.f
        if row[1] + ondisk.RECORD_HEADER > BLOCK:
            self.Say("ROW_OFFSET_OVERRUN", "damage", "fid %u is named at block %u offset %u, past "
                     "where a record header fits — the kernel unpacks it off the end of a 4 KiB "
                     "stack buffer (K2, meta_pool.c:1047-1052; step 1.4)" % (fid, row[0], row[1]))
            return self._File(fid, "offset_overrun", None, row)
        raw = f.read_block(row[0])
        rec = ondisk.ParseRecord(raw, row[1], row[0]) if raw is not None else None
        if rec is not None and rec.unpack_ok and rec.fid == fid:
            self.Say("ROW_OFF_CHAIN", "note", "%s is named at block %u+%u, %s — its record reads "
                     "there and nowhere the mirror walks (meta_pool.c:1160-1166)"
                     % (_Label(fid, rec), row[0], row[1],
                        "on the pool chain at an offset its walk never reaches"
                        if row[0] in f.chains["pool"][0] else "a block not on the pool chain"))
            return rec
        if rec is not None and rec.unpack_ok:
            self.Say("ROW_NAMES_OTHER", "damage", "fid %u is named at block %u+%u and the record "
                     "there is fid %u '%s'" % (fid, row[0], row[1], rec.fid, rec.name))
            return self._File(fid, "names_other", None, row)
        self.Say("FILE_LOST", "damage", "fid %u is named at block %u+%u and there is no readable "
                 "record for it there or anywhere in the pool — the mount deletes its name "
                 "(tagfs_init's roll call)" % (fid, row[0], row[1]))
        return self._File(fid, "lost", None, row)

    def _Files(self):
        f, self.absent = self.f, {}
        next_fid = f.ledger["next_file_id"] if f.ledger else None
        for fid in sorted(set(f.rows) | set(f.newest)):
            row, rec = f.rows.get(fid), f.newest.get(fid)
            if row is None:
                if rec.flags & FILE_ACTIVE:
                    self._File(fid, "unnamed", rec, None)
                continue
            past = next_fid is not None and fid >= next_fid
            if rec is None:
                rec = self._AtRow(fid, row)
                if rec is None:
                    continue
            elif (rec.block, rec.at) != row and not past:
                self.Say("TABLE_CORRECTED", "note", "%s is named at block %u+%u and its newest "
                         "record is at block %u+%u — the mount corrects the table to it "
                         "(tagfs_init's roll call)" % (_Label(fid, rec), row[0], row[1], rec.block, rec.at))
            if past:
                self.Say("FID_PAST_NEXT", "damage", "%s has a table row and the ledger's "
                         "next_file_id is %u — the roll call never visits it (tagfs_init) and "
                         "the next create is issued its id and overwrites the row "
                         "(file_table.c:203-227)" % (_Label(fid, rec), next_fid))
                self._File(fid, "past_next", rec, row)
                if rec.flags & FILE_ACTIVE:
                    self._Owns(fid, rec)
            elif rec.flags & FILE_ACTIVE:
                self._File(fid, "live", rec, row)
                self._Owns(fid, rec)
            else:
                self.Say("QUITCLAIM_PENDING", "note", "%s is named and its newest record is not "
                         "ACTIVE — the mount deletes the name and frees its blocks without a word "
                         "(tagfs_init's roll call)" % _Label(fid, rec))
                self._File(fid, "quitclaim", rec, row)
        for tag, holders in sorted(self.absent.items()):
            self.Say("TAG_ABSENT", "damage", "tag id %u is carried by %u file(s) and the registry does "
                     "not define it: %s" % (tag, len(holders), ", ".join(holders)))

    def _Boot(self):
        f, boot = self.f, self.f.deed["boot"]
        if boot is None or f.stand.stood_on != "head":
            return
        start = boot["kernel_block"] - f.layout["data_block"]
        run = [(start, boot["kernel_blocks"])]
        want = max(1, (boot["kernel_bytes"] + BLOCK - 1) // BLOCK)
        holders = [e for e in self.files.values() if e["state"] == "live" and
                   e["record"].size == boot["kernel_bytes"] and
                   not RunsSubtract(run, RunsNormal(e["record"].extents_at_mount))]
        why = None
        if boot["kernel_block"] < f.layout["data_block"] or start + boot["kernel_blocks"] > f.D:
            why = "it lies outside the data run"
        elif boot["kernel_blocks"] != want:
            why = "%u blocks is not how mkfs lays down %u bytes (%u)" % (
                boot["kernel_blocks"], boot["kernel_bytes"], want)
        elif len(holders) != 1:
            why = "%u live file(s) of %u bytes cover it" % (len(holders), boot["kernel_bytes"])
        if why:
            self.Say("BOOT_MISMATCH", "damage", "the head deed's BOOT run, volume block %u for %u "
                     "blocks and %u bytes, is not the kernel file: %s — the loaders read it "
                     "(volume_deed.h:216-235, create_tagfs.c:1503-1518)"
                     % (boot["kernel_block"], boot["kernel_blocks"], boot["kernel_bytes"], why))
            return
        self.boot_file = _Label(holders[0]["fid"], holders[0]["record"])
        self.Claim("boot", "BOOT", run, exclusive=False, partner=self.boot_file)

    # --- the sweep -----------------------------------------------------------

    def _Sweep(self):
        f = self.f
        self.swept = True
        segs = Sweep(self.claims)
        doubles, missing = {}, {}
        free = RunsSubtract([(0, f.D)], f.used)
        for s, n, cs in segs:
            excl = [c for c in cs if c.exclusive]
            bad = len(excl) >= 2 or any(not a.Allows(o) for a in cs if not a.exclusive for o in excl)
            if bad:
                doubles.setdefault(tuple(sorted(c.label for c in cs)), []).append((s, n))
            for r in RunsIntersect([(s, n)], free):
                missing.setdefault(tuple(sorted(c.label for c in cs)), []).append(r)
        for labels, runs in doubles.items():
            self.Say("DOUBLE_OWNER", "damage", "data block(s) %s are claimed by %s at once"
                     % (RunsText(RunsNormal(runs)), " and ".join(labels)), RunsNormal(runs))
        for labels, runs in missing.items():
            runs = RunsNormal(runs)
            kept = RunsIntersect(runs, self.kernel.marked)
            if kept:
                self.Say("MISSING", "note", "data block(s) %s of %s read free in the bitmap — the "
                         "kernel's union repair keeps them, in memory, at every mount "
                         "(tagfs_init's union repair)" % (RunsText(kept), " and ".join(labels)), kept)
            lost = RunsSubtract(runs, self.kernel.marked)
            if lost:
                self.Say("MISSING", "damage", "data block(s) %s of %s read free in the bitmap and %s"
                         % (RunsText(lost), " and ".join(labels),
                            "the kernel's roll call does not reach them — the allocator hands them out"
                            if self.kernel.refusal is None else
                            "the kernel, which refuses this mount, repairs nothing"), lost)
        self.claimed = RunsNormal([(s, n) for s, n, _c in segs])
        self.orphans = RunsSubtract(f.used, self.claimed)
        self._Orphans()
        for fid, e in sorted(self.files.items()):
            if e["state"] != "unnamed":
                continue
            ext = RunsIntersect(RunsNormal(e["record"].extents_at_mount), [(0, f.D)])
            leak = RunsIntersect(RunsSubtract(ext, self.claimed), f.used)
            taken = RunsIntersect(ext, self.claimed)
            self.Say("UNNAMED_RECORD", "leak" if leak else "note",
                     "%s is an ACTIVE newest record no table row names — %s"
                     % (_Label(fid, e["record"]),
                        "its blocks %s are marked used and nothing owns them: an interrupted "
                        "create, or a delete whose retire never landed (tagfs.c "
                        "tagfs_delete_file_inside, tagfs_sync_inside)"
                        % RunsText(leak) if leak else
                        "stale: its blocks %s" % ("went to others" if taken else "are free")), leak)
        if self.kernel.fsck is not None:
            for c in self.claims:
                blind = RunsIntersect(c.runs, self.kernel.fsck["orphan_runs"])
                if blind:
                    self.Say("KERNEL_BLIND", "note", "%s at data block(s) %s — the kernel's roll "
                             "call does not reach it and calls it an orphan" % (c.label, RunsText(blind)),
                             blind)

    def _Explanations(self):
        """What each orphan block might be the extent of: {block: text}, the
        most telling explanation winning."""
        f, out = self.f, {}
        state = {e["record"]: e["state"] for e in self.files.values() if e["record"] is not None}
        for r in f.records:
            if state.get(r) in ("live", "past_next"):
                continue                         # an owner, not an explanation
            if state.get(r) == "unnamed":
                rank, text = 0, "extents of unnamed record %s" % _Label(r.fid, r)
            elif state.get(r) == "quitclaim":
                rank, text = 1, "extents of %s, whose newest record is not ACTIVE" % _Label(r.fid, r)
            elif r.fid and r.unpack_ok:
                rank, text = 1, "extents of a superseded record of %s" % _Label(r.fid, r)
            elif r.fid:
                rank, text = 2, "extents named by a record of fid %u that fails its checksum" % r.fid
            else:
                rank, text = 3, "extents of a retired record '%s'" % r.name
            for s, n in RunsIntersect(RunsNormal(r.extents), self.orphans):
                for b in range(s, s + n):
                    if b not in out or out[b][0] > rank:
                        out[b] = (rank, text)
        return {b: t for b, (_r, t) in out.items()}

    def _Orphans(self):
        f, explained, groups, links = self.f, self._Explanations(), {}, []
        for s, n in self.orphans:
            for b in range(s, s + n):
                raw = f.vread(f.layout["data_block"] + b)
                magic = struct.unpack_from("<I", raw, 0)[0] if raw else None
                spare = ondisk.SpareShape(raw) if raw else None
                if magic == TUSE_MAGIC:
                    links.append((b, ondisk.ReadUseChainLink(raw)))
                    continue
                if spare:
                    code, text = "SPARE_UNNAMED", "a spare %s header linked from nowhere" % spare
                elif magic == MPOOL_MAGIC:
                    alive = any(fid for _a, _l, fid in ondisk.ReadPoolBlock(raw)["mirror_walk"])
                    text = ("a pool block linked from nowhere (a severed tail, or the "
                            "stamp-then-link window meta_pool.c:922-937)" if alive else
                            "a retired pool block whose unlink the bitmap had not caught up with "
                            "(meta_pool.c:1095-1136)")
                    code = "ORPHAN"
                elif magic in (FILETBL_MAGIC, REGISTRY_MAGIC):
                    code, text = "ORPHAN", "a %s block linked from nowhere" % (
                        "table" if magic == FILETBL_MAGIC else "registry")
                elif magic == COW_MANIFEST_MAGIC:
                    code, text = "ORPHAN", "a snapshot manifest the ledger does not name"
                else:
                    code, text = "ORPHAN", explained.get(b, "no recognisable content")
                groups.setdefault((code, text), []).append((b, 1))
        for (code, text), runs in groups.items():
            runs = RunsNormal(runs)
            self.Say(code, "leak", "data block(s) %s are used in the bitmap and nothing owns them: %s"
                     % (RunsText(runs), text), runs)
        self._UseChains(links)

    def _UseChains(self, links):
        """TUSE chains among the orphans (Quill design §1.2): from each link of
        index 0, exactly n_links hops, every link agreeing with the first."""
        f, by_block, whole = self.f, dict(links), set()
        for b, head in links:
            if head["index"] != 0:
                continue
            chain, cur, crc, total = [], b, 0, 0
            for i in range(head["n_links"]):
                link = by_block.get(cur)
                if (link is None or not 1 <= cur < f.D or link["index"] != i or
                        any(link[k] != head[k] for k in ("seq", "attempt", "n_links",
                                                          "total_bytes", "text_crc32")) or
                        link["used"] > TUSE_TEXT or
                        (i < head["n_links"] - 1 and link["used"] != TUSE_TEXT)):
                    chain = None
                    break
                chain.append(cur)
                crc, total = ondisk.Crc32(link["text"], crc), total + link["used"]
                cur = link["next"]
            if chain and cur == 0 and total == head["total_bytes"] and crc == head["text_crc32"]:
                whole.update(chain)
                runs = RunsNormal([(x, 1) for x in chain])
                self.Say("TUSE_UNNAMED", "leak", "data block(s) %s hold a whole TUSE chain nothing "
                         "names: %u links, %u bytes, crc ok" % (RunsText(runs), len(chain), total), runs)
        broken = RunsNormal([(b, 1) for b, _l in links if b not in whole])
        if broken:
            self.Say("TUSE_BROKEN", "leak", "data block(s) %s hold TUSE links that make no whole "
                     "chain" % RunsText(broken), broken)

    def _Drift(self):
        f, k, led = self.f, self.kernel, self.f.ledger
        if led is None:
            return
        derived = k.free if k.fsck is not None else f.D - RunsCount(f.used)
        if derived != led["free_blocks"]:
            self.Say("LEDGER_FREE_DRIFT", "note", "the ledger says %u free and the bitmap derives %u "
                     "— the mount writes the ledger back (tagfs_init's free-count rewrite)"
                     % (led["free_blocks"], derived))
        live = sum(1 for e in self.files.values() if e["state"] == "live")
        if led["total_files"] != live:
            self.Say("LEDGER_FILES_DRIFT", "note", "the ledger says %u files and %u are live"
                     % (led["total_files"], live))
        if k.writes:
            self.Say("MOUNT_WRITES", "note", "its next mount writes " +
                     " and ".join(w for w, _r in k.writes),
                     RunsNormal([r for _w, runs in k.writes for r in runs]))

    # --- the answer ------------------------------------------------------------

    def Counts(self):
        return {s: sum(1 for x in self.findings if x.severity == s) for s in SEVERITY}

    def Verdict(self):
        c = self.Counts()
        return "inconsistent" if c["damage"] or c["leak"] else "consistent"

    def ExitCode(self):
        return 0 if self.Verdict() == "consistent" else 1

    def Sorted(self):
        return sorted(self.findings, key=lambda x: SEVERITY.index(x.severity))

    def FileRecord(self, fid):
        """The record a file is read through: the mount's, when it sees it."""
        v = self.kernel.verdicts.get(fid)
        if v is not None and v.record is not None:
            return v.record
        return self.files[fid]["record"] if fid in self.files else None

    def Sha256(self, rec):
        """sha256 of a file's first `size` bytes read through the extents the
        mount keeps; None when the medium ends inside them."""
        h, left, f = hashlib.sha256(), rec.size, self.f
        for s, n in rec.extents_at_mount:
            for b in range(s, s + n):
                if left <= 0:
                    return h.hexdigest()
                raw = f.vread(f.layout["data_block"] + b)
                if raw is None:
                    return None
                h.update(raw[:min(BLOCK, left)])
                left -= BLOCK
        return h.hexdigest()

    def HumanLines(self):
        f, k, st, out = self.f, self.kernel, self.f.stand, []
        out.append("ground sector %u (%s), volume %s" % (st.start, st.how, st.deed["uuid"].hex()))
        if st.stood_on == "head" and st.tail is not None:
            out.append("the deed at the head and its far copy agree — %u sectors, far copy at %u"
                       % (st.deed["sectors"], st.deed["tail_sector"]))
        if f.growth_why:
            out.append(f.growth_why)
        good = [c for c in f.ledgers if c["ok"]]
        if len(good) == LEDGER_COPIES:
            seqs = [c["seq"] for c in good]
            out.append("ledger copies 0 and 1 are intact, %s — the kernel stands on copy %u"
                       % ("both at seq %u" % seqs[0] if seqs[0] == seqs[1] else
                          "at seq %u and %u" % tuple(seqs), f.ledger["copy"]))
        if self.swept:
            used = RunsCount(f.used)
            out.append("data run %u blocks at volume block %u — %u used, %u free, the ledger says %u"
                       % (f.D, f.layout["data_block"], used, f.D - used, f.ledger["free_blocks"]))
            live = [e for e in self.files.values() if e["state"] == "live"]
            pool = f.chains["pool"][0]
            out.append("owners — registry %u block(s), table %u, pool %u (%s), %u files in %u blocks%s"
                       % (len(f.chains["registry"][0]), len(f.chains["table"][0]), len(pool),
                          ", ".join(map(str, pool)), len(live),
                          sum(RunsCount(RunsIntersect(RunsNormal(e["record"].extents_at_mount),
                                                      [(0, f.D)])) for e in live),
                          ", BOOT %u blocks = %s" % (f.deed["boot"]["kernel_blocks"], self.boot_file)
                          if self.boot_file else ""))
        if k.refusal is not None:
            out.append("the kernel would NOT mount this volume: %s" % k.refusal)
        else:
            out.append("the kernel would mount this volume — fsck: %u orphan, %u missing, "
                       "%u corrected, %u lost" % (k.fsck["orphan"], k.fsck["missing"],
                                                  k.fsck["corrected"], k.fsck["lost"]))
            out.append('the kernel would say "[TagFS] %u data blocks, %u free, %u files"' % k.says)
        for x in self.Sorted():
            out.append("%s %s: %s" % (x.severity, x.code, x.text))
        c = self.Counts()
        out.append("%s — %u damage, %u leak, %u note" % (self.Verdict(), c["damage"], c["leak"], c["note"]))
        return out

    def Json(self):
        f, st, k = self.f, self.f.stand, self.kernel
        deed = {key: (val.hex() if isinstance(val, bytes) else val)
                for key, val in st.deed.items() if key not in ("stamps", "magic")}
        for key in ("born", "packed"):
            if deed.get(key):
                deed[key] = {a: (b.split(b"\0", 1)[0].decode("utf-8", "replace")
                                 if isinstance(b, bytes) else b) for a, b in deed[key].items()}
        files = []
        for fid, e in sorted(self.files.items()):
            rec, v = self.FileRecord(fid), k.verdicts.get(fid)
            files.append({"fid": fid, "state": e["state"],
                          "name": rec.name if rec else None,
                          "row": list(e["row"]) if e["row"] else None,
                          "record": [rec.block, rec.at] if rec else None,
                          "size": rec.size if rec else None,
                          "extents": [list(x) for x in rec.extents_at_mount] if rec else [],
                          "tags": list(rec.tags) if rec else [],
                          "visible": v is not None and v.outcome == "visible",
                          "sha256": self.Sha256(rec) if rec else None})
        led_copies = []
        for c in f.ledgers:
            led_copies.append({a: (b.decode("latin-1") if isinstance(b, bytes) else b)
                               for a, b in c.items()})
        return {
            "medium": {"path": self.medium.path, "sectors": self.medium.SizeSectors()},
            "ground": {"start": st.start, "sectors": st.sectors, "how": st.how,
                       "stood_on": st.stood_on, "kernel_ground": bool(st.kernels)},
            "deed": deed,
            "tail": {"ok": st.tail is not None, "why": st.tail_why,
                     "far_end_sectors": st.far_end["sectors"] if st.far_end else None},
            "regions": [{"name": n, "first": a, "blocks": c} for n, a, c in self.Regions()],
            "ledger": {"copies": led_copies,
                       "standing_on": f.ledger["copy"] if f.ledger else None},
            "diskbook": {"head_ok": bool(f.diskbook[0] and f.diskbook[0]["valid"]),
                         "backup_ok": bool(f.diskbook[1] and f.diskbook[1]["valid"]),
                         "acceptable": f.book_ok, "count": f.book["count"] if f.book else None,
                         "records_bad": [i for i, e in enumerate(f.book_records) if not e["valid"]]},
            "cow": {"snapshots": sorted(f.snapshots), "redirects": [list(r) for r in f.redirects],
                    "stray": [list(r) for r in f.stray]},
            "chains": {"registry": f.chains["registry"][0], "table": f.chains["table"][0],
                       "pool": f.chains["pool"][0], "refused": dict(f.refused) or None},
            "files": files,
            "census": {"used": RunsCount(f.used), "free": f.D - RunsCount(f.used),
                       "swept": self.swept,
                       "claims": [{"kind": c.kind, "label": c.label,
                                   "runs": [list(r) for r in c.runs],
                                   "kernel_sees": c.kernel_sees} for c in self.claims],
                       "orphan_runs": [list(r) for r in getattr(self, "orphans", [])],
                       "missing_runs": [list(r) for x in self.findings if x.code == "MISSING"
                                        for r in x.runs],
                       "double": [list(r) for x in self.findings if x.code == "DOUBLE_OWNER"
                                  for r in x.runs]},
            "kernel": k.Json(),
            "findings": [{"code": x.code, "severity": x.severity, "text": x.text,
                          "runs": [list(r) for r in x.runs]} for x in self.Sorted()],
            "verdict": self.Verdict()}
