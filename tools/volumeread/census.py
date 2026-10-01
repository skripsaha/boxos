"""census — GroundCensus: every owner of every block, and what the mount makes of it.

Two roll calls over the same image, held against each other.

The KERNELVIEW is the mount: what MountReadVolume and its GroundCensus decide
on this image (mount.MountModel, the kernel's decision procedure said once) —
the refusals, which seal it stands on, which files it takes up, lets go and
loses, the orphan and missing runs its census names, the free count it keeps
and the line it prints. A change to the mount has to change mount.py in the
same commit: logcheck's `census` scenario boots the image and holds the
kernel's own named runs against this model, so a drift is a red line, not a
surprise.

The CENSUS is the account of the same ground, owner by owner — the owners the
kernel hears (src/include/ground_owner.h), each named for what it is — set
against the bitmap: a block with two owners, a block used with no owner (an
orphan, explained by what the volume says it was) and a block owned and free
(missing) are each named, as runs; and beside them what the records, the
registry, the rows and the seals say that is damage, a leak, or a note.
"""

import hashlib
import struct

from . import ondisk
from .ondisk import (SECTOR, BLOCK, SECTORS_PER_BLOCK, REGISTRY_MAGIC,
                     FILETBL_MAGIC, MPOOL_MAGIC, TUSE_MAGIC, COW_MANIFEST_MAGIC,
                     FILE_ACTIVE, MAX_TAG_ID, LEDGER_COPIES, DISKBOOK_BLOCKS,
                     REGISTRY_DATA, REGISTRY_RECORD, FTABLE_PER_BLOCK, RECORD_HEADER,
                     RunsFromBits, RunsNormal, RunsIntersect, RunsSubtract,
                     RunsCount, RunsText)
from .medium import VolumeFacts
from .mount import Label
from .ground_owners import GROUND_OWNER_ALONE

SEVERITY = ("damage", "leak", "note")        # exit 1 on any damage or leak


class Finding:
    """One statement about the volume: a stable code, a severity, one sentence
    with the runtime numbers in it, and the blocks it is about as runs."""

    __slots__ = ("code", "severity", "text", "runs")

    def __init__(self, code, severity, text, runs=()):
        self.code, self.severity, self.text, self.runs = code, severity, text, list(runs)


class GroundClaim:
    """One owner's claim on the data run. An alias (the BOOT run) is not
    exclusive: it may coincide with its partner — the one file it is — and
    with nothing else; with no partner found, with any file."""

    __slots__ = ("kind", "label", "runs", "exclusive", "partner")

    def __init__(self, kind, label, runs, exclusive=True, partner=None):
        self.kind, self.label, self.runs = kind, label, RunsNormal(runs)
        self.exclusive, self.partner = exclusive, partner

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
# KernelView — what the mount decides on this image
# ---------------------------------------------------------------------------


