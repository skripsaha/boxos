"""medium — the thing a volume is read off, where on it the volume stands, and
what the volume reads as.

A Medium is a raw device or an image file, read in whole sectors and never
written. An OverlayMedium is a Medium with sectors laid over it in memory —
what the census reads a crash state or a planted defect through, so that no
file is ever written to find out what a volume would look like.

Where the volume stands is found the way the kernel finds it: candidates come
from the partition table, the answer comes from the Deed on the medium. What
the volume reads as (VolumeFacts) is read once, in the order the mount reads
it, and judged by nobody here — the census judges.
"""

import os
import shutil
import stat
import struct
import subprocess
import sys

from . import ondisk
from .ondisk import (SECTOR, BLOCK, SECTORS_PER_BLOCK, DEED_MAGIC, DEED_SECTORS,
                     DEED_ROLE_HEAD, DEED_ROLE_TAIL, BOXOS_TYPE_GUID,
                     MBR_TYPE_BOXOS, MBR_TYPE_PROTECTIVE, LEDGER_COPIES,
                     REGISTRY_MAGIC, FILETBL_MAGIC, MPOOL_MAGIC,
                     DISKBOOK_TYPE_REDIRECT, RunsFromBits, RunsIntersect)


class Unreadable(Exception):
    """The medium carries nothing a census can begin from: no Deed at any
    candidate, no layout, or it ends inside what has to be read first."""


