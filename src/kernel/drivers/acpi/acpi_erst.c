#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"
#include "io.h"
#include "hourglass.h"
#include "atomics.h"

/*
 * ERST (Error Record Serialization Table) runtime — ACPI 6.5 §18.5.
 *
 * Each "action" (BEGIN_WRITE, END_WRITE, BEGIN_READ, GET_STATUS,
 * GET_RECORD_IDENTIFIER, etc.) is represented in the firmware table as
 * a list of "instructions" the OS must replay in order. Instructions
 * move data between in/out registers and a single accumulator, with
 * loops/branches limited enough that we can execute them with a tiny
 * interpreter — no need to bring in a full AML stack.
 *
 * Public API (acpi_internal.h):
 *   erst_run_action(action, in_value, out_value)
 *   erst_write_record(record_id, status_out)  — §18.5.2.1 sequence
 *   erst_read_record(record_id, status_out)   — §18.5.2.2 sequence
 *   erst_clear_record(record_id, status_out)  — §18.5.2.3 sequence
 *   erst_get_record_count()
 *
 * ‼ NOT IN SERVICE. Nothing outside this file calls any of them. The one
 * entry point the boot path uses is acpi_erst_bind(), from acpi_parse_apei:
 * it records where the table is and executes nothing. So no firmware
 * instruction of this interpreter runs on any boot today, and the first
 * caller — a RAS consumer that persists an error record — is what puts it
 * in service.
 *
 * The record itself never travels through those wrappers: ACPI 6.5 §18.5.2
 * transfers every record through the ERROR LOG ADDRESS RANGE the firmware
 * publishes (Table 18.18 action 0x0D), and the wrappers drive the action
 * sequence around that window. The window is obtained and mapped on first
 * use — see erst_erange_ready.
 *
 * The interpreter implements the spec's "Serialization Instruction Entry"
 * semantics; the broader storage backend (NVRAM, vendor-specific) lives on
 * the firmware side.
 */

/* Serialization Actions (ACPI 6.5 §18.5.1.1, Table 18.18). */
enum erst_action {
    ERST_ACT_BEGIN_WRITE             = 0x00,
    ERST_ACT_BEGIN_READ              = 0x01,
    ERST_ACT_BEGIN_CLEAR             = 0x02,
    ERST_ACT_END                     = 0x03,
    ERST_ACT_SET_RECORD_OFFSET       = 0x04,
    ERST_ACT_EXECUTE_OPERATION       = 0x05,
    ERST_ACT_CHECK_BUSY              = 0x06,
    ERST_ACT_GET_COMMAND_STATUS      = 0x07,
    ERST_ACT_GET_RECORD_IDENTIFIER   = 0x08,
    ERST_ACT_SET_RECORD_IDENTIFIER   = 0x09,
    ERST_ACT_GET_RECORD_COUNT        = 0x0A,
    ERST_ACT_BEGIN_DUMMY_WRITE       = 0x0B,
    ERST_ACT_GET_ERROR_LOG_RANGE     = 0x0D,
    ERST_ACT_GET_ERROR_LOG_LENGTH    = 0x0E,
    ERST_ACT_GET_ERROR_LOG_ATTR      = 0x0F,
    ERST_ACT_EXECUTE_TIMINGS         = 0x10,
};

/* Serialization Instructions (ACPI 6.5 §18.5.1.2, Table 18.21). */
enum erst_instr {
    ERST_INS_READ_REGISTER           = 0x00,
    ERST_INS_READ_REGISTER_VALUE     = 0x01,
    ERST_INS_WRITE_REGISTER          = 0x02,
    ERST_INS_WRITE_REGISTER_VALUE    = 0x03,
    ERST_INS_NOOP                    = 0x04,
    ERST_INS_LOAD_VAR1               = 0x05,
    ERST_INS_LOAD_VAR2               = 0x06,
    ERST_INS_STORE_VAR1              = 0x07,
    ERST_INS_ADD                     = 0x08,
    ERST_INS_SUBTRACT                = 0x09,
    ERST_INS_ADD_VALUE               = 0x0A,
    ERST_INS_SUBTRACT_VALUE          = 0x0B,
    ERST_INS_STALL                   = 0x0C,
    ERST_INS_STALL_WHILE_TRUE        = 0x0D,
    ERST_INS_SKIP_NEXT_IF_TRUE       = 0x0E,
    ERST_INS_GOTO                    = 0x0F,
    ERST_INS_SET_SRC_ADDR_BASE       = 0x10,
    ERST_INS_SET_DST_ADDR_BASE       = 0x11,
    ERST_INS_MOVE_DATA               = 0x12,
};

