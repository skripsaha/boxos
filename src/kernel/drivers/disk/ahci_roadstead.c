#include "ahci.h"
#include "klib.h"
#include "amp.h"
#include "atomics.h"
#include "hourglass.h"
#include "scheduler.h"
#include "system_halt.h"

/*
 * An AHCI port's roadstead (core/boardroom/roadstead.h): who gets a command
 * slot, who gets the whole port, and in what order.
 *
 * Every command on a port comes through AhciAdmit. A slot is handed out on the
 * spot only when nothing is waiting, the door is open and the engine is not
 * halted on a fault; otherwise the command moors, in the same critical section
 * that found it no place. From then on it is called in by whoever makes room —
 * a completion handing its slot back, a flush giving the door back, a recovery
 * restarting the engine — through the port's knock, and by any caller standing
 * over its own berth, which calls the roadstead in as it waits.
 *
 * ERR_BUSY does not come out of here. It used to: a port with its door shut
 * for a FLUSH CACHE EXT, or with every NCQ tag out, or — on a drive without NCQ
 * — with its one slot taken by any other command at all, refused a read, and
 * the storage deck turned that into ERR_IO for the program.
 */

/* Nothing outstanding on the HBA for this port: the fact a whole-port command
 * is issued on, and the one an abandoned door is opened on. */
static inline bool AhciPortQuiet(const ahci_port_t* port)
{
    return (port->regs->sact & port->slot_mask) == 0 &&
           (port->regs->ci   & port->slot_mask) == 0;
}

RoadsteadAnswer AhciAdmit(ahci_port_t* port, RoadsteadBerth* berth, uint8_t* slot_out)
{
    spin_lock(&port->lock);
    if (!port->active) {
        spin_unlock(&port->lock);
        return ROADSTEAD_GONE;
    }
    if (!port->halted && !port->slot_held && port->slot_bitmap != 0 &&
        RoadsteadIsEmpty(&port->roadstead)) {
        uint8_t slot = (uint8_t)__builtin_ctz(port->slot_bitmap);
        port->slot_bitmap &= ~(1U << slot);
        port->aboard[slot] = berth->answered ? berth : NULL;
        spin_unlock(&port->lock);
        *slot_out = slot;
        return ROADSTEAD_TAKEN;
    }
    RoadsteadMoor(&port->roadstead, berth);
    spin_unlock(&port->lock);
    return ROADSTEAD_WAIT;
}

/* A slot comes back. The berth at the head is called in by the knock rather
 * than here, because this runs in the completion interrupt. A port that left
 * takes nothing back: its pool was emptied when it did. */
void AhciSlotReturn(ahci_port_t* port, uint8_t slot)
{
    if (slot >= AHCI_MAX_SLOTS) {
        return;
    }
    spin_lock(&port->lock);
    port->aboard[slot] = NULL;
    if (!port->active) {
        spin_unlock(&port->lock);
        return;
    }
    port->slot_bitmap |= (1U << slot);
    bool waiting = !RoadsteadIsEmpty(&port->roadstead);
    spin_unlock(&port->lock);
    if (waiting) {
        KnockOn(&port->roadstead_knock);
    }
}

error_t AhciBoard(ahci_port_t* port, RoadsteadBerth* berth, uint8_t slot)
{
    error_t err = ahci_build_io(port->port_num, slot, berth->lba,
                                (uint16_t)berth->sectors, berth->dma_phys,
                                berth->write);
    if (err != OK) {
        AhciSlotReturn(port, slot);
        return err;
    }
    __atomic_fetch_add(&port->stats.cmd_count, 1, __ATOMIC_RELAXED);

    volatile ahci_port_regs_t* regs = port->regs;
    uint32_t bit = 1U << slot;

    spin_lock(&port->lock);
    if (!port->active) {
        port->aboard[slot] = NULL;
        spin_unlock(&port->lock);
        return ERR_DEVICE_NOT_READY;
    }
    if (port->halted) {
        /* The engine stopped on a fault between admission and here. The slot
         * goes back and the berth to the front of the roadstead, where the
         * recovery that restarts the engine calls it in first. */
        port->slot_bitmap |= bit;
        port->aboard[slot] = NULL;
        RoadsteadMoorFirst(&port->roadstead, berth);
        spin_unlock(&port->lock);
        return OK;
    }
    /* PxCI / PxSACT are write-1-to-set, so a direct single-bit store is the
     * arm; an RMW `|=` could re-arm a slot the HBA just cleared. For NCQ,
     * PxSACT is set before PxCI (AHCI 1.3.1 §5.5.3). The submit timestamp and
     * the issued_mask bit go in under the lock, where ahci_watchdog_scan and
     * AhciPortFault read them. */
    if (port->ncq) {
        regs->sact = bit;
    }
    regs->ci = bit;
    port->submit_tsc[slot] = rdtsc();
    __sync_fetch_and_or(&port->issued_mask, bit);
    spin_unlock(&port->lock);
    return OK;
}

