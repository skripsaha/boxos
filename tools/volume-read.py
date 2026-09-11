#!/usr/bin/env python3
"""volume-read — read a file out of a BoxOS volume, from the host.

A machine on a bench writes its account onto its own volume, and then the
volume is the only copy. `dd` cannot get it back: a file's contents are a list
of extents scattered through the data run, not a range of sectors, and nothing
on a Mac or a Linux box knows how to follow them.

So this follows them. It reads the medium exactly the way TagBoot does on the
way in — partition table, Deed, layout, metadata pool, extents — and it is
deliberately READ ONLY. Nothing here writes a byte to the medium: a tool used
to recover evidence must not be able to damage it.

    tools/volume-read.py DEVICE list
    tools/volume-read.py DEVICE get NAME [-o OUT]
    tools/volume-read.py DEVICE deed

DEVICE is a raw device or an image file:

    sudo tools/volume-read.py /dev/rdisk4 list
    tools/volume-read.py build/boxos.img get watch.log -o watch.log

On macOS use /dev/rdiskN rather than /dev/diskN — the raw node reads without
the buffer cache, and diskN refuses unaligned reads.

The format lives in three places and this is the fourth, which is one too
many; every offset below cites the file it comes from so a drift is findable:
  src/include/volume_deed.h        — the Deed and its stamps
  src/boot/uefi/tagfs_boot.h       — blocks, records, extents
  src/boot/uefi/tagboot.c          — the read path this mirrors
"""

import argparse
import struct
import sys

SECTOR = 512
BLOCK = 4096
SECTORS_PER_BLOCK = BLOCK // SECTOR

DEED_MAGIC = b"BOXDEED\0"
MPOOL_MAGIC = 0x544D504C          # "TMPL"
FILETBL_MAGIC = 0x54465442        # "TFTB"
FTABLE_PER_BLOCK = 510

FILE_ACTIVE = 1 << 0
FILE_TRASHED = 1 << 1

STAMP_LAYOUT = 2

# src/boot/uefi/tagboot.c, g_boxos_type_guid — the partition type that says
# "BoxOS lives here". Bytes as they sit on the medium, not the text form.
BOXOS_TYPE_GUID = bytes([
    0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
    0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb,
])

# Metadata record header — src/boot/uefi/tagfs_boot.h
REC_LEN_OFF = 0
REC_FLAGS_OFF = 6
REC_SIZE_OFF = 10
REC_TAGCOUNT_OFF = 34
REC_EXTCOUNT_OFF = 36
REC_NAMELEN_OFF = 38
REC_VARDATA_OFF = 42


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


def has_deed(medium, lba):
    """Does a Deed start at this sector?"""
    try:
        return medium.sectors(lba, 1)[0:8] == DEED_MAGIC
    except IOError:
        return False


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

    candidates = []

    # GPT: header at LBA 1, entries where it says. UEFI 2.10 §5.3.
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
                first = struct.unpack_from("<Q", e, 32)[0]
                named = e[0:16] == BOXOS_TYPE_GUID
                candidates.append((first, f"GPT entry {i}"
                                          + (" (BoxOS type)" if named else "")))

    # MBR, including the protective one a GPT hides behind.
    try:
        mbr = medium.sectors(0, 1)
    except IOError:
        mbr = b""
    if mbr[510:512] == b"\x55\xaa":
        for i in range(4):
            e = mbr[446 + i * 16:446 + (i + 1) * 16]
            first = struct.unpack_from("<I", e, 8)[0]
            if first:
                candidates.append((first, f"MBR entry {i} (type 0x{e[4]:02x})"))

    # A bare volume with no table at all — an image handed straight to QEMU.
    candidates.append((0, "the medium itself, with no partition table"))

    for lba, how in candidates:
        if has_deed(medium, lba):
            return lba, how

    tried = ", ".join(f"{lba}" for lba, _ in candidates)
    raise ValueError(
        "no Deed found at any partition start on this medium "
        f"(looked at sector(s): {tried}). If the partition table is damaged, "
        "point --start at the ground yourself.")