#define GAS_AS_SYSTEM_MEMORY  0
#define GAS_AS_SYSTEM_IO      1

/* Serialization Instruction Flags — ACPI 6.5 §18.5.1.2, Table 18.22. The
 * only flag the spec defines: for a WRITE, "bits within the register that
 * are not being written must be preserved rather than destroyed". */
#define ERST_FLAG_PRESERVE_REGISTER  0x01u

typedef struct {
    uint8_t  action;
    uint8_t  instr;
    uint8_t  flags;
    uint8_t  reserved;
    acpi_gas_t reg;
    uint64_t value;
    uint64_t mask;
} __attribute__((packed)) erst_entry_t;
_Static_assert(sizeof(erst_entry_t) == 32, "ERST entry = 32 bytes");

static acpi_erst_t* g_erst = NULL;

/* The entry array, and how many of it are actually THERE.
 *
 * instruction_entry_count is a firmware-supplied number that used to be
 * returned as-is, and every loop over the array trusted it. A table that
 * declares more entries than its own header.length can hold sends those loops
 * reading past the end of the mapping — the count and the length are two
 * independent claims by the same firmware, and only one of them bounds real
 * memory. Take the smaller. */
static erst_entry_t* erst_entries(uint32_t* count) {
    if (!g_erst) { *count = 0; return NULL; }

    uint32_t len = g_erst->header.length;
    if (len <= sizeof(acpi_erst_t)) { *count = 0; return NULL; }

    uint32_t fits = (uint32_t)((len - sizeof(acpi_erst_t)) / sizeof(erst_entry_t));
    uint32_t said = g_erst->instruction_entry_count;

    if (said > fits) {
        debug_printf("[ERST] table declares %u instructions but only %u fit in "
                     "its %u bytes — trusting the length\n", said, fits, len);
        said = fits;
    }
    *count = said;
    return (erst_entry_t*)((uint8_t*)g_erst + sizeof(acpi_erst_t));
}

/* SystemMemory GAS registers, mapped once each.
 *
 * gas_read and gas_write used to call vmm_map_mmio on every access and never
 * unmap. That is a leak anywhere; inside erst_run_action it is a weapon: the
 * interpreter runs up to 262144 instructions, every one of which may touch a
 * register, so firmware with a long enough action could ask the kernel for a
 * quarter of a million permanent MMIO mappings and the page-table pages under
 * them. An ERST action addresses a handful of distinct registers, so a handful
 * of slots is all it takes to make the mapping happen once.
 *
 * Not freed, deliberately: these registers are firmware-owned and live for the
 * kernel's lifetime, exactly like the ACPI tables themselves. */
/* Longest STALL this interpreter will honour, in microseconds. Same value
 * Linux uses, and for the same reason: past it the table is wrong. */
#define ERST_MAX_STALL_US 32000u

/* How long an ERST poll may spin before the firmware is treated as stuck.
 *
 * DECLARED POLICY, not a spec quantity. ACPI 6.5 §18.5 gives neither the
 * STALL_WHILE_TRUE instruction nor the CHECK_BUSY loop the wrappers run a
 * maximum time or an iteration count: the table says poll, and that is all it
 * says. One second is this kernel's patience, written here once. */
#define ERST_POLL_PATIENCE_US 1000000u

/* Spend `us` microseconds of REAL time.
 *
 * Every delay in this interpreter used to be a count of writes to port 0x80,
 * which is roughly a microsecond on a legacy chipset and roughly nothing at
 * all on a board where 0x80 is absorbed by the eSPI bridge. A firmware asking
 * for a settling time got whatever that board happened to give it.
 *
 * On a machine with no time ruler the stall is SKIPPED rather than faked, and
 * said out loud once: a stall has no hardware fact to wait on, so there is
 * nothing to wait for unbounded, and inventing a spin count is the one thing a
 * bound must never do. An action whose registers really did need the settling
 * time then fails at its own CHECK_BUSY or GET_COMMAND_STATUS — which is a
 * fact, reported as one. */
