#!/usr/bin/env python3
"""
make_mbr.py — write an MBR partition table into a raw disk image.

Why a raw BoxOS image needs one at all
--------------------------------------
Without a partition table the first sector of a USB stick is just 512 bytes
ending in 0xAA55, and firmware has to guess what kind of device it is looking
at. Guessing is not hypothetical: the guess a modern BIOS makes for an
unpartitioned stick is commonly USB-FDD, which presents the media as a 1.44 MB
floppy — 2880 sectors — while the BoxOS kernel lives at sectors 2088..3272 and
runs off the end of that emulation partway through loading. A valid table with
a bootable entry is what makes the same firmware present the stick as a hard
disk with the whole of it addressable.

It is also how UEFI finds an EFI System Partition on the same stick, so one
piece of media boots both ways — which is the only arrangement a real user
ever holds in their hand.

What this writes, and what it refuses to write
----------------------------------------------
Bytes 446..509 of sector 0, exactly: four 16-byte records. The boot code in
bytes 0..445 and the 0xAA55 signature are left alone, and the tool refuses to
run if either is not what it expects — in particular, if anything non-zero is
already sitting in the table area, that is a boot sector whose code has grown
into the space and the answer is to shrink it, not to overwrite it here.

Every geometry field is filled with the 255-head, 63-sector translation that
every BIOS since the mid-90s uses, clamped to the (1023, 254, 63) marker once
a partition runs past what CHS can name. Modern firmware reads the LBA fields
and ignores these; firmware that reads them gets an answer that agrees with
the LBA fields rather than zeros.

Usage:
  make_mbr.py IMAGE start,count,type[,boot] [ ... up to four ]

  start  — first LBA of the partition (decimal)
  count  — length in 512-byte sectors (decimal)
  type   — MBR partition type byte (hex, e.g. ef for an ESP)
  boot   — the literal word "boot" to set the 0x80 active flag
"""
import sys
import os

SECTOR = 512
TABLE_OFFSET = 446
ENTRY_SIZE = 16
MAX_ENTRIES = 4

# The translation geometry every BIOS has agreed on since LBA-assist existed.
# It is a fiction — the media has no heads — but it is the fiction the fields
# are defined in, and filling them with a consistent one beats filling them
# with zeros that contradict the LBA fields beside them.
HEADS = 255
SECTORS_PER_TRACK = 63


def chs(lba):
    """LBA → (head, sector, cylinder) packed as the three bytes MBR wants.

    Returns the (1023, 254, 63) saturation marker for anything CHS cannot
    name, which is what fdisk writes and what firmware is expected to read as
    'use the LBA fields'.
    """
    cyl = lba // (HEADS * SECTORS_PER_TRACK)
    rem = lba % (HEADS * SECTORS_PER_TRACK)
    head = rem // SECTORS_PER_TRACK
    sec = rem % SECTORS_PER_TRACK + 1          # sectors are 1-based
    if cyl > 1023:
        cyl, head, sec = 1023, 254, 63
    return bytes([head & 0xFF,
                  (sec & 0x3F) | ((cyl >> 2) & 0xC0),
                  cyl & 0xFF])


def parse(spec):
    parts = spec.split(',')
    if len(parts) not in (3, 4):
        raise ValueError(f"malformed partition spec: {spec!r}")
    start, count = int(parts[0], 0), int(parts[1], 0)
    ptype = int(parts[2], 16)
    boot = False
    if len(parts) == 4:
        if parts[3] != 'boot':
            raise ValueError(f"fourth field must be the word 'boot', got {parts[3]!r}")
        boot = True
    if start < 1:
        raise ValueError(f"partition starts at LBA {start}: sector 0 is the MBR itself")
    if count < 1:
        raise ValueError("a zero-length partition describes nothing")
    if not 0 <= ptype <= 0xFF:
        raise ValueError(f"partition type {ptype:#x} is not a byte")
    return start, count, ptype, boot


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    image = argv[1]
    specs = [parse(s) for s in argv[2:]]

    if len(specs) > MAX_ENTRIES:
        sys.stderr.write(f"ERROR: an MBR holds {MAX_ENTRIES} partitions, given {len(specs)}\n")
        return 1
    if sum(1 for s in specs if s[3]) > 1:
        sys.stderr.write("ERROR: more than one partition marked bootable; firmware "
                         "picks one and the choice would not be ours\n")
        return 1

    img_sectors = os.path.getsize(image) // SECTOR

    # Overlap and bounds. Checked pairwise rather than by sorting, because the
    # order of the entries is meaningful to firmware and must survive.
    for i, (start, count, _, _) in enumerate(specs):
        end = start + count
        if end > img_sectors:
            sys.stderr.write(f"ERROR: partition {i+1} runs to LBA {end} but the image "
                             f"holds {img_sectors} sectors\n")
            return 1
        for j, (ostart, ocount, _, _) in enumerate(specs):
            if j <= i:
                continue
            if start < ostart + ocount and ostart < end:
                sys.stderr.write(f"ERROR: partitions {i+1} and {j+1} overlap\n")
                return 1

    with open(image, 'r+b') as f:
        f.seek(0)
        sector0 = bytearray(f.read(SECTOR))
        if len(sector0) != SECTOR:
            sys.stderr.write("ERROR: image is shorter than one sector\n")
            return 1

        if sector0[510:512] != b'\x55\xAA':
            sys.stderr.write("ERROR: sector 0 has no 0xAA55 signature — this is not a "
                             "boot sector, and writing a partition table into it would "
                             "make it neither one thing nor the other\n")
            return 1

        existing = sector0[TABLE_OFFSET:510]
        if any(existing):
            sys.stderr.write("ERROR: bytes 446..509 of the boot sector are not empty.\n"
                             "       That space is the partition table by definition — "
                             "firmware and every partitioning tool\n"
                             "       will read whatever is there as four partition "
                             "records. The boot code has grown\n"
                             "       into it and must shrink; overwriting it here would "
                             "silently delete boot code.\n")
            return 1

        table = bytearray(64)
        for i, (start, count, ptype, boot) in enumerate(specs):
            e = bytearray(ENTRY_SIZE)
            e[0] = 0x80 if boot else 0x00
            e[1:4] = chs(start)
            e[4] = ptype
            e[5:8] = chs(start + count - 1)
            e[8:12] = start.to_bytes(4, 'little')
            e[12:16] = count.to_bytes(4, 'little')
            table[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = e

        sector0[TABLE_OFFSET:510] = table
        f.seek(0)
        f.write(sector0)

    for i, (start, count, ptype, boot) in enumerate(specs):
        mb = count * SECTOR / (1024 * 1024)
        sys.stdout.write(f"    p{i+1}: LBA {start}..{start + count - 1}  "
                         f"({mb:.1f} MB)  type 0x{ptype:02X}"
                         f"{'  [bootable]' if boot else ''}\n")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
