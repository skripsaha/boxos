/*
 * roster_ops.c — SYSTEM_OP_ROSTER: every program aboard, each with its strands.
 *
 * proc.crew answers who wears one tag, and answers by strand: a program of two
 * strands is two records, each with the program's tags, and nothing says they
 * are one program. Asked what runs on the machine, that is a person's program
 * counted twice. The roster answers by program — a cabin — with its strands
 * inside it, and with what the kernel knows of the program and no program can
 * learn of another: when it began, which program started it, its file, the
 * memory mapped for it and how often the system raised it again after a fall.
 *
 * Every record is a program that was live at the walk; none is cut short — one
 * that does not fit whole is left out of `delivered` and shows as the
 * shortfall against `total`, so the asker comes back with room, as for crew.
 *
 * Lock ordering: the process list lock alone, while the strands are read; a
 * strand is pinned (process_find_ref) for its program's record, outside it —
 * process_snapshot_tags takes process_lock itself.
 */

#include "system_deck.h"
#include "boxos_decks.h"
#include "boxos_sizes.h"
#include "cabin.h"
#include "cabin_info.h"
#include "crate_io.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "process.h"
#include "vmm.h"
#include "klib.h"

#define ROSTER_ANSWER_MAX  262144u   /* every seat with its tags, as proc.crew's */
#define ROSTER_HEADER      8u
#define ROSTER_PROGRAM     56u
#define ROSTER_STRAND      24u

/* A strand as the walk saw it. Cabin is the program it is of, a key compared
 * and never followed once the list lock is let go. */
typedef struct {
    const cabin_t *Cabin;
    uint32_t       Pid;
    uint32_t       Generation;
    uint32_t       State;
    uint32_t       Core;
    uint64_t       CpuUs;
} RosterSeen;

/* Every live strand that is a program's, under the list lock: counted, made
 * room for, filled — never a fixed tray that drops the last one. NULL with
 * *count 0 when there are none; NULL with *count nonzero when there was no
 * memory. */
static RosterSeen *RosterWalk(uint32_t *count)
{
    RosterSeen *seen = NULL;
    uint32_t    room = 0;
    for (;;) {
        uint32_t found = 0;
        process_list_lock();
        for (process_t *iter = process_get_first(); iter; iter = iter->next) {
            process_state_t state = iter->state;
            if (state == PROC_CRASHED || state == PROC_DONE || !iter->cabin) continue;
            /* Dead from the moment its death is committed (as proc.crew). */
            if (__atomic_load_n(&iter->touch_cleaned, __ATOMIC_ACQUIRE)) continue;
            if (found < room)
                seen[found] = (RosterSeen){ iter->cabin, iter->pid, iter->generation, (uint32_t)state,
                                            iter->home_core, process_cpu_time_us(iter) };
            found++;
        }
        process_list_unlock();
        *count = found;
        if (found <= room) return seen;
        kfree(seen);
        room = found;
        if (!(seen = kmalloc((size_t)room * sizeof(RosterSeen)))) return NULL;
    }
}

/* Strands of one program side by side. */
static int RosterOrder(const void *a, const void *b, void *ctx)
{
    (void)ctx;
    const RosterSeen *x = a, *y = b;
    if (x->Cabin != y->Cabin) return (uintptr_t)x->Cabin < (uintptr_t)y->Cabin ? -1 : 1;
    return x->Pid < y->Pid ? -1 : x->Pid > y->Pid;
}

/* One strand's record at `at`. */
static void RosterPutStrand(uint8_t *at, const RosterSeen *strand)
{
    memcpy(at + 0, &strand->Pid, sizeof(uint32_t));
    memcpy(at + 4, &strand->Generation, sizeof(uint32_t));
    memcpy(at + 8, &strand->State, sizeof(uint32_t));
    memcpy(at + 12, &strand->Core, sizeof(uint32_t));
    memcpy(at + 16, &strand->CpuUs, sizeof(uint64_t));
}

typedef enum { ROSTER_PUT, ROSTER_NO_ROOM, ROSTER_ENDED } RosterPut;

/* The program the strands group[0..strands) are, written at `at` when it fits
 * in `room` bytes, *wrote its bytes. ROSTER_ENDED when the strand it is read
 * by is gone since the walk: the program is no longer aboard. */
