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

typedef struct {
    char     version[32];
    uint64_t uptime_ns;
    uint64_t total_memory;
    uint64_t used_memory;
    uint64_t free_memory;
    uint64_t tsc_freq_khz;
    uint32_t cpu_total;
    uint32_t cpu_k_cores;
    uint32_t cpu_app_cores;
    uint32_t process_count;
    uint32_t pit_freq_hz;
    uint8_t  multicore_active;
    uint8_t  has_invariant_tsc;
    uint8_t  has_waitpkg;
    uint8_t  reserved;
} system_info_t;

int proc_info(uint16_t pid, proc_info_t* info);

int proc_cpu_time(uint64_t* out_us);

void exit(int exit_code) __attribute__((noreturn));

void _Exit(int exit_code) __attribute__((noreturn));
int proc_exec(const char* line);
int proc_exec_tagged(const char* line, const char* tags);
int proc_exec_gen(const char* line, const char* tags, uint32_t* out_gen);

int process_gone(uint32_t pid, uint32_t generation, int32_t* out_exit);
int proc_kill(uint32_t pid);

int proc_finish(uint32_t pid, uint32_t generation);

typedef struct ProcMate {
    uint32_t    pid;
    uint32_t    generation;
    uint32_t    state;
    uint64_t    cpu_us;
    const char *tags;
} ProcMate;

int proc_crew(const char *tag, ProcMate **out_mates, uint32_t *out_total);

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

int tls_set_fsbase(uint64_t base);


typedef struct {
    uint32_t version;
    uint8_t  rt_available;
    uint8_t  esrt_available;
    uint8_t  sb_available;
    uint8_t  sb_enforced;
    uint8_t  sb_setup_mode;
    uint8_t  sb_audit_mode;
    uint8_t  sb_deployed_mode;
    uint8_t  _pad;
    uint32_t esrt_count;
    uint32_t cert_count_total;
    uint32_t hash_count_total;
    uint32_t cert_count_by_db[6];
    uint32_t hash_count_by_db[6];
} efi_info_t;

typedef struct {
    uint8_t  fw_class[16];
    uint32_t fw_type;
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

int efi_info(efi_info_t *out);

int efi_esrt_entry(uint32_t idx, efi_esrt_entry_t *out);

int efi_verify_pe(const void *pe_buf, uint32_t pe_size, efi_verify_t *out);

#ifdef __cplusplus
}
#endif

#endif