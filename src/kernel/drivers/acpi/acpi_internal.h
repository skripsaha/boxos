#ifndef ACPI_INTERNAL_H
#define ACPI_INTERNAL_H

#include "acpi.h"

typedef struct {
    /* Root pointer + main fixed table */
    acpi_rsdp_t* rsdp;
    acpi_fadt_t* fadt;

    /* Resolved PM1 control ports (legacy SystemIO view, used for log/info). */
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;

    /* The ACPI PM Timer's address and width used to live here. It now
     * lives in the driver that reads it (pmtimer.c), handed over once by
     * acpi_parse_tables through pmtimer_adopt_from_fadt — the description
     * and the reader belong together, and while they were apart the reader
     * truncated an address the describer had stored at full width. */

    /* _S5 sleep type values for PM1 control word. */
    uint16_t slp_typa;
    uint16_t slp_typb;
    bool s5_found;

    /* Optional tables — parsed during acpi_init, kept zero/false if absent. */
    acpi_hpet_info_t hpet;
    acpi_mcfg_info_t mcfg;
    acpi_numa_info_t numa;
    acpi_slit_info_t slit;
    acpi_dmar_info_t dmar;
    acpi_ivrs_info_t ivrs;
    acpi_apei_info_t apei;
    acpi_wsmt_info_t wsmt;

    bool initialized;
} acpi_state_t;

extern acpi_state_t g_acpi;

/* ============================================================
 * The two GAS registers the ONE-WAY path writes: RESET_REG (ACPI 6.5
 * §4.8.3.6, every reboot) and SLEEP_CONTROL_REG (§4.8.3.7, poweroff on a
 * hardware-reduced platform).
 *
 * Both are reached from system_halt.c's sequence — after cli, after
 * halt_all_ap_cores() froze the APs wherever they stood, and after
 * halt_stop_hardware(). Mapping a System Memory descriptor there takes
 * kernel_mmio_lock and the address space's own lock and allocates page
 * tables, any of which a frozen AP may hold forever. So both are resolved
 * and mapped ONCE in acpi_init (AcpiOneWayRegistersLocate) and the one-way
 * path only dereferences — the shape pm1_control_bind already gives a System
 * Memory PM1 control register.
 * ============================================================ */
typedef struct {
    bool              present;  /* the FADT is long enough to carry the field */
    bool              named;    /* and the descriptor names a non-zero address */
    bool              usable;   /* and it can be written without allocating */
    uint8_t           space;    /* GAS address_space_id, ACPI 6.5 Table 5.2 */
    uint64_t          address;  /* as firmware wrote it; never a kernel VA */
    volatile uint8_t *mapped;   /* System Memory only, mapped once at init */
} AcpiOneWayRegister;

const AcpiOneWayRegister *AcpiResetRegister(void);
const AcpiOneWayRegister *AcpiSleepControlRegister(void);

/* ============================================================
 * FACS — ACPI 6.5 §5.2.10, Table 5.17, Firmware ACPI Control Structure.
 *
 * Not an SDT: it carries no checksum, it is not listed in the RSDT or the
 * XSDT, and the only thing that names it is the FADT's FIRMWARE_CTRL or
 * X_FIRMWARE_CTRL (§5.2.9). §5.2.10 aligns it on a 64-byte boundary and
 * gives it a length of "64 bytes or larger".
 * ============================================================ */
typedef struct {
    uint8_t  signature[4];              /* "FACS", offset 0                 */
    uint32_t length;
    uint32_t hardware_signature;
    uint32_t firmware_waking_vector;
    uint32_t global_lock;               /* offset 16 — §5.2.10.1            */
    uint32_t flags;
    uint64_t x_firmware_waking_vector;
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t ospm_flags;
    uint8_t  reserved2[24];
} __attribute__((packed)) acpi_facs_t;
_Static_assert(offsetof(acpi_facs_t, global_lock) == 16,
               "FACS Global Lock is at offset 16 (ACPI 6.5 Table 5.17)");
_Static_assert(sizeof(acpi_facs_t) == 64,
               "FACS is 64 bytes (ACPI 6.5 Table 5.17)");

