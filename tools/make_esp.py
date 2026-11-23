#!/usr/bin/env python3
"""
make_esp.py — Create a minimal FAT32 EFI System Partition image.

Produces a 33 MB FAT32 raw disk image (the minimum valid FAT32 size)
containing:
  EFI/BOOT/BOOTX64.EFI   — default UEFI boot application
  startup.nsh             — UEFI Shell fallback: executes BOOTX64.EFI
                            when no NVRAM boot entry exists (e.g., fresh vars.fd)

FAT32 is used because:
  - UEFI spec mandates FAT32 for EFI System Partitions (ESP)
  - All UEFI firmware implementations are required to support FAT32
  - FAT16/FAT12 are legacy; using FAT32 is the production-correct choice

BPB layout (at sector 0):
  Bytes 0-511: FAT32 BPB + EBPB + boot signature
  Sectors 1-5: reserved (set to zero)
  Sector 6-7: FAT32 FSInfo + backup boot record
  FAT1 at sector 32 (standard FAT32 reserved count)
  FAT2 mirror after FAT1
  Root cluster (cluster 2) immediately after FAT2
  File data in subsequent clusters

The FAT's sector is the medium's block: 512 bytes, or 4096 on an image laid
out for a 4Kn medium (--sector 4096) — firmware and every other system read
an ESP whose BytesPerSector is its disk's logical block, and the smallest
valid FAT32 of 4096-byte sectors is some 260 MB, as on every 4Kn disk.

Usage:
  python3 tools/make_esp.py <input.efi> <output.img> [hidden_sectors] [--sector 512|4096]
"""
import struct
import sys

# ── Geometry ────────────────────────────────────────────────────────────────
#
# FAT32 requires ≥ 65,525 data clusters (per FAT specification §3.5).
# UEFI firmware validates this; images with fewer clusters are rejected as
# FAT16 or refused outright. One sector a cluster keeps the image the smallest
# FAT32 there is in its sector — about 33 MB of 512-byte sectors, 260 MB of
# 4096-byte ones; fragmentation is irrelevant for a read-only ESP of a few
# files.
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS    = 32       # FAT32 standard (BPB + FSInfo + backup)
NUM_FATS            = 2
ROOT_CLUSTER        = 2        # FAT32: root dir is a cluster chain
MIN_DATA_CLUSTERS   = 70000    # FAT32 minimum is 65525: overshot for safety
FSINFO_BYTES        = 512      # the FSInfo structure, at the head of its sector


class Fat32:
    """The layout of a FAT32 of `sector` bytes a sector."""

    def __init__(self, sector):
        if sector % 512 or not 512 <= sector <= 4096:
            raise ValueError("a FAT's sector is a whole number of 512-byte sectors up to 4096, not %d" % sector)
        self.sector = sector
        fat_sectors = ((MIN_DATA_CLUSTERS + 2) * 4 + sector - 1) // sector
        self.image_sectors = RESERVED_SECTORS + NUM_FATS * fat_sectors + MIN_DATA_CLUSTERS * SECTORS_PER_CLUSTER
        entries = (self.image_sectors - RESERVED_SECTORS) // SECTORS_PER_CLUSTER + 2
        self.fat_sectors = (entries * 4 + sector - 1) // sector
        self.data_start = RESERVED_SECTORS + NUM_FATS * self.fat_sectors
        self.total_clusters = (self.image_sectors - self.data_start) // SECTORS_PER_CLUSTER
        self.image_size = self.image_sectors * sector
        self.cluster_size = sector * SECTORS_PER_CLUSTER
        assert self.total_clusters >= 65525, \
            f"FAT32 requires ≥65525 data clusters, got {self.total_clusters}"

    def cluster_offset(self, clust):
        return (self.data_start + (clust - 2) * SECTORS_PER_CLUSTER) * self.sector


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def make_short_name(base, ext):
    """8.3 FAT directory entry name, space-padded, 11 bytes."""
    return (base.upper()[:8].ljust(8) + ext.upper()[:3].ljust(3)).encode('ascii')


def dir_entry(name83, attr, cluster, size, is_dir=False):
    """Build a 32-byte FAT directory entry."""
    e = bytearray(32)
    e[0:11] = name83
    e[11]   = attr
    struct.pack_into('<H', e, 20, (cluster >> 16) & 0xFFFF)  # FstClusHI
    struct.pack_into('<H', e, 26, cluster & 0xFFFF)           # FstClusLO
    struct.pack_into('<I', e, 28, size if not is_dir else 0)  # FileSize
    return bytes(e)


ATTR_READ_ONLY  = 0x01
ATTR_HIDDEN     = 0x02
ATTR_SYSTEM     = 0x04
ATTR_VOLUME_ID  = 0x08
ATTR_DIRECTORY  = 0x10
ATTR_ARCHIVE    = 0x20


