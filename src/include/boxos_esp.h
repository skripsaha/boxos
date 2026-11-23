#ifndef BOXOS_ESP_H
#define BOXOS_ESP_H

/*
 * What BoxOS keeps on an EFI System Partition, and under what names — one
 * owner for every side that puts it there or looks for it: the image's own
 * ESP (the Makefile, tools/fat.c), a volume's maker installing the system on
 * a disk (the kernel), and Gangway finding what is its on the ESP it was
 * started from. The Makefile reads the names out of this file.
 *
 * ‼ ITS OWN DIRECTORY. An ESP is shared by every system on its disk, each
 * under a directory of its own beneath EFI (UEFI 2.10, the System Partition's
 * directory structure): everything BoxOS puts on one is under ESP_DIR, and
 * nothing of another system's is touched. The one name outside it is the
 * default a firmware boots a medium by when no boot option names a file
 * (ESP_FALLBACK, UEFI 2.10 §3.5.1), written on an ESP of BoxOS's own alone —
 * never on one another system shares, whose default is that system's.
 *
 * Paths are parted by '/', as src/include/fat.h takes them. Integer types
 * come from the includer, as volume_deed.h's do.
 */

#define ESP_DIR           "EFI/BoxOS"
#define ESP_LOADER        "EFI/BoxOS/gangway.efi"   /* the loader a boot option names */
#define ESP_FALLBACK      "EFI/BOOT/BOOTX64.EFI"    /* the removable-media default, x64 */
#define ESP_LOADER_KEPT   "gangway.efi"             /* the loader as a volume keeps it, in the system's role */

/*
 * What a boot option of BoxOS's carries past its file — EFI_LOAD_OPTION's
 * OptionalData, handed to the loader it starts as its LoadOptions (UEFI 2.10
 * §3.1.3): the volume to boot, by the uuid its Deed holds, so a machine with
 * more than one volume starts the one installed. A start by the
 * removable-media default carries none, and Gangway boots the volume it
 * finds.
 */
#define ESP_VOLUME_MAGIC  "BoxOSVol"                /* 8 bytes, no terminator */

typedef struct __attribute__((packed)) {
    uint8_t magic[8];
    uint8_t uuid[16];
} EspVolumeAsked;

#endif /* BOXOS_ESP_H */
