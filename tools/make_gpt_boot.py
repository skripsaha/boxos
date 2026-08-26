#!/usr/bin/env python3
"""
make_gpt_boot.py — turn the MBR image the build produces into the same machine
on a GPT disk, and prove the BIOS path can still start it.

Why this exists
---------------
BoxOS lays its own image out with an MBR, and both loaders read that. A disk
that already belongs to somebody is the other case, and on any such disk made
this decade the table is a GPT — so "can BoxOS boot from a partition on a GPT
disk" is a question about real machines, not a hypothetical.

Two things stand in the way of it, and both are answered here rather than
argued about:

  * Sector 1 is the GPT header and the sectors after it are its entry array,
    so stage2 cannot live at sector 1 the way it does on our own image. It goes
    into a BIOS boot partition, and stage1 is TOLD where — the address is a
    field at offset 432 of the boot sector, which this writes.

  * The partition table stage2 reads is a GPT, with two checksums that both
    have to be verified before anything in it is believed. That is what the
    disk below exercises: a real header, a real 16 KiB entry array, real sums.

    ./tools/make_gpt_boot.py build/boxos.img <out.img>

The volume itself is copied across unchanged, at the same sector it already
occupies — which is the point. Nothing inside a volume knows what kind of
table the medium carries.
"""
import binascii
import struct
import sys
import uuid

SECTOR = 512
GPT_ENTRIES = 128
GPT_ENTRY_BYTES = 128
GPT_ARRAY_SECTORS = (GPT_ENTRIES * GPT_ENTRY_BYTES) // SECTOR   # 32

# Where the pieces go on the GPT disk. The array ends at sector 33, so stage2
# starts at 34 — the first sector that is nobody else's.
STAGE2_LBA = 34
STAGE2_SECTORS = 16
FIRST_USABLE = 2048           # the volume stays exactly where it was

STAGE2_LBA_FIELD = 432        # the field in stage1, see src/boot/stage1/stage1.asm

# Permanent, never reissued — the same sixteen bytes ground.c, tagboot.c and
# stage2.asm each compare against.
BOXOS_TYPE = uuid.UUID('cf8ae49a-d26a-4959-9a6f-71c932e0c9eb')
ESP_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b')
# The one GRUB uses for the same job, so a partitioning tool that meets this
# disk recognises what the run before the volume is for.
BIOS_BOOT_TYPE = uuid.UUID('21686148-6449-6e6f-744e-656564454649')


def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


def gpt_entry(type_guid, first, last, name):
    e = bytearray(GPT_ENTRY_BYTES)
    e[0:16] = type_guid.bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    e[32:40] = struct.pack('<Q', first)
    e[40:48] = struct.pack('<Q', last)
    e[56:56 + len(name) * 2] = name.encode('utf-16-le')
    return bytes(e)