def read_deed(medium, ground):
    """The Deed at the head of the ground: identity, extent, and the layout."""
    raw = medium.sectors(ground, 1)
    if raw[0:8] != DEED_MAGIC:
        raise ValueError(
            f"no Deed at sector {ground} — this is not a BoxOS ground "
            f"(first eight bytes: {raw[0:8]!r})")

    prologue_bytes, stamp_bytes = struct.unpack_from("<HH", raw, 8)
    crc32 = struct.unpack_from("<I", raw, 12)[0]
    uuid = raw[16:32]
    sectors, tail_sector = struct.unpack_from("<QQ", raw, 32)
    role = struct.unpack_from("<I", raw, 48)[0]

    deed = {
        "uuid": uuid,
        "sectors": sectors,
        "tail_sector": tail_sector,
        "role": role,
        "crc32": crc32,
        "layout": None,
    }

    # Walk the stamps. Each is kind(2) + bytes(2) + payload, and one this tool
    # has never been taught about is stepped over rather than refused — which
    # is the whole reason the Deed is stamps and not fields.
    at = prologue_bytes
    end = prologue_bytes + stamp_bytes
    while at + 4 <= end and at + 4 <= len(raw):
        kind, nbytes = struct.unpack_from("<HH", raw, at)
        body = raw[at + 4:at + 4 + nbytes]
        if kind == STAMP_LAYOUT and len(body) >= 60:
            (total_blocks, state_block, state_blocks,
             tag_registry_block, tag_registry_blocks,
             file_table_block, file_table_blocks,
             metadata_pool_block, metadata_pool_blocks,
             block_bitmap_block, block_bitmap_blocks,
             disk_book_block, disk_book_blocks,
             data_block, data_blocks) = struct.unpack_from("<QIIIIIIIIIIIIII", body, 0)
            deed["layout"] = {
                "total_blocks": total_blocks,
                "metadata_pool_block": metadata_pool_block,
                "file_table_block": file_table_block,
                "data_block": data_block,
                "data_blocks": data_blocks,
            }
        at += 4 + ((nbytes + 3) & ~3)

    if deed["layout"] is None:
        raise ValueError("the Deed carries no layout stamp — nothing here can "
                         "say where the files are")
    return deed


class Volume:
    def __init__(self, medium, ground, deed):
        self.medium = medium
        self.ground = ground
        self.layout = deed["layout"]
        self.deed = deed

    def data_block(self, rel):
        """One 4 KiB block of the DATA RUN, counted from the run's start.

        Mirrors ReadDataBlock in tagboot.c: what a file's metadata carries is
        a block counted from the run, and where the run sits is the Deed's
        business.
        """
        lba = self.ground + (self.layout["data_block"] + rel) * SECTORS_PER_BLOCK
        return self.medium.sectors(lba, SECTORS_PER_BLOCK)

    def volume_block(self, abs_b):
        """One 4 KiB block counted from the START of the volume."""
        lba = self.ground + abs_b * SECTORS_PER_BLOCK
        return self.medium.sectors(lba, SECTORS_PER_BLOCK)

    def _record_at(self, blk, offset):
        """Parse the metadata record sitting at `offset` inside a pool block."""
        if offset + REC_VARDATA_OFF > len(blk):
            return None
        r = blk[offset:]
        record_len = struct.unpack_from("<H", r, REC_LEN_OFF)[0]
        if record_len == 0 or offset + record_len > len(blk):
            return None

        flags = struct.unpack_from("<I", r, REC_FLAGS_OFF)[0]
        size = struct.unpack_from("<Q", r, REC_SIZE_OFF)[0]
        tag_count = struct.unpack_from("<H", r, REC_TAGCOUNT_OFF)[0]
        ext_count = struct.unpack_from("<H", r, REC_EXTCOUNT_OFF)[0]
        name_len = struct.unpack_from("<H", r, REC_NAMELEN_OFF)[0]

        ext_at = REC_VARDATA_OFF + tag_count * 2
        name_at = ext_at + ext_count * 6
        if name_at + name_len > record_len:
            return None

        extents = [struct.unpack_from("<IH", r, ext_at + e * 6)
                   for e in range(ext_count)]

        return {
            "name": r[name_at:name_at + name_len].decode("utf-8", "replace"),
            "size": size,
            "flags": flags,
            "extents": extents,
        }

    def files(self):
        """Every live file, taken from the FILE TABLE.

        ‼ Not from the metadata pool, and the difference is the whole
        correctness of this tool. The pool is append-only: a file that is
        written in several goes leaves a record per go — measured on a log
        poured by `logsave`, four records for one file, sized 0, 4096, 8192 and
        11948 — and EVERY ONE of them still carries the ACTIVE flag. Scanning
        the pool and taking the first match returns the empty one, which is
        what the first version of this did: it reported the file as zero bytes
        and would have been believed.

        The file table is the index that says which record is current. One
        entry per live file, (meta_block, meta_offset), meta_block counted from
        the data run and meta_offset a byte offset into that block.
        """
        out = []
        seen_blocks = set()
        # ‼ TWO COORDINATE SYSTEMS, AND THIS WALK USED TO MIX THEM.
        #
        # file_table_block in the Deed is counted from the START OF THE VOLUME;
        # every next_block in the chain is counted from the START OF THE DATA
        # RUN — that is what mkfs writes and what file_table_flush writes, and
        # tagboot.c:931 does the subtraction for the same reason. Reading the
        # continuation through volume_block() therefore landed in the bitmap
        # region, the magic check failed, and the walk stopped at the first
        # block. MEASURED on a 701-file image: this tool listed 510 files and
        # said nothing about the rest, which is exactly the shape of the kernel
        # defect it exists to check for.
        block = self.layout["file_table_block"] - self.layout["data_block"]

        for _ in range(4096):
            if block in seen_blocks:
                break
            seen_blocks.add(block)

            blk = self.data_block(block)
            magic, next_block, entry_count, _reserved = struct.unpack_from(
                "<IIII", blk, 0)
            if magic != FILETBL_MAGIC:
                break
            if entry_count > FTABLE_PER_BLOCK:
                entry_count = FTABLE_PER_BLOCK

            for i in range(entry_count):
                meta_block, meta_offset = struct.unpack_from(
                    "<II", blk, 16 + i * 8)
                if meta_block == 0 and meta_offset == 0:
                    continue                     # an empty slot, not a file
                pool = self.data_block(meta_block)
                rec = self._record_at(pool, meta_offset)
                if rec is None:
                    continue
                if not (rec["flags"] & FILE_ACTIVE):
                    continue
                if rec["flags"] & FILE_TRASHED:
                    continue
                out.append(rec)

            if next_block == 0:
                break
            block = next_block

        return out

    def contents(self, entry):
        """The file's bytes, following its extents and stopping at its size."""
        out = bytearray()
        for start, count in entry["extents"]:
            for i in range(count):
                if len(out) >= entry["size"]:
                    break
                out += self.data_block(start + i)
            if len(out) >= entry["size"]:
                break
        return bytes(out[:entry["size"]])