/* Call the roadstead in, as far as it can go. Any core may run this at any
 * moment — the knock's run on the drain core, a caller standing over its own
 * berth — and each berth leaves the list under the lock, so one is never
 * called twice. A caller that did not stay is issued here; one that stays is
 * told its slot and issues its own command. */
static void AhciCallIn(ahci_port_t* port)
{
    for (;;) {
        uint8_t         abandoned_by = 0xFF;
        RoadsteadBerth* berth;
        uint8_t         slot;
        bool            whole = false;
        bool            go    = false;

        /* Nobody waiting and no door to look at: the lock is not taken, since
         * a caller standing over the port asks this on every turn. A berth
         * moored a moment later waits on a slot, a door or a recovery, and
         * whichever frees it knocks. */
        if (!__atomic_load_n(&port->roadstead.head, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&port->slot_held, __ATOMIC_ACQUIRE)) {
            return;
        }

        spin_lock(&port->lock);
        if (!port->active) {
            spin_unlock(&port->lock);
            return;
        }
        /* A door held by a core that is no longer running — every AP once an
         * orderly halt has stopped them, or any core that died holding it —
         * would never be given back. Once the device owes nothing it is opened,
         * the same fact a door is first closed on. */
        if (port->slot_held &&
            !amp_core_online(&g_amp.cores[port->door_holder]) &&
            AhciPortQuiet(port)) {
            abandoned_by      = port->door_holder;
            port->slot_held   = false;
            port->slot_bitmap = port->slot_mask;
        }
        berth = RoadsteadHead(&port->roadstead);
        slot  = 0;
        if (berth && !port->halted && !port->slot_held) {
            /* A flush at the head: nothing behind it is called in, however
             * many slots come free, until every one of them has — so the
             * slots drain and the flush is not starved by what came after. */
            if (port->roadstead.door_wanted) {
                if (port->slot_bitmap == port->slot_mask) {
                    port->slot_held   = true;
                    port->door_holder = berth->core;
                    port->slot_bitmap = 0;
                    slot  = (uint8_t)__builtin_ctz(port->slot_mask);
                    whole = true;
                    go    = true;
                }
            } else if (port->slot_bitmap != 0) {
                slot = (uint8_t)__builtin_ctz(port->slot_bitmap);
                port->slot_bitmap &= ~(1U << slot);
                port->aboard[slot] = berth->answered ? berth : NULL;
                go = true;
            }
            if (go) {
                RoadsteadCastOff(&port->roadstead);
            }
        }
        spin_unlock(&port->lock);

        if (abandoned_by != 0xFF) {
            kprintf("[AHCI] port %u: its whole-port door was held by core %u, "
                    "which is no longer running, and the device owes nothing "
                    "— the door is opened rather than waited for\n",
                    port->port_num, abandoned_by);
        }
        if (!go) {
            return;
        }
        if (!berth->answered) {
            RoadsteadCallIn(berth, slot);
            if (whole) {
                return;
            }
            continue;
        }
        error_t err = AhciBoard(port, berth, slot);
        if (err != OK) {
            berth->answered(port->port_num, slot, err, berth->ctx);
        }
    }
}

void AhciRoadsteadKnocked(void* ctx)
{
    ahci_port_t* port = (ahci_port_t*)ctx;
    KnockOpen(&port->roadstead_knock);
    AhciCallIn(port);
}

/* One turn of a caller standing over this port. It never pumps the drain
 * core's batons: a standing caller may itself be a continuation on that core
 * (the asynchronous write's flush is one), and a pump from inside it could run
 * a second flush that would wait behind this one forever. Everything the turn
 * needs, it does in place instead — the recovery a fault owes, the
 * completions the interrupt brings to one core that may have it shut (the
 * halt's roll call runs with interrupts off, on the core they are sent to),
 * and the call-in the knock would have made. */