static void erst_stall_us(uint64_t us) {
    static bool said_no_ruler = false;
    HourGlass glass;
    if (!HourGlassTurn(&glass, us)) {
        if (!said_no_ruler) {
            said_no_ruler = true;
            kprintf("[ERST] no time source on this machine — firmware STALL "
                    "instructions are being skipped, not guessed at\n");
        }
        return;
    }
    while (!HourGlassRunOut(&glass)) cpu_pause();
}

#define GAS_MAP_SLOTS 8
static struct {
    uint64_t       phys;
    volatile void* virt;
} g_gas_map[GAS_MAP_SLOTS];
static uint8_t g_gas_map_used = 0;

static volatile void* gas_map(uint64_t phys) {
    for (uint8_t i = 0; i < g_gas_map_used; i++)
        if (g_gas_map[i].phys == phys)
            return g_gas_map[i].virt;

    volatile void* p = vmm_map_mmio((uintptr_t)phys, 8, VMM_FLAGS_KERNEL_RW);
    if (p && g_gas_map_used < GAS_MAP_SLOTS) {
        g_gas_map[g_gas_map_used].phys = phys;
        g_gas_map[g_gas_map_used].virt = p;
        g_gas_map_used++;
    }
    return p;
}

static uint64_t gas_read(const acpi_gas_t* g) {
    if (g->address == 0) return 0;
    uint64_t addr = g->address;
    uint8_t  bw   = g->bit_width;
    if (g->address_space == GAS_AS_SYSTEM_IO) {
        switch (bw) {
            case 8:  return inb((uint16_t)addr);
            case 16: return inw((uint16_t)addr);
            case 32: return inl((uint16_t)addr);
            default: return 0;
        }
    }
    if (g->address_space == GAS_AS_SYSTEM_MEMORY) {
        volatile void* p = gas_map(addr);
        if (!p) return 0;
        switch (bw) {
            case 8:  return *(volatile uint8_t*)p;
            case 16: return *(volatile uint16_t*)p;
            case 32: return *(volatile uint32_t*)p;
            case 64: return *(volatile uint64_t*)p;
            default: return 0;
        }
    }
    return 0;
}

static void gas_write(const acpi_gas_t* g, uint64_t v) {
    if (g->address == 0) return;
    uint64_t addr = g->address;
    uint8_t  bw   = g->bit_width;
    if (g->address_space == GAS_AS_SYSTEM_IO) {
        switch (bw) {
            case 8:  outb((uint16_t)addr, (uint8_t)v);  break;
            case 16: outw((uint16_t)addr, (uint16_t)v); break;
            case 32: outl((uint16_t)addr, (uint32_t)v); break;
        }
        return;
    }
    if (g->address_space == GAS_AS_SYSTEM_MEMORY) {
        volatile void* p = gas_map(addr);
        if (!p) return;
        switch (bw) {
            case 8:  *(volatile uint8_t*)p  = (uint8_t)v;  break;
            case 16: *(volatile uint16_t*)p = (uint16_t)v; break;
            case 32: *(volatile uint32_t*)p = (uint32_t)v; break;
            case 64: *(volatile uint64_t*)p = v;           break;
        }
    }
}

/* An instruction entry does not name a REGISTER — it names a REGISTER
 * REGION: the bit range that starts at the GAS Bit Offset and is selected
 * by the entry's Mask. ACPI 6.5 §18.5.1.2, Table 18.20 is explicit that the
 * Mask is bit-range-relative — "note that bit 0 of the bit mask corresponds
 * to the lowest bit in the bit range" — so a mask applied to the raw
 * register is wrong by construction, not by convention.
 *
 * §18.5.1.2.1-.4 spell both directions out:
 *   READ:  X = Read(register); X = X >> Bit Offset; X = X & Mask
 *   WRITE: X = Value & Mask; X = X << Bit Offset;
 *          If (Preserve Register) { Y = Read(register);
 *                                   Y = Y & ~(Mask << Bit Offset);
 *                                   X = X | Y }
 *          Write(X, Register)
 *
 * Every instruction in this interpreter that touches a register goes
 * through these two functions, so no call site can forget the shift again.
 * Linux keeps the same pair in drivers/acpi/apei/apei-base.c
 * (__apei_exec_read_register / __apei_exec_write_register).
 *
 * Table 18.18 action 0x7 is the reason this matters at once:
 * "Bits [8:1] of the value returned from the Register Region indicate the
 * command status, which requires that the Bit Offset of the GAS for the
 * Register Region is set to 1" — read without the shift, every command
 * status comes back doubled with a foreign bit 0 attached. */