def main():
    ap = argparse.ArgumentParser(
        description="Read a file out of a BoxOS volume. Read only.")
    ap.add_argument("device", help="raw device or image file")
    ap.add_argument("command", choices=["list", "get", "deed"])
    ap.add_argument("name", nargs="?", help="file to get")
    ap.add_argument("-o", "--out", help="write to this path instead of stdout")
    ap.add_argument("--start", type=int, default=None,
                    help="ground start in 512-byte sectors, when the "
                         "partition table cannot be trusted")
    args = ap.parse_args()

    try:
        medium = Medium(args.device)
    except OSError as e:
        sys.exit(f"cannot open {args.device}: {e}\n"
                 f"on macOS a raw device usually needs sudo, and /dev/rdiskN "
                 f"rather than /dev/diskN")

    try:
        ground, how = find_ground(medium, args.start)
        deed = read_deed(medium, ground)
    except (ValueError, IOError) as e:
        sys.exit(f"{e}")

    if args.command == "deed":
        uuid = deed["uuid"].hex()
        lay = deed["layout"]
        print(f"ground      : sector {ground} ({how})")
        print(f"volume      : {uuid}")
        print(f"sectors     : {deed['sectors']}")
        print(f"role        : {deed['role']} (1=head, 2=tail)")
        print(f"blocks      : {lay['total_blocks']}")
        print(f"metadata at : block {lay['metadata_pool_block']}")
        print(f"data run    : block {lay['data_block']}, "
              f"{lay['data_blocks']} block(s)")
        return

    vol = Volume(medium, ground, deed)
    files = vol.files()

    if args.command == "list":
        print(f"ground sector {ground} ({how}), "
              f"volume {deed['uuid'].hex()[:16]}…")
        if not files:
            print("no active files")
            return
        width = max(len(f["name"]) for f in files)
        for f in sorted(files, key=lambda x: x["name"]):
            blocks = sum(c for _s, c in f["extents"])
            print(f"  {f['name']:<{width}}  {f['size']:>10} bytes  "
                  f"{len(f['extents'])} extent(s), {blocks} block(s)")
        return

    if not args.name:
        sys.exit("get needs a file name")

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


if __name__ == "__main__":
    main()
