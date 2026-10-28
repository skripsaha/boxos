"""census_claims — GroundCensus's owners (census.py), each read off the volume
and its ground claimed: the Deed and the regions, the Ledger and the seals,
the Use Context chain, the DiskBook, the CoW manifests, the chains, the
registry, the records and the files — and what each says that is damage, a
leak or a note, said. A part of GroundCensus, which sweeps the claims against
the bitmap and answers; its own file for the size of the class."""

from .ondisk import (BLOCK, SECTORS_PER_BLOCK, MAX_TAG_ID, LEDGER_COPIES, DISKBOOK_BLOCKS, RECORD_HEADER,
                     RunsFromBits, RunsNormal, RunsIntersect, RunsSubtract, RunsCount, RunsText)
from .mount import Label


class CensusClaims:
    """The owners a GroundCensus hears, each read and claimed (Say, Claim:
    census.py)."""

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
        elif p["stood"] == "none" and not p["refused"] and p["lied"]:
            self.Say("SEAL_ALONE_TORN", "note", "the newest seal (cycle %u) is the only one the "
                     "Ledger holds, and the pool's open block %u does not hold what it says — "
                     "written in place over records a kernel before the seal may have kept "
                     "there, so nothing is rolled back: the whole chain is read as such a kernel "
                     "reads it, and the medium is marked SETTLE_MEDIUM_LIED (meta_pool_mount.c "
                     "WalkChoose)" % (s0["settle"], s0["pool_to_block"]))
        elif p["stood"] == "none" and not p["refused"]:
            self.Say("SEAL_UNBORNE", "note", "no copy's seal is borne out by the pool's closed "
                     "blocks (verdicts %s) — a kernel before the seal rewrote records in them in "
                     "place, or they rotted — so the whole chain is read as such a kernel reads it: "
                     "the file table says which files there are, and nothing is rolled back "
                     "(meta_pool_mount.c WalkChoose)" % "/".join(str(v) for v in p["verdicts"]))
        elif p["fell_back"]:
            let_go = ["fid %u at block %u+%u" % (fid, v["row"][0], v["row"][1])
                      for fid, v in sorted(m.files.items()) if v["state"] == "unsettled"]
            self.Say("SEAL_FELL_BACK", "note", "the newest seal (cycle %u) names pool records the "
                     "medium does not hold — its verdict %s — so the volume stands on cycle %u, "
                     "the one before it, and the medium is one that once held less than a seal "
                     "(SETTLE_MEDIUM_LIED; meta_pool_mount.c SayFellBack); the rows that named "
                     "records only the newest seal held go with its cycle: %s"
                     % (s0["settle"], p["verdicts"][0], s1["settle"], ", ".join(let_go) or "none"))
        elif p["stood"] == "torn":
            mark = m.seals[p["seal"]]
            lost = sorted(fid for fid, v in m.files.items() if v["rolled"])
            self.Say("SEAL_TAIL_TORN", "note", "the pool's open block %u does not hold what the "
                     "seal of cycle %u says — it was torn as it was written: its %u byte(s) of "
                     "records are rolled back to the last closed block%s, every file whose newest "
                     "record was there stands on the one before it, %u file(s) with none before "
                     "it are lost (%s), and the medium is marked SETTLE_MEDIUM_LIED "
                     "(meta_pool_mount.c WalkChoose)"
                     % (mark["pool_to_block"], mark["settle"], mark["pool_to_bytes"],
                        ", which holds the other seal's open block whole"
                        if p["seal"] == 0 and p["closed"][1] else "", len(lost), ", ".join("fid %u" % f for f in lost) or "none"))
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
            elif r.fid != 0 and (not r.shape_ok or (r.extents and not r.extents_at_mount
                                                     and not r.outside)):
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
        if rec.outside:
            outside = RunsSubtract(RunsNormal([(s, c) for s, c in rec.extents if c]), [(0, self.f.D)])
            self.Say("EXTENT_OUTSIDE", "damage", "%s names block(s) %s past the data run's %u "
                     "blocks — the mount follows none of its extents: the file is taken up "
                     "naming no blocks, and nothing is written over it (plot_pack.c "
                     "PlotExtentsInRun)" % (Label(fid, rec), RunsText(outside), self.f.D))
        for tag in rec.tags:
            if tag not in self.by_id:
                self.absent.setdefault(tag, []).append(Label(fid, rec))

    def _Files(self):
        """Every id the mount's map holds, as MountFile judges it
        (mount.MountFiles): a file it takes up is live; a record no row names,
        with no seal, is unnamed; a row whose newest record is a Quitclaim is a
        delete whose row had not yet gone, and goes with it; a row with no
        record is a cycle that did not settle under a seal when the stood
        Ledger had not issued its id, and a file lost otherwise — with no seal,
        or a file that settled: in the open block a seal rolled back, or gone
        with a block the chain no longer holds."""
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
            elif state == "quitclaim":
                self.Say("QUITCLAIM_PENDING", "note", "%s is named at block %u+%u and its newest "
                         "record is a Quitclaim — the file was deleted and its row had not gone "
                         "yet; the mount lets the row go with it (mount.c MountQuitclaimedRows)"
                         % (Label(fid, newest), row[0], row[1]))
                self._File(fid, "quitclaim", newest, row)
            elif state == "unsettled":
                unsettled.append("fid %u at block %u+%u" % (fid, row[0], row[1]))
                self._File(fid, "unsettled", None, row)
            else:
                self.Say("FILE_LOST", "damage", "fid %u is named at block %u+%u and the pool holds "
                         "no record of it that reads%s — the mount deletes its name (mount.c "
                         "MountFile)" % (fid, row[0], row[1], ", its only one settled in the open "
                                         "block the mount rolled back" if v["rolled"] else
                                         ", though the seal it stands on had settled it"
                                         if f.mount.sealed else ""))
                self._File(fid, "lost", None, row)
        if unsettled:
            self.Say("ROW_UNSETTLED", "note", "%u table row(s) name files the sealed pool holds no "
                     "record of — %s — rows of a cycle that did not settle, whose ids the stood "
                     "Ledger had not issued; they are let go (mount.c MountFile)"
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
                     "(volume_deed.h VolumeBoot, create_tagfs_v1_pack.c V1PackDeed's BOOT)"
                     % (boot["kernel_block"], boot["kernel_blocks"], boot["kernel_bytes"], why))
            return
        self.boot_file = Label(holders[0]["fid"], holders[0]["record"])

    def _Claims(self):
        """Every owner's ground as the mount's census hears it (MountModel):
        the BOOT run beside the kernel file it is, when the census found it."""
        for kind, label, runs, _partner in self.f.mount.claims:
            self.Claim(kind, label, runs, partner=self.boot_file if kind == "boot" else None)