static void AhciTurn(void* ctx)
{
    ahci_port_t* port = (ahci_port_t*)ctx;
    if (!port->active) {
        return;
    }
    if (AhciRecoveryOwed(port)) {
        AhciRecoverIfOwed(port);
    }
    AhciPortReap(port);
    AhciCallIn(port);
}

void AhciPortTurn(uint8_t port_num)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (port) {
        AhciTurn(port);
    }
}

/* The end of a span a caller stood in, however it ended: the core may be
 * switched away from again, and a core an orderly halt found in the span stops
 * here, holding nothing. */
static void AhciLetGo(const char* after)
{
    SchedulerLetGoHere();
    HaltStopIfOwedHere(after);
}

bool AhciAwaitEngine(uint8_t port_num)
{
    for (;;) {
        ahci_port_t* port = ahci_get_port_state(port_num);
        if (!port) {
            return false;
        }
        if (!__atomic_load_n(&port->halted, __ATOMIC_ACQUIRE)) {
            return true;
        }
        AhciTurn(port);
        cpu_pause();
    }
}

int AhciStandForSlot(uint8_t port_num)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (!port) {
        kprintf("[AHCI] port %u has left the machine — the command is not "
                "issued\n", port_num);
        return -1;
    }

    RoadsteadBerth berth;
    memset(&berth, 0, sizeof(berth));
    berth.want = BERTH_ONE;
    uint8_t slot = 0;

    /* Held from the first ask: a standing caller switched away from while it
     * waits, or while it holds its slot, would keep that slot from a caller
     * on the same core whose interrupts are shut — a system call on one core
     * — and that caller would wait for it for ever. */
    SchedulerHoldHere();
    RoadsteadAnswer answer = AhciAdmit(port, &berth, &slot);
    if (answer == ROADSTEAD_TAKEN) {
        return slot;
    }
    if (answer == ROADSTEAD_WAIT &&
        RoadsteadStandBy(&berth, AhciTurn, port) == BERTH_CALLED) {
        return berth.slot;
    }
    kprintf("[AHCI] port %u left the machine %s — the command is not issued\n",
            port_num, answer == ROADSTEAD_WAIT ? "while a command waited for a slot on it"
                                               : "as a command asked it for a slot");
    AhciLetGo("a command slot it asked for on an AHCI port that left");
    return -1;
}

void AhciLeaveSlot(uint8_t port_num, uint8_t slot)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (port) {
        AhciSlotReturn(port, slot);
    }
    AhciLetGo("the command it stood over on an AHCI port");
}

bool AhciArmStanding(uint8_t port_num, uint8_t slot, bool queued,
                     uint32_t* trouble_at_arm)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (!port || slot >= AHCI_MAX_SLOTS) {
        return false;
    }
    volatile ahci_port_regs_t* regs = port->regs;

    spin_lock(&port->lock);
    if (!port->active || port->halted) {
        spin_unlock(&port->lock);
        return false;
    }
    *trouble_at_arm = __atomic_load_n(&port->trouble, __ATOMIC_RELAXED);
    /* Direct single-bit stores, never RMW (AHCI 1.3.1 §10.3.2): the HBA clears
     * bits independently as commands complete, and a `|=` could re-arm one it
     * just cleared. A slot stood over is not entered in issued_mask — its
     * caller's own poll takes its completion. */
    if (queued) {
        regs->sact = (1U << slot);
    }
    regs->ci = (1U << slot);
    spin_unlock(&port->lock);
    return true;
}