/* ============================================================
 * The ACPI Global Lock — ACPI 6.5 §5.2.10.1 and §5.2.10.2.
 *
 * One DWORD inside the FACS, shared between this kernel and the platform's
 * own SMM code, with two bits of protocol in it: Owned (bit 1) and Pending
 * (bit 0).
 *
 * What makes it mandatory rather than advisory: §20.2.5.2 FieldFlags bit 4
 * and §19.6.45 FieldUnit. A firmware that writes
 *
 *     Field (GNVS, AnyAcc, Lock, Preserve)
 *
 * is asking, in the declaration itself, that every access OSPM makes to
 * those bits be made while holding this lock — because its own SMM code
 * touches the same memory and has no other way to be excluded from it. A
 * kernel that writes such a field holding nothing is racing the firmware.
 *
 * Taken and dropped BY NAME so that a refusal can say whose access it
 * refused. Both calls say out loud what they did; neither is silent, and
 * the acquire never returns HELD without the word in the FACS proving it.
 * ============================================================ */
typedef enum {
    ACPI_GLOBAL_LOCK_HELD = 0,  /* taken — AcpiGlobalLockRelease owes a call */
    ACPI_GLOBAL_LOCK_ABSENT,    /* this platform has no Global Lock to take  */
    ACPI_GLOBAL_LOCK_BUSY,      /* the firmware holds it and did not let go  */
} AcpiGlobalLockState;

/* `who` names the access for the log — "\\_PIC", a field name. Never NULL.
 * Anything but ACPI_GLOBAL_LOCK_HELD means the access must not be made:
 * there is nothing holding the firmware off it. Release is owed only after
 * HELD, and only from the same core. */
AcpiGlobalLockState AcpiGlobalLockAcquire(const char *who);
void AcpiGlobalLockRelease(const char *who);

/* RSDP discovery + table mapping. */
acpi_rsdp_t* acpi_find_rsdp(void);
acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp);

/* AML _S5 extraction from a single loaded table buffer (DSDT or SSDT body).
 * `aml` points at the bytes immediately after the table header. */
bool acpi_search_s5_in_aml(uint8_t* aml, uint32_t aml_len);

/* HPET / MCFG / NUMA / IOMMU / APEI parsers — called from acpi_parse_tables. */
void acpi_parse_hpet(void);
void acpi_parse_mcfg(void);
void acpi_parse_srat(void);
void acpi_parse_slit(void);
void acpi_parse_dmar(void);
void acpi_parse_ivrs(void);
void acpi_parse_apei(void);
void acpi_parse_wsmt(void);
void acpi_apei_consume(void);
void acpi_dmar_probe_registers(void);

/* ERST runtime executor. Implementation in acpi_erst.c. */
void acpi_erst_bind(acpi_erst_t* erst);
int  erst_run_action(uint8_t action, uint64_t in_value, uint64_t* out_value);
int  erst_write_record(uint64_t record_id, uint64_t* status_out);
int  erst_read_record(uint64_t record_id, uint64_t* status_out);
int  erst_clear_record(uint64_t record_id, uint64_t* status_out);
uint64_t erst_get_record_count(void);

uint8_t acpi_checksum(void* data, size_t length);
volatile void* acpi_map_physical(uintptr_t phys_addr, size_t size);

/* Validate an SDT (signature + checksum + minimum length). */
bool acpi_validate_table(const acpi_sdt_header_t* header);

/* Find a table by signature in either XSDT (preferred when present per
 * ACPI 6.5 §5.2.5.3) or RSDT. Returns NULL if absent. */
acpi_sdt_header_t* acpi_find_table(const char* signature);

/* Iterate all tables of a given signature (e.g. multiple SSDTs).
 * `cb` is invoked for each match until it returns false or list exhausts. */
typedef bool (*acpi_table_iter_cb_t)(acpi_sdt_header_t* tbl, void* user);
void acpi_for_each_table(const char* signature,
                         acpi_table_iter_cb_t cb,
                         void* user);

#endif // ACPI_INTERNAL_H
