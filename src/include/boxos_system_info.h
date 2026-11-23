#ifndef BOXOS_SYSTEM_INFO_H
#define BOXOS_SYSTEM_INFO_H

/*
 * boxos_system_info.h — what the system deck tells a program about the
 * machine, shared kernel/userspace: the answers of system.info,
 * system.resources and system.efi.info (SYSTEM_OP_INFO, SYSTEM_OP_RESOURCES,
 * SYSTEM_OP_EFI_INFO in boxos_decks.h). The kernel fills one of these and
 * writes it whole into the out_crate (system_ops.c); boxlib hands it to its
 * caller as system_info_t, system_resources_t and efi_info_t (box/system.h).
 * One spelling of each answer: neither side names an offset of its own.
 *
 * Kernel:    #include "ktypes.h" first.
 * Userspace: #include "box/types.h" first.
 */

/* Which part is actually producing the system tick. Read it before reporting
 * tick_freq_hz: the frequency alone used to be labelled "PIT", and on every
 * board where the HPET takes IRQ0 through LegacyReplacement that named a chip
 * the kernel never programmed. */
#define SYSINFO_TICK_SOURCE_NONE          0u  /* nothing is driving IRQ0 */
#define SYSINFO_TICK_SOURCE_HPET_LEGACY   1u  /* HPET timer 0, LegacyReplacement */
#define SYSINFO_TICK_SOURCE_8254          2u  /* 8254 channel 0, divisor ours */

/* system.info — a real-time snapshot. Each numeric field is sized to its
 * natural range (uptime in nanoseconds fits 64 bits for ~584 years; physical
 * memory amounts up to 16 EiB fit too). tick_freq_hz and tick_source travel
 * together and must be read together. */
typedef struct SystemInfo {
    char     name[32];           /* "BoxOS", NUL-padded             */
    uint64_t uptime_ns;          /* nanosecond-precision uptime     */
    uint64_t total_memory;       /* total physical RAM (bytes)      */
    uint64_t used_memory;        /* in-use RAM (bytes)              */
    uint64_t free_memory;        /* free RAM (bytes)                */
    uint64_t tsc_freq_khz;       /* calibrated TSC freq, 0 = N/A    */
    uint32_t cpu_total;          /* total cores detected            */
    uint32_t cpu_k_cores;        /* K-Cores (kernel/IO)             */
    uint32_t cpu_app_cores;      /* App-Cores (userspace)           */
    uint32_t process_count;      /* live processes                  */
    uint32_t tick_freq_hz;       /* rate of the system tick, in Hz  */
    uint8_t  multicore_active;   /* 1 if AMP currently active       */
    uint8_t  has_invariant_tsc;  /* 1 if TSC stable across cores    */
    uint8_t  has_waitpkg;        /* 1 if UMWAIT available           */
    uint8_t  tick_source;        /* SYSINFO_TICK_SOURCE_*           */
} SystemInfo;

/* system.resources — read-only machine resources. The CPU counters are
 * cumulative TSC counters; compare two snapshots to obtain utilization over
 * that interval. The name comes from the mounted volume's Deed; the UUID and
 * capacity from the mounted TagFS state. */
typedef struct SystemResources {
    char     volume_name[16];
    uint64_t volume_free_bytes;
    uint8_t  volume_present;
    uint8_t  volume_free_counted; /* 0: the kernel holds no count of this volume's
                                   * free ground (a book whose Tally names
                                   * a bookmark, whose ground it does not read), and
                                   * the free and used bytes say nothing */
    uint8_t  _volume_pad[6];
    uint64_t cpu_busy_tsc;
    uint64_t cpu_total_tsc;
    uint8_t  volume_uuid[16];
    uint64_t volume_total_bytes;
    uint64_t volume_used_bytes;
} SystemResources;

/* system.efi.info — EFI runtime, ESRT and Secure Boot state in one answer, so
 * a single op call gives userspace the full picture without round-trips. */
