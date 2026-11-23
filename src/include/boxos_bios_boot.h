#ifndef BOXOS_BIOS_BOOT_H
#define BOXOS_BIOS_BOOT_H

/*
 * What BoxOS keeps to start a disk through a BIOS, and where on the disk it
 * goes — one owner for every side: the build (the Makefile reads the names and
 * the field's place out of this file), stage1 (told the field's place by the
 * build), and the kernel laying both on a disk (boardroom/disk_boot.c). What
 * BoxOS keeps to start a disk through UEFI is boxos_esp.h's.
 *
 * ‼ ON A GPT DISK. The boot sector a BIOS runs is the GPT's protective MBR
 * (UEFI 2.10 §5.2.3): its code, up to the field below, is BoxOS's — no other
 * system's, since a BIOS runs one; its disk signature, its one entry and its
 * 0x55AA are the table's, and are never touched. The second stage lies at the
 * head of a BIOS boot partition (boxos_disks.h DISK_PART_BIOS_BOOT), which no
 * file system claims, and stage1 finds it by the field: the partition's first
 * block, 64 bits little-endian. A BIOS reads 512-byte sectors, so both are
 * laid on a medium of 512-byte blocks alone.
 *
 * The names are the files as a volume keeps them, in the system's role
 * (create_tagfs --system), so every volume laid carries the loaders that boot
 * it, as it carries Gangway.
 */

#define BIOS_STAGE1_KEPT     "stage1.bin"   /* the boot sector, whole */
#define BIOS_STAGE2_KEPT     "stage2.bin"   /* the second stage */
#define BIOS_STAGE2_LBA_AT   432u           /* where stage1 keeps the second stage's first block */
#define BIOS_SECTOR_BYTES    512u

#endif /* BOXOS_BIOS_BOOT_H */
