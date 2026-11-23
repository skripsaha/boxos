/*
 * system.c — userspace process/buffer/tag/perf wrappers (Phase 12: Manifest-only).
 */

#include "box/system.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/debug.h"
#include "box/print.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"
#include "box/timeouts.h" /* BOX_ANSWER_GUARANTEED — every op here is answered */
#include "box/memory.h"   /* strand_pool_flush_self — main-pool flush at exit */
#include "boxos_decks.h"  /* SYSTEM_OP_* opcodes — single source */

/* SYSTEM_OP_* opcodes come from boxos_decks.h (included via box headers).
 * Local aliases below keep call sites readable without redefining the
 * numeric values. */
#define SYS_PROC_SPAWN  SYSTEM_OP_PROC_SPAWN
#define SYS_PROC_KILL   SYSTEM_OP_PROC_KILL
#define SYS_PROC_INFO   SYSTEM_OP_PROC_INFO
#define SYS_PROC_CPUTIME SYSTEM_OP_PROC_CPUTIME
#define SYS_PROC_EXEC   SYSTEM_OP_PROC_EXEC
#define SYS_PROC_EXEC_FILE SYSTEM_OP_PROC_EXEC_FILE
#define SYS_DEFRAG      SYSTEM_OP_DEFRAG_FILE
#define SYS_FRAG_SCORE  SYSTEM_OP_FRAG_SCORE
#define EFI_VERIFY_PE_OUT_SIZE   80u
#define EFI_ESRT_ENTRY_SIZE      40u
#define SYS_TAG_ADD     SYSTEM_OP_TAG_ADD
#define SYS_TAG_REMOVE  SYSTEM_OP_TAG_REMOVE
#define SYS_TAG_CHECK   SYSTEM_OP_TAG_CHECK
#define SYS_PERF_DUMP   SYSTEM_OP_PERF_DUMP

/* Upper bound on a caller-supplied tag augment for proc_exec_tagged: the
 * augment rides in the op's params, and MfCall1 carries at most 256 bytes of
 * them. The kernel reads an augment of any length. */
#define PROC_EXEC_TAGS_MAX 256

/* =========================================================================
 *  Process lifecycle
 * ========================================================================= */

int proc_cpu_time(uint64_t *out_us)
{
    if (!out_us) return -ERR_INVALID_ARGUMENT;

    uint8_t  out[8]  = {0};
    uint32_t actual  = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_CPUTIME,
                     NULL, 0,
                     NULL, 0,
                     out, sizeof(out), &actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    if (actual < 8) return -ERR_INTERNAL;

    memcpy(out_us, out, 8);
    return OK;
}

int process_gone(uint32_t pid, uint32_t generation, int32_t *out_exit)
{
    if (pid == 0 || generation == 0) return -ERR_INVALID_ARGUMENT;

    uint8_t params[8];
    memcpy(params,     &pid,        4);
    memcpy(params + 4, &generation, 4);

    /* No deadline, and here that is a statement about the kernel rather than
     * optimism: the op parks only on an incarnation it has just confirmed is
     * live, and the single place a life ends owes every parked waiter an
     * answer. A child may legitimately run for hours — a clock would only be
     * able to lie about that — while a reply that never comes is a kernel
     * defect, which Nightwatch can now see precisely because this wait is a
     * real park rather than a userspace spin. */
    Result r;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_PROCESS_GONE,
                     params, sizeof(params),
                     NULL, 0,
                     NULL, 0, NULL,
                     0 /* no deadline */, &r);
    if (rc != 0) return box_fail(rc);

    /* The disposition rides in data_length as a value (proc_exit.h): >= 0 is
     * the code the process passed to exit(), negative is how it was ended. */
    if (out_exit) *out_exit = (int32_t)r.data_length;
    return 0;
}

const char *process_fault_said(uint8_t vector)
{
    switch (vector) {
    case 0:  return "division by zero";
    case 1:  return "debug trap";
    case 3:  return "breakpoint";
    case 4:  return "overflow trap";
    case 5:  return "bound range exceeded";
    case 6:  return "invalid instruction";
    case 7:  return "no floating-point unit";
    case 8:  return "double fault";
    case 10: return "invalid task state";
    case 11: return "segment not present";
    case 12: return "stack segment fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "floating-point error";
    case 17: return "misaligned access";
    case 18: return "machine check";
    case 19: return "SIMD floating-point error";
    case 20: return "virtualization fault";
    case 21: return "control-protection fault";
    default: return "fault";
    }
}