typedef struct EfiInfo {
    uint8_t  rt_available;        /* 1 iff EFI runtime services online */
    uint8_t  esrt_available;      /* 1 iff ESRT was published by firmware */
    uint8_t  sb_available;        /* 1 iff Secure Boot vars were enumerated */
    uint8_t  sb_enforced;         /* SecureBoot variable value */
    uint8_t  sb_setup_mode;       /* SetupMode variable value */
    uint8_t  sb_audit_mode;       /* AuditMode variable value */
    uint8_t  sb_deployed_mode;    /* DeployedMode variable value */
    uint8_t  _pad;
    uint32_t esrt_count;          /* total EFI System Resource entries */
    uint32_t cert_count_total;    /* X.509 certs across all DBs */
    uint32_t hash_count_total;    /* raw hashes across all DBs */
    uint32_t cert_count_by_db[6]; /* PK, KEK, db, dbx, dbt, dbr */
    uint32_t hash_count_by_db[6];
} EfiInfo;

/* system.machine.info — what the machine calls itself, as its firmware's SMBIOS
 * table says (DMTF DSP0134; drivers/smbios): System Information's maker,
 * product, version and family, the board's maker and product, the chassis.
 * Each text UTF-8 as the firmware wrote it, NUL-ended — empty for a field the
 * table leaves out, or fills with a vendor's placeholder ("To be filled by
 * O.E.M.", "Default string", ...), which names no machine. How a person is
 * told the machine's name is boxlib's (machine_shown). */
#define MACHINE_TEXT_MOST    64u
#define MACHINE_FROM_NONE    0u  /* the firmware published no SMBIOS table */
#define MACHINE_FROM_SMBIOS2 1u  /* the 32-bit entry point, "_SM_" */
#define MACHINE_FROM_SMBIOS3 2u  /* the 64-bit entry point, "_SM3_" */
typedef struct MachineInfo {
    char    maker[MACHINE_TEXT_MOST];        /* type 1: manufacturer */
    char    product[MACHINE_TEXT_MOST];      /* type 1: product name */
    char    version[MACHINE_TEXT_MOST];      /* type 1: version */
    char    family[MACHINE_TEXT_MOST];       /* type 1: family */
    char    board_maker[MACHINE_TEXT_MOST];  /* type 2: manufacturer */
    char    board[MACHINE_TEXT_MOST];        /* type 2: product */
    uint8_t chassis;                         /* type 3: its type, the lock bit off; 0 for none */
    uint8_t from;                            /* MACHINE_FROM_* */
    uint8_t major;                           /* the table's version */
    uint8_t minor;
    uint8_t _pad[4];
} MachineInfo;

#ifdef __cplusplus
static_assert(sizeof(MachineInfo) == 392, "MachineInfo is a kernel/userspace ABI and must stay 392 bytes");
static_assert(sizeof(SystemInfo) == 96, "SystemInfo is a kernel/userspace ABI and must stay 96 bytes");
static_assert(sizeof(SystemResources) == 80, "SystemResources is a kernel/userspace ABI and must stay 80 bytes");
static_assert(sizeof(EfiInfo) == 68, "EfiInfo is a kernel/userspace ABI and must stay 68 bytes");
#else
_Static_assert(sizeof(MachineInfo) == 392, "MachineInfo is a kernel/userspace ABI and must stay 392 bytes");
_Static_assert(sizeof(SystemInfo) == 96, "SystemInfo is a kernel/userspace ABI and must stay 96 bytes");
_Static_assert(sizeof(SystemResources) == 80, "SystemResources is a kernel/userspace ABI and must stay 80 bytes");
_Static_assert(sizeof(EfiInfo) == 68, "EfiInfo is a kernel/userspace ABI and must stay 68 bytes");
#endif

#endif /* BOXOS_SYSTEM_INFO_H */