static uint64_t erst_region_read(const erst_entry_t* e) {
    return (gas_read(&e->reg) >> e->reg.bit_offset) & e->mask;
}

static void erst_region_write(const erst_entry_t* e, uint64_t v) {
    uint8_t off = e->reg.bit_offset;
    uint64_t out = (v & e->mask) << off;
    if (e->flags & ERST_FLAG_PRESERVE_REGISTER) {
        out |= gas_read(&e->reg) & ~(e->mask << off);
    }
    gas_write(&e->reg, out);
}

/* MOVE_DATA — ACPI 6.5 §18.5.1.2, Table 18.21, instruction 0x12: "Moves
 * VAR2 bytes of data from SRC_BASE + Offset to DST_BASE + Offset, where
 * Offset is the contents of the register region." SRC_BASE and DST_BASE are
 * loaded by instructions 0x10 and 0x11 from their own register regions.
 *
 * Both windows are firmware-owned physical ranges that matter for the
 * duration of one move, so they are mapped around the copy and released
 * after it — the shape Linux gives erst_exec_move_data() with
 * ioremap/iounmap. The copy carries memmove semantics because the two
 * physical ranges may overlap.
 *
 * Returns false, out loud, when a window cannot be mapped: a move that did
 * not happen must never be reported as one. */
static bool erst_move_data(uint64_t src_phys, uint64_t dst_phys, uint64_t len) {
    if (len == 0) return true;

    volatile uint8_t* src = (volatile uint8_t*)
        vmm_map_mmio((uintptr_t)src_phys, (size_t)len, VMM_FLAGS_KERNEL_RO);
    if (!src) {
        kprintf("[ERST] MOVE_DATA: cannot map %lu bytes of source at 0x%lx — "
                "the record was NOT moved\n",
                (unsigned long)len, (unsigned long)src_phys);
        return false;
    }
    volatile uint8_t* dst = (volatile uint8_t*)
        vmm_map_mmio((uintptr_t)dst_phys, (size_t)len, VMM_FLAGS_KERNEL_RW);
    if (!dst) {
        vmm_unmap_mmio(src, (size_t)len);
        kprintf("[ERST] MOVE_DATA: cannot map %lu bytes of destination at "
                "0x%lx — the record was NOT moved\n",
                (unsigned long)len, (unsigned long)dst_phys);
        return false;
    }

    if (dst_phys > src_phys && dst_phys < src_phys + len) {
        for (uint64_t i = len; i > 0; i--) dst[i - 1] = src[i - 1];
    } else {
        for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    }

    vmm_unmap_mmio(dst, (size_t)len);
    vmm_unmap_mmio(src, (size_t)len);
    return true;
}

/* Run every instruction whose `action` matches `action_id`, threading
 * `value_in` through and writing the final accumulator to `*value_out`.
 *
 * An action's instructions are listed consecutively (ACPI 6.5 §18.5.1:
 * "the instructions must be listed consecutively and they will
 * consequently be performed sequentially"), so the run for an action is
 * the contiguous stretch that begins at its first entry — and a GOTO's
 * operand is an index INSIDE that stretch, not into the whole table
 * (Table 18.21, 0x0F: "The instruction is specified as the zero-based
 * index. Each instruction for a given action has an index based on its
 * relative position in the array of instructions for the action").
 *
 * We additionally bound total ticks at 256K to defeat malicious firmware.
 *
 * Returns 0 on success; every other value is a REFUSAL, and none of them
 * is 0, because an abandoned action that reports success is read by the
 * caller as a firmware that answered:
 *   -1 no instruction entries for this action
 *   -2 tick guard tripped
 *   -3 instruction opcode this interpreter does not know
 *   -4 a polled register never changed within the patience
 *   -5 a GOTO whose target is outside this action's own instructions
 *   -6 an entry whose GAS Bit Offset names no bit of any register
 *   -7 a MOVE_DATA whose windows could not be mapped */
