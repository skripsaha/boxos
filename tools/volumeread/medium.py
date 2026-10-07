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
                     MBR_TYPE_BOXOS, MBR_TYPE_PROTECTIVE, GPT_ARRAY_MAX_BYTES,
                     GPT_BLOCK_MAX_BYTES, GROUND_MAX_PER_MEDIUM, LEDGER_COPIES,
                     DISKBOOK_TYPE_REDIRECT, RunsFromBits, RunsIntersect)
from .mount import MountModel


class Unreadable(Exception):
    """The medium carries nothing a census can begin from: no Deed at any
    candidate, no layout, or it ends inside what has to be read first."""


class MediumError(IOError):
    """A sector the medium answers it cannot read — UNC on ATA and AHCI,
    MEDIUM ERROR on a USB disk: what the medium holds, not a refusal
    (boardroom.c BoardReadAnswer), which the mount judges by where it lies.
    Only an overlay a case plants one in raises it: the host hears an EIO,
    which says neither, and is taken as a read refused."""


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
        self.bad, self.refusing = set(), set()

    def Bad(self, lba, count):
        """Sectors the medium answers it cannot read: a read that touches one
        raises MediumError."""
        self.bad.update(range(lba, lba + count))

    def Refuse(self, lba, count):
        """Sectors whose read the medium refuses — a dropped command, a link
        gone: a read that touches one raises IOError."""
        self.refusing.update(range(lba, lba + count))

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
        if self.bad and any((lba + i) in self.bad for i in range(count)):
            raise MediumError("a sector of %u+%u cannot be read" % (lba, count))
        if self.refusing and any((lba + i) in self.refusing for i in range(count)):
            raise IOError("the read of %u+%u is refused" % (lba, count))
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
        if self.bad or self.refusing:
            raise ValueError("an image file carries no sector the medium cannot read and no "
                             "read it refuses — this overlay is the census's alone")
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