def build_fat32(efi_path: str, out_path: str, hidden_sectors=0, sector=512):
    g = Fat32(sector)
    efi_data    = open(efi_path, 'rb').read()
    startup_nsh = b'@echo -off\r\n\\EFI\\BOOT\\BOOTX64.EFI\r\n'

    def file_clusters(size):
        return max(1, align_up(size, g.cluster_size) // g.cluster_size)

    # Cluster allocation (after ROOT_CLUSTER=2 which holds root dir)
    clust_efi_dir    = 3   # EFI/ directory
    clust_boot_dir   = 4   # EFI/BOOT/ directory
    clust_efi_file   = 5   # BOOTX64.EFI data, possibly multi-cluster
    efi_clust_count  = file_clusters(len(efi_data))
    clust_nsh        = clust_efi_file + efi_clust_count   # startup.nsh

    # ── FAT32 BPB ────────────────────────────────────────────────────────────
    bpb = bytearray(g.sector)
    bpb[0:3]  = b'\xEB\x58\x90'                       # jmp + nop
    bpb[3:11] = b'BOXOS   '                            # OEM name
    struct.pack_into('<H', bpb,  11, g.sector)          # BytesPerSector
    bpb[13]   = SECTORS_PER_CLUSTER                     # SectorsPerCluster
    struct.pack_into('<H', bpb,  14, RESERVED_SECTORS)  # ReservedSectors
    bpb[16]   = NUM_FATS                                # NumFATs
    struct.pack_into('<H', bpb,  17, 0)                 # RootEntries (0 for FAT32)
    struct.pack_into('<H', bpb,  19, 0)                 # TotalSectors16 (0 for FAT32)
    bpb[21]   = 0xF8                                    # Media = fixed disk
    struct.pack_into('<H', bpb,  22, 0)                 # SectorsPerFAT16 (0 for FAT32)
    struct.pack_into('<H', bpb,  24, 63)                # SectorsPerTrack
    struct.pack_into('<H', bpb,  26, 255)               # NumHeads
    # HiddenSectors — sectors preceding the partition that holds this volume,
    # in this FAT's sectors. Zero is right for a standalone image and wrong the
    # moment the same bytes are embedded in a partitioned disk, which is what
    # the hybrid BoxOS image does: the FAT specification defines this field as
    # the partition's offset, and firmware that consults it computes every
    # absolute address from it.
    struct.pack_into('<I', bpb,  28, hidden_sectors)     # HiddenSectors
    struct.pack_into('<I', bpb,  32, g.image_sectors)   # TotalSectors32 ← critical
    # FAT32 extended BPB (starts at offset 36)
    struct.pack_into('<I', bpb,  36, g.fat_sectors)     # SectorsPerFAT32
    struct.pack_into('<H', bpb,  40, 0)                 # ExtFlags
    struct.pack_into('<H', bpb,  42, 0)                 # FSVersion (0.0)
    struct.pack_into('<I', bpb,  44, ROOT_CLUSTER)      # RootCluster
    struct.pack_into('<H', bpb,  48, 1)                 # FSInfo sector
    struct.pack_into('<H', bpb,  50, 6)                 # BkBootSec
    bpb[64]   = 0x80                                    # DriveNumber
    bpb[66]   = 0x29                                    # BootSignature
    struct.pack_into('<I', bpb,  67, 0xB0320033)        # VolumeID
    bpb[71:82]  = b'BOXOS ESP  '                        # VolumeLabel
    bpb[82:90]  = b'FAT32   '                           # FSType
    bpb[510] = 0x55                                     # the signature is at 510, whatever the sector
    bpb[511] = 0xAA

    # ── FSInfo (sector 1): its structure at the head of the sector, the trail
    # signature 0xAA550000 closing the structure's 512 bytes ─────────────────
    fsinfo = bytearray(g.sector)
    struct.pack_into('<I', fsinfo,   0, 0x41615252)    # LeadSig
    struct.pack_into('<I', fsinfo, 484, 0x61417272)    # StrucSig
    # FreeCount: clusters 2 to clust_nsh are taken, every other data cluster free
    struct.pack_into('<I', fsinfo, 488, g.total_clusters - (clust_nsh - 1))
    struct.pack_into('<I', fsinfo, 492, clust_nsh + 1) # NextFree
    struct.pack_into('<I', fsinfo, FSINFO_BYTES - 4, 0xAA550000)       # TrailSig

    # ── FAT32 table ───────────────────────────────────────────────────────────
    fat_bytes = g.fat_sectors * g.sector
    fat = bytearray(fat_bytes)
    struct.pack_into('<I', fat,  0, 0x0FFFFFF8)   # cluster 0: media descriptor
    struct.pack_into('<I', fat,  4, 0x0FFFFFFF)   # cluster 1: end-of-chain
    struct.pack_into('<I', fat,  8, 0x0FFFFFFF)   # cluster 2: root dir (EOC)
    struct.pack_into('<I', fat, 12, 0x0FFFFFFF)   # cluster 3: EFI/ dir (EOC)
    struct.pack_into('<I', fat, 16, 0x0FFFFFFF)   # cluster 4: EFI/BOOT/ dir (EOC)

    # BOOTX64.EFI cluster chain
    for i in range(efi_clust_count):
        clust = clust_efi_file + i
        nxt   = clust + 1 if i + 1 < efi_clust_count else 0x0FFFFFFF
        struct.pack_into('<I', fat, clust * 4, nxt)

    # startup.nsh (single cluster)
    struct.pack_into('<I', fat, clust_nsh * 4, 0x0FFFFFFF)

    # ── Directory: root cluster (cluster 2) ───────────────────────────────────
    root_dir = bytearray(g.cluster_size)
    vol_entry = bytearray(32)
    vol_entry[0:11] = b'BOXOS ESP  '
    vol_entry[11]   = ATTR_VOLUME_ID
    root_dir[0:32]  = vol_entry
    root_dir[32:64] = dir_entry(make_short_name('EFI', '   '),
                                ATTR_DIRECTORY, clust_efi_dir, 0, is_dir=True)
    root_dir[64:96] = dir_entry(make_short_name('STARTUP', 'NSH'),
                                ATTR_ARCHIVE, clust_nsh, len(startup_nsh))

    # ── Directory: EFI/ cluster ───────────────────────────────────────────────
    efi_dir = bytearray(g.cluster_size)
    efi_dir[0:32]  = dir_entry(make_short_name('.', '  '),
                                ATTR_DIRECTORY, clust_efi_dir, 0, is_dir=True)
    # '..' of a directory in the root names cluster 0, not the root's own (FAT §6.1)
    efi_dir[32:64] = dir_entry(make_short_name('..', '  '),
                                ATTR_DIRECTORY, 0, 0, is_dir=True)
    efi_dir[64:96] = dir_entry(make_short_name('BOOT', '   '),
                                ATTR_DIRECTORY, clust_boot_dir, 0, is_dir=True)

    # ── Directory: EFI/BOOT/ cluster ─────────────────────────────────────────
    boot_dir = bytearray(g.cluster_size)
    boot_dir[0:32]  = dir_entry(make_short_name('.', '  '),
                                 ATTR_DIRECTORY, clust_boot_dir, 0, is_dir=True)
    boot_dir[32:64] = dir_entry(make_short_name('..', '  '),
                                 ATTR_DIRECTORY, clust_efi_dir, 0, is_dir=True)
    boot_dir[64:96] = dir_entry(make_short_name('BOOTX64', 'EFI'),
                                 ATTR_ARCHIVE, clust_efi_file, len(efi_data))

    # ── Assemble image ────────────────────────────────────────────────────────
    image = bytearray(g.image_size)
    image[0:g.sector] = bpb                                   # BPB at sector 0
    image[g.sector:2 * g.sector] = fsinfo                     # FSInfo at sector 1
    image[6 * g.sector:7 * g.sector] = bpb                    # backup boot sector at 6
    fat1_off = RESERVED_SECTORS * g.sector
    image[fat1_off:fat1_off + fat_bytes] = fat                # FAT1
    fat2_off = fat1_off + fat_bytes
    image[fat2_off:fat2_off + fat_bytes] = fat                # FAT2 mirror
    for clust, data in ((ROOT_CLUSTER, root_dir), (clust_efi_dir, efi_dir), (clust_boot_dir, boot_dir),
                        (clust_efi_file, efi_data), (clust_nsh, startup_nsh)):
        at = g.cluster_offset(clust)
        image[at:at + len(data)] = data

    open(out_path, 'wb').write(bytes(image))

    print(f"  {out_path}: {g.image_size // (1024*1024)} MB FAT32 ESP of {g.sector}-byte sectors")
    print(f"    TotalSectors32 = {g.image_sectors}  (BPB offset 32)")
    print(f"    FAT sectors    = {g.fat_sectors} × 2")
    print(f"    DataStart      = sector {g.data_start}")
    print(f"    BOOTX64.EFI    = {len(efi_data)} bytes, {efi_clust_count} cluster(s)")
    print(f"    startup.nsh    = {len(startup_nsh)} bytes")


if __name__ == '__main__':
    args = sys.argv[1:]
    sector = 512
    if '--sector' in args:
        at = args.index('--sector')
        sector = int(args[at + 1], 0)
        del args[at:at + 2]
    if len(args) not in (2, 3):
        print(f"Usage: {sys.argv[0]} <input.efi> <output.img> [hidden_sectors] [--sector 512|4096]",
              file=sys.stderr)
        print("  hidden_sectors — where this volume will start once it is embedded in a",
              file=sys.stderr)
        print("                   partition, in its own sectors (0 for a standalone image)",
              file=sys.stderr)
        sys.exit(1)
    try:
        build_fat32(args[0], args[1], int(args[2], 0) if len(args) == 3 else 0, sector)
    except Exception as exc:
        import traceback; traceback.print_exc()
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