int erst_run_action(uint8_t action_id, uint64_t value_in, uint64_t* value_out) {
    uint32_t count = 0;
    erst_entry_t* base = erst_entries(&count);
    if (!base || count == 0) return -1;

    /* Find first instruction for this action; iterate forward, honoring
     * SKIP_NEXT_IF_TRUE / GOTO inside the contiguous run for this
     * action. */
    uint32_t first = 0;
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (base[i].action == action_id) { first = i; found = true; break; }
    }
    if (!found) return -1;

    uint64_t acc = value_in;
    uint64_t var1 = 0, var2 = 0;
    uint64_t src_base = 0, dst_base = 0;
    uint32_t pc = first;
    uint32_t ticks = 0;
    while (pc < count && base[pc].action == action_id) {
        if (ticks++ > 262144) return -2;
        erst_entry_t* e = &base[pc];
        uint64_t reg_val;
        /* A Bit Offset of 64 or more names no bit of any register the CPU
         * can address, and the shifts §18.5.1.2 asks for would be
         * undefined. The table is malformed; the action is refused rather
         * than executed against a guessed bit position.
         *
         * The offset is a property of the TABLE, not of the register, so a
         * zero address does not excuse it: gas_read/gas_write return early
         * on address 0, but the shift in erst_region_read/erst_region_write
         * is computed before either is called and would be undefined all
         * the same. */
        if (e->reg.bit_offset >= 64) {
            kprintf("[ERST] entry %u of action 0x%02x puts its register "
                    "region at bit %u — no register has one; action "
                    "refused\n", pc, action_id, e->reg.bit_offset);
            return -6;
        }
        switch (e->instr) {
            case ERST_INS_READ_REGISTER:
                acc = erst_region_read(e);
                break;
            case ERST_INS_READ_REGISTER_VALUE:
                reg_val = erst_region_read(e);
                acc = (reg_val == e->value) ? 1 : 0;
                break;
            case ERST_INS_WRITE_REGISTER:
                erst_region_write(e, acc);
                break;
            case ERST_INS_WRITE_REGISTER_VALUE:
                erst_region_write(e, e->value);
                break;
            case ERST_INS_NOOP:
                break;
            case ERST_INS_LOAD_VAR1:
                var1 = erst_region_read(e);
                break;
            case ERST_INS_LOAD_VAR2:
                var2 = erst_region_read(e);
                break;
            case ERST_INS_STORE_VAR1:
                erst_region_write(e, var1);
                break;
            case ERST_INS_ADD:
                acc = var1 + var2;
                break;
            case ERST_INS_SUBTRACT:
                acc = var1 - var2;
                break;
            case ERST_INS_ADD_VALUE:
                erst_region_write(e, erst_region_read(e) + e->value);
                break;
            case ERST_INS_SUBTRACT_VALUE:
                erst_region_write(e, erst_region_read(e) - e->value);
                break;
            case ERST_INS_STALL:
                /* `value` is microseconds, and it comes from the firmware.
                 * The interpreter's tick budget bounds how many instructions
                 * run; it says nothing about how long ONE of them takes, and
                 * an unbounded spin here is a table entry away from a machine
                 * that never finishes booting. Linux caps the same instruction
                 * at 32 ms for the same reason (FIRMWARE_MAX_STALL); a stall
                 * longer than that is a malformed table, not a slow register.
                 *
                 * The stall itself is now real time (erst_stall_us) rather
                 * than a count of writes to port 0x80: a firmware register
                 * given a settling time is entitled to the time, and on a
                 * board whose 0x80 is absorbed by the eSPI bridge the old
                 * count delivered almost none of it. */
                if (e->value > ERST_MAX_STALL_US) {
                    debug_printf("[ERST] STALL of %lu us at pc=%u exceeds the "
                                 "%u us cap — table is malformed, skipping\n",
                                 (unsigned long)e->value, pc,
                                 (unsigned)ERST_MAX_STALL_US);
                } else {
                    erst_stall_us(e->value);
                }
                break;
            case ERST_INS_STALL_WHILE_TRUE: {
                /* ACPI 6.5 §18.5.1.2, Table 18.21, instruction 0x0D: "OSPM
                 * continually compares the contents of the specified register
                 * region to Value until the values are not equal. OSPM stalls
                 * between each successive comparison. The amount of time to
                 * stall is specified by VAR1 and is expressed in
                 * microseconds."
                 *
                 * The comparison was right and the STALL half was missing:
                 * the loop re-read the register as fast as the core could
                 * issue the access, so the gap the firmware named in VAR1
                 * never happened. On a register that lives behind an eSPI or
                 * an SMBus bridge that is a poll rate the platform never
                 * agreed to, and a settling time the table asked for and did
                 * not get.
                 *
                 * The gap is capped the way the STALL instruction is: past
                 * ERST_MAX_STALL_US the table is describing something other
                 * than a settling time. A VAR1 of zero is a firmware that
                 * asked for no gap at all, and then cpu_pause is the whole
                 * of it.
                 *
                 * The exit is still the FACT; the glass only bounds it, and
                 * on a machine with no ruler the fact is the only thing left
                 * to wait on. */
                uint64_t gap = var1;
                if (gap > ERST_MAX_STALL_US) {
                    debug_printf("[ERST] STALL_WHILE_TRUE at pc=%u asks %lu us "
                                 "between comparisons, past the %u us cap — "
                                 "polling at the cap\n",
                                 pc, (unsigned long)gap,
                                 (unsigned)ERST_MAX_STALL_US);
                    gap = ERST_MAX_STALL_US;
                }

                HourGlass glass;
                if (!HourGlassTurn(&glass, ERST_POLL_PATIENCE_US)) {
                    kprintf("[ERST] no time source — waiting out a firmware "
                            "poll at pc=%u on the fact itself, with no bound "
                            "and without the %lu us gap the table asked "
                            "for\n", pc, (unsigned long)gap);
                    while (erst_region_read(e) == e->value)
                        cpu_pause();
                    break;
                }
                for (;;) {
                    if (erst_region_read(e) != e->value) break;
                    if (HourGlassRunOut(&glass)) {
                        kprintf("[ERST] the register at pc=%u never changed — "
                                "%lu us on the %s; abandoning this action\n",
                                pc, (unsigned long)HourGlassElapsedUs(&glass),
                                HourGlassRulerName(&glass));
                        return -4;
                    }
                    if (gap) erst_stall_us(gap);
                    else     cpu_pause();
                }
                break;
            }
            case ERST_INS_SKIP_NEXT_IF_TRUE:
                /* "OSPM skips the next instruction in the sequence for the
                 * current action" — and since the sequence is contiguous
                 * (§18.5.1), the next instruction of the action is the next
                 * entry of the table. A relative +2 needs no bias. */
                reg_val = erst_region_read(e);
                if (reg_val == e->value) pc += 2;
                else                     pc += 1;
                continue;
            case ERST_INS_GOTO: {
                /* The operand is the instruction's index WITHIN THIS ACTION,
                 * counted from the action's own first instruction — so the
                 * table entry it names is `first + Value`. Taken as an
                 * absolute table index (what stood here), every action that
                 * does not begin at entry 0 branched into a different
                 * action's instructions, and when the guard then rejected
                 * that entry the action was abandoned mid-sequence while the
                 * function still returned success. */
                uint64_t target = (uint64_t)first + e->value;
                if (e->value < count && target < count &&
                    base[target].action == action_id) {
                    pc = (uint32_t)target;
                    continue;
                }
                kprintf("[ERST] action 0x%02x: GOTO %lu at pc=%u leaves the "
                        "action's own instructions — action refused\n",
                        action_id, (unsigned long)e->value, pc);
                return -5;
            }
            case ERST_INS_SET_SRC_ADDR_BASE:
                src_base = erst_region_read(e);
                break;
            case ERST_INS_SET_DST_ADDR_BASE:
                dst_base = erst_region_read(e);
                break;
            case ERST_INS_MOVE_DATA: {
                /* Table 18.21, 0x12: VAR2 bytes from SRC_BASE + Offset to
                 * DST_BASE + Offset, Offset being this entry's register
                 * region. This used to be an empty case, so a record
                 * transfer expressed with MOVE_DATA moved nothing and the
                 * action still reported success. */
                uint64_t offset = erst_region_read(e);
                if (!erst_move_data(src_base + offset, dst_base + offset, var2))
                    return -7;
                break;
            }
            default:
                debug_printf("[ERST] unknown instr 0x%02x at pc=%u — abort action\n",
                             e->instr, pc);
                return -3;
        }
        pc++;
    }

    if (value_out) *value_out = acc;
    return 0;
}

