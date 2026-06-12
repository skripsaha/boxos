#ifndef SYSTEM_H
#define SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"

#define PROC_STATE_TERMINATED   4

typedef struct {
    uint16_t pid;
    uint8_t state;
    uint8_t priority;
    uint32_t memory_usage;
} proc_info_t;

/* Real per-cabin system snapshot — populated by `system.info` kernel op.
 *
 * Each numeric field is sized to its natural range (uptime in nanoseconds
 * fits 64 bits for ~584 years; physical memory amounts up to 16 EiB fit too).
 * No legacy 32-bit memory cap, no fake values. */
typedef struct {
    char     version[32];        /* "BoxOS X.Y", NUL-padded         */
    uint64_t uptime_ns;          /* nanosecond-precision uptime     */
    uint64_t total_memory;       /* total physical RAM (bytes)      */
    uint64_t used_memory;        /* in-use RAM (bytes)              */
    uint64_t free_memory;        /* free RAM (bytes)                */
    uint64_t tsc_freq_khz;       /* calibrated TSC freq, 0 = N/A    */
    uint32_t cpu_total;          /* total cores detected            */
    uint32_t cpu_k_cores;        /* K-Cores (kernel/IO)             */
    uint32_t cpu_app_cores;      /* App-Cores (userspace)           */
    uint32_t process_count;      /* live processes                  */
    uint32_t pit_freq_hz;        /* configured PIT frequency        */
    uint8_t  multicore_active;   /* 1 if AMP currently active       */
    uint8_t  has_invariant_tsc;  /* 1 if TSC stable across cores    */
    uint8_t  has_waitpkg;        /* 1 if UMWAIT available           */
    uint8_t  reserved;
} system_info_t;

int proc_info(uint16_t pid, proc_info_t* info);
void exit(uint32_t exit_code);
int proc_exec(const char* filename);

int proc_tag_add(const char* tag);
int proc_tag_remove(const char* tag);
int proc_tag_check(const char* tag, bool* has_tag);

int reboot(void);
int shutdown(void);
int sysinfo(system_info_t* info);

int defrag(uint32_t file_id, uint32_t target_block);
int fragmentation(void);
int perf_dump(void);

void yield(void);

/* TLS thread-pointer fallback for CPUs without FSGSBASE: asks the kernel
 * to program this process's FS base (SYSTEM_OP_TLS_FSBASE). The value
 * lands at the next context restore — call yield() afterwards before
 * touching any thread_local. boxcxx tls_init is the intended caller. */
int tls_set_fsbase(uint64_t base);

/* =========================================================================
 *  EFI / Secure Boot / ESRT — userspace introspection
 *
 *  These wrap the kernel ops in system_ops.c:
 *    SYSTEM_OP_EFI_INFO        — full state snapshot in one call.
 *    SYSTEM_OP_EFI_ESRT_GET    — fetch one EFI System Resource Table entry.
 *    SYSTEM_OP_EFI_VERIFY_PE   — Authenticode-verify a PE image against db/dbx.
 * ========================================================================= */

typedef struct {
    uint32_t version;             /* layout version of this struct */
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
} efi_info_t;

typedef struct {
    uint8_t  fw_class[16];        /* GUID */
    uint32_t fw_type;             /* 0=unknown, 1=system, 2=device, 3=uefi-driver */
    uint32_t fw_version;
    uint32_t lowest_supported_fw_version;
    uint32_t capsule_flags;
    uint32_t last_attempt_version;
    uint32_t last_attempt_status;
} efi_esrt_entry_t;

typedef enum {
    EFI_VERIFY_OK                   = 0,
    EFI_VERIFY_BAD_PE               = 1,
    EFI_VERIFY_NO_CERT_TABLE        = 2,
    EFI_VERIFY_BAD_CERT_TABLE       = 3,
    EFI_VERIFY_BAD_SIGNED_DATA      = 4,
    EFI_VERIFY_AUTH_HASH_MISMATCH   = 5,
    EFI_VERIFY_UNSUPPORTED_DIGEST   = 6,
    EFI_VERIFY_SIGNER_NOT_FOUND     = 7,
    EFI_VERIFY_BAD_SIGNER_CERT      = 8,
    EFI_VERIFY_BAD_RSA_KEY          = 9,
    EFI_VERIFY_SIGNATURE_INVALID    = 10,
    EFI_VERIFY_CHAIN_UNTRUSTED      = 11,
    EFI_VERIFY_REVOKED_BY_DBX       = 12,
    EFI_VERIFY_SB_UNAVAILABLE       = 13,
} efi_verify_result_t;

typedef struct {
    efi_verify_result_t result;
    uint32_t            pe_size;
    uint8_t             pe_sha256[32];
    uint8_t             signer_sha256[32];
} efi_verify_t;

/* Returns 0 on success, -1 on failure. */
int efi_info(efi_info_t *out);

/* idx in [0, efi_info().esrt_count). Returns 0 on success. */
int efi_esrt_entry(uint32_t idx, efi_esrt_entry_t *out);

/* Verify a PE image. `pe_buf` is the entire image bytes; `pe_size` is its
 * length. `out` receives the result + hashes. Returns 0 if the call itself
 * succeeded — `out->result` carries the verification verdict (== 0 means
 * the PE is trusted; != 0 means rejected). */
int efi_verify_pe(const void *pe_buf, uint32_t pe_size, efi_verify_t *out);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_H
