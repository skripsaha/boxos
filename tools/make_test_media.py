#!/usr/bin/env python3
"""
Disks to point BoxOS at, when the thing being tested is what it does with a
medium it has never seen.

The kernel's own image is one layout, written by one tool, and every boot reads
it. That proves the path works on the one disk we make. It says nothing about a
GPT disk, a partition belonging to somebody else, a Deed whose checksum is
wrong, or a volume that claims more ground than it was given — and those are
the cases a real machine hands you.

    ./tools/make_test_media.py <outdir>

writes the set, and each one is plugged in the same way:

    make run-bg USB=on CORES=4 MEM=4G
    ./tools/qemu-input.sh raw "drive_add 0 if=none,id=t,file=<outdir>/gpt.img,format=raw"
    ./tools/qemu-input.sh raw "device_add usb-storage,drive=t,id=td"

or, to have it present before the boot-time survey runs, straight to QEMU as a
second IDE drive:

    qemu-system-x86_64 -drive format=raw,file=build/boxos.img,index=0,media=disk \\
                       -drive format=raw,file=<outdir>/deed.img,index=1,media=disk \\
                       -cpu qemu64,+fsgsbase -m 4G -display none \\
                       -serial file:build/serial.log -daemonize

WHAT EACH ONE IS FOR — every one of these has caught or confirmed something:

  gpt.img          a real GPT: protective MBR, header and entry-array CRC32,
                   two BoxOS partitions and one Linux one. Proves the GPT
                   reader finds ours and leaves theirs alone.
  gpt_badhdr.img   one byte flipped in the header checksum.
  gpt_badarr.img   one byte flipped inside an entry.
                   Both must be REFUSED, by name. A reader that only ever
                   succeeds is a reader nobody has tested.
  deed.img         a volume with a Deed at the head of its ground and its copy
                   at the far end, on a 1 MiB-aligned MBR partition.
  deed_crc.img     the Deed's own checksum broken.
  deed_role.img    the head says it is the tail — a read that went to the wrong
                   place, which is otherwise indistinguishable from a good one.
  deed_long.img    the volume claims more sectors than its ground has. This is
                   an image copied short, and without the check it mounts
                   happily and reads into whatever is past its end.
"""

import binascii
import os
import struct
import sys
import time
import uuid

SECTOR = 512

# The permanent GPT type GUID for BoxOS ground — the same sixteen bytes
# core/boardroom/ground.c compares against, in the mixed-endian order GPT
# stores a GUID in, which is the order it appears on the medium.
BOXOS_TYPE_GUID = bytes([0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
                         0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb])
LINUX_TYPE_GUID = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le

MBR_TYPE_BOXOS = 0x7F
MBR_TYPE_PROTECTIVE = 0xEE

DEED_SECTORS = 8            # four kilobytes: one physical block everywhere
DEED_PROLOGUE_BYTES = 52
DEED_ROLE_HEAD = 1
DEED_ROLE_TAIL = 2


def mbr_entry(part_type, first_lba, sectors):
    e = bytearray(16)
    e[4] = part_type
    e[8:12] = struct.pack('<I', first_lba)
    e[12:16] = struct.pack('<I', sectors)
    return bytes(e)


def put_mbr(disk, entries):
    for i, e in enumerate(entries):
        disk[446 + i * 16: 446 + (i + 1) * 16] = e
    disk[510:512] = b'\x55\xaa'


# ── GPT ──────────────────────────────────────────────────────────────────

def gpt_entry(type_guid, first, last, name):
    e = bytearray(128)
    e[0:16] = type_guid
    e[16:32] = uuid.uuid4().bytes_le
    e[32:40] = struct.pack('<Q', first)
    e[40:48] = struct.pack('<Q', last)
    nm = name.encode('utf-16-le')[:72]
    e[56:56 + len(nm)] = nm
    return bytes(e)


def gpt_header(my_lba, alt_lba, entry_lba, entry_count, entry_bytes,
               array_crc, first_usable, last_usable):
    h = bytearray(92)
    h[0:8] = b'EFI PART'
    h[8:12] = struct.pack('<I', 0x00010000)
    h[12:16] = struct.pack('<I', 92)
    h[16:20] = b'\0\0\0\0'                      # zeroed while summing
    h[24:32] = struct.pack('<Q', my_lba)
    h[32:40] = struct.pack('<Q', alt_lba)
    h[40:48] = struct.pack('<Q', first_usable)
    h[48:56] = struct.pack('<Q', last_usable)
    h[56:72] = uuid.uuid4().bytes_le
    h[72:80] = struct.pack('<Q', entry_lba)
    h[80:84] = struct.pack('<I', entry_count)
    h[84:88] = struct.pack('<I', entry_bytes)
    h[88:92] = struct.pack('<I', array_crc)
    h[16:20] = struct.pack('<I', binascii.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h)