static RosterPut RosterPutProgram(uint8_t *at, uint64_t room, const RosterSeen *group, uint32_t strands,
                                  uint64_t *wrote)
{
    const uint64_t fixed = ROSTER_PROGRAM + (uint64_t)strands * ROSTER_STRAND;
    if (fixed >= room) return ROSTER_NO_ROOM;

    /* Pinned, and still the strand the walk saw: its cabin lives while the
     * pin is held. */
    process_t *pin = process_find_ref(group[0].Pid);
    if (!pin) return ROSTER_ENDED;
    if (pin->generation != group[0].Generation || pin->cabin != group[0].Cabin) {
        process_ref_dec(pin);
        return ROSTER_ENDED;
    }
    const cabin_t   *cabin = pin->cabin;
    const CabinInfo *info  = (const CabinInfo *)vmm_phys_to_virt(cabin->cabin_info_phys);
    uint32_t         pid = group[0].Pid, generation = group[0].Generation;
    uint64_t         file = 0;
    uint32_t         raised = 0;
    (void)CabinProgram(cabin, &pid, &generation);
    if (cabin_info_valid(info)) {
        file   = info->program_file;
        raised = info->raised;
    }
    const uint64_t memory   = cabin->vmm ? (uint64_t)cabin->vmm->user_pages * PAGE_SIZE : 0;
    const uint32_t tags_len = (uint32_t)process_snapshot_tags(pin, (char *)(at + fixed), (size_t)(room - fixed));
    const uint32_t spawner = cabin->spawner_pid, spawner_gen = cabin->spawner_gen, unused = 0;
    const uint64_t began   = cabin->began_us;
    process_ref_dec(pin);
    /* A list that filled the room to its last byte may have been cut there
     * (process_snapshot_tags does not say): left out whole, as crew does. */
    if ((uint64_t)tags_len + 1 >= room - fixed) return ROSTER_NO_ROOM;

    memcpy(at + 0, &pid, sizeof(uint32_t));
    memcpy(at + 4, &generation, sizeof(uint32_t));
    memcpy(at + 8, &spawner, sizeof(uint32_t));
    memcpy(at + 12, &spawner_gen, sizeof(uint32_t));
    memcpy(at + 16, &file, sizeof(uint64_t));
    memcpy(at + 24, &began, sizeof(uint64_t));
    memcpy(at + 32, &memory, sizeof(uint64_t));
    memcpy(at + 40, &raised, sizeof(uint32_t));
    memcpy(at + 44, &strands, sizeof(uint32_t));
    memcpy(at + 48, &tags_len, sizeof(uint32_t));
    memcpy(at + 52, &unused, sizeof(uint32_t));
    uint8_t *strand = at + ROSTER_PROGRAM;
    for (int main_pass = 1; main_pass >= 0; main_pass--) {
        for (uint32_t i = 0; i < strands; i++) {
            if ((group[i].Pid == pid) != (main_pass == 1)) continue;
            RosterPutStrand(strand, &group[i]);
            strand += ROSTER_STRAND;
        }
    }
    *wrote = fixed + tags_len;
    return ROSTER_PUT;
}

static int SysRoster(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc || op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < ROSTER_HEADER) return ERR_BUFFER_TOO_SMALL;
    const uint64_t size = out->capacity > ROSTER_ANSWER_MAX ? ROSTER_ANSWER_MAX : out->capacity;
    uint8_t       *kp   = crate_out_alloc(out, size);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint32_t    count = 0;
    RosterSeen *seen  = RosterWalk(&count);
    if (!seen && count) {
        crate_buf_free(kp);
        return ERR_NO_MEMORY;
    }
    KSort(seen, count, sizeof(RosterSeen), RosterOrder, NULL);
    uint32_t delivered = 0, total = 0;
    uint64_t used      = ROSTER_HEADER;
    for (uint32_t first = 0, end; first < count; first = end) {
        for (end = first + 1; end < count && seen[end].Cabin == seen[first].Cabin; end++) {}
        uint64_t  wrote = 0;
        RosterPut put   = used < size ? RosterPutProgram(kp + used, size - used, seen + first, end - first, &wrote)
                                      : ROSTER_NO_ROOM;
        if (put == ROSTER_ENDED) continue;
        total++;
        if (put == ROSTER_PUT) {
            used += wrote;
            delivered++;
        }
    }
    kfree(seen);

    memcpy(kp + 0, &delivered, sizeof(uint32_t));
    memcpy(kp + 4, &total, sizeof(uint32_t));
    int rc = crate_out_commit(out, ctx, kp, used);
    crate_buf_free(kp);
    if (rc != OK) return rc;
    out->size = used;
    return OK;
}

error_t RosterOpsRegister(void)
{
    error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, SYSTEM_OP_ROSTER), SysRoster, OP_AUTH_NONE,
                                    "system.roster");
    if (rc != OK && rc != ERR_ALREADY_EXISTS) {
        kprintf("[Roster] register system.roster failed: %s\n", ErrorString(rc));
        return rc;
    }
    return OK;
}