/* Bind g_erst when acpi_parse_apei sees the table. Called from there. */
void acpi_erst_bind(acpi_erst_t* erst) {
    g_erst = erst;
}

/* The Error Log Address Range — ACPI 6.5 §18.5, Table 18.18 actions 0x0D
 * GET_ERROR_LOG_ADDRESS_RANGE and 0x0E GET_ERROR_LOG_LENGTH.
 *
 * Every record transfer goes through this window and nowhere else:
 * §18.5.2.1 has OSPM write the record into it before the write operation
 * runs, and §18.5.2.2 has the platform place the record there for OSPM to
 * read. Both actions were declared in the enum above and never executed, so
 * the driver had no window at all — which is why SET_RECORD_OFFSET had
 * nothing to point at and why a "successful" write serialized nothing.
 *
 * Obtained on first use rather than at bind: running firmware instructions
 * is not something ACPI table parsing needs to do, and no boot is made
 * slower by a window nobody has asked for yet. Linux obtains the same three
 * values in erst_init() and maps the range once (erst_erange).
 *
 * Mapped once and kept: the window is firmware-owned and lives as long as
 * the kernel does. `tried` makes a firmware that publishes no range cost
 * one attempt, not one per call. */
static uint64_t       g_erange_base = 0;
static uint64_t       g_erange_size = 0;
static volatile void* g_erange_va   = NULL;
static bool           g_erange_tried = false;