def _GptReadOne(medium, header_block, block_bytes, medium_sectors, said):
    """gpt_read_one: the GPT whose header sits at block `header_block` of a
    medium addressed in `block_bytes` blocks, every check the kernel makes, in
    its order. Returns ("read", [(start, sectors, entry)]) with both in 512-byte
    sectors, "absent" or "damaged"; the kernel's own sentence for a refusal is
    appended to `said`. `medium_sectors` 0 is a length nobody could say, and
    every bound on it is then skipped, as the kernel skips it."""
    ratio = block_bytes // SECTOR
    medium_blocks = medium_sectors // ratio
    at = header_block * ratio
    try:
        header = bytearray(medium.sectors(at, ratio))
    except IOError:
        said.append("sector %u would not be read" % at)
        return "damaged", []
    if header[0:8] != b"EFI PART":
        return "absent", []
    header_bytes = struct.unpack_from("<I", header, 0x0C)[0]
    if not 92 <= header_bytes <= block_bytes:
        said.append("the GPT header at sector %u states a length of %u, which cannot be one"
                    % (at, header_bytes))
        return "damaged", []
    want = struct.unpack_from("<I", header, 0x10)[0]
    header[0x10:0x14] = bytes(4)
    have = ondisk.Crc32(bytes(header[:header_bytes]))
    if have != want:
        said.append("the GPT header at sector %u does not match its own checksum "
                    "(0x%08x against 0x%08x)" % (at, have, want))
        return "damaged", []
    my_lba = struct.unpack_from("<Q", header, 0x18)[0]
    if my_lba != header_block:
        said.append("the GPT header at sector %u is block %u of %u bytes and says it lives at "
                    "block %u" % (at, header_block, block_bytes, my_lba))
        return "damaged", []
    entry_lba, count, size, want = struct.unpack_from("<QIII", header, 0x48)
    if (size < 128 or size & (size - 1) or count == 0 or size > GPT_ARRAY_MAX_BYTES or
            count > GPT_ARRAY_MAX_BYTES // size):
        said.append("its GPT states %u entries of %u bytes, which is not a table the kernel reads"
                    % (count, size))
        return "damaged", []
    array_bytes = count * size
    array_secs = (array_bytes + SECTOR - 1) // SECTOR
    array_at = entry_lba * ratio
    if medium_sectors and (array_at >= medium_sectors or array_at + array_secs > medium_sectors):
        said.append("its GPT puts the entry array at sectors %u..%u and the medium has %u"
                    % (array_at, array_at + array_secs - 1, medium_sectors))
        return "damaged", []
    try:
        array = medium.sectors(array_at, array_secs)
    except IOError:
        said.append("it would not give up its GPT entries")
        return "damaged", []
    have = ondisk.Crc32(array[:array_bytes])
    if have != want:
        said.append("the GPT entries at sector %u do not match their own checksum "
                    "(0x%08x against 0x%08x)" % (array_at, have, want))
        return "damaged", []
    found = []
    for i in range(count):
        e = array[i * size:(i + 1) * size]
        if e[0:16] != BOXOS_TYPE_GUID:
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        if last < first:
            said.append("GPT entry %u ends before it begins" % i)
            continue
        if medium_blocks and last >= medium_blocks:
            said.append("GPT entry %u claims blocks %u..%u and the medium has %u — not using it"
                        % (i, first, last, medium_blocks))
            continue
        if len(found) == GROUND_MAX_PER_MEDIUM:
            said.append("the medium carries more BoxOS ground than the kernel's survey holds (%u)"
                        % GROUND_MAX_PER_MEDIUM)
            break
        found.append((first * ratio, (last - first + 1) * ratio, i))
    if not found:
        said.append("its GPT names %u partition entries, none carrying BoxOS ground" % count)
    return "read", found


def KernelGround(medium):
    """GroundSurvey (ground.c: ground_survey_medium, ground_survey_gpt,
    gpt_read_one) as this tree has it: [(start, sectors, how)] — every run of
    the medium the kernel would stand on, in 512-byte sectors, in its order —
    and the kernel's own sentences for every table, copy and entry it will not
    use. The GPT is tried in every unit the medium could be addressed in,
    512 to 4096 bytes, the primary and then, when the medium says how far it
    runs, the copy at the far end; the first table that checks out is the only
    one the kernel reads."""
    said, medium_sectors = [], medium.SizeSectors() or 0
    try:
        mbr = medium.sectors(0, 1)
    except IOError:
        return [], ["the medium could not be read at all"]
    if mbr[510:512] != b"\x55\xaa":
        return [], ["the medium carries no partition table"]
    entries = [mbr[446 + i * 16:446 + (i + 1) * 16] for i in range(4)]
    if any(e[4] == MBR_TYPE_PROTECTIVE for e in entries):
        block = SECTOR
        while block <= GPT_BLOCK_MAX_BYTES:
            ratio = block // SECTOR
            if medium_sectors and medium_sectors < 2 * ratio:
                break
            unit = "" if block == SECTOR else ", in %u-byte blocks" % block
            verdict, found = _GptReadOne(medium, 1, block, medium_sectors, said)
            if verdict == "read":
                return [(s, n, "GPT entry %u (BoxOS type)%s" % (i, unit)) for s, n, i in found], said
            if medium_sectors:
                far = medium_sectors // ratio - 1
                verdict, found = _GptReadOne(medium, far, block, medium_sectors, said)
                if verdict == "read":
                    return [(s, n, "GPT entry %u (BoxOS type), from the copy at the far end%s"
                             % (i, unit)) for s, n, i in found], said
            block *= 2
        said.append("neither copy of its GPT could be read, in any unit this medium could be "
                    "addressed in" if medium_sectors else
                    "its GPT is damaged, and the medium will not say how far it runs")
        return [], said
    grounds = []
    for i, e in enumerate(entries):
        if e[4] != MBR_TYPE_BOXOS:
            continue
        start, count = struct.unpack_from("<II", e, 8)
        if count == 0:
            said.append("MBR entry %u claims no sectors" % i)
            continue
        if medium_sectors and (start >= medium_sectors or start + count > medium_sectors):
            said.append("MBR entry %u claims sectors %u..%u and the medium has %u — not using it"
                        % (i, start, start + count - 1, medium_sectors))
            continue
        if len(grounds) == GROUND_MAX_PER_MEDIUM:
            break
        grounds.append((start, count, "MBR entry %u (type 0x%02x)" % (i, e[4])))
    if not grounds:
        said.append("its MBR holds no BoxOS ground — its four entries are types %s"
                    % " ".join("%u:0x%02x" % (i, e[4]) for i, e in enumerate(entries)))
    return grounds, said


def GroundCandidates(medium):
    """Every place a volume could begin: ([(start, sectors|None, how, kernels)],
    the kernel's sentences for the ground it will not use).

    The kernel's own ground comes first, kernels=True (KernelGround). Then
    everything else this tool looks at, kernels=False: the primary GPT's
    entries of any type as they stand (UEFI 2.10 §5.3, `last - first + 1`
    sectors), every MBR entry, and the bare medium at its own length. This
    tool looks further than the kernel does on purpose, because it exists to
    get evidence off volumes the kernel will not take."""
    kernel, said = KernelGround(medium)
    candidates = [(s, n, how, True) for s, n, how in kernel]
    taken = {(s, n) for s, n, _how in kernel}

    def Evidence(start, sectors, how):
        if (start, sectors) not in taken:
            taken.add((start, sectors))
            candidates.append((start, sectors, how, False))

    try:
        mbr = medium.sectors(0, 1)
    except IOError:
        mbr = b""
    try:
        hdr = medium.sectors(1, 1)
    except IOError:
        hdr = b""
    if hdr[0:8] == b"EFI PART":
        entry_lba, entry_count, entry_size = struct.unpack_from("<QII", hdr, 72)
        if 0 < entry_count and 128 <= entry_size and entry_count * entry_size <= GPT_ARRAY_MAX_BYTES:
            try:
                table = medium.sectors(entry_lba, (entry_count * entry_size + SECTOR - 1) // SECTOR)
            except IOError:
                table = b""
            for i in range(len(table) // entry_size):
                e = table[i * entry_size:(i + 1) * entry_size]
                if len(e) < 56 or e[0:16] == bytes(16):
                    continue
                first, last = struct.unpack_from("<QQ", e, 32)
                Evidence(first, last - first + 1 if last >= first else None,
                         f"GPT entry {i}" + (" (BoxOS type)" if e[0:16] == BOXOS_TYPE_GUID else ""))
    if mbr[510:512] == b"\x55\xaa":
        for i in range(4):
            e = mbr[446 + i * 16:446 + (i + 1) * 16]
            first, count = struct.unpack_from("<II", e, 8)
            if first:
                Evidence(first, count, f"MBR entry {i} (type 0x{e[4]:02x})")
    Evidence(0, medium.SizeSectors(), "the medium itself, with no partition table")
    return candidates, said


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

    candidates, _said = GroundCandidates(medium)
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
    the far copy says. `kernel_why`, on a ground the kernel does not stand on,
    is why not, in its words."""

    __slots__ = ("start", "sectors", "how", "kernels", "kernel_why", "head_why",
                 "stood_on", "deed", "tail", "tail_why", "far_end")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))


def StandOnDeed(medium, forced_start):
    """The ground and the Deed the kernel would stand on (tagfs.c
    volume_take_ground).

    Each candidate's head is tried with all of DeedReadHead; failing that, the
    far copy alone at the ground's last block. The first candidate that yields
    either wins, the one the kernel itself would take ahead of the others.
    With the head chosen, the far copy it names is read and checked the way
    DeedReadTail checks it (deed.c:158-201); with or without it, the ground's
    last block is read too, because a growth cut between its two writes leaves
    the new far copy THERE (deed.c:177-198)."""
    said = []
    if forced_start is not None:
        candidates = [(forced_start, None, "you said so", True)]
    else:
        candidates, said = GroundCandidates(medium)
    kernel = [c for c in candidates if c[3]]

    for start, sectors, how, kernels in candidates:
        why = None
        if not kernels:
            why = ("the kernel's ground (%s) holds no deed it can read"
                   % "; ".join("%s, sectors %u+%u" % (h, s, n) for s, n, h, _k in kernel)
                   if kernel else "the kernel finds no BoxOS ground on this medium: " + "; ".join(said))
        head, head_why = _HeadAt(medium, start, sectors)
        if head is not None:
            stand = Stand(start=start, sectors=sectors, how=how, kernels=kernels,
                          kernel_why=why, stood_on="head", deed=head)
        else:
            tail = _TailAlone(medium, start, sectors)
            if tail is None:
                continue
            stand = Stand(start=start, sectors=sectors, how=how, kernels=kernels,
                          kernel_why=why, head_why=head_why, stood_on="tail", deed=tail)
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