def build_gpt(total_sectors=8192):
    disk = bytearray(SECTOR * total_sectors)
    put_mbr(disk, [mbr_entry(MBR_TYPE_PROTECTIVE, 1, total_sectors - 1)])

    count, size = 128, 128
    entries = bytearray(count * size)
    entries[0:128] = gpt_entry(LINUX_TYPE_GUID, 2048, 4095, "somebody else")
    entries[128:256] = gpt_entry(BOXOS_TYPE_GUID, 4096, 6143, "BoxOS")
    entries[256:384] = gpt_entry(BOXOS_TYPE_GUID, 6144, total_sectors - 34,
                                 "BoxOS spare")
    array_crc = binascii.crc32(bytes(entries)) & 0xFFFFFFFF

    array_lba, array_secs = 2, (count * size + SECTOR - 1) // SECTOR
    disk[array_lba * SECTOR: array_lba * SECTOR + len(entries)] = entries

    first_usable = array_lba + array_secs
    last_usable = total_sectors - 2 - array_secs
    disk[SECTOR: SECTOR + 92] = gpt_header(
        1, total_sectors - 1, array_lba, count, size, array_crc,
        first_usable, last_usable)

    back = total_sectors - 1 - array_secs
    disk[back * SECTOR: back * SECTOR + len(entries)] = entries
    disk[(total_sectors - 1) * SECTOR: (total_sectors - 1) * SECTOR + 92] = \
        gpt_header(total_sectors - 1, 1, back, count, size, array_crc,
                   first_usable, last_usable)
    return disk


# ── Deed ─────────────────────────────────────────────────────────────────

def stamp(kind, payload):
    s = struct.pack('<HH', kind, len(payload)) + payload
    return s + b'\0' * ((-len(s)) % 4)      # next four-byte boundary


def build_deed_bytes(vol_uuid, ground_sectors, tail_sector, role):
    stamps = b''
    stamps += stamp(1, struct.pack('<IIII', 512, 4096, 1024 * 1024, 4096))
    # VolumeLayout: total_blocks, then state/registry/ftable/mpool/bitmap/
    # DiskBook/data, each a first block and a count — all in volume blocks.
    stamps += stamp(2, struct.pack('<Q' + 'I' * 14,
                                   1024,          # total_blocks
                                   1, 2,          # state (the Ledger, two copies)
                                   32, 1,         # tag registry
                                   33, 1,         # file table
                                   34, 1,         # metadata pool
                                   4, 1,          # block bitmap
                                   5, 26,         # DiskBook
                                   32, 991))      # data run
    stamps += stamp(3, struct.pack('<Q16s', int(time.time()), b'boxos-mkfs'))
    stamps += stamp(4, struct.pack('<IIII', 64, 128, 520000, 0))

    def prologue(crc):
        return struct.pack('<8sHHI16sQQI', b'BOXDEED\0', DEED_PROLOGUE_BYTES,
                           len(stamps), crc, vol_uuid, ground_sectors,
                           tail_sector, role)

    crc = binascii.crc32(prologue(0) + stamps) & 0xFFFFFFFF
    out = prologue(crc) + stamps
    return out + b'\0' * (DEED_SECTORS * SECTOR - len(out))


def build_deed(total_sectors=16384, ground_start=2048, ground_sectors=8192):
    disk = bytearray(SECTOR * total_sectors)
    put_mbr(disk, [mbr_entry(MBR_TYPE_BOXOS, ground_start, ground_sectors)])

    vol = uuid.uuid4().bytes
    tail = ground_sectors - DEED_SECTORS

    head_at = ground_start
    tail_at = ground_start + tail
    disk[head_at * SECTOR:(head_at + DEED_SECTORS) * SECTOR] = \
        build_deed_bytes(vol, ground_sectors, tail, DEED_ROLE_HEAD)
    disk[tail_at * SECTOR:(tail_at + DEED_SECTORS) * SECTOR] = \
        build_deed_bytes(vol, ground_sectors, tail, DEED_ROLE_TAIL)
    return disk, ground_start


def reseal(disk, at_sector):
    """Recompute a Deed's checksum after changing a field, so the ONE thing
    wrong with the disk is the field and not the sum."""
    base = at_sector * SECTOR
    stamp_bytes = struct.unpack('<H', bytes(disk[base + 10:base + 12]))[0]
    body = bytearray(disk[base: base + DEED_PROLOGUE_BYTES + stamp_bytes])
    body[12:16] = b'\0\0\0\0'
    disk[base + 12: base + 16] = struct.pack(
        '<I', binascii.crc32(bytes(body)) & 0xFFFFFFFF)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    def write(name, data):
        with open(os.path.join(out, name), 'wb') as f:
            f.write(bytes(data))
        print(f"  {name}")

    print("test media:")

    gpt = build_gpt()
    write('gpt.img', gpt)

    bad = bytearray(gpt); bad[SECTOR + 16] ^= 0xFF
    write('gpt_badhdr.img', bad)

    bad = bytearray(gpt); bad[2 * SECTOR + 40] ^= 0xFF
    write('gpt_badarr.img', bad)

    deed, g = build_deed()
    write('deed.img', deed)

    bad = bytearray(deed); bad[g * SECTOR + 12] ^= 0xFF
    write('deed_crc.img', bad)

    bad = bytearray(deed)
    bad[g * SECTOR + 8 + 2 + 2 + 4 + 16 + 8 + 8] = DEED_ROLE_TAIL
    reseal(bad, g)
    write('deed_role.img', bad)

    bad = bytearray(deed)
    bad[g * SECTOR + 32: g * SECTOR + 40] = struct.pack('<Q', 99999)
    reseal(bad, g)
    write('deed_long.img', bad)
    return 0


if __name__ == '__main__':
    sys.exit(main())