static bool erst_erange_ready(void) {
    if (g_erange_va) return true;
    if (g_erange_tried) return false;
    g_erange_tried = true;

    uint64_t base = 0, size = 0;
    int rc_base = erst_run_action(ERST_ACT_GET_ERROR_LOG_RANGE, 0, &base);
    int rc_size = erst_run_action(ERST_ACT_GET_ERROR_LOG_LENGTH, 0, &size);
    if (rc_base < 0 || rc_size < 0 || base == 0 || size == 0) {
        kprintf("[ERST] the firmware publishes no error log address range "
                "(range rc=%d addr=0x%lx, length rc=%d size=%lu) — no error "
                "record can be transferred on this machine\n",
                rc_base, (unsigned long)base, rc_size, (unsigned long)size);
        return false;
    }

    volatile void* va = vmm_map_mmio((uintptr_t)base, (size_t)size,
                                      VMM_FLAGS_KERNEL_RW);
    if (!va) {
        kprintf("[ERST] error log address range 0x%lx (%lu bytes) cannot be "
                "mapped — no error record can be transferred\n",
                (unsigned long)base, (unsigned long)size);
        return false;
    }

    g_erange_base = base;
    g_erange_size = size;
    g_erange_va   = va;
    /* kprintf, not debug_printf: this window is where every serialized
     * error record of this machine passes through, and the one line that
     * says where it is has to survive into a shipped build. It is printed
     * once, and only when something actually asks for a record. */
    kprintf("[ERST] error log address range 0x%lx (%lu bytes) mapped — "
            "records are staged at its base\n",
            (unsigned long)g_erange_base, (unsigned long)g_erange_size);
    return true;
}

/* An action the firmware did not publish is not a failure of the sequence:
 * ACPI 6.5 §18.5.1 lets a platform omit the actions it does not need, and
 * erst_run_action reports that with -1 ("no instruction entries") — the one
 * return value that means "this firmware does not do that step". Linux
 * draws the same line with apei_exec_run_optional(). Every other negative
 * is a real refusal and propagates. */
static int erst_run_optional(uint8_t action, uint64_t in_value) {
    uint64_t v;
    int rc = erst_run_action(action, in_value, &v);
    return (rc == -1) ? 0 : rc;
}