int proc_info(uint16_t pid, proc_info_t *info)
{
    if (!info) return -ERR_INVALID_ARGUMENT;

    uint32_t pid32 = pid;
    uint8_t  out[256] = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_INFO,
                     &pid32, sizeof(pid32),
                     NULL, 0,
                     out, sizeof(out), &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 32) return -ERR_INTERNAL;

    /* Layout: [u32 pid][u32 state][i32 score][u32 _pad][u64 cstart][u64 csize][char tags[]] */
    uint32_t blob_pid, state;
    int32_t  score;
    memcpy(&blob_pid, out + 0,  4);
    memcpy(&state,    out + 4,  4);
    memcpy(&score,    out + 8,  4);

    info->pid          = (uint16_t)blob_pid;
    info->state        = (uint8_t)state;
    info->priority     = (uint8_t)((score < 0) ? 0 : (score > 255 ? 255 : score));
    info->memory_usage = 0;  /* not exposed by the new op */
    return OK;
}

int tls_set_fsbase(uint64_t base)
{
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_TLS_FSBASE,
                     &base, sizeof(base),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

/* runtime_init.c — .fini_array + __cxa_finalize teardown (idempotent). */
void __box_runtime_fini(void);

void exit(int exit_code)
{
    /* Static destructors / atexit callbacks may still print — run them
     * BEFORE the final io_flush so their output reaches the console. */
    __box_runtime_fini();

    /* Ф20e — return the main strand's StrandPool cache to the global heap after
     * all destructors have run (they may still free), so heap leak diagnostics
     * see a quiesced heap with no blocks held out in the magazine. */
    strand_pool_flush_self();

    io_depart();

    _Exit(exit_code);
}

void _Exit(int exit_code)
{
    /* params:[u32 target_pid][i32 exit_code] — target 0 means this program
     * exits, every strand of it; the code rides into every strand's
     * process:died (masked to [0, INT32_MAX] kernel-side). This
     * is the ONLY exit signal now: observers watch the process:died Touch
     * (which also fires on crash), so exit() sends the spawner nothing. */
    struct __attribute__((packed)) { uint32_t target; int32_t code; } kill_param = {
        0, (int32_t)exit_code
    };

    /* Asked ONCE. Nothing stands between a self-kill and its answer — no
     * medium, no other process — and a self-kill that was honoured does not
     * come back: the process is PROC_DONE before the guide can answer it,
     * and a corpse still on its core until the next tick only spins below.
     * What stood here was three tries with a yield between, a counter over a
     * ring-full refusal that pocket_submit no longer gives (it waits for
     * room). A refusal that does come back is therefore a kernel defect, not
     * contention to be out-waited: name it, and stay parked. */
    int kill_rc = MfCall1(DECK_SYSTEM, SYS_PROC_KILL,
                          &kill_param, sizeof(kill_param),
                          NULL, 0, NULL, 0, NULL,
                          BOX_ANSWER_GUARANTEED, NULL);
    if (kill_rc != 0)
        kdbg_print("[boxlib] _Exit(): self-kill refused rc=%d — kernel defect; halting here", kill_rc);

    /* If the kernel honoured kill we will not run another instruction.
     * If it didn't (or returned and somehow rescheduled us), park the
     * process indefinitely so we don't fall through to undefined code.
     * cpu_pause keeps the core friendly under contention. */
    while (1) {
        __asm__ volatile("pause");
    }
}

/* One SYS_PROC_EXEC-family call and its {pid, generation} answer. */
static int proc_exec_submit(uint16_t opcode, const void *params, uint16_t params_size,
                            const void *crate, uint32_t crate_len, uint32_t *out_gen)
{
    /* 8-byte out: the kernel writes {pid, generation} when the crate fits both.
     * out_actual tells us whether the generation half actually arrived. */
    uint32_t out_blob[2] = { 0, 0 };
    uint32_t out_actual = 0;
    /* WITHOUT a deadline, deliberately. Spawn reads the program off the
     * medium INSIDE this call — on a throttled stick that is minutes, and a
     * guessed budget here split one fact into two lies: the caller was told
     * "refused" while the kernel went on to start the child ("late"). The
     * reply is guaranteed either way — success or a real error — so there
     * is nothing for a timer to guard; an answer that never comes is a
     * kernel defect Nightwatch names, not something to paper over. */
    int rc = MfCall1(DECK_SYSTEM, opcode,
                     params, params_size,
                     crate, crate_len,
                     out_blob, sizeof(out_blob), &out_actual,
                     0 /* no deadline */, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_gen) *out_gen = (out_actual >= 8) ? out_blob[1] : 0;
    return (int)out_blob[0];
}

int proc_exec_gen(const char *line, const char *tags, uint32_t *out_gen)
{
    if (out_gen) *out_gen = 0;
    if (!line) return -ERR_INVALID_ARGUMENT;

    /* The program is the line's first word; the whole line, as typed, is the
     * program's Luggage. Leading blanks are not part of what was said. */
    while (*line == ' ' || *line == '\t') line++;
    size_t name_len = 0;
    while (line[name_len] != '\0' && line[name_len] != ' ' && line[name_len] != '\t') name_len++;
    if (name_len == 0) return -ERR_INVALID_ARGUMENT;
    size_t line_len = strlen(line);

    const void *pbuf = NULL; uint16_t psize = 0;
    if (tags && tags[0]) {
        size_t tlen = strlen(tags);
        if (tlen >= PROC_EXEC_TAGS_MAX) return -ERR_INVALID_ARGUMENT;
        pbuf = tags; psize = (uint16_t)tlen;   /* strlen, no NUL — kernel bounds + NUL-terminates */
    }

    /* in_crate: [name][NUL][the line as typed]. */
    size_t crate_len = name_len + 1 + line_len;
    char  *crate     = malloc(crate_len);
    if (!crate) return -ERR_NO_MEMORY;
    memcpy(crate, line, name_len);
    crate[name_len] = '\0';
    memcpy(crate + name_len + 1, line, line_len);

    int pid = proc_exec_submit(SYS_PROC_EXEC, pbuf, psize, crate, (uint32_t)crate_len, out_gen);
    free(crate);
    return pid;
}

bool started_by_system(void)
{
    return cabin_info()->spawner_pid == 0;
}

int proc_exec_file(file_id_t program, const void *luggage, size_t length,
                   const char *tags, uint32_t *generation)
{
    if (generation) *generation = 0;
    if (length > 0 && !luggage) return -ERR_INVALID_ARGUMENT;
    if (length > UINT32_MAX)    return -ERR_INVALID_ARGUMENT;

    /* params: [u64 file id][caller-tag augment]. */
    uint8_t params[PROC_EXEC_TAGS_MAX];
    memcpy(params, &program, sizeof(program));
    size_t psize = sizeof(program);
    if (tags && tags[0]) {
        size_t tlen = strlen(tags);
        if (tlen >= PROC_EXEC_TAGS_MAX - sizeof(program)) return -ERR_INVALID_ARGUMENT;
        memcpy(params + psize, tags, tlen);
        psize += tlen;
    }

    return proc_exec_submit(SYS_PROC_EXEC_FILE, params, (uint16_t)psize,
                            luggage, (uint32_t)length, generation);
}

/* proc_exec_tagged / proc_exec are the generation-agnostic spellings: the wire
 * is identical (the kernel still pid-gates the 8-byte out), they just discard
 * the generation. Existing callers keep their int-pid return unchanged. */
int proc_exec_tagged(const char *filename, const char *tags)
{
    return proc_exec_gen(filename, tags, NULL);
}

int proc_exec(const char *filename) { return proc_exec_tagged(filename, NULL); }

int proc_kill(uint32_t pid)
{
    if (pid == 0)                 return -ERR_INVALID_ARGUMENT; /* 0 == self-exit in kernel; use exit() */
    if (pid == cabin_info()->pid) return -ERR_INVALID_ARGUMENT; /* self-termination is exit()'s job */
    uint32_t target = pid;                                      /* 4-byte form -> kill-other -> PROC_EXIT_KILLED */
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_KILL,
                     &target, (uint16_t)sizeof(target),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

int proc_finish(uint32_t pid, uint32_t generation)
{
    if (generation == 0) return proc_kill(pid);
    if (pid == 0)                 return -ERR_INVALID_ARGUMENT;
    if (pid == cabin_info()->pid) return -ERR_INVALID_ARGUMENT;

    /* 12-byte form: [u32 pid][i32 code][u32 generation]. The code is unread
     * for a kill-other — the one ending never chose it — but it holds the
     * place the 8-byte form defined, so the three forms stay one layout. */
    uint32_t params[3] = { pid, 0u, generation };
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_KILL,
                     params, (uint16_t)sizeof(params),
                     NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

/* Where the crew answer starts and how big a record's fixed half is —
 * boxos_decks.h SYSTEM_OP_PROC_CREW holds the layout itself. */
#define PROC_CREW_HEADER  8u
#define PROC_CREW_FIXED   24u
/* First ask, and the ceiling the kernel's own bounce keeps. Between them the
 * ask doubles: a crew of three that answers in 8 KiB costs one syscall, and a
 * crew of a thousand costs five rather than a guessed allocation. */
#define PROC_CREW_FIRST_ASK  8192u
#define PROC_CREW_MAX_ASK    262144u

int proc_crew(const char *tag, ProcMate **out_mates, uint32_t *out_total)
{
    if (!tag || !tag[0] || !out_mates) return -ERR_INVALID_ARGUMENT;
    *out_mates = NULL;
    if (out_total) *out_total = 0;

    size_t tag_len = strlen(tag);
    if (tag_len >= 64) return -ERR_INVALID_ARGUMENT;

    uint32_t cap   = PROC_CREW_FIRST_ASK;
    uint8_t *blob  = NULL;
    uint32_t got   = 0;
    uint32_t delivered = 0, total = 0;

    for (;;) {
        uint8_t *fresh = (uint8_t *)malloc(cap);
        if (!fresh) { free(blob); return -ERR_NO_MEMORY; }
        free(blob);
        blob = fresh;

        got = 0;
        int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_PROC_CREW,
                         tag, (uint16_t)tag_len,
                         NULL, 0,
                         blob, cap, &got,
                         BOX_ANSWER_GUARANTEED, NULL);
        if (rc != 0) { free(blob); return box_fail(rc); }
        if (got < PROC_CREW_HEADER) { free(blob); return -ERR_INTERNAL; }

        memcpy(&delivered, blob + 0, sizeof(uint32_t));
        memcpy(&total,     blob + 4, sizeof(uint32_t));

        /* Everyone who wears the tag is in hand, or the room is at the
         * ceiling and what came back is what there was room for — said as a
         * count either way, never as a silent truncation. */
        if (delivered >= total || cap >= PROC_CREW_MAX_ASK) break;
        cap = cap * 2u > PROC_CREW_MAX_ASK ? PROC_CREW_MAX_ASK : cap * 2u;
    }

    if (out_total) *out_total = total;
    if (delivered == 0) { free(blob); return 0; }

    /* One allocation holds the array and every tag string after it, so the
     * caller frees once and no member outlives its own name. */
    size_t names = (size_t)got - PROC_CREW_HEADER
                 - (size_t)delivered * PROC_CREW_FIXED;
    size_t bytes = (size_t)delivered * sizeof(ProcMate) + names + delivered;
    ProcMate *mates = (ProcMate *)malloc(bytes);
    if (!mates) { free(blob); return -ERR_NO_MEMORY; }

    char    *name_at = (char *)(mates + delivered);
    uint32_t at      = PROC_CREW_HEADER;
    for (uint32_t i = 0; i < delivered; i++) {
        if (at + PROC_CREW_FIXED > got) { free(mates); free(blob); return -ERR_INTERNAL; }
        uint32_t tags_len = 0;
        memcpy(&mates[i].pid,        blob + at +  0, sizeof(uint32_t));
        memcpy(&mates[i].generation, blob + at +  4, sizeof(uint32_t));
        memcpy(&mates[i].state,      blob + at +  8, sizeof(uint32_t));
        memcpy(&tags_len,            blob + at + 12, sizeof(uint32_t));
        memcpy(&mates[i].cpu_us,     blob + at + 16, sizeof(uint64_t));
        at += PROC_CREW_FIXED;
        if (at + tags_len > got) { free(mates); free(blob); return -ERR_INTERNAL; }

        memcpy(name_at, blob + at, tags_len);
        name_at[tags_len] = '\0';
        mates[i].tags = name_at;
        name_at += tags_len + 1;
        at      += tags_len;
    }

    free(blob);
    *out_mates = mates;
    return (int)delivered;
}

int journey_read(JourneyHead *head, JourneyMoment moments[JOURNEY_MOMENTS_MAX])
{
    if (!head || !moments) return -ERR_INVALID_ARGUMENT;

    uint8_t  answer[sizeof(JourneyHead) + JOURNEY_MOMENTS_MAX * sizeof(JourneyMoment)];
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_JOURNEY, NULL, 0, NULL, 0, answer, sizeof(answer), &got,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    if (got < sizeof(JourneyHead)) return -ERR_INTERNAL;

    memcpy(head, answer, sizeof(JourneyHead));
    if (head->count > JOURNEY_MOMENTS_MAX || head->from_loader > head->count ||
        got != sizeof(JourneyHead) + (uint32_t)head->count * sizeof(JourneyMoment))
        return -ERR_INTERNAL;
    memcpy(moments, answer + sizeof(JourneyHead), (uint32_t)head->count * sizeof(JourneyMoment));
    return 0;
}

/* The roster's answer — boxos_decks.h SYSTEM_OP_ROSTER holds the layout — and
 * its asks, which grow as the crew's do. */
#define ROSTER_HEADER   8u
#define ROSTER_PROGRAM  56u
#define ROSTER_STRAND   24u

/* The answer laid out for the caller: the programs, then every strand, then
 * every tag list — one allocation. False when the answer does not hold what
 * it says. */
static bool roster_lay(const uint8_t *blob, uint32_t got, uint32_t delivered, RosterProgram *programs,
                       RosterStrand *strands, char *names)
{
    uint32_t at = ROSTER_HEADER;
    for (uint32_t i = 0; i < delivered; i++) {
        RosterProgram *one = &programs[i];
        uint32_t       tags_len = 0;
        if (at + ROSTER_PROGRAM > got) return false;
        memcpy(&one->pid, blob + at + 0, sizeof(uint32_t));
        memcpy(&one->generation, blob + at + 4, sizeof(uint32_t));
        memcpy(&one->spawner_pid, blob + at + 8, sizeof(uint32_t));
        memcpy(&one->spawner_generation, blob + at + 12, sizeof(uint32_t));
        memcpy(&one->program_file, blob + at + 16, sizeof(uint64_t));
        memcpy(&one->began_us, blob + at + 24, sizeof(uint64_t));
        memcpy(&one->memory_bytes, blob + at + 32, sizeof(uint64_t));
        memcpy(&one->raised, blob + at + 40, sizeof(uint32_t));
        memcpy(&one->strand_count, blob + at + 44, sizeof(uint32_t));
        memcpy(&tags_len, blob + at + 48, sizeof(uint32_t));
        at += ROSTER_PROGRAM;
        if ((uint64_t)at + (uint64_t)one->strand_count * ROSTER_STRAND + tags_len > got) return false;
        one->strands = strands;
        for (uint32_t s = 0; s < one->strand_count; s++, strands++, at += ROSTER_STRAND) {
            memcpy(&strands->pid, blob + at + 0, sizeof(uint32_t));
            memcpy(&strands->generation, blob + at + 4, sizeof(uint32_t));
            memcpy(&strands->state, blob + at + 8, sizeof(uint32_t));
            memcpy(&strands->core, blob + at + 12, sizeof(uint32_t));
            memcpy(&strands->cpu_us, blob + at + 16, sizeof(uint64_t));
        }
        memcpy(names, blob + at, tags_len);
        names[tags_len] = '\0';
        one->tags = names;
        names += tags_len + 1;
        at += tags_len;
    }
    return true;
}

int proc_roster(RosterProgram **out_programs, uint32_t *out_total)
{
    if (!out_programs) return -ERR_INVALID_ARGUMENT;
    *out_programs = NULL;
    if (out_total) *out_total = 0;

    uint32_t cap = PROC_CREW_FIRST_ASK, got = 0, delivered = 0, total = 0;
    uint8_t *blob = NULL;
    for (;;) {
        uint8_t *fresh = (uint8_t *)malloc(cap);
        if (!fresh) { free(blob); return -ERR_NO_MEMORY; }
        free(blob);
        blob = fresh;
        got  = 0;
        int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_ROSTER, NULL, 0, NULL, 0, blob, cap, &got,
                         BOX_ANSWER_GUARANTEED, NULL);
        if (rc != 0) { free(blob); return box_fail(rc); }
        if (got < ROSTER_HEADER) { free(blob); return -ERR_INTERNAL; }
        memcpy(&delivered, blob + 0, sizeof(uint32_t));
        memcpy(&total, blob + 4, sizeof(uint32_t));
        if (delivered >= total || cap >= PROC_CREW_MAX_ASK) break;
        cap = cap * 2u > PROC_CREW_MAX_ASK ? PROC_CREW_MAX_ASK : cap * 2u;
    }
    if (out_total) *out_total = total;
    if (delivered == 0) { free(blob); return 0; }

    /* Every strand and every name of the answer counted first, for the one
     * allocation that holds them. */
    uint64_t strand_total = 0, name_bytes = 0;
    uint64_t at = ROSTER_HEADER;
    for (uint32_t i = 0; i < delivered; i++) {
        uint32_t strand_count = 0, tags_len = 0;
        if (at + ROSTER_PROGRAM > got) { free(blob); return -ERR_INTERNAL; }
        memcpy(&strand_count, blob + at + 44, sizeof(uint32_t));
        memcpy(&tags_len, blob + at + 48, sizeof(uint32_t));
        strand_total += strand_count;
        name_bytes += (uint64_t)tags_len + 1;
        at += ROSTER_PROGRAM + (uint64_t)strand_count * ROSTER_STRAND + tags_len;
    }
    size_t bytes = (size_t)delivered * sizeof(RosterProgram) + (size_t)strand_total * sizeof(RosterStrand) +
                   (size_t)name_bytes;
    RosterProgram *programs = (RosterProgram *)malloc(bytes);
    if (!programs) { free(blob); return -ERR_NO_MEMORY; }
    RosterStrand *strands = (RosterStrand *)(programs + delivered);
    if (!roster_lay(blob, got, delivered, programs, strands, (char *)(strands + strand_total))) {
        free(programs);
        free(blob);
        return -ERR_INTERNAL;
    }
    free(blob);
    *out_programs = programs;
    return (int)delivered;
}

/* =========================================================================
 *  Tags
 * ========================================================================= */

static int proc_tag_op(const char *tag, uint16_t opcode, uint8_t *out_byte)
{
    if (!tag) return -ERR_INVALID_ARGUMENT;
    size_t tlen = strlen(tag);
    if (tlen == 0) return -ERR_INVALID_ARGUMENT;

    uint32_t my_pid = cabin_info()->pid;
    uint8_t out_storage = 0;
    int rc = MfCall1(DECK_SYSTEM, opcode,
                     &my_pid, sizeof(my_pid),
                     tag, (uint32_t)tlen,
                     out_byte ? &out_storage : NULL,
                     out_byte ? 1 : 0,
                     NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (out_byte) *out_byte = out_storage;
    return box_fail(rc);
}

int proc_tag_add(const char *tag)    { return proc_tag_op(tag, SYS_TAG_ADD,    NULL); }
int proc_tag_remove(const char *tag) { return proc_tag_op(tag, SYS_TAG_REMOVE, NULL); }

int proc_tag_check(const char *tag, bool *has_tag)
{
    if (!tag || !has_tag) return -ERR_INVALID_ARGUMENT;
    uint8_t flag = 0;
    int rc = proc_tag_op(tag, SYS_TAG_CHECK, &flag);
    if (rc != 0) return box_fail(rc);
    *has_tag = (flag != 0);
    return OK;
}

/* =========================================================================
 *  System power
 * ========================================================================= */

int reboot(void)
{
    /* hw.system.reboot is noreturn on success; reaching the return is always
     * a failure, so surface the real cause (or ERR_INTERNAL if the call came
     * back OK yet the machine did not reboot). */
    int rc = MfCall1(DECK_HARDWARE, HW_SYSTEM_REBOOT,
                     NULL, 0, NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return rc != 0 ? box_fail(rc) : -ERR_INTERNAL;
}

int shutdown(void)
{
    int rc = MfCall1(DECK_HARDWARE, HW_SYSTEM_SHUTDOWN,
                     NULL, 0, NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return rc != 0 ? box_fail(rc) : -ERR_INTERNAL;
}

int sysinfo(system_info_t *info)
{
    if (!info) return -ERR_INVALID_ARGUMENT;

    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_INFO,
                     NULL, 0, NULL, 0,
                     info, sizeof(*info), &got, BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0)             return box_fail(rc);
    if (got < sizeof(*info)) return -ERR_INTERNAL;
    info->name[sizeof(info->name) - 1] = '\0';
    return 0;
}

int system_resources(system_resources_t *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;

    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_RESOURCES,
                     NULL, 0, NULL, 0,
                     out, sizeof(*out), &got, BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(*out)) return -ERR_INTERNAL;
    return 0;
}

int system_core_usage(uint32_t core_index, system_core_usage_t *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;

    uint8_t blob[24] = {0};
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_CORE_USAGE,
                     &core_index, sizeof(core_index),
                     NULL, 0, blob, sizeof(blob), &got,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    if (got < sizeof(blob)) return -ERR_INTERNAL;

    memcpy(&out->cpu_busy_tsc, blob + 0, 8);
    memcpy(&out->cpu_total_tsc, blob + 8, 8);
    out->parked = blob[16];
    out->kernel = blob[17];
    memset(out->_pad, 0, sizeof(out->_pad));
    return 0;
}

/* =========================================================================
 *  Filesystem maintenance / telemetry
 * ========================================================================= */

int defrag(file_id_t file_id)
{
    uint32_t score = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_DEFRAG,
                     &file_id, sizeof(file_id),
                     NULL, 0,
                     &score, sizeof(score), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    return (int)score;
}

int fragmentation(void)
{
    uint8_t out[12] = {0};
    int rc = MfCall1(DECK_SYSTEM, SYS_FRAG_SCORE,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return box_fail(rc);
    uint32_t score = 0;
    memcpy(&score, out, 4);
    return (int)score;
}

int perf_dump(void)
{
    int rc = MfCall1(DECK_SYSTEM, SYS_PERF_DUMP,
                     NULL, 0, NULL, 0, NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    return box_fail(rc);
}

/* =========================================================================
 *  EFI / Secure Boot introspection
 * ========================================================================= */

int efi_info(efi_info_t *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;

    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_EFI_INFO,
                     NULL, 0, NULL, 0,
                     out, sizeof(*out), &got, BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(*out)) return -ERR_INTERNAL;
    return 0;
}

int machine_info(machine_info_t *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_MACHINE_INFO, NULL, 0, NULL, 0, out, sizeof(*out), &got,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(*out)) return -ERR_INTERNAL;
    return 0;
}

/* Whether `text` begins with `word`, the case folded, and no letter follows. */
static bool machine_word_begins(const char *text, const char *word)
{
    size_t n = 0;
    for (; word[n]; n++) {
        char a = text[n], b = word[n];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    char next = text[n];
    return !((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z'));
}

/* The brands makers are known by, against the long names their firmware
 * gives ("ASUSTeK COMPUTER INC.", "Micro-Star International Co., Ltd."),
 * matched at the beginning of the maker's name. */
static const struct {
    const char *begins;
    const char *brand;
} machine_brands[] = {
    { "asustek", "ASUS" },     { "asus", "ASUS" },          { "lenovo", "Lenovo" },     { "dell", "Dell" },
    { "hewlett-packard", "HP" }, { "hp", "HP" },            { "acer", "Acer" },         { "apple", "Apple" },
    { "micro-star", "MSI" },   { "msi", "MSI" },            { "gigabyte", "Gigabyte" }, { "samsung", "Samsung" },
    { "microsoft", "Microsoft" }, { "huawei", "Huawei" },   { "honor", "Honor" },       { "xiaomi", "Xiaomi" },
    { "timi", "Xiaomi" },      { "toshiba", "Toshiba" },    { "dynabook", "Dynabook" }, { "sony", "Sony" },
    { "fujitsu", "Fujitsu" },  { "framework", "Framework" }, { "intel", "Intel" },      { "asrock", "ASRock" },
    { "biostar", "Biostar" },  { "supermicro", "Supermicro" }, { "razer", "Razer" },    { "lg electronics", "LG" },
    { "panasonic", "Panasonic" }, { "medion", "Medion" },   { "system76", "System76" }, { "google", "Google" },
    { "chuwi", "Chuwi" },      { "qemu", "QEMU" },          { "innotek", "VirtualBox" }, { "vmware", "VMware" },
};

/* The brand `maker` is known by into `out`: a known one, else the maker's
 * first word, a comma or a dot after it left off. */
static void machine_brand(const char *maker, char *out, size_t room)
{
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(machine_brands) / sizeof(machine_brands[0]); i++)
        if (machine_word_begins(maker, machine_brands[i].begins)) {
            snprintf(out, room, "%s", machine_brands[i].brand);
            return;
        }
    size_t n = 0;
    while (maker[n] && maker[n] != ' ' && maker[n] != ',' && n + 1 < room) n++;
    while (n && maker[n - 1] == '.') n--;
    memcpy(out, maker, n);
    out[n] = '\0';
}

size_t machine_shown(const machine_info_t *m, char *out, size_t room)
{
    if (!out || !room) return 0;
    out[0] = '\0';
    if (!m) return 0;
    char        brand[MACHINE_TEXT_MOST];
    const char *maker = m->maker, *model = m->product;
    machine_brand(maker, brand, sizeof(brand));
    if (strcmp(brand, "Lenovo") == 0 && m->version[0]) model = m->version;
    if (!model[0] && m->board[0]) {
        model = m->board;
        maker = m->board_maker[0] ? m->board_maker : m->maker;
        machine_brand(maker, brand, sizeof(brand));
    }
    int n = !model[0]                                        ? snprintf(out, room, "%s", brand)
          : !brand[0] || machine_word_begins(model, brand)   ? snprintf(out, room, "%s", model)
                                                             : snprintf(out, room, "%s %s", brand, model);
    if (n < 0) { out[0] = '\0'; return 0; }
    return (size_t)n < room ? (size_t)n : room - 1u;
}

int efi_esrt_entry(uint32_t idx, efi_esrt_entry_t *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    uint8_t  params[4];
    memcpy(params, &idx, 4);
    uint8_t  blob[EFI_ESRT_ENTRY_SIZE] = {0};
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_EFI_ESRT_GET,
                     NULL, 0, params, sizeof(params),
                     blob, sizeof(blob), &got, BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(blob)) return -ERR_INTERNAL;
    memcpy(out->fw_class,                       blob + 0,  16);
    memcpy(&out->fw_type,                       blob + 16, 4);
    memcpy(&out->fw_version,                    blob + 20, 4);
    memcpy(&out->lowest_supported_fw_version,   blob + 24, 4);
    memcpy(&out->capsule_flags,                 blob + 28, 4);
    memcpy(&out->last_attempt_version,          blob + 32, 4);
    memcpy(&out->last_attempt_status,           blob + 36, 4);
    return 0;
}

int efi_verify_pe(const void *pe_buf, uint32_t pe_size, efi_verify_t *out)
{
    if (!out || !pe_buf || pe_size == 0) return -ERR_INVALID_ARGUMENT;
    uint8_t  blob[EFI_VERIFY_PE_OUT_SIZE] = {0};
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_EFI_VERIFY_PE,
                     pe_buf, pe_size, NULL, 0,
                     blob, sizeof(blob), &got, BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(blob)) return -ERR_INTERNAL;
    uint32_t result_u32;
    uint32_t pe_size_u32;
    memcpy(&result_u32,  blob + 0,  4);
    memcpy(&pe_size_u32, blob + 4,  4);
    out->result  = (efi_verify_result_t)result_u32;
    out->pe_size = pe_size_u32;
    memcpy(out->pe_sha256,     blob + 8,  32);
    memcpy(out->signer_sha256, blob + 40, 32);
    return 0;
}