class Medium:
    """The device, read in whole sectors and never written."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")

    def sectors(self, lba, count):
        self.f.seek(lba * SECTOR)
        data = self.f.read(count * SECTOR)
        if len(data) != count * SECTOR:
            raise IOError(
                f"the medium ended inside sector {lba}+{count} "
                f"(got {len(data)} of {count * SECTOR} bytes)")
        return data

    def SizeSectors(self):
        """How far the medium runs, in sectors — for a regular file. A device
        answers None: how long a raw device is has not been verified on macOS
        (/dev/rdiskN), and a guessed length would be a guessed growth."""
        st = os.fstat(self.f.fileno())
        if stat.S_ISREG(st.st_mode):
            return st.st_size // SECTOR
        return None


class OverlayMedium:
    """A Medium with whole sectors laid over it, in memory.

    `over` maps a sector number to its 512 bytes; everything else is the
    base's. Nothing here writes a file — a crash state, a planted defect and a
    cut image are all read through one of these."""

    def __init__(self, base):
        self.base = base
        self.path = base.path
        self.over = {}

    def Put(self, lba, data):
        """Lay whole sectors over the base, starting at `lba`."""
        if len(data) % SECTOR:
            raise ValueError("an overlay takes whole sectors, not %d bytes" % len(data))
        for i in range(len(data) // SECTOR):
            self.over[lba + i] = bytes(data[i * SECTOR:(i + 1) * SECTOR])

    def Patch(self, offset, data):
        """Change bytes at a byte offset: the covering sectors are read (from
        the overlay where it has them), edited, and laid over."""
        first = offset // SECTOR
        last = (offset + len(data) - 1) // SECTOR
        cur = bytearray(self.sectors(first, last - first + 1))
        at = offset - first * SECTOR
        cur[at:at + len(data)] = data
        self.Put(first, bytes(cur))

    def sectors(self, lba, count):
        if not any((lba + i) in self.over for i in range(count)):
            return self.base.sectors(lba, count)
        out = bytearray()
        for i in range(count):
            s = self.over.get(lba + i)
            out += s if s is not None else self.base.sectors(lba + i, 1)
        return bytes(out)

    def SizeSectors(self):
        return self.base.SizeSectors()

    def WriteOut(self, out_path, never=()):
        """The overlaid medium as a new file: the base cloned (`cp -c`, a
        copy-on-write clone on APFS) or, where cloning is refused, streamed —
        then the overlay's sectors written into it, in order. `never` is
        [(path, what)] that out_path must not be; the base is always one.
        Returns True when the base was cloned rather than copied."""
        for other, what in ((self.path, "the base"),) + tuple(never):
            if os.path.exists(out_path) and _SameFile(out_path, other):
                raise ValueError("%s is %s; it would be written over" % (out_path, what))
        if os.path.exists(out_path):
            os.unlink(out_path)
        cloned = subprocess.run(["cp", "-c", self.path, out_path], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL).returncode == 0
        if not cloned:
            with open(self.path, "rb") as src, open(out_path, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
        keys = sorted(self.over)
        with open(out_path, "r+b") as dst:
            i = 0
            while i < len(keys):
                j = i
                while j + 1 < len(keys) and keys[j + 1] == keys[j] + 1:
                    j += 1
                dst.seek(keys[i] * SECTOR)
                dst.write(b"".join(bytes(self.over[k]) for k in keys[i:j + 1]))
                i = j + 1
        return cloned


def _SameFile(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.realpath(a) == os.path.realpath(b)


def has_deed(medium, lba):
    """Does a Deed start at this sector?"""
    try:
        return medium.sectors(lba, 1)[0:8] == DEED_MAGIC
    except IOError:
        return False


def candidate_devices():
    """Raw devices that could carry a BoxOS volume, system disk excluded.

    The system disk is excluded by name, not by hope: this is a read-only
    tool, but a scan that opens the disk the host is running from is a scan
    that will one day be copied into something that is not."""
    import glob
    import subprocess

    out = []
    if sys.platform == "darwin":
        # diskutil knows which disks are external; asking it is cheaper and
        # narrower than opening every raw device to find out.
        try:
            listing = subprocess.run(
                ["diskutil", "list", "-plist", "external", "physical"],
                capture_output=True, text=True, timeout=10).stdout
            import plistlib
            plist = plistlib.loads(listing.encode())
            for disk in plist.get("AllDisks", []):
                out.append("/dev/r" + disk)
        except Exception:
            out = []
        if not out:
            out = [d.replace("/dev/disk", "/dev/rdisk")
                   for d in sorted(glob.glob("/dev/disk[1-9]*"))]
    else:
        for pat in ("/dev/sd[b-z]", "/dev/sd[b-z][0-9]*",
                    "/dev/nvme[1-9]n[0-9]*", "/dev/mmcblk[1-9]*"):
            out.extend(sorted(glob.glob(pat)))

    return [d for d in out
            if not d.startswith(("/dev/rdisk0", "/dev/disk0",
                                 "/dev/sda", "/dev/nvme0n1"))]


def find_the_stick():
    """The one medium here that carries a BoxOS volume.

    Returns (path, Medium). Refuses on two answers rather than guessing:
    none found, or more than one — because picking one of two sticks by
    enumeration order is how the wrong prog run gets analysed."""
    seen, denied = {}, []
    for dev in candidate_devices():
        try:
            m = Medium(dev)
        except PermissionError:
            denied.append(dev)
            continue
        except OSError:
            continue
        try:
            ground, _how = find_ground(m, None)
            uuid = read_deed(m, ground)["uuid"]
        except Exception:
            m.f.close()
            continue

        # ‼ COUNT VOLUMES, NOT PATHS. A stick answers to its whole disk AND
        # to the partition inside it (/dev/rdisk5 and /dev/rdisk5s1), and the
        # Deed found through either is the same Deed — one volume seen twice,
        # not two volumes. The volume's own uuid is what says so; the paths
        # cannot. Whole-disk wins the tie because its sector 0 is the one
        # every other tool here counts from.
        prev = seen.get(uuid)
        if prev is None:
            seen[uuid] = (dev, m)
        elif len(dev) < len(prev[0]):
            prev[1].f.close()
            seen[uuid] = (dev, m)
        else:
            m.f.close()

    found = list(seen.values())
    if len(found) == 1:
        return found[0]
    for _d, m in found:
        m.f.close()
    if len(found) > 1:
        sys.exit("more than one BoxOS volume is plugged in: "
                 + ", ".join(d for d, _m in found)
                 + "\nname the one you mean instead of 'auto'.")
    if denied:
        sys.exit(permission_advice(denied[0]))
    sys.exit("no BoxOS volume found on any external medium.\n"
             "is the stick plugged in? `diskutil list` / `lsblk` will say.")


def permission_advice(dev):
    """The refusal that names its own cure, once, instead of 'use sudo'."""
    if sys.platform != "darwin":
        return (f"cannot open {dev}: permission denied.\n"
                f"add yourself to the 'disk' group, then log in again:\n"
                f"  sudo usermod -aG disk $USER")
    return (f"cannot open {dev}: permission denied.\n"
            f"\n"
            f"macOS keeps raw devices as root:operator, mode 0640 — readable\n"
            f"by the 'operator' group and nobody else. Join it ONCE and this\n"
            f"tool never needs sudo again:\n"
            f"\n"
            f"  sudo dseditgroup -o edit -a $(whoami) -t user operator\n"
            f"\n"
            f"Measured on this desk: it takes for new processes at once, no\n"
            f"logout needed — but if this still refuses, log in again. 0640\n"
            f"has no write bit for the group, so\n"
            f"this grants READING raw disks and nothing else — `make usb`\n"
            f"still asks for sudo, because writing is a different question.")


def GroundCandidates(medium):
    """Every place a volume could begin: [(start, sectors|None, how, kernels)].

    GPT entries (UEFI 2.10 §5.3) give `last - first + 1` sectors, MBR entries
    give their count, and the bare medium gives its own length. `kernels` says
    whether the kernel would stand there at all: it takes an MBR entry of type
    0x7f, or a GPT entry of the BoxOS type behind a protective MBR
    (ground.c:696-731, 500) — and never a medium with no table. This tool
    looks further than the kernel does on purpose, because it exists to get
    evidence off volumes the kernel will not take."""
    candidates = []

    try:
        mbr = medium.sectors(0, 1)
    except IOError:
        mbr = b""
    have_mbr = mbr[510:512] == b"\x55\xaa"
    protective = have_mbr and any(
        mbr[446 + i * 16 + 4] == MBR_TYPE_PROTECTIVE for i in range(4))

    try:
        hdr = medium.sectors(1, 1)
    except IOError:
        hdr = b""
    if hdr[0:8] == b"EFI PART":
        entry_lba, entry_count, entry_size = struct.unpack_from("<QII", hdr, 72)
        if 0 < entry_count <= 512 and 128 <= entry_size <= 1024:
            want = (entry_count * entry_size + SECTOR - 1) // SECTOR
            table = medium.sectors(entry_lba, want)
            for i in range(entry_count):
                e = table[i * entry_size:(i + 1) * entry_size]
                if len(e) < 56 or e[0:16] == bytes(16):
                    continue
                first, last = struct.unpack_from("<QQ", e, 32)
                named = e[0:16] == BOXOS_TYPE_GUID
                candidates.append((first, last - first + 1 if last >= first else None,
                                   f"GPT entry {i}" + (" (BoxOS type)" if named else ""),
                                   protective and named))

    if have_mbr:
        for i in range(4):
            e = mbr[446 + i * 16:446 + (i + 1) * 16]
            first, count = struct.unpack_from("<II", e, 8)
            if first:
                candidates.append((first, count, f"MBR entry {i} (type 0x{e[4]:02x})",
                                   not protective and e[4] == MBR_TYPE_BOXOS and count > 0))

    candidates.append((0, medium.SizeSectors(),
                       "the medium itself, with no partition table", False))
    return candidates


def find_ground(medium, forced_start):
    """Where the BoxOS volume begins, in 512-byte sectors from the medium's 0.

    ‼ Candidates come from the partition table; the ANSWER comes from the
    medium. Every candidate is confirmed by reading its first sector and
    finding the Deed's magic there, because a partition type byte is a claim
    and the magic is the thing itself. The first version of this trusted the
    type byte, looked for 0x83, and found nothing on an image whose BoxOS
    partition is type 0x7f — a tool for recovering evidence that fails on the
    ordinary case is worse than none.
    """
    if forced_start is not None:
        return forced_start, "you said so"

    candidates = GroundCandidates(medium)
    for lba, _sectors, how, _kernels in candidates:
        if has_deed(medium, lba):
            return lba, how

    tried = ", ".join(f"{lba}" for lba, _s, _h, _k in candidates)
    raise ValueError(
        "no Deed found at any partition start on this medium "
        f"(looked at sector(s): {tried}). If the partition table is damaged, "
        "point --start at the ground yourself.")


def read_deed(medium, ground):
    """The Deed at the head of the ground: identity, extent, and the layout.

    Deliberately forgiving: it is what `list`, `get` and `deed` read a volume
    through, and those exist to get evidence off volumes that may not mount.
    The census judges the same bytes strictly (StandOnDeed)."""
    raw = medium.sectors(ground, DEED_SECTORS)
    if raw[0:8] != DEED_MAGIC:
        raise ValueError(
            f"no Deed at sector {ground} — this is not a BoxOS ground "
            f"(first eight bytes: {raw[0:8]!r})")
    deed = ondisk.VOLUME_DEED.Unpack(raw)
    parsed = ondisk.ReadDeed(raw)
    layout = parsed["layout"]
    if layout is None and parsed["why"] is not None:
        # A Deed whose checksum is off still says where things are: read its
        # stamps without the checksum, which is what this reader always did.
        resealed = ondisk.ReadDeed(ondisk.DeedSeal(raw))
        layout = resealed["layout"]
    if layout is None:
        raise ValueError("the Deed carries no layout stamp — nothing here can "
                         "say where the files are")
    deed["layout"] = layout
    return deed


def _ReadCopy(medium, lba):
    try:
        return ondisk.ReadDeed(medium.sectors(lba, DEED_SECTORS))
    except IOError:
        return None


def _HeadAt(medium, start, ground_sectors):
    """DeedReadHead (deed.c:116-156): a valid Deed of role HEAD, no longer than
    its ground, whose far copy lies inside itself. (deed, why)."""
    d = _ReadCopy(medium, start)
    if d is None:
        return None, "the medium ends inside the deed at the head"
    if not d["ok"]:
        return None, d["why"]
    if d["role"] != DEED_ROLE_HEAD:
        return None, "the copy at the head says it is not the head (role %u)" % d["role"]
    if ground_sectors is not None and d["sectors"] > ground_sectors:
        return None, ("the volume claims %u sectors and the ground it stands on "
                      "has %u" % (d["sectors"], ground_sectors))
    if d["tail_sector"] == 0 or d["tail_sector"] + DEED_SECTORS > d["sectors"]:
        return None, "it puts its own far copy at sector %u, which is not inside itself" % d["tail_sector"]
    return d, None


def _TailAlone(medium, start, ground_sectors):
    """DeedReadTailAlone (deed.c:203-229): a valid Deed of role TAIL in the
    last whole block of the ground, no longer than it."""
    if not ground_sectors or ground_sectors // DEED_SECTORS == 0:
        return None
    lba = start + (ground_sectors // DEED_SECTORS - 1) * DEED_SECTORS
    d = _ReadCopy(medium, lba)
    if d is None or not d["ok"] or d["role"] != DEED_ROLE_TAIL:
        return None
    if d["sectors"] > ground_sectors:
        return None
    d["at_sector"] = lba - start
    return d


class Stand:
    """Where the volume stands: which ground, which copy of its Deed, and what
    the far copy says."""

    __slots__ = ("start", "sectors", "how", "kernels", "head", "head_why",
                 "stood_on", "deed", "tail", "tail_why", "far_end")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))


def StandOnDeed(medium, forced_start):
    """The ground and the Deed the kernel would stand on (tagfs.c:1082-1142).

    Each candidate's head is tried with all of DeedReadHead; failing that, the
    far copy alone at the ground's last block. The first candidate that yields
    either wins, the one the kernel itself would take ahead of the others.
    With the head chosen, the far copy it names is read and checked the way
    DeedReadTail checks it (deed.c:158-201); with or without it, the ground's
    last block is read too, because a growth cut between its two writes leaves
    the new far copy THERE (deed.c:177-198)."""
    if forced_start is not None:
        candidates = [(forced_start, None, "you said so", True)]
    else:
        candidates = GroundCandidates(medium)
        # The kernel's own grounds first, then everything this tool looks at.
        candidates.sort(key=lambda c: not c[3])

    for start, sectors, how, kernels in candidates:
        head, head_why = _HeadAt(medium, start, sectors)
        if head is not None:
            stand = Stand(start=start, sectors=sectors, how=how, kernels=kernels,
                          head=head, stood_on="head", deed=head)
        else:
            tail = _TailAlone(medium, start, sectors)
            if tail is None:
                continue
            stand = Stand(start=start, sectors=sectors, how=how, kernels=kernels,
                          head=None, head_why=head_why, stood_on="tail", deed=tail)
        if stand.deed["layout"] is None:
            raise Unreadable("the deed at sector %u carries no layout stamp this "
                             "tool understands — nothing can say where anything is"
                             % start)
        _ReadFarCopies(medium, stand)
        return stand

    tried = ", ".join(str(c[0]) for c in candidates)
    raise Unreadable("no deed this kernel could stand on at any candidate ground "
                     "(sector(s) %s), head or far copy" % tried)


def _ReadFarCopies(medium, stand):
    """Fill stand.tail/tail_why (the copy the head names) and stand.far_end (a
    valid far copy of this volume at the ground's end claiming more ground than
    the head: a growth caught between its two writes)."""
    deed = stand.deed
    if stand.stood_on == "head":
        t = _ReadCopy(medium, stand.start + deed["tail_sector"])
        if t is None:
            stand.tail_why = "the medium ends inside it"
        elif not t["ok"]:
            stand.tail_why = t["why"]
        elif t["role"] != DEED_ROLE_TAIL:
            stand.tail_why = "the copy there says it is not the far copy (role %u)" % t["role"]
        elif t["uuid"] != deed["uuid"]:
            stand.tail_why = "it belongs to a different volume"
        elif t["sectors"] != deed["sectors"]:
            stand.tail_why = ("it says %u sectors and the head says %u"
                              % (t["sectors"], deed["sectors"]))
        else:
            stand.tail = t
    end = _TailAlone(medium, stand.start, stand.sectors)
    if (end is not None and end["uuid"] == deed["uuid"] and
            end["sectors"] > deed["sectors"]):
        stand.far_end = end


# ---------------------------------------------------------------------------
# VolumeFacts — the volume read once, the way the mount reads it
# ---------------------------------------------------------------------------


class VolumeFacts:
    """Everything read off the medium, once, with nothing judged yet."""

    def __init__(self, medium, forced_start):
        self.medium = medium
        self.stand = StandOnDeed(medium, forced_start)
        self.base = self.stand.start
        self.deed = self.stand.deed
        self.layout = dict(self.deed["layout"])
        self.geometry = self.deed["geometry"]
        self._cache = {}
        self.region = self._ReadBitmapRegion()
        self.growth, self.growth_why = self._PredictGrowth()
        if self.growth:
            self.layout.update(total_blocks=self.growth["blocks"],
                               data_blocks=self.growth["data_blocks"])
        self.D = self.layout["data_blocks"]
        self.bits = bytes(self.region[:(self.D + 7) // 8]).ljust((self.D + 7) // 8, b"\0")
        self.used = RunsFromBits(self.bits, self.D)
        self._ReadLedger()
        self._ReadDiskBook()
        self._ReadCow()
        self._ReadChains()

    # --- reading ---------------------------------------------------------

    def vread(self, vblock, sectors=SECTORS_PER_BLOCK):
        try:
            return self.medium.sectors(self.base + vblock * SECTORS_PER_BLOCK, sectors)
        except IOError:
            return None

    def read_block(self, b):
        """A block of the data run, or None where the medium ends. Unbounded
        by the run on purpose: tagfs_read_block bounds nothing (tagfs.c:1982)."""
        if b not in self._cache:
            self._cache[b] = self.vread(self.layout["data_block"] + b)
        return self._cache[b]

    def Head(self, volume_block):
        return (volume_block - self.layout["data_block"]) & 0xFFFFFFFF

    def _ReadBitmapRegion(self):
        lay = self.layout
        raw = self.vread(lay["block_bitmap_block"], lay["block_bitmap_blocks"] * SECTORS_PER_BLOCK)
        return raw if raw is not None else b""

    def _PredictGrowth(self):
        """volume_take_more_ground (tagfs.c:828-1071), up to its commit. None
        and a sentence when nothing is taken."""
        st, deed, lay = self.stand, self.deed, self.layout
        if st.stood_on != "head":
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
            self.ledgers.append(d)
        self.ledger = ondisk.LedgerChoice(self.ledgers)

    def _ReadDiskBook(self):
        db = self.layout["disk_book_block"]
        records_sector = (db + 2) * SECTORS_PER_BLOCK
        heads = [self.vread(db + i, 1) for i in (0, 1)]
        self.diskbook = [ondisk.ReadDiskBookHead(h, records_sector) if h else None
                         for h in heads]
        # read_superblock_into (disk_book.c:76-90): the head, else its copy.
        self.book = None
        for h in self.diskbook:
            if h is not None and h["valid"]:
                self.book = h
                break
        self.book_ok = self.book is not None and self.book["acceptable"]
        self.book_records = []
        if self.book_ok:
            for i in range(self.book["count"]):
                sector = records_sector + i * ondisk.DISKBOOK_SECTORS_PER_ENTRY
                try:
                    raw = self.medium.sectors(self.base + sector, ondisk.DISKBOOK_SECTORS_PER_ENTRY)
                except IOError:
                    break                        # replay stops at a failed read (disk_book.c:173-174)
                self.book_records.append(ondisk.ReadDiskBookEntry(raw))

    def _ReadCow(self):
        self.manifests, self.snapshots = [], set()
        led = self.ledger or {}
        for which in ("cow_manifest_block", "cow_manifest_backup_block"):
            b = led.get(which, 0)
            if not b:
                continue
            raw = self.read_block(b)
            m = ondisk.ReadManifest(raw) if raw is not None else None
            self.manifests.append((which, b, m))
            if which == "cow_manifest_block" and m is not None and m["ok"]:
                self.snapshots = {e["snapshot_id"] for e in m["entries"]}
        # DiskBookValidateAndReplay (disk_book.c:166-193) into the snapshots the
        # manifest restored; a reformatted book has nothing to replay.
        self.redirects, self.stray = [], []
        for e in self.book_records:
            if not e["valid"] or e["type"] != DISKBOOK_TYPE_REDIRECT:
                continue
            if e["old_block"] == 0 or e["new_block"] == 0:
                continue
            item = (e["snapshot_id"], e["old_block"], e["new_block"])
            (self.redirects if e["snapshot_id"] in self.snapshots else self.stray).append(item)

    def _ReadChains(self):
        lay, D = self.layout, self.D
        self.chains, self.refused = {}, {}
        for name, head, magic in (("registry", lay["tag_registry_block"], REGISTRY_MAGIC),
                                  ("table", lay["file_table_block"], FILETBL_MAGIC),
                                  ("pool", lay["metadata_pool_block"], MPOOL_MAGIC)):
            blocks, raws, why = ondisk.WalkChain(self.read_block, self.Head(head), magic, D)
            self.chains[name] = (blocks, raws)
            if why:
                self.refused[name] = why
        self.registry = [(b, ondisk.ReadRegistryBlock(r)) for b, r in zip(*self.chains["registry"])]
        self.rows = ondisk.TableRows(self.chains["table"][1])
        self.pool = [(b, ondisk.ReadPoolBlock(r), r) for b, r in zip(*self.chains["pool"])]
        # Every record in append order, and the newest of each file with no
        # capacity: the truth view (the mount's mirror is KernelView's).
        self.records = [ondisk.ParseRecord(raw, at, b)
                        for b, blk, raw in self.pool for at, _l, _f in blk["mirror_walk"]]
        self.newest = {}
        for r in self.records:
            if r.fid and r.unpack_ok:
                self.newest[r.fid] = r
