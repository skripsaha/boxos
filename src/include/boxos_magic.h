#ifndef BOXOS_MAGIC_H
#define BOXOS_MAGIC_H

// ============================================================================
// BOXOS MAGIC NUMBERS
// ============================================================================
// Shared between kernel and userspace for validation

// Page magics
#define BOXOS_NOTIFY_MAGIC       0x4E4F5449  // "NOTI" (little-endian)
#define BOXOS_RESULT_MAGIC       0x52455355  // "RESU" (little-endian)
#define BOXOS_EVENT_MAGIC        0x45565421  // "EVT!" (little-endian)

// TagFS magics
#define BOXOS_TAGFS_MAGIC        0x54414746  // "TAGF"
#define BOXOS_TAGFS_META_MAGIC   0x544D4554  // "TMET"
#define BOXOS_JOURNAL_MAGIC      0x4A4F5552  // "JOUR"
#define BOXOS_JOURNAL_ENTRY_MAGIC 0x4A454E54 // "JENT"

// Process magic
#define BOXOS_PROCESS_MAGIC      0x50524F43  // "PROC"

// Validation helper
#define BOXOS_MAGIC_VALID(val, expected) ((val) == (expected))

#endif // BOXOS_MAGIC_H
