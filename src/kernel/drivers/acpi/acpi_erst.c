#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"
#include "io.h"

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
 * Public API:
 *   erst_run_action(action, in_value, out_value)
 *   erst_read_record(rec_id, buf, size)   — convenience wrapper
 *   erst_write_record(rec_id, buf, size)  — convenience wrapper
 *
 * The interpreter only implements the spec's "Serialization Instruction
 * Entry" semantics; the broader storage backend (BERT log block, NVRAM,
 * vendor-specific) lives on the firmware side.
 */

/* Serialization Actions (Table 18-401). */
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

/* Serialization Instructions (Table 18-402). */
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

/* Run every instruction whose `action` matches `action_id`, threading
 * `value_in` through and writing the final accumulator to `*value_out`.
 *
 * Loop / branch caps: the spec says GOTO targets must be within the
 * same action's instructions; we additionally bound total ticks at
 * 256K to defeat malicious firmware. Returns 0 on success, -1 if no
 * matching entries, -2 on guard trip. */
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
    uint32_t pc = first;
    uint32_t ticks = 0;
    while (pc < count && base[pc].action == action_id) {
        if (ticks++ > 262144) return -2;
        erst_entry_t* e = &base[pc];
        uint64_t reg_val;
        switch (e->instr) {
            case ERST_INS_READ_REGISTER:
                acc = (gas_read(&e->reg) & e->mask);
                break;
            case ERST_INS_READ_REGISTER_VALUE:
                reg_val = (gas_read(&e->reg) & e->mask);
                acc = (reg_val == e->value) ? 1 : 0;
                break;
            case ERST_INS_WRITE_REGISTER:
                gas_write(&e->reg, acc & e->mask);
                break;
            case ERST_INS_WRITE_REGISTER_VALUE:
                gas_write(&e->reg, e->value & e->mask);
                break;
            case ERST_INS_NOOP:
                break;
            case ERST_INS_LOAD_VAR1:
                var1 = (gas_read(&e->reg) & e->mask);
                break;
            case ERST_INS_LOAD_VAR2:
                var2 = (gas_read(&e->reg) & e->mask);
                break;
            case ERST_INS_STORE_VAR1:
                gas_write(&e->reg, var1 & e->mask);
                break;
            case ERST_INS_ADD:
                acc = var1 + var2;
                break;
            case ERST_INS_SUBTRACT:
                acc = var1 - var2;
                break;
            case ERST_INS_ADD_VALUE: {
                uint64_t v = gas_read(&e->reg);
                gas_write(&e->reg, v + e->value);
                break;
            }
            case ERST_INS_SUBTRACT_VALUE: {
                uint64_t v = gas_read(&e->reg);
                gas_write(&e->reg, v - e->value);
                break;
            }
            case ERST_INS_STALL:
                /* `value` is microseconds, and it comes from the firmware.
                 * The interpreter's tick budget bounds how many instructions
                 * run; it says nothing about how long ONE of them takes, and
                 * an unbounded spin here is a table entry away from a machine
                 * that never finishes booting. Linux caps the same instruction
                 * at 32 ms for the same reason (FIRMWARE_MAX_STALL); a stall
                 * longer than that is a malformed table, not a slow register.
                 *
                 * The spin itself is `outb 0x80`, which is roughly a
                 * microsecond on legacy hardware and only roughly anything on
                 * modern hardware — but ERST stalls exist to let a firmware
                 * register settle, and erring long is the safe direction. */
                if (e->value > ERST_MAX_STALL_US) {
                    debug_printf("[ERST] STALL of %lu us at pc=%u exceeds the "
                                 "%u us cap — table is malformed, skipping\n",
                                 (unsigned long)e->value, pc,
                                 (unsigned)ERST_MAX_STALL_US);
                } else {
                    for (uint64_t u = 0; u < e->value; u++) outb(0x80, 0);
                }
                break;
            case ERST_INS_STALL_WHILE_TRUE: {
                uint32_t guard = 1000000;
                while (guard-- && (gas_read(&e->reg) & e->mask) == e->value) {
                    outb(0x80, 0);
                }
                break;
            }
            case ERST_INS_SKIP_NEXT_IF_TRUE:
                reg_val = (gas_read(&e->reg) & e->mask);
                if (reg_val == e->value) pc += 2;
                else                     pc += 1;
                continue;
            case ERST_INS_GOTO: {
                uint64_t target = e->value;
                if (target < count && base[target].action == action_id) {
                    pc = (uint32_t)target;
                    continue;
                }
                /* Out-of-bounds target — terminate this action. */
                pc = count;
                continue;
            }
            case ERST_INS_SET_SRC_ADDR_BASE:
                /* MOVE_DATA support is rare in published firmware and
                 * needs an additional DRAM staging buffer; treat as
                 * NOP here so the rest of the action still runs. */
                break;
            case ERST_INS_SET_DST_ADDR_BASE:
                break;
            case ERST_INS_MOVE_DATA:
                break;
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

/* Public convenience wrappers — every firmware does the same dance:
 *   BEGIN_*(record_id) -> EXECUTE_OPERATION -> CHECK_BUSY loop ->
 *   GET_COMMAND_STATUS -> END. We expose the loop, not every step,
 *   because clients only care about status. */
static int erst_pump(uint8_t begin_action, uint64_t record_id,
                      uint64_t* out_status) {
    uint64_t v;
    if (erst_run_action(ERST_ACT_SET_RECORD_IDENTIFIER, record_id, &v) < 0)
        return -1;
    if (erst_run_action(begin_action, 0, &v) < 0)
        return -1;
    if (erst_run_action(ERST_ACT_EXECUTE_OPERATION, 0, &v) < 0)
        return -1;
    for (int i = 0; i < 1000; i++) {
        if (erst_run_action(ERST_ACT_CHECK_BUSY, 0, &v) < 0) return -1;
        if (v == 0) break;
        for (uint32_t k = 0; k < 1000; k++) outb(0x80, 0);
    }
    if (erst_run_action(ERST_ACT_GET_COMMAND_STATUS, 0, out_status) < 0)
        return -1;
    erst_run_action(ERST_ACT_END, 0, &v);
    return 0;
}

int erst_write_record(uint64_t record_id, uint64_t* status_out) {
    if (!g_erst) return -1;
    return erst_pump(ERST_ACT_BEGIN_WRITE, record_id, status_out);
}

int erst_read_record(uint64_t record_id, uint64_t* status_out) {
    if (!g_erst) return -1;
    return erst_pump(ERST_ACT_BEGIN_READ, record_id, status_out);
}

int erst_clear_record(uint64_t record_id, uint64_t* status_out) {
    if (!g_erst) return -1;
    return erst_pump(ERST_ACT_BEGIN_CLEAR, record_id, status_out);
}

uint64_t erst_get_record_count(void) {
    uint64_t v;
    if (erst_run_action(ERST_ACT_GET_RECORD_COUNT, 0, &v) < 0) return 0;
    return v;
}
