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
Each owner is read off the volume and its ground claimed in census_claims.py.
"""

import hashlib
import struct

from . import ondisk
from .ondisk import (SECTOR, BLOCK, SECTORS_PER_BLOCK, REGISTRY_MAGIC,
                     FILETBL_MAGIC, MPOOL_MAGIC, TUSE_MAGIC, COW_MANIFEST_MAGIC,
                     FILE_ACTIVE, LEDGER_COPIES, DISKBOOK_TYPE_REDIRECT,
                     REGISTRY_DATA, REGISTRY_RECORD, FTABLE_PER_BLOCK,
                     RunsFromBits, RunsNormal, RunsIntersect, RunsSubtract,
                     RunsCount, RunsText)
from .medium import FormatVerdict, MediumError, StandOnDeed, Unreadable
from .mount import Label, MountModel
from .bookcensus import SEVERITY, Finding, GroundClaim, Sweep
from .ground_owners import GROUND_OWNER_ALONE
from .census_claims import CensusClaims

# ---------------------------------------------------------------------------
# VolumeFacts — the volume read once, the way the mount reads it
# ---------------------------------------------------------------------------


class VolumeFacts:
    """Everything read off the medium, once, with nothing judged yet."""

    def __init__(self, medium, forced_start):
        self.medium = medium
        self.stand = StandOnDeed(medium, forced_start)
        if self.stand.format != "v1":
            raise Unreadable(FormatVerdict(self.stand.deed))
        self.base = self.stand.start
        self.deed = self.stand.deed
        self.layout = dict(self.deed["layout"])
        self.geometry = self.deed["geometry"]
        self._cache = {}
        self.bad = set()          # volume blocks a read met a sector the medium cannot read in
        self.region = self._ReadBitmapRegion()
        self.growth, self.growth_why = self._PredictGrowth()
        if self.growth:
            self.layout.update(total_blocks=self.growth["blocks"],
                               data_blocks=self.growth["data_blocks"])
        self.D = self.layout["data_blocks"]
        self.bits = bytes(self.region[:(self.D + 7) // 8]).ljust((self.D + 7) // 8, b"\0")
        self.used = RunsFromBits(self.bits, self.D)
        self._ReadLedger()
        self._Mount()
        self._ReadDiskBook()
        self._ReadCow()

    # --- reading ---------------------------------------------------------

    def vread(self, vblock, sectors=SECTORS_PER_BLOCK):
        try:
            return self.medium.sectors(self.base + vblock * SECTORS_PER_BLOCK, sectors)
        except MediumError:
            self.bad.add(vblock)
            return None
        except IOError:
            return None

    def read_block(self, b):
        """A block of the data run, or None where the medium ends. Unbounded
        by the run on purpose: tagfs_read_block bounds nothing (tagfs.c)."""
        if b not in self._cache:
            self._cache[b] = self.vread(self.layout["data_block"] + b)
        return self._cache[b]

    def bad_block(self, b):
        """Whether data-run block `b`, read already, would not read because
        the medium answered a sector of it cannot be read — what it holds —
        rather than refusing the read (meta_pool_mount.c WalkBadSector)."""
        return self.layout["data_block"] + b in self.bad

    def _ReadBitmapRegion(self):
        lay = self.layout
        raw = self.vread(lay["block_bitmap_block"], lay["block_bitmap_blocks"] * SECTORS_PER_BLOCK)
        return raw if raw is not None else b""

    def _PredictGrowth(self):
        """volume_take_more_ground (tagfs.c), up to its commit. None and a
        sentence when nothing is taken — and nothing is on ground the kernel
        does not stand on."""
        st, deed, lay = self.stand, self.deed, self.layout
        if st.stood_on != "head" or not st.kernels:
            return None, None
        if st.sectors is None:
            return None, "the ground's length is not known here, so growth is not predicted"
        have = deed["sectors"]
        if st.sectors <= have + SECTORS_PER_BLOCK:
            return None, None
        medium = self.medium.SizeSectors()
        if medium is None:
            return None, ("there is ground behind this volume and the medium's own "
                          "length is not known here, so growth is not predicted")
        if self.base + st.sectors > medium:
            return None, None
        vol_want = st.sectors // SECTORS_PER_BLOCK
        if vol_want <= lay["data_block"] + 1:
            return None, None
        data_want = vol_want - lay["data_block"] - 1
        cover = lay["block_bitmap_blocks"] * BLOCK * 8
        clamped = data_want > cover
        if clamped:
            data_want, vol_want = cover, lay["data_block"] + cover + 1
        if data_want <= lay["data_blocks"] or vol_want > 0xFFFFFFFF:
            return None, None
        ahead = RunsIntersect(RunsFromBits(self.region, len(self.region) * 8),
                              [(lay["data_blocks"], data_want - lay["data_blocks"])])
        if ahead or len(self.region) * 8 < data_want:
            return None, None
        return {"from_sectors": have, "to_sectors": vol_want * SECTORS_PER_BLOCK,
                "blocks": vol_want, "data_blocks": data_want,
                "more": data_want - lay["data_blocks"], "clamped": clamped,
                "new_tail": (vol_want - 1) * SECTORS_PER_BLOCK,
                "old_tail": deed["tail_sector"]}, None

    def _ReadLedger(self):
        self.ledgers = []
        for c in range(LEDGER_COPIES):
            raw = self.vread(self.layout["state_block"] + c, 1)
            if raw is None:
                d = ondisk.ReadLedgerCopy(b"", c)
                d["why"] = "could not be read off the medium"
            else:
                d = ondisk.ReadLedgerCopy(raw, c)
            # The Use Context's chain the copy names, read as the mount reads it
            # (use_memory_mount.c UseMemoryJudge).
            chain = d.get("use_chain")
            d["use_chain_read"] = (ondisk.ReadUseChain(self.read_block, chain, self.D)
                                   if d["ok"] and chain and chain["first"] else None)
            self.ledgers.append(d)

    def _ReadDiskBook(self):
        db = self.layout["disk_book_block"]
        records_sector = (db + 2) * SECTORS_PER_BLOCK
        heads = [self.vread(db + i, 1) for i in (0, 1)]
        self.diskbook = [ondisk.ReadDiskBookHead(h, records_sector) if h else None
                         for h in heads]
        # DiskBookReplay (disk_book.c DiskBookHeadTaken): the first copy of the
        # head that is whole and describes the book the kernel knows; with none,
        # the first whole one, to say why it is not taken.
        whole = [h for h in self.diskbook if h is not None and h["valid"]]
        taken = [h for h in whole if h["acceptable"]]
        self.book = taken[0] if taken else (whole[0] if whole else None)
        self.book_ok = bool(taken)
        self.book_records = []
        if self.book_ok:
            for i in range(self.book["count"]):
                sector = records_sector + i * ondisk.DISKBOOK_SECTORS_PER_ENTRY
                try:
                    raw = self.medium.sectors(self.base + sector, ondisk.DISKBOOK_SECTORS_PER_ENTRY)
                except IOError:
                    break                        # replay stops at a failed read (disk_book.c DiskBookReplay)
                self.book_records.append(ondisk.ReadDiskBookEntry(raw))

    def _ReadCow(self):
        self.manifests, self.snapshots = [], set()
        led = self.ledger or {}
        # The manifest the Ledger the mount stands on names (cow.c
        # CowClaimGround); its backup was never allocated by any kernel, and
        # the kernel does not read it.
        b = led.get("cow_manifest_block", 0)
        if b:
            raw = self.read_block(b)
            m = ondisk.ReadManifest(raw) if raw is not None else None
            self.manifests.append(("cow_manifest_block", b, m))
            if m is not None and m["ok"]:
                self.snapshots = {e["snapshot_id"] for e in m["entries"]}
        # DiskBookReplay (disk_book.c) into the snapshots the manifest names; a
        # book the mount does not take has nothing to replay.
        self.redirects, self.stray = [], []
        for e in self.book_records:
            if not e["valid"] or e["type"] != DISKBOOK_TYPE_REDIRECT:
                continue
            if e["old_block"] == 0 or e["new_block"] == 0:
                continue
            item = (e["snapshot_id"], e["old_block"], e["new_block"])
            (self.redirects if e["snapshot_id"] in self.snapshots else self.stray).append(item)

    def _Mount(self):
        """The mount's read of the volume (mount.MountModel), and what the
        census reads off it: the chains as the mount walks them, the pool's
        span and the records in it, the table's rows, and the Ledger copy the
        volume stands on — carrying the next file id the mount derives."""
        m = self.mount = MountModel(self.read_block, self.layout, self.ledgers, self.deed["boot"],
                                    self.bad_block)
        self.refused = m.Refused()
        self.chains = {"registry": list(m.registry["blocks"]), "table": list(m.table["links"]),
                       "pool": list(m.pool["chain"])}
        self.registry = [(b, {"records": recs}) for b, recs in m.registry["taken"]]
        self.pool = [(b, ondisk.ReadPoolBlock(raw, upto), raw) for b, raw, upto in m.pool["spans"]]
        self.records = list(m.pool["records"])
        self.newest = {}
        for r in self.records:
            if r.fid and r.unpack_ok:
                self.newest[r.fid] = r
        self.rows = dict(m.table["rows"])
        self.ledger = None
        if m.stood is not None:
            self.ledger = dict(m.stood)
            self.ledger["next_file_id"] = m.next_file_id


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
        c, p = m.counts, m.pool
        self.fsck = {"orphan": RunsCount(orphans), "missing": RunsCount(missing),
                     "corrected": c["corrected"], "lost": c["lost"], "derived": c["derived"],
                     "unsettled": c["unsettled"], "unnamed": c["unnamed"],
                     "quitclaimed": c["quitclaimed"],
                     "unreadable": len(p["unreadable"]),
                     "skipped": [len(p["skipped"]), sum(b for _k, _a, b in p["skipped"])],
                     "skipped_first": list(p["skipped"][0][:2]) if p["skipped"] else None,
                     "dropped": len(p["dropped"]),
                     "dropped_first": p["dropped"][0].fid if p["dropped"] else None,
                     "orphan_runs": orphans, "missing_runs": missing}
        self.says = (D, self.free, c["checked"])

    def Seal(self):
        """What the pool stood on: 'seal' (borne out), 'torn' (its open block
        rolled back) or 'none' (the whole chain, as a kernel before the seal
        reads it); the copy of the Ledger the volume stands on, its cycle and
        the open block it names; whether the mount marks the medium
        SETTLE_MEDIUM_LIED, and whether it stood on nothing because the newest
        seal, the only one, was not borne out ('alone'); the blocks the Deed's
        head names before the chain's first (Glean); and the registry block the
        seal rolled back."""
        m = self.f.mount
        p, at = m.pool, m.pool["seal"]
        return {"stood": p["stood"], "copy": m.stood["copy"] if m.stood else None,
                "settle": m.seals[at]["settle"] if at is not None else None,
                "to_block": m.seals[at]["pool_to_block"] if at is not None else None,
                "fell_back": p["fell_back"], "verdicts": p["verdicts"], "lied": p["lied"],
                "alone": p["stood"] == "none" and p["lied"],
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


class GroundCensus(CensusClaims):
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
                       "standing_on": f.ledger["copy"] if f.ledger else None,
                       # The id the mount hands the next create: past every
                       # id a row or a record of this medium shows (medium.py).
                       "next_file_id": f.ledger["next_file_id"] if f.ledger else None},
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
