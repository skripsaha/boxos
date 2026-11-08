#ifndef BOXOS_KCTX_H
#define BOXOS_KCTX_H

/*
 * boxos_kctx.h — shared kernel/userspace ABI for the KResult `context` tag.
 *
 * Every Result the kernel pushes into a Cabin's ResultRing carries a 32-bit
 * `context` field telling the consumer which subsystem produced it. boxlib
 * uses this to fan out kernel-broadcast Touches (KCTX_TOUCH) from manifest
 * replies and IPC payloads without an extra round-trip.
 *
 * Keep the enum byte-identical with the kernel-internal KResultContext in
 * src/include/kresult.h (it now includes this header). Adding a new context
 * means: append here, rebuild kernel+boxlib together, update consumers.
 */

typedef enum {
    KCTX_NONE   = 0,
    KCTX_PMM    = 1,
    KCTX_VMM    = 2,
    KCTX_TAGFS  = 3,
    KCTX_AHCI   = 4,
    KCTX_SCHED  = 5,
    KCTX_IPC    = 6,
    KCTX_GUIDE  = 7,
    KCTX_FRIEND = 8,
    KCTX_TOUCH  = 9,
    KCTX_STORAGE = 10,   /* async file-I/O completion (box::ferry) — carries a
                          * per-strand correlation waybill in Result.data_addr.
                          * Stamped ONLY when the storage op requested one
                          * (waybill != 0); synchronous fread/fwrite keep
                          * KCTX_GUIDE. Fully isolated: boxlib routes these to
                          * the ferry stash so no other consumer ever sees one. */
    KCTX_ASK    = 11,    /* a question (system.ask) delivered to the carrier
                          * it was asked of: sender_pid is the asker, the high
                          * 24 bits its cookie — the pair names the question
                          * for system.answer — and the payload begins with
                          * AskFrom (below). Only the kernel stamps this kind,
                          * so a plain send cannot pass for one. */
} KResultContext;

/*
 * `context` carries TWO facts packed in one u32:
 *   low 8 bits  — the KResultContext kind above;
 *   high 24 bits — the submit's cloakroom token (Pocket.cookie24) on every
 *                  Result that answers a synchronous submit. Zero on
 *                  results that answer nobody (IPC payload deliveries,
 *                  Touch, ferry completions).
 * The waiter adopts only the Result whose token matches its own submit;
 * anything else of a reply kind is a proven orphan of an earlier call and
 * is dropped where it stands. Readers therefore compare kinds through
 * KCTX_KIND(), never against the raw field.
 */
#define KCTX_KIND(ctx)        ((uint32_t)(ctx) & 0xFFu)
#define KCTX_COOKIE24(ctx)    ((uint32_t)(ctx) >> 8)
#define KCTX_PACK24(kind, ck) ((uint32_t)(kind) | ((uint32_t)(ck) << 8))

/* Who asked, in the kernel's words: the head of every KCTX_ASK payload, the
 * question after it. The asking strand is sender_pid in this incarnation; its
 * program is the cabin it runs in, named by the strand that founded it — the
 * (pid, generation) that cabin's own CabinInfo carries. A carrier that keeps
 * something per program counts it here, never by what the question says. */
typedef struct AskFrom {
    uint32_t generation;
    uint32_t program_pid;
    uint32_t program_generation;
} AskFrom;

#endif /* BOXOS_KCTX_H */
