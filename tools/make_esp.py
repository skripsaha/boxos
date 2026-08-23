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

Usage:
  python3 tools/make_esp.py <input.efi> <output.img>
"""
import struct
import sys

# ── Geometry ────────────────────────────────────────────────────────────────
#
# FAT32 requires ≥ 65,525 data clusters (per FAT specification §3.5).
# UEFI firmware validates this; images with fewer clusters are rejected as
# FAT16 or refused outright.
#
# Minimum valid FAT32 sizes by cluster size:
#   512-byte clusters (1 sector): ~33 MB minimum
#   2 KB clusters (4 sectors):   ~130 MB minimum
#   4 KB clusters (8 sectors):   ~257 MB minimum
#
# We use 512-byte clusters for the smallest valid FAT32 image.  Cluster
# fragmentation is irrelevant for a read-only ESP with only a few KB of files.
#
SECTOR_SIZE          = 512
SECTORS_PER_CLUSTER  = 1       # 512-byte clusters → minimum valid FAT32 ~33 MB
RESERVED_SECTORS     = 32      # FAT32 standard (BPB + FSInfo + backup)
NUM_FATS             = 2
ROOT_CLUSTER         = 2       # FAT32: root dir is a cluster chain

# FAT32 minimum: 65525 data clusters.  We overshoot to 70000 for safety.
MIN_DATA_CLUSTERS = 70000
# Estimate FAT size for MIN_DATA_CLUSTERS (iterate once to converge)
_fat_entries = MIN_DATA_CLUSTERS + 2
_fat_sectors = (_fat_entries * 4 + SECTOR_SIZE - 1) // SECTOR_SIZE
IMAGE_SECTORS = RESERVED_SECTORS + NUM_FATS * _fat_sectors + MIN_DATA_CLUSTERS * SECTORS_PER_CLUSTER

# Recompute with actual IMAGE_SECTORS to be exact
FAT_ENTRIES    = (IMAGE_SECTORS - RESERVED_SECTORS) // SECTORS_PER_CLUSTER + 2
FAT_SECTORS    = (FAT_ENTRIES * 4 + SECTOR_SIZE - 1) // SECTOR_SIZE
DATA_START     = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS
TOTAL_CLUSTERS = (IMAGE_SECTORS - DATA_START) // SECTORS_PER_CLUSTER
IMAGE_SIZE     = IMAGE_SECTORS * SECTOR_SIZE
CLUSTER_SIZE   = SECTOR_SIZE * SECTORS_PER_CLUSTER

assert TOTAL_CLUSTERS >= 65525, \
    f"FAT32 requires ≥65525 data clusters, got {TOTAL_CLUSTERS}"


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def cluster_offset(clust):
    return (DATA_START + (clust - 2) * SECTORS_PER_CLUSTER) * SECTOR_SIZE


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


def build_fat32(efi_path: str, out_path: str, hidden_sectors=0):
    efi_data    = open(efi_path, 'rb').read()
    startup_nsh = b'@echo -off\r\n\\EFI\\BOOT\\BOOTX64.EFI\r\n'

    def file_clusters(size):
        return max(1, align_up(size, CLUSTER_SIZE) // CLUSTER_SIZE)

    # Cluster allocation (after ROOT_CLUSTER=2 which holds root dir)
    clust_efi_dir    = 3   # EFI/ directory
    clust_boot_dir   = 4   # EFI/BOOT/ directory
    clust_efi_file   = 5   # BOOTX64.EFI data, possibly multi-cluster
    efi_clust_count  = file_clusters(len(efi_data))
    clust_nsh        = clust_efi_file + efi_clust_count   # startup.nsh

    # ── FAT32 BPB ────────────────────────────────────────────────────────────
    bpb = bytearray(SECTOR_SIZE)
    bpb[0:3]  = b'\xEB\x58\x90'                       # jmp + nop
    bpb[3:11] = b'BOXOS   '                            # OEM name
    struct.pack_into('<H', bpb,  11, SECTOR_SIZE)       # BytesPerSector
    bpb[13]   = SECTORS_PER_CLUSTER                     # SectorsPerCluster
    struct.pack_into('<H', bpb,  14, RESERVED_SECTORS)  # ReservedSectors
    bpb[16]   = NUM_FATS                                # NumFATs
    struct.pack_into('<H', bpb,  17, 0)                 # RootEntries (0 for FAT32)
    struct.pack_into('<H', bpb,  19, 0)                 # TotalSectors16 (0 for FAT32)
    bpb[21]   = 0xF8                                    # Media = fixed disk
    struct.pack_into('<H', bpb,  22, 0)                 # SectorsPerFAT16 (0 for FAT32)
    struct.pack_into('<H', bpb,  24, 63)                # SectorsPerTrack
    struct.pack_into('<H', bpb,  26, 255)               # NumHeads
    # HiddenSectors — sectors preceding the partition that holds this volume.
    # Zero is right for a standalone image and wrong the moment the same bytes
    # are embedded in a partitioned disk, which is what the hybrid BoxOS image
    # does: the FAT specification defines this field as the partition's offset,
    # and firmware that consults it computes every absolute address from it.
    struct.pack_into('<I', bpb,  28, hidden_sectors)     # HiddenSectors
    struct.pack_into('<I', bpb,  32, IMAGE_SECTORS)     # TotalSectors32 ← critical
    # FAT32 extended BPB (starts at offset 36)
    struct.pack_into('<I', bpb,  36, FAT_SECTORS)       # SectorsPerFAT32
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
    bpb[510] = 0x55
    bpb[511] = 0xAA

    # ── FSInfo sector (sector 1) ──────────────────────────────────────────────
    fsinfo = bytearray(SECTOR_SIZE)
    struct.pack_into('<I', fsinfo,   0, 0x41615252)    # LeadSig
    struct.pack_into('<I', fsinfo, 484, 0x61417272)    # StrucSig
    struct.pack_into('<I', fsinfo, 488, TOTAL_CLUSTERS - clust_nsh)  # FreeCount
    struct.pack_into('<I', fsinfo, 492, clust_nsh + 1) # NextFree
    struct.pack_into('<I', fsinfo, 508, 0xAA550000)    # TrailSig (note: big-endian stored)
    # Correct trail signature: 0x000055AA at offset 508 little-endian
    struct.pack_into('<I', fsinfo, 508, 0xAA550000)
    # Actually spec says 0xAA550000 — Microsoft SDK says value is 0xAA55
    # stored at offset 510, which makes bpb bytes [510]=0x55 [511]=0xAA
    # For FSInfo sector the trail signature at 508 is 0xAA550000:
    fsinfo[508] = 0x00
    fsinfo[509] = 0x00
    fsinfo[510] = 0x55
    fsinfo[511] = 0xAA

    # ── FAT32 table ───────────────────────────────────────────────────────────
    fat_bytes = FAT_SECTORS * SECTOR_SIZE
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
    root_dir = bytearray(CLUSTER_SIZE)
    # Volume label entry
    vol_entry = bytearray(32)
    vol_entry[0:11] = b'BOXOS ESP  '
    vol_entry[11]   = ATTR_VOLUME_ID
    root_dir[0:32]  = vol_entry
    # EFI/ subdirectory entry
    root_dir[32:64] = dir_entry(make_short_name('EFI', '   '),
                                ATTR_DIRECTORY, clust_efi_dir, 0, is_dir=True)
    # STARTUP.NSH file entry
    root_dir[64:96] = dir_entry(make_short_name('STARTUP', 'NSH'),
                                ATTR_ARCHIVE, clust_nsh, len(startup_nsh))

    # ── Directory: EFI/ cluster ───────────────────────────────────────────────
    efi_dir = bytearray(CLUSTER_SIZE)
    efi_dir[0:32]  = dir_entry(make_short_name('.', '  '),
                                ATTR_DIRECTORY, clust_efi_dir, 0, is_dir=True)
    efi_dir[32:64] = dir_entry(make_short_name('..', '  '),
                                ATTR_DIRECTORY, ROOT_CLUSTER, 0, is_dir=True)
    efi_dir[64:96] = dir_entry(make_short_name('BOOT', '   '),
                                ATTR_DIRECTORY, clust_boot_dir, 0, is_dir=True)

    # ── Directory: EFI/BOOT/ cluster ─────────────────────────────────────────
    boot_dir = bytearray(CLUSTER_SIZE)
    boot_dir[0:32]  = dir_entry(make_short_name('.', '  '),
                                 ATTR_DIRECTORY, clust_boot_dir, 0, is_dir=True)
    boot_dir[32:64] = dir_entry(make_short_name('..', '  '),
                                 ATTR_DIRECTORY, clust_efi_dir, 0, is_dir=True)
    boot_dir[64:96] = dir_entry(make_short_name('BOOTX64', 'EFI'),
                                 ATTR_ARCHIVE, clust_efi_file, len(efi_data))

    # ── Assemble image ────────────────────────────────────────────────────────
    image = bytearray(IMAGE_SIZE)

    # BPB at sector 0
    image[0:SECTOR_SIZE] = bpb

    # FSInfo at sector 1
    image[SECTOR_SIZE : 2*SECTOR_SIZE] = fsinfo

    # Backup boot sector at sector 6
    image[6*SECTOR_SIZE : 7*SECTOR_SIZE] = bpb

    # FAT1
    fat1_off = RESERVED_SECTORS * SECTOR_SIZE
    image[fat1_off : fat1_off + fat_bytes] = fat

    # FAT2 mirror
    fat2_off = fat1_off + fat_bytes
    image[fat2_off : fat2_off + fat_bytes] = fat

    # Root directory (cluster 2)
    image[cluster_offset(ROOT_CLUSTER) : cluster_offset(ROOT_CLUSTER) + CLUSTER_SIZE] = root_dir

    # EFI/ directory (cluster 3)
    image[cluster_offset(clust_efi_dir) : cluster_offset(clust_efi_dir) + CLUSTER_SIZE] = efi_dir

    # EFI/BOOT/ directory (cluster 4)
    image[cluster_offset(clust_boot_dir) : cluster_offset(clust_boot_dir) + CLUSTER_SIZE] = boot_dir

    # BOOTX64.EFI data (clusters 5..)
    efi_dest = cluster_offset(clust_efi_file)
    image[efi_dest : efi_dest + len(efi_data)] = efi_data

    # startup.nsh data
    nsh_dest = cluster_offset(clust_nsh)
    image[nsh_dest : nsh_dest + len(startup_nsh)] = startup_nsh

    open(out_path, 'wb').write(bytes(image))

    print(f"  {out_path}: {IMAGE_SIZE // (1024*1024)} MB FAT32 ESP")
    print(f"    TotalSectors32 = {IMAGE_SECTORS}  (BPB offset 32)")
    print(f"    FAT sectors    = {FAT_SECTORS} × 2")
    print(f"    DataStart      = sector {DATA_START}")
    print(f"    BOOTX64.EFI    = {len(efi_data)} bytes, {efi_clust_count} cluster(s)")
    print(f"    startup.nsh    = {len(startup_nsh)} bytes")


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        print(f"Usage: {sys.argv[0]} <input.efi> <output.img> [hidden_sectors]",
              file=sys.stderr)
        print("  hidden_sectors — the LBA this volume will start at once it is",
              file=sys.stderr)
        print("                   embedded in a partition (0 for a standalone image)",
              file=sys.stderr)
        sys.exit(1)
    try:
        build_fat32(sys.argv[1], sys.argv[2],
                    int(sys.argv[3], 0) if len(sys.argv) == 4 else 0)
    except Exception as exc:
        import traceback; traceback.print_exc()
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
