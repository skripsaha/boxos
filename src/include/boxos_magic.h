#ifndef BOXOS_MAGIC_H
#define BOXOS_MAGIC_H

// Cabin
#define CABIN_INFO_MAGIC       0x4341424E  // "CABN"
#define POCKET_RING_MAGIC      0x504B5452  // "PKTR"
#define RESULT_RING_MAGIC      0x52534C54  // "RSLT"

// TagFS
#define TAGFS_MAGIC            0x54414746  // "TAGF"
#define TAGFS_REGISTRY_MAGIC   0x54524547  // "TREG"
#define TAGFS_FILETBL_MAGIC    0x54465442  // "TFTB"
#define TAGFS_MPOOL_MAGIC      0x544D504C  // "TMPL"
#define JOURNAL_MAGIC          0x4A4F5552  // "JOUR"
#define JOURNAL_ENTRY_MAGIC    0x4A454E54  // "JENT"

// Process
#define PROCESS_MAGIC          0x50524F43  // "PROC"

// Kernel boot
#define KERNEL_HEADER_MAGIC    0x4E52454B  // "KERN"
#define KERNEL_HEADER_MAGIC_HI 0x4C45      // "EL"

#define MAGIC_VALID(val, expected) ((val) == (expected))

#endif // BOXOS_MAGIC_H
