#ifndef BOXOS_MAGIC_H
#define BOXOS_MAGIC_H

// Cabin
#define CABIN_INFO_MAGIC       0x4341424E  // "CABN"
#define POCKET_RING_MAGIC      0x504B5452  // "PKTR"
#define RESULT_RING_MAGIC      0x52534C54  // "RSLT"
#define TOUCH_RING_MAGIC       0x54434852  // "TCHR"
#define BROOK_HEADER_MAGIC     0x42524B48  // "BRKH" — Brook SPSC stream header

// Process
#define PROCESS_MAGIC          0x50524F43  // "PROC"

// Nameplate — the in-image "address -> name" table (nameplate_format.h)
#define NAMEPLATE_MAGIC        0x4E504C54  // "NPLT"

// BuildPlate — what the image was made from (buildplate_format.h). Riveted by
// tools/buildplate after the kernel is linked, said in the kernel's first line.
#define BUILDPLATE_MAGIC       0x42504C54  // "BPLT"

// Kernel boot
#define KERNEL_HEADER_MAGIC    0x4E52454B  // "KERN"
#define KERNEL_HEADER_MAGIC_HI 0x4C45      // "EL"

#define MAGIC_VALID(val, expected) ((val) == (expected))

#endif // BOXOS_MAGIC_H
