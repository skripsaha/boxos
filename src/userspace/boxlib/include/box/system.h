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

/* Processor microseconds this cabin has been given — NOT wall time: a cabin
 * that is parked spends none of it, and two strands running at once spend it
 * twice as fast as the clock on the wall. Self only; there is no pid form,
 * because another cabin's processor time is not ours to read. Backs
 * std::clock(). */
int proc_cpu_time(uint64_t* out_us);

/* Normal termination: runs __cxa_finalize callbacks (std::atexit + static
 * destructors) and the .fini_array, flushes the console, then ends the
 * process. The parameter is `int` because [support.start.term] says so and
 * because the C++ <cstdlib> exports this very function as std::exit — it used
 * to be uint32_t, which no translation unit could reconcile with <cstdlib>.
 * The kernel masks the code to [0, INT32_MAX]. */
void exit(int exit_code) __attribute__((noreturn));

/* Abnormal-immediate termination: no atexit callbacks, no static destructors,
 * no .fini_array, no flush. C's _Exit, and the primitive std::quick_exit and
 * std::abort are built on. */
void _Exit(int exit_code) __attribute__((noreturn));
/* Start a program from a command LINE — "say hello world" — the way the
 * shell does. The first word names the program; the whole line, as typed,
 * becomes the program's Luggage (box/luggage.h), in its cabin before its
 * first instruction. A bare name — proc_exec("touch_test") — starts the
 * program with a one-word luggage. */
int proc_exec(const char* line);
int proc_exec_tagged(const char* line, const char* tags); /* child = file-tags ∪ caller-tags */
/* Like proc_exec_tagged, but also reports the child's pid-allocator generation
 * (the second half of its canonical (pid, generation) identity) via *out_gen,
 * so a supervisor can later match the child's process:died to the exact
 * incarnation. Returns the pid (>0) or -err; *out_gen is 0 on any failure or if
 * the kernel did not report a generation. */
int proc_exec_gen(const char* line, const char* tags, uint32_t* out_gen);

/* Wait until the incarnation (pid, generation) is gone, then report how it
 * ended in *out_exit (proc_exit.h: >= 0 the code it passed to exit(), -1
 * killed, -2 crashed). out_exit may be NULL.
 *
 * Being gone is asked as a STATE, so an incarnation that has already finished
 * returns at once and a live one is parked on — there is no gap between
 * asking and being registered, and therefore nothing to miss. That is the
 * difference from watching the process:died multicast, where a single dropped
 * announcement left a supervisor waiting for good.
 *
 * generation is mandatory: a pid names a seat, and seats are re-let.
 * Returns 0 when gone, -ERR_PROCESS_NOT_FOUND if that incarnation was never
 * issued (a mistyped pid must not read as success).
 */
int process_gone(uint32_t pid, uint32_t generation, int32_t* out_exit);
int proc_kill(uint32_t pid);                                  /* kill another process by pid */

/* End the exact incarnation (pid, generation) — the pair that names a
 * process, since a pid is a seat and seats are re-let. Between learning a
 * pid and using it the process can leave and its seat be taken; a kill by
 * pid alone lands on whoever sits there now, and reports success. This one
 * answers -ERR_PROCESS_NOT_FOUND instead, which is the truth. generation 0
 * means "whoever is in that seat" and is exactly proc_kill. */
int proc_finish(uint32_t pid, uint32_t generation);

/* One member of a tag's crew, as proc_crew found them. `tags` points inside
 * the same allocation as the array: free(mates) frees the names too. */
typedef struct ProcMate {
    uint32_t    pid;
    uint32_t    generation;   /* with pid, the incarnation — pass to proc_finish */
    uint32_t    state;        /* process_state_t; 4 DONE / 5 CRASHED are gone */
    uint64_t    cpu_us;       /* processor time this cabin has been given */
    const char *tags;         /* comma-joined, NUL-terminated */
} ProcMate;

/* Who wears this tag, right now.
 *
 * A tag names a role, not an individual: three hundred processes may wear
 * one, and `broadcast` has always walked exactly this crew to speak to them.
 * This asks the same walk to answer instead. Returns how many were DELIVERED
 * (>= 0) and hands back a malloc'd array of that many — NULL and 0 when
 * nobody wears it — or a negative -error_t. The caller frees the array.
 *
 * `out_total` (optional) receives how many wear it altogether. It differs from
 * the return only for a crew too large to fit one answer, and then the
 * difference is the point: a caller that acts on the answer as if it were the
 * whole crew would leave the rest running and report success. The two numbers
 * are the same distinction storage's tag query draws, for the same reason.
 *
 * The kernel promises two things about the answer: every cabin in it was LIVE
 * at the moment of the walk (the dead are dropped before the answer is built),
 * and no member's tag list is ever delivered cut short — a record that would
 * not fit whole is not delivered at all, and shows up in the shortfall.
 *
 * The caller is in its own answer when it wears the tag.
 *
 * A member found here may already have left by the time it is used: that is
 * not a flaw in the answer but the nature of the question, and it is why
 * each member carries its generation — see proc_finish. */
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