/* Drive one serialization operation to its status.
 *
 * The order is the spec's own, not a guess:
 *   §18.5.2.1 Writing   BEGIN_WRITE -> SET_RECORD_OFFSET -> EXECUTE ->
 *                       CHECK_BUSY -> GET_COMMAND_STATUS -> END
 *   §18.5.2.2 Reading   BEGIN_READ -> SET_RECORD_OFFSET ->
 *                       SET_RECORD_IDENTIFIER -> EXECUTE -> ...
 *   §18.5.2.3 Clearing  BEGIN_CLEAR -> SET_RECORD_IDENTIFIER -> EXECUTE ->
 *                       ...
 * What stood here ran SET_RECORD_IDENTIFIER first — before the BEGIN_*
 * action that establishes the operational context — and never ran
 * SET_RECORD_OFFSET at all, so the firmware was never told where in the
 * Error Log Address Range to transfer from or to. On firmware that
 * publishes no SET_RECORD_IDENTIFIER action (the write path needs none:
 * the platform reads the Record Id out of the staged record itself) the
 * old order also failed the write before it began. */
static int erst_pump(uint8_t begin_action, uint64_t record_id,
                      bool needs_offset, bool needs_record_id,
                      uint64_t* out_status) {
    uint64_t v;
    int rc;

    if (needs_offset && !erst_erange_ready()) return -1;

    if ((rc = erst_run_optional(begin_action, 0)) < 0)
        return rc;
    /* One record is staged at a time, at the base of the window — so the
     * offset from the base of the Error Log is zero. Linux passes the same
     * 0 from erst_write()/erst_read(). */
    if (needs_offset &&
        erst_run_action(ERST_ACT_SET_RECORD_OFFSET, 0, &v) < 0)
        return -1;
    if (needs_record_id &&
        erst_run_action(ERST_ACT_SET_RECORD_IDENTIFIER, record_id, &v) < 0)
        return -1;
    if (erst_run_action(ERST_ACT_EXECUTE_OPERATION, 0, &v) < 0)
        return -1;
    /* CHECK_BUSY until the firmware says it is done. The exit is that FACT;
     * the glass bounds it. What stood here was a thousand rounds of a thousand
     * writes to port 0x80 — a number of I/O cycles, not a span of time, and on
     * a board that absorbs 0x80 the whole loop retires in a blink and calls a
     * busy firmware finished. */
    HourGlass glass;
    bool timed = HourGlassTurn(&glass, ERST_POLL_PATIENCE_US);
    if (!timed) {
        kprintf("[ERST] no time source — waiting for the firmware to leave "
                "BUSY on the fact itself, with no bound\n");
    }
    for (;;) {
        if (erst_run_action(ERST_ACT_CHECK_BUSY, 0, &v) < 0) return -1;
        if (v == 0) break;
        if (timed && HourGlassRunOut(&glass)) {
            kprintf("[ERST] the firmware stayed BUSY for %lu us on the %s — "
                    "reading its command status anyway\n",
                    (unsigned long)HourGlassElapsedUs(&glass),
                    HourGlassRulerName(&glass));
            break;
        }
        cpu_pause();
    }
    if (erst_run_action(ERST_ACT_GET_COMMAND_STATUS, 0, out_status) < 0)
        return -1;
    (void)erst_run_optional(ERST_ACT_END, 0);
    return 0;
}

/* Serialize the record staged at the base of the Error Log Address Range
 * (ACPI 6.5 §18.5.2.1: OSPM writes the record into the range, then runs the
 * operation). The Record Id travels inside the record's own header, which
 * is why the write path runs no SET_RECORD_IDENTIFIER — `record_id` is
 * accepted for symmetry with the read and clear paths and is not sent. */
int erst_write_record(uint64_t record_id, uint64_t* status_out) {
    if (!g_erst) return -1;
    return erst_pump(ERST_ACT_BEGIN_WRITE, record_id, true, false, status_out);
}

/* Bring the record with this identifier into the Error Log Address Range
 * (ACPI 6.5 §18.5.2.2). */
int erst_read_record(uint64_t record_id, uint64_t* status_out) {
    if (!g_erst) return -1;
    return erst_pump(ERST_ACT_BEGIN_READ, record_id, true, true, status_out);
}

/* Remove the record with this identifier from the persistent store
 * (ACPI 6.5 §18.5.2.3). Nothing is transferred, so no offset is set. */
int erst_clear_record(uint64_t record_id, uint64_t* status_out) {
    if (!g_erst) return -1;
    return erst_pump(ERST_ACT_BEGIN_CLEAR, record_id, false, true, status_out);
}

uint64_t erst_get_record_count(void) {
    uint64_t v;
    if (erst_run_action(ERST_ACT_GET_RECORD_COUNT, 0, &v) < 0) return 0;
    return v;
}