class KernelView:
    """The mount in this tree, on this image: the facts' MountModel and the
    GroundCensus it runs. It reads nothing the facts have not read."""

    def __init__(self, facts):
        self.f = facts
        self.refusal = self.step = None
        self.fsck = None
        self.says = None
        self.free = None
        self.writes = []                 # [(sentence, runs of data blocks written)]
        self.kept_line = None
        self.marked = []
        self.union = []
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
        m = f.mount
        if m.refusal is not None:
            return self.Refuse(*m.refusal)
        need = ((D + 7) // 8 + SECTOR - 1) // SECTOR
        if need > lay["block_bitmap_blocks"] * SECTORS_PER_BLOCK:
            return self.Refuse("layout", "its block bitmap region is too small to hold a bit for every "
                               "block of its data run")
        if len(f.region) < need * SECTOR:
            return self.Refuse("bitmap", "its block bitmap would not read, and a missing bitmap reads "
                               "as a volume with nothing on it")
        self._Census()
        if m.pool["disagreed"]:
            self.kept_line = (m.pool["disagreed"], len(m.pool["chain"]))

    def _Census(self):
        """MountCensus: every owner's ground (MountModel.claims) against the
        bitmap. An orphan is kept taken, a missing block is taken in memory
        (the additive repair the next Settle cycle writes), and the free count
        is what is left of the data run."""
        f, m, D = self.f, self.f.mount, self.f.D
        marks = []
        for _kind, _label, runs, _partner in m.claims:
            marks += RunsIntersect(RunsNormal(runs), [(0, D)])
        self.marked = RunsNormal(marks)
        orphans = RunsSubtract(f.used, self.marked)
        missing = RunsSubtract(self.marked, f.used)
        self.union = RunsNormal(f.used + self.marked)
        self.free = D - RunsCount(self.union)
        c = m.counts
        self.fsck = {"orphan": RunsCount(orphans), "missing": RunsCount(missing),
                     "corrected": c["corrected"], "lost": c["lost"], "derived": c["derived"],
                     "unsettled": c["unsettled"], "unnamed": c["unnamed"],
                     "unreadable": len(m.pool["unreadable"]),
                     "orphan_runs": orphans, "missing_runs": missing}
        self.says = (D, self.free, c["checked"])

    def Seal(self):
        """What the pool stood on: 'seal' (borne out), 'torn' (its open block
        rolled back) or 'none' (the whole chain, as a kernel before the seal
        reads it); the copy of the Ledger the volume stands on, its cycle and
        the open block it names; the blocks the Deed's head names before the
        chain's first (Glean); and the registry block the seal rolled back."""
        m = self.f.mount
        at = m.pool["seal"]
        return {"stood": m.pool["stood"], "copy": m.stood["copy"] if m.stood else None,
                "settle": m.seals[at]["settle"] if at is not None else None,
                "to_block": m.seals[at]["pool_to_block"] if at is not None else None,
                "fell_back": m.pool["fell_back"], "verdicts": m.pool["verdicts"],
                "newest_sealed": m.seals[0] is not None, "prefix": list(m.prefix),
                "registry_rolled": m.registry["rolled"]}

    def Json(self):
        fsck = None
        if self.fsck is not None:
            fsck = dict(self.fsck)
            fsck["orphan_runs"] = [list(r) for r in self.fsck["orphan_runs"]]
            fsck["missing_runs"] = [list(r) for r in self.fsck["missing_runs"]]
        g, m = self.f.growth, self.f.mount
        return {"mounts": self.refusal is None, "refusal": self.refusal, "fsck": fsck,
                "says": (None if self.says is None else
                         {"data_blocks": self.says[0], "free": self.says[1], "files": self.says[2]}),
                "seal": self.Seal(), "next_file_id": m.next_file_id, "next_tag_id": m.next_tag_id,
                "writes_at_mount": [w for w, _r in self.writes],
                "kept_line": (None if self.kept_line is None else
                              {"disagreed": self.kept_line[0], "blocks": self.kept_line[1]}),
                "grows": None if g is None else {k: g[k] for k in sorted(g)}}


# ---------------------------------------------------------------------------
# GroundCensus — the account, and the findings
# ---------------------------------------------------------------------------


class GroundCensus:
    """Every owner of every block of the data run, against the bitmap and
    against the KernelView of the same image."""

    def __init__(self, medium, forced_start=None):
        self.f = f = VolumeFacts(medium, forced_start)
        self.medium = medium
        self.findings, self.claims, self.files = [], [], {}
        self.by_id = f.mount.by_id
        self.kernel = KernelView(f)
        self.swept, self.boot_file = False, None
        self._Deed()
        if self.kernel.step == "layout" or not self._Regions():
            return
        self._Ledger()
        self._Seal()
        self._UseChain()
        self._DiskBook()
        self._Cow()
        self._Chains()
        self._Registry()
        self._Records()
        self._Files()
        self._Boot()
        self._Claims()
        if f.ledger is not None and self.kernel.step != "bitmap":
            self._Sweep()
        self._Drift()

    def Say(self, code, severity, text, runs=()):
        self.findings.append(Finding(code, severity, text, runs))

    def Claim(self, kind, label, runs, partner=None):
        """One owner's ground. The kinds, and which of them own alone, are the
        kernel's (src/include/ground_owner.h, generated into ground_owners)."""
        if kind not in GROUND_OWNER_ALONE:
            raise ValueError("%s is no owner src/include/ground_owner.h names" % kind)
        c = GroundClaim(kind, label, RunsIntersect(RunsNormal(runs), [(0, self.f.D)]),
                        GROUND_OWNER_ALONE[kind], partner)
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
                     "%s — and the kernel refuses the mount (ledger.c ledger_load); the ownership "
                     "sweep is not run, because the ledger names blocks nothing else can"
                     % (bad[0]["why"], bad[1]["why"]))
        elif bad:
            good = f.mount.newest
            self.Say("LEDGER_TORN", "note", "ledger copy %u %s — the kernel runs on copy %u "
                     "(seq %u) alone (ledger.c ledger_load)" % (bad[0]["copy"], bad[0]["why"],
                                                            good["copy"], good["seq"]))
        for c in f.ledgers:
            if c["ok"] and c["use_misplaced"]:
                self.Say("LEDGER_USE_MISPLACED", "note", "ledger copy %u places its Use Context at "
                         "%u for %u bytes, outside its own record — it remembers none "
                         "(ledger.c ledger_read_copy)" % (c["copy"], c["use_context_offset"],
                                                  c["use_context_bytes"]))

    def _Seal(self):
        """What the mount stood on, said when it is not the newest seal borne
        out (meta_pool_mount.c PoolMount, tag_registry_load.c)."""
        m = self.f.mount
        if m.newest is None:
            return
        s0, s1 = m.seals
        p = m.pool
        if s0 is None:
            self.Say("SEAL_ABSENT", "note", "the newest ledger copy (%u, seq %u) carries no seal: a "
                     "kernel before the seal wrote it last, so the volume is read as that kernel "
                     "reads it — the file table says which files there are, the pool is read to "
                     "its end (mount.c MountSealsRead)" % (m.newest["copy"], m.newest["seq"]))
        elif p["stood"] == "none" and not p["refused"]:
            self.Say("SEAL_UNBORNE", "note", "no copy's seal is borne out by the pool's closed "
                     "blocks (verdicts %s) — a kernel before the seal rewrote records in them in "
                     "place, or they rotted — so the whole chain is read as such a kernel reads it: "
                     "the file table says which files there are, and nothing is rolled back "
                     "(meta_pool_mount.c WalkChoose)" % "/".join(str(v) for v in p["verdicts"]))
        elif p["fell_back"]:
            self.Say("SEAL_FELL_BACK", "note", "the newest seal (cycle %u) names pool records the "
                     "medium does not hold — its verdict %s — so the volume stands on cycle %u, "
                     "the one before it, and the medium is one that once held less than a seal "
                     "(SETTLE_MEDIUM_LIED; meta_pool_mount.c SayFellBack)"
                     % (s0["settle"], p["verdicts"][0], s1["settle"]))
        elif p["stood"] == "torn":
            mark = m.seals[p["seal"]]
            gone = sorted(fid for fid, v in self.f.mount.files.items() if v["state"] != "mounted")
            self.Say("SEAL_TAIL_TORN", "note", "the pool's open block %u does not hold what the "
                     "seal of cycle %u says — it was torn as it was written again: its %u byte(s) "
                     "of records are rolled back to the last closed block, every file whose newest "
                     "record was there stands on the one before it, and %u name(s) with none "
                     "before it go (meta_pool_mount.c WalkChoose)"
                     % (mark["pool_to_block"], mark["settle"], mark["pool_to_bytes"], len(gone)))
        r = m.registry
        if r["rolled"] is not None:
            self.Say("REGISTRY_ROLLED_BACK", "note", "block %u, the registry's last, does not hold "
                     "what the newest seal says it does — its records are rolled back: the tags "
                     "they named are forgotten, and their numbers are never given out again "
                     "(tag_registry_load.c RegistrySealedBytes)" % r["rolled"])

    def _UseChain(self):
        """The Use Context's chains the copies name (use_memory_mount.c): a
        newest copy's chain that does not read whole is the mount rolling the
        memory back to the copy before — the medium's lie only when a link of
        the chain the record's own cycle wrote holds another's content."""
        f = self.f
        newest = f.mount.newest
        for c in f.ledgers:
            got = c.get("use_chain_read")
            if not got or newest is None or c["copy"] != newest["copy"] or got["state"] == "whole":
                continue
            first = c["use_chain"]["first"]
            lied = got["state"] == "foreign" and c["use_chain"]["seq"] == c["seq"]
            self.Say("TUSE_TORN", "note", "the newest ledger copy names a Use Context chain "
                     "at data block %u that reads %s — the mount remembers what the copy "
                     "before it names%s (use_memory_mount.c UseMemoryJudge)"
                     % (first, got["state"], ", and says the medium lied about its FLUSH"
                        if lied else ""))

    def _DiskBook(self):
        f = self.f
        if not f.book_ok:
            why = ("its head and the copy both fail their checksum" if f.book is None else
                   "its head reads version %u, capacity %u, count %u, records at sector %u"
                   % (f.book["version"], f.book["capacity"], f.book["count"], f.book["start_sector"]))
            self.Say("DISKBOOK_INVALID", "note",
                     "the DiskBook is not one the mount accepts — %s — so the mount takes it as "
                     "holding nothing, writes nothing over it, and replays no redirect "
                     "(disk_book.c DiskBookHeadTaken)%s"
                     % (why, "; the manifest names %u snapshot(s) whose frozen blocks are then "
                        "orphans nothing explains" % len(f.snapshots) if f.snapshots else ""))
        bad = [i for i, e in enumerate(f.book_records) if not e["valid"]]
        if bad:
            self.Say("DISKBOOK_RECORD_BAD", "note", "DiskBook record(s) %s fail their magic or "
                     "checksum and are skipped at replay (disk_book.c DiskBookRecordWhole)"
                     % ", ".join(map(str, bad)))

    def _Cow(self):
        f = self.f
        for _which, b, m in f.manifests:
            if b >= f.D or m is None or not m["ok"]:
                self.Say("MANIFEST_BAD", "damage", "the snapshot manifest the ledger names at data "
                         "block %u %s (cow.c CowManifestRead)" % (b, "lies past the data run" if b >= f.D
                                                  else "will not read" if m is None else
                                                  "reads magic 0x%08x, count %u"
                                                  % (m["magic"], m["count"])), [(b, 1)])
        # What a snapshot kept is no owner's (ground_owner.h): its frozen blocks
        # are orphans, named in _Orphans for what they are.
        if f.stray:
            self.Say("REDIRECT_STRAY", "note", "%u DiskBook redirect(s) name snapshots the manifest "
                     "does not carry (%s) — the kernel passes them over and counts them "
                     "(cow.c CowRedirectSeen)"
                     % (len(f.stray), ", ".join(str(s[0]) for s in f.stray)))

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
        for name in ("registry", "pool", "table"):
            if name in f.refused:
                self.Say("CHAIN_REFUSED", "damage", "the %s: %s — the kernel refuses the mount: %s"
                         % (name, f.refused[name], self.kernel.refusal or "(an earlier refusal)"))
        drift = [(b, blk["kept"], blk["kept_counted"]) for b, blk, _r in f.pool
                 if blk["kept"] != blk["kept_counted"]]
        if drift:
            self.Say("POOL_KEPT_DRIFT", "note", "%u of %u pool block(s) state a kept count the records "
                     "do not bear out (%s) — the mount says so and counts (meta_pool_mount.c "
                     "PoolMount)" % (len(drift), len(f.pool), ", ".join("block %u says %u, holds %u" % d
                                                                       for d in drift)))
        for b, blk, _r in f.pool:
            if blk["walk_stop"] is not None:
                at, ln = blk["walk_stop"]
                self.Say("RECORD_SHAPE", "damage", "pool block %u: the walk stops at byte %u, where a "
                         "record says it is %u long, with %u used bytes stated — the records behind "
                         "it are never read (meta_pool_mount.c WalkRecords)"
                         % (b, at, ln, blk["used_bytes"]))

    def _Registry(self):
        """RegistryAdopt's rules (tag_registry.c) over what the mount takes of
        the chain (mount.RegistryAdopt)."""
        dup, rng = [], []
        for why, b, r, other in self.f.mount.dropped:
            where = "block %u +%u" % (b, r["at"])
            if why == "repeats":
                dup.append("id %u at %s repeats id %u's key/value and is dropped"
                           % (r["tag_id"], where, other))
            elif why == "range":
                rng.append("id %u at %s is past %u and is dropped" % (r["tag_id"], where, MAX_TAG_ID))
            else:
                dup.append("id %u at %s collides with an earlier record of that id and is "
                           "dropped" % (r["tag_id"], where))
        if dup:
            self.Say("TAG_DUPLICATE", "damage", "tag registry: " + "; ".join(dup))
        if rng:
            self.Say("TAG_ID_RANGE", "damage", "tag registry: " + "; ".join(rng))

    def _Records(self):
        bad, shape = [], []
        for r in self.f.records:
            # A retired record has its file_id zeroed with its CRC left as it
            # was (an earlier kernel's meta_pool_delete): a fid-0 record fails
            # its sum by construction, and is not damage.
            if r.fid != 0 and not r.unpack_ok:
                (bad if not r.crc_ok else shape).append(
                    "fid %u at block %u +%u%s" % (r.fid, r.block, r.at, "" if not r.crc_ok else
                                                  " (its counts reach past its length of %u)" % r.len))
            elif r.fid != 0 and (not r.shape_ok or (r.extents and not r.extents_at_mount)):
                shape.append("fid %u at block %u +%u (%s)" % (
                    r.fid, r.block, r.at, "length %u against a shape of %u" % (
                        r.len, RECORD_HEADER + 2 * r.tag_count + 6 * r.extent_count + r.name_len)
                    if not r.shape_ok else "an extent the mount drops, and with it every extent"))
        if bad:
            self.Say("RECORD_CRC", "damage", "%u pool record(s) fail their CRC16: %s — the mount "
                     "passes them over, and each file keeps the newest of its records that reads "
                     "(meta_pool_mount.c WalkPut)" % (len(bad), ", ".join(bad)))
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
                     "(plot_pack.c PlotExtentsInRun)" % (Label(fid, rec), RunsText(outside), self.f.D))
        for tag in rec.tags:
            if tag not in self.by_id:
                self.absent.setdefault(tag, []).append(Label(fid, rec))

    def _Files(self):
        """Every id the mount's map holds, as MountFile judges it
        (mount.MountFiles): a file it takes up is live; a record no row names,
        with no seal, is unnamed; a row with no record is a cycle that did not
        settle under a seal and a file lost without one — or, where the newest
        record the span holds of it is a Quitclaim, a delete whose row had not
        yet gone."""
        f, self.absent = self.f, {}
        unsettled = []
        for fid, v in sorted(f.mount.files.items()):
            rec, row, state = v["record"], v["row"], v["state"]
            newest = f.newest.get(fid)
            if state == "mounted":
                if v["corrected"]:
                    self.Say("TABLE_CORRECTED", "note", "%s is named at block %u+%u and its newest "
                             "record is at block %u+%u — the mount corrects the table to it "
                             "(mount.c MountFile)" % (Label(fid, rec), row[0], row[1], rec.block, rec.at))
                self._File(fid, "live", rec, row)
                self._Owns(fid, rec)
            elif state == "unnamed":
                self._File(fid, "unnamed", rec, None)
            elif newest is not None and not newest.flags & FILE_ACTIVE:
                self.Say("QUITCLAIM_PENDING", "note", "%s is named at block %u+%u and its newest "
                         "record is a Quitclaim — the file was deleted and its row had not gone "
                         "yet; the mount lets the name go and counts it %s (mount.c MountFile)"
                         % (Label(fid, newest), row[0], row[1],
                            "among the rows of a cycle that did not settle" if f.mount.sealed
                            else "among the lost"))
                self._File(fid, "quitclaim", newest, row)
            elif state == "unsettled":
                unsettled.append("fid %u at block %u+%u" % (fid, row[0], row[1]))
                self._File(fid, "unsettled", None, row)
            else:
                self.Say("FILE_LOST", "damage", "fid %u is named at block %u+%u and the pool holds "
                         "no record of it that reads — the mount deletes its name (mount.c "
                         "MountFile)" % (fid, row[0], row[1]))
                self._File(fid, "lost", None, row)
        if unsettled:
            self.Say("ROW_UNSETTLED", "note", "%u table row(s) name files the sealed pool holds no "
                     "record of — %s — rows of a cycle that did not settle, or of records the "
                     "mount rolled back; they are let go (mount.c MountFile)"
                     % (len(unsettled), ", ".join(unsettled)))
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
        self.boot_file = Label(holders[0]["fid"], holders[0]["record"])

    def _Claims(self):
        """Every owner's ground as the mount's census hears it (MountModel):
        the BOOT run beside the kernel file it is, when the census found it."""
        for kind, label, runs, _partner in self.f.mount.claims:
            self.Claim(kind, label, runs, partner=self.boot_file if kind == "boot" else None)

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
                         "mount's census takes them again, in memory, and the next Settle cycle "
                         "writes them (block_map_mount.c MapCensusWindow)"
                         % (RunsText(kept), " and ".join(labels)), kept)
            lost = RunsSubtract(runs, self.kernel.marked)
            if lost:
                self.Say("MISSING", "damage", "data block(s) %s of %s read free in the bitmap and "
                         "the kernel, which refuses this mount, repairs nothing"
                         % (RunsText(lost), " and ".join(labels)), lost)
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
                     "%s is an ACTIVE newest record no table row names, on a volume with no seal "
                     "the mount stands on — %s"
                     % (Label(fid, e["record"]),
                        "its blocks %s are marked used and nothing owns them: an interrupted "
                        "create a kernel before the seal left (mount.c MountFile)"
                        % RunsText(leak) if leak else
                        "stale: its blocks %s" % ("went to others" if taken else "are free")), leak)

    def _Explanations(self):
        """What each orphan block might be the extent of: {block: text}, the
        most telling explanation winning."""
        f, out = self.f, {}
        state = {id(e["record"]): e["state"] for e in self.files.values() if e["record"] is not None}
        past = [(r, "past") for r in f.mount.pool["past"]]
        for r, where in [(r, None) for r in f.records] + past:
            st = state.get(id(r))
            if st == "live":
                continue                         # an owner, not an explanation
            if st == "unnamed":
                rank, text = 0, "extents of unnamed record %s" % Label(r.fid, r)
            elif st == "quitclaim":
                rank, text = 1, "extents of %s, whose newest record is a Quitclaim" % Label(r.fid, r)
            elif where == "past" and r.fid and r.unpack_ok:
                rank, text = 1, ("extents of a record of %s past the seal's span — a cycle that "
                                 "did not settle" % Label(r.fid, r))
            elif r.fid and r.unpack_ok:
                rank, text = 1, "extents of a superseded record of %s" % Label(r.fid, r)
            elif r.fid:
                rank, text = 2, "extents named by a record of fid %u that does not unpack" % r.fid
            else:
                rank, text = 3, "extents of a retired record '%s'" % r.name
            for s, n in RunsIntersect(RunsNormal(r.extents), self.orphans):
                for b in range(s, s + n):
                    if b not in out or out[b][0] > rank:
                        out[b] = (rank, text)
        return {b: t for b, (_r, t) in out.items()}

    def _Orphans(self):
        f, explained, groups, links = self.f, self._Explanations(), {}, []
        kept = {old for _snap, old, _new in f.redirects}
        past_pool = set(f.mount.pool["past_blocks"])
        past_registry = f.mount.registry["past"]
        for s, n in self.orphans:
            for b in range(s, s + n):
                if b in kept:
                    groups.setdefault(("SNAPSHOT_KEPT", "blocks snapshots an earlier kernel took "
                                       "keep — nothing reads a format-v1 snapshot, so nothing owns "
                                       "them, and the census keeps them taken until it gives "
                                       "orphans back (cow.h)"), []).append((b, 1))
                    continue
                raw = f.vread(f.layout["data_block"] + b)
                magic = struct.unpack_from("<I", raw, 0)[0] if raw else None
                spare = ondisk.SpareShape(raw) if raw else None
                if magic == TUSE_MAGIC:
                    links.append((b, ondisk.ReadUseChainLink(raw)))
                    continue
                if spare:
                    code, text = "SPARE_UNNAMED", "a spare %s header linked from nowhere" % spare
                elif magic == MPOOL_MAGIC and b in past_pool:
                    code, text = "ORPHAN", ("a pool block a cycle that did not settle linked past "
                                            "the seal's open block")
                elif magic == MPOOL_MAGIC:
                    alive = any(fid for _a, _l, fid in ondisk.ReadPoolBlock(raw)["mirror_walk"])
                    text = ("a pool block linked from nowhere (a severed tail, or a block a "
                            "cycle took off the chain's front)" if alive else
                            "a pool block holding no record of a file")
                    code = "ORPHAN"
                elif magic == REGISTRY_MAGIC and b == past_registry:
                    code, text = "ORPHAN", ("a registry block linked past the block the seal "
                                            "names — a cycle that did not settle")
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
            self.Say(code, "note" if code == "SNAPSHOT_KEPT" else "leak",
                     "data block(s) %s are used in the bitmap and nothing owns them: %s"
                     % (RunsText(runs), text), runs)
        self._UseChains(links)

    def _UseChains(self, links):
        """TUSE chains among the orphans, read as the kernel reads them
        (ondisk.ReadUseChain, use_chain.c UseChainFindOrphans): from each link
        of index 0, by the name it states, every link an orphan. A mount whose
        newest copy an older kernel wrote takes the newest of them up again,
        or lets them rest (use_memory_mount.c)."""
        f, by_block, whole = self.f, dict(links), set()
        read = lambda b: f.read_block(b) if b in by_block else None
        for b, head in links:
            if head["index"] != 0:
                continue
            ref = {"first": b, "attempt": head["attempt"], "seq": head["seq"],
                   "bytes": head["total_bytes"], "crc32": head["text_crc32"]}
            got = ondisk.ReadUseChain(read, ref, f.D)
            if got["state"] == "whole":
                chain = got["blocks"]
                whole.update(chain)
                runs = RunsNormal([(x, 1) for x in chain])
                self.Say("TUSE_UNNAMED", "leak", "data block(s) %s hold a whole TUSE chain nothing "
                         "names: %u links, %u bytes, crc ok" % (RunsText(runs), len(chain),
                                                               len(got["text"])), runs)
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
                     "— the mount keeps the bitmap's number, and the next Settle cycle writes it "
                     "(mount.c MountCensus)" % (led["free_blocks"], derived))
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

    def RegistryTailUsed(self):
        """The bytes of records the registry's last block holds as the mount
        took it — where the next record is appended (tag_registry_load.c)."""
        taken = self.f.mount.registry["taken"]
        if not taken:
            return None
        return sum(REGISTRY_RECORD.size + r["key_len"] + r["value_len"] for r in taken[-1][1])

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
            out.append("ledger copies 0 and 1 are intact, %s — the newest is copy %u"
                       % ("both at seq %u" % seqs[0] if seqs[0] == seqs[1] else
                          "at seq %u and %u" % tuple(seqs), f.mount.newest["copy"]))
        if f.ledger is not None:
            seal = k.Seal()
            out.append("the pool stands on %s, ledger copy %u%s — next file id %u, next tag id %u"
                       % ({"seal": "the seal of cycle %s" % seal["settle"],
                           "torn": "the seal of cycle %s, its open block rolled back" % seal["settle"],
                           "none": "no seal"}[seal["stood"]], seal["copy"],
                          " (fell back)" if seal["fell_back"] else "",
                          f.mount.next_file_id, f.mount.next_tag_id))
        if self.swept:
            used = RunsCount(f.used)
            out.append("data run %u blocks at volume block %u — %u used, %u free, the ledger says %u"
                       % (f.D, f.layout["data_block"], used, f.D - used, f.ledger["free_blocks"]))
            live = [e for e in self.files.values() if e["state"] == "live"]
            pool = f.chains["pool"]
            out.append("owners — registry %u block(s), table %u, pool %u (%s), %u files in %u blocks%s"
                       % (len(f.chains["registry"]), len(f.chains["table"]), len(pool),
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
            rec = e["record"]
            files.append({"fid": fid, "state": e["state"],
                          "name": rec.name if rec else None,
                          "row": list(e["row"]) if e["row"] else None,
                          "record": [rec.block, rec.at] if rec else None,
                          "size": rec.size if rec else None,
                          "extents": [list(x) for x in rec.extents_at_mount] if rec else [],
                          "tags": list(rec.tags) if rec else [],
                          "visible": e["state"] == "live",
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
            "chains": {"registry": f.chains["registry"], "table": f.chains["table"],
                       "pool": f.chains["pool"], "refused": dict(f.refused) or None,
                       # How far each chain is from taking a new block: the
                       # registry's last block is filled record by record and a
                       # record that does not fit starts the next one; the
                       # table takes its next block for the first row past what
                       # its blocks hold (tag_registry.c, file_table.c flush).
                       "registry_tail_used": self.RegistryTailUsed(),
                       "registry_block_data": REGISTRY_DATA,
                       "registry_record_header": REGISTRY_RECORD.size,
                       "table_rows_per_block": FTABLE_PER_BLOCK},
            "files": files,
            "census": {"used": RunsCount(f.used), "free": f.D - RunsCount(f.used),
                       "swept": self.swept,
                       "claims": [{"kind": c.kind, "label": c.label,
                                   "runs": [list(r) for r in c.runs]} for c in self.claims],
                       "orphan_runs": [list(r) for r in getattr(self, "orphans", [])],
                       "missing_runs": [list(r) for x in self.findings if x.code == "MISSING"
                                        for r in x.runs],
                       "double": [list(r) for x in self.findings if x.code == "DOUBLE_OWNER"
                                  for r in x.runs]},
            "kernel": k.Json(),
            "findings": [{"code": x.code, "severity": x.severity, "text": x.text,
                          "runs": [list(r) for r in x.runs]} for x in self.Sorted()],
            "verdict": self.Verdict()}
