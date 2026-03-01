#ifndef BOXOS_MAGIC_H
#define BOXOS_MAGIC_H

#define NOTIFY_MAGIC       0x4E4F5449  // "NOTI"
#define RESULT_MAGIC       0x52455355  // "RESU"
#define EVENT_MAGIC        0x45565421  // "EVT!"

#define TAGFS_MAGIC        0x54414746  // "TAGF"
#define TAGFS_META_MAGIC   0x544D4554  // "TMET"
#define JOURNAL_MAGIC      0x4A4F5552  // "JOUR"
#define JOURNAL_ENTRY_MAGIC 0x4A454E54 // "JENT"

#define PROCESS_MAGIC      0x50524F43  // "PROC"

#define KERNEL_HEADER_MAGIC    0x4E52454B  // "KERN" (first 4 bytes of "KERNEL", little-endian)
#define KERNEL_HEADER_MAGIC_HI 0x4C45      // "EL" (next 2 bytes, little-endian)

#define MAGIC_VALID(val, expected) ((val) == (expected))

#endif // BOXOS_MAGIC_H