AhciTake ahci_port_take_whole(uint8_t port_num, uint32_t patience_ms,
                              uint8_t* slot_out)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (!port || !slot_out) {
        return AHCI_TAKE_NO_PORT;
    }

    RoadsteadBerth berth;
    memset(&berth, 0, sizeof(berth));
    berth.want = BERTH_WHOLE;
    berth.core = amp_get_core_index();

    SchedulerHoldHere();
    spin_lock(&port->lock);
    if (!port->active) {
        spin_unlock(&port->lock);
        AhciLetGo("the whole AHCI port it asked for, which had left");
        return AHCI_TAKE_NO_PORT;
    }
    bool door = false;
    if (!port->halted && !port->slot_held && RoadsteadIsEmpty(&port->roadstead) &&
        port->slot_bitmap == port->slot_mask) {
        port->slot_held   = true;
        port->door_holder = berth.core;
        port->slot_bitmap = 0;
        door = true;
    } else {
        RoadsteadMoor(&port->roadstead, &berth);
    }
    spin_unlock(&port->lock);

    if (!door && RoadsteadStandBy(&berth, AhciTurn, port) != BERTH_CALLED) {
        AhciLetGo("the whole AHCI port it waited for, which left");
        return AHCI_TAKE_NO_PORT;
    }

    /* The door is this caller's. Every slot was free when it closed, which is
     * also the proof that no submitter sits between admission and its PxCI
     * store; what the HBA may still show is work whose slot went back without
     * it — a command given up on — and that is the device's to finish. */
    *slot_out = (uint8_t)__builtin_ctz(port->slot_mask);
    if (AhciPortQuiet(port)) {
        return AHCI_TAKE_GOT;
    }

    uint32_t seen = __atomic_load_n(&port->trouble, __ATOMIC_ACQUIRE);
    HourGlass glass;
    if (!HourGlassTurn(&glass, (uint64_t)patience_ms * 1000ULL)) {
        kprintf("[AHCI] no time source — there is no clock to wait out port %u's "
                "outstanding commands on, and issuing a non-queued command beside "
                "queued ones is what AHCI 1.3.1 §1.7 forbids; not issuing it\n",
                port_num);
        ahci_port_give_back(port_num);
        return AHCI_TAKE_SILENT;
    }
    for (;;) {
        if (!ahci_get_port_state(port_num)) {
            ahci_port_give_back(port_num);
            return AHCI_TAKE_NO_PORT;
        }
        if (AhciPortQuiet(port)) {
            return AHCI_TAKE_GOT;
        }
        if (HourGlassRunOut(&glass)) {
            kprintf("[AHCI] port %u: %lu us on the %s and the commands it was "
                    "given never came back (SACT=0x%08x CI=0x%08x) — the device "
                    "has stopped answering, and the port goes to its recovery\n",
                    port_num, (unsigned long)HourGlassElapsedUs(&glass),
                    HourGlassRulerName(&glass), port->regs->sact, port->regs->ci);
            AhciPortTroubled(port_num, seen);
            ahci_port_give_back(port_num);
            return AHCI_TAKE_SILENT;
        }
        AhciTurn(port);
        cpu_pause();
    }
}

void ahci_port_give_back(uint8_t port_num)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (port) {
        bool opened  = false;
        bool waiting = false;
        spin_lock(&port->lock);
        if (port->slot_held) {
            port->slot_bitmap |= port->slot_mask;
            port->slot_held    = false;
            opened = true;
        }
        waiting = !RoadsteadIsEmpty(&port->roadstead);
        spin_unlock(&port->lock);
        if (opened && waiting) {
            KnockOn(&port->roadstead_knock);
        }
    }
    AhciLetGo("a command it held a whole AHCI port for");
}

#ifdef CONFIG_VOLUME_TESTS
uint32_t AhciPortStandNarrow(uint8_t port_num, uint32_t mask)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (!port || mask == 0) {
        return 0;
    }

    uint32_t had     = 0;
    bool     waiting = false;

    spin_lock(&port->lock);
    if ((mask & ~port->slot_mask) == 0) {
        /* Narrowing: only a port with nothing out and nobody waiting, so no
         * slot outside the new mask is anybody's. */
        if (port->active && !port->halted && !port->slot_held &&
            RoadsteadIsEmpty(&port->roadstead) &&
            port->slot_bitmap == port->slot_mask &&
            __atomic_load_n(&port->issued_mask, __ATOMIC_ACQUIRE) == 0) {
            had               = port->slot_mask;
            port->slot_mask   = mask;
            port->slot_bitmap = mask;
        }
    } else if (port->active) {
        /* Widening back: the slots added were never handed out while the port
         * was narrow, so every one of them is free. */
        had                = port->slot_mask;
        port->slot_bitmap |= mask & ~had;
        port->slot_mask    = mask;
        waiting = !RoadsteadIsEmpty(&port->roadstead);
    }
    spin_unlock(&port->lock);

    if (waiting) {
        KnockOn(&port->roadstead_knock);
    }
    return had;
}
#endif