def gpt_header(my_lba, alt_lba, entry_lba, first_usable, last_usable,
               disk_guid, array_crc):
    h = bytearray(92)
    h[0:8] = b'EFI PART'
    h[8:12] = struct.pack('<I', 0x00010000)
    h[12:16] = struct.pack('<I', 92)
    h[24:32] = struct.pack('<Q', my_lba)
    h[32:40] = struct.pack('<Q', alt_lba)
    h[40:48] = struct.pack('<Q', first_usable)
    h[48:56] = struct.pack('<Q', last_usable)
    h[56:72] = disk_guid.bytes_le
    h[72:80] = struct.pack('<Q', entry_lba)
    h[80:84] = struct.pack('<I', GPT_ENTRIES)
    h[84:88] = struct.pack('<I', GPT_ENTRY_BYTES)
    h[88:92] = struct.pack('<I', array_crc)
    h[16:20] = struct.pack('<I', crc32(bytes(h)))
    return bytes(h)


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip())
        return 1

    src = open(sys.argv[1], 'rb').read()
    total = len(src) // SECTOR
    if total < FIRST_USABLE:
        print('the source image is shorter than the volume it should contain')
        return 1

    disk = bytearray(src)

    # ── the protective MBR ────────────────────────────────────────────────
    # stage1's code is kept: the firmware still loads and runs it. What it
    # loses is the partition table, which becomes one entry covering the whole
    # disk so that a tool knowing only MBR sees the disk as taken.
    stage2 = bytes(src[SECTOR:SECTOR * (1 + STAGE2_SECTORS)])

    covered = min(total - 1, 0xFFFFFFFF)
    entry = bytearray(16)
    entry[4] = 0xEE
    entry[8:12] = struct.pack('<I', 1)
    entry[12:16] = struct.pack('<I', covered)
    disk[446:462] = entry
    disk[462:510] = b'\0' * 48
    disk[510:512] = b'\x55\xAA'

    # ── tell stage1 where stage2 now is ───────────────────────────────────
    disk[STAGE2_LBA_FIELD:STAGE2_LBA_FIELD + 8] = struct.pack('<Q', STAGE2_LBA)

    # ── stage2 into the BIOS boot partition ───────────────────────────────
    off = STAGE2_LBA * SECTOR
    disk[off:off + len(stage2)] = stage2
    # and out of where it used to be, so a boot that still reads sector 1 fails
    # loudly instead of working by accident.
    disk[SECTOR:SECTOR * (1 + STAGE2_SECTORS)] = b'\0' * (STAGE2_SECTORS * SECTOR)

    # ── the GPT ───────────────────────────────────────────────────────────
    # Everything from the volume on is left exactly where it was; the entries
    # simply describe it.
    # The ESP is whatever follows the volume in the source image: the build
    # puts it at a 2048-aligned sector, and the volume ends where it begins.
    esp_first = 51200 if total > 51200 else None

    entries = [
        gpt_entry(BIOS_BOOT_TYPE, STAGE2_LBA, STAGE2_LBA + STAGE2_SECTORS - 1,
                  'BoxOS loader'),
        gpt_entry(BOXOS_TYPE, FIRST_USABLE,
                  (esp_first - 1) if esp_first else (total - 34),
                  'BoxOS'),
    ]
    if esp_first:
        entries.append(gpt_entry(ESP_TYPE, esp_first, total - 34, 'EFI'))

    array = b''.join(entries)
    array += b'\0' * (GPT_ENTRIES * GPT_ENTRY_BYTES - len(array))
    array_crc = crc32(array)

    disk_guid = uuid.uuid4()
    primary_entry_lba = 2
    backup_entry_lba = total - 1 - GPT_ARRAY_SECTORS
    first_usable = 2 + GPT_ARRAY_SECTORS
    last_usable = backup_entry_lba - 1

    disk[SECTOR:SECTOR + 92] = gpt_header(1, total - 1, primary_entry_lba,
                                          first_usable, last_usable,
                                          disk_guid, array_crc)
    disk[primary_entry_lba * SECTOR:primary_entry_lba * SECTOR + len(array)] = array

    disk[backup_entry_lba * SECTOR:backup_entry_lba * SECTOR + len(array)] = array
    disk[(total - 1) * SECTOR:(total - 1) * SECTOR + 92] = gpt_header(
        total - 1, 1, backup_entry_lba, first_usable, last_usable,
        disk_guid, array_crc)

    open(sys.argv[2], 'wb').write(bytes(disk))
    print(f'{sys.argv[2]}: GPT disk, {total} sectors')
    print(f'  stage1  sector 0, told stage2 is at {STAGE2_LBA}')
    print(f'  stage2  sectors {STAGE2_LBA}..{STAGE2_LBA + STAGE2_SECTORS - 1}'
          f'  (BIOS boot partition)')
    print(f'  volume  sectors {FIRST_USABLE}..'
          f'{(esp_first - 1) if esp_first else total - 34}')
    if esp_first:
        print(f'  ESP     sectors {esp_first}..{total - 34}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
