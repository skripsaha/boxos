#include "acpi_internal.h"
#include "io.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "pci.h"
#include "efi.h"
#include "serial.h"   /* WireForceRelease / WireDrain — acpi_reboot()'s ladder is heard */
#include "klib_logring.h" /* LogKeepFlush — a rung's account survives the reset the next rung causes */

/* ACPI Generic Address Structure address_space_id values (ACPI 6.x §5.2.3.2) */
#define GAS_AS_SYSTEM_MEMORY  0  /* MMIO */
#define GAS_AS_SYSTEM_IO      1  /* I/O port */

/* True iff `len` bytes of FADT cover the X_/extended field that ends at
 * `field_end` (offsetof(field) + sizeof(field)). ACPI 1.0b FADT is only
 * 116 bytes — its `x_*` and `reset_reg` fields are not present, and
 * struct accesses past EOT yield garbage from neighbouring memory. */
static inline bool fadt_field_present(uint32_t len, size_t field_end) {
    return (size_t)len >= field_end;
}
#define GAS_AS_PCI_CONFIG     2  /* PCI config space */

/* ACPI 6.5 Table 5.10, FADT Fixed Feature Flags, bit 20: "If set, indicates
 * that the ACPI Hardware Interface is not implemented." acpi.c's
 * acpi_enable_mode() and pmtimer.c's probe already carry this same bit under
 * this same name — each file keeps its own copy rather than share one from
 * acpi.h, and this is a third. Left as-is here rather than centralised
 * unasked: three lanes touch ACPI in this tree at once, and acpi.h's FADT
 * flags are not this task's to redesign. */
#define FADT_FLAG_HW_REDUCED_ACPI   (1u << 20)

/* ACPI 6.5 §5.2.9, FADT Flags, RESET_REG_SUP: "If set, indicates the system
 * supports system reset via the FADT RESET_REG." */
#define FADT_FLAG_RESET_REG_SUP     (1u << 10)

static void delay_ms(uint32_t ms) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while (rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
}

/* Every debug_printf from here through the end of acpi_reboot() narrated one
 * rung of the reset/reboot/shutdown ladder — and debug_printf compiles to
 * nothing outside a CONFIG_DEBUG_ENABLED build (klib.h). On real hardware
 * there is no debugger and no such build attached, so a ladder narrated in
 * debug_printf said nothing at all when a board actually needed it,
 * including the successful 0xCF9 rewrite that would otherwise be the last
 * thing anyone could confirm ran. kprintf is used instead, everywhere below,
 * the same way it already was for the final "Soft poweroff failed" line.
 * acpi_print_info()'s table dump further down is NOT part of this: it is
 * read on demand from a running machine, not from one that has already gone
 * silent, so it keeps debug_printf. */
static void triple_fault(void) {
    kprintf("[ACPI] Triggering triple fault...\n");

    /* This function never returns — the int3 below either genuinely triple
     * faults the CPU (garbage IDTR, so the CPU cannot vector the exception
     * and resets) or, failing that, parks in the hlt loop at the bottom
     * forever. Either way nothing after this line runs again, so the line
     * above is drained here, in place, rather than by a caller that will
     * never regain control to do it. The carry-over window goes with it: a
     * triple fault IS a warm reset, and a warm reset invalidates the caches
     * without writing them back (Intel SDM Vol 3A §11.5.1). */
    LogKeepFlush();
    WireForceRelease();
    WireDrain();

    __asm__ volatile (
        "cli\n"
        "lidt %0\n"
        "int $0x03\n"
        :
        : "m"((struct { uint16_t limit; uint64_t base; }){ 0, 0 })
    );

    while (1) hlt();
}

static void attempt_keyboard_reset(void) {
    kprintf("[ACPI] Attempting keyboard controller reset...\n");

    uint8_t status;
    for (int i = 0; i < 10; i++) {
        status = inb(0x64);
        if ((status & 0x02) == 0) break;
        delay_ms(1);
    }

    outb(0x64, 0xFE);

    delay_ms(100);
}

/* attempt_cf9_reset — reboot via the chipset Reset Control Register (RST_CNT).
 *
 * Intel PCH/ICH and the QEMU/Bochs PIIX southbridge expose a byte-wide reset
 * register at I/O port 0xCF9 (Intel 400-series PCH register database, "Reset
 * Control Register (RST_CNT) — offset cf9"; layout unchanged back to ICH/PIIX):
 *   bit 1  SYS_RST  : 0 = soft reset (CPU INIT only), 1 = hard (asserts PCIRST#)
 *   bit 2  RST_CPU  : a 0->1 transition initiates the reset
 *   bit 3  FULL_RST : 1 = full power cycle (cold), 0 = warm
 *
 * Sequence matches Linux `reboot=pci`: arm a warm hard reset (SYS_RST), pause,
 * then pulse RST_CPU 0->1 to fire it. 0x06 keeps it warm — no power cycle.
 *
 * This is the one method that resets legacy-BIOS Bochs (whose rombios FADT is
 * ACPI 1.0 and carries no reset register), QEMU, and every modern x86 chipset,
 * so it sits ahead of the 8042 and triple-fault fallbacks.
 *
 * 0xCF9 overlaps the high byte of the 0xCF8 PCI CONFIG_ADDRESS dword. The
 * chipset decodes a *byte* write at 0xCF9 as RST_CNT, but a byte READ returns
 * CONFIG_ADDRESS[15:8] — not the reset register. So we write fully-defined
 * values instead of read-modify-write: an RMW would fold stray PCI-address
 * bits (and any firmware-set FULL_RST) into the write and could turn the
 * intended warm reset into a cold power-cycle. Explicit writes guarantee
 * SYS_RST=1, FULL_RST=0, with a clean 0->1 edge on RST_CPU. */
static void attempt_cf9_reset(void) {
    kprintf("[ACPI] Attempting 0xCF9 reset control register...\n");

    outb(0xCF9, 0x02u);   /* SYS_RST=1, RST_CPU=0, FULL_RST=0: arm warm hard reset */
    delay_ms(1);
    outb(0xCF9, 0x06u);   /* RST_CPU 0->1: trigger warm hard reset */
    delay_ms(100);
}

/* ACPI 6.5 §5.2.3.2, Table 5.2, address space 2 "PCI Configuration Space".
 *
 * "PCI Configuration space addresses must be confined to devices on PCI
 * Segment Group 0, bus 0." The 64-bit address is four 16-bit words:
 *
 *   [63:48]  reserved, must be 0
 *   [47:32]  PCI device number on bus 0
 *   [31:16]  PCI function number
 *   [15:0]   offset in the configuration space header
 *
 * The spec's own worked example settles the bit positions beyond argument:
 * offset 23h of function 2 on device 7 is 0x0000000700020023.
 *
 * What stood here read the bus from [31:24] and the device from [23:19] and
 * claimed "ACPI 6.5 §5.2.3.2 + edk2 layout" for it. Neither source says that:
 * §5.2.3.2 is the table above, and edk2's PCI_SEGMENT_LIB_ADDRESS puts
 * function at 14:12, device at 19:15, bus at 27:20 and segment at 47:32.
 * Under the old packing the device word was never read at all, so every
 * conformant address decoded to device 0 — bus and function survived only by
 * coincidence — and the FADT reset register, which §4.8.3.6 expressly allows
 * to sit on a device such as the LPC bridge at 00:1F.0, was written into the
 * host bridge at 00:00.0 while acpi_reboot() moved on to the next method with
 * nothing said.
 *
 * A word that is not in Table 5.2 form is refused out loud rather than
 * decoded into a plausible BDF: the device and function words are sixteen
 * bits wide here and five and three bits wide in PCI, so a value that does
 * not fit is firmware saying something this kernel has not understood.
 */
typedef struct {
    uint8_t  device;
    uint8_t  function;
    uint16_t offset;
} GasPciAddress;

static bool gas_pci_decode(uint64_t address, GasPciAddress *out) {
    uint64_t reserved = (address >> 48) & 0xFFFFu;
    uint64_t device   = (address >> 32) & 0xFFFFu;
    uint64_t function = (address >> 16) & 0xFFFFu;

    if (reserved != 0 || device > 31 || function > 7) {
        kprintf("[ACPI] GAS PCI-config address 0x%lx is not the form ACPI 6.5 "
                "Table 5.2 defines (reserved=0x%lx device=%lu function=%lu); "
                "refusing the access rather than inventing a BDF for it\n",
                (unsigned long)address, (unsigned long)reserved,
                (unsigned long)device, (unsigned long)function);
        return false;
    }

    out->device   = (uint8_t)device;
    out->function = (uint8_t)function;
    out->offset   = (uint16_t)(address & 0xFFFFu);
    return true;
}

/* The one writer on the one-way path. It allocates nothing and maps nothing:
 * a System Memory register was mapped in acpi_init (AcpiOneWayRegistersLocate)
 * and is dereferenced here. It used to call vmm_map_mmio at this point, which
 * takes kernel_mmio_lock and the address space's lock and creates page tables
 * — after halt_all_ap_cores() has frozen the APs wherever they stood, any of
 * which can be held by a core that is never coming back. */
static int one_way_write_byte(const AcpiOneWayRegister *r, uint8_t value) {
    if (!r || !r->usable) return -1;

    switch (r->space) {
        case GAS_AS_SYSTEM_IO:
            outb((uint16_t)r->address, value);
            return 0;

        case GAS_AS_SYSTEM_MEMORY:
            *r->mapped = value;
            return 0;

        case GAS_AS_PCI_CONFIG: {
            GasPciAddress pci;
            if (!gas_pci_decode(r->address, &pci)) return -1;

            /* Said out loud, not with debug_printf: this is the reset
             * register, it is written once in the machine's life, and which
             * function it actually reached is the whole question when the
             * board does not reset. */
            kprintf("[ACPI] GAS PCI_CONFIG 0000:00:%02x.%x off=0x%x val=0x%x\n",
                    pci.device, pci.function, pci.offset, value);

            if (pci_has_ecam()) {
                pci_ecam_write_byte(0, 0, pci.device, pci.function,
                                    pci.offset, value);
                return 0;
            }
            if (pci.offset <= 0xFFu) {
                pci_config_write_byte(0, pci.device, pci.function,
                                      (uint8_t)pci.offset, value);
                return 0;
            }
            kprintf("[ACPI] GAS PCI_CONFIG offset 0x%x needs extended config "
                    "space and this machine published no MCFG\n", pci.offset);
            return -1;
        }

        default:
            return -1;
    }
}


static void attempt_acpi_reset(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return;

    /* reset_reg + reset_value were added in ACPI 2.0 (FADT rev 3, length
     * grew to 244). On a rev 1 FADT (len=116) those bytes don't exist —
     * accessing them reads adjacent table data and would issue a wild
     * write to a random MMIO/IO address. Guard with a length check. */
    uint32_t flen = g_acpi.fadt->header.length;
    if (!fadt_field_present(flen, offsetof(acpi_fadt_t, reset_value) + 1)) {
        kprintf("[ACPI] FADT too short for reset register (len=%u)\n", flen);
        return;
    }

    if (!(g_acpi.fadt->flags & FADT_FLAG_RESET_REG_SUP)) {
        kprintf("[ACPI] FADT does not support reset register\n");
        return;
    }

    /* Resolved and, in System Memory, mapped during acpi_init. `usable` is
     * the whole question here — the reasons it can be false were each said
     * out loud at bind time, and the boot survey prints the answer. */
    const AcpiOneWayRegister *reg = AcpiResetRegister();
    uint8_t val = g_acpi.fadt->reset_value;

    if (!reg->named) {
        kprintf("[ACPI] FADT flags claim RESET_REG_SUP but the descriptor "
                "names no address\n");
        return;
    }

    kprintf("[ACPI] reset: addr_space=%u addr=0x%lx value=0x%x\n",
            reg->space, (unsigned long)reg->address, val);

    if (one_way_write_byte(reg, val) != 0) {
        kprintf("[ACPI] reset write failed — this register was not usable "
                "without allocating, and this path allocates nothing\n");
        return;
    }
    delay_ms(100);
}

/* attempt_sleep_control_reg — the S5 write for a Hardware-Reduced platform
 * (FADT_FLAG_HW_REDUCED_ACPI set), which has no PM1a/b control block for
 * acpi_pm1_request_sleep to write.
 *
 * ACPI 6.5 §4.8.3.7 "Sleep Control and Status Registers": SLEEP_CONTROL_REG
 * takes the same three-bit \_Sx sleep type that would otherwise go in
 * PM1x_CNT.SLP_TYP, at bits [4:2], plus SLP_EN at bit 5 (Table 4.15) — ONE
 * byte, ONE write, because there is one register instead of an A/B pair to
 * sequence. `slp_typ` is masked to three bits because it is read out of
 * g_acpi.slp_typa, a uint16_t sized for the PM1 field this platform does not
 * have; a \_S5 package malformed enough to set bits above 2:0 must not walk
 * into bits this function does not own.
 *
 * The register itself is a Table-5.9 extension at offset 244 — acpi.h's
 * sleep_control_reg — absent on any FADT under 268 bytes. That length gate,
 * and the mapping a System Memory descriptor needs, both happened once in
 * acpi_init; what arrives here is an AcpiOneWayRegister that already knows
 * the answer, and one_way_write_byte() allocates nothing to use it. */
static bool attempt_sleep_control_reg(uint8_t slp_typ) {
    const AcpiOneWayRegister *reg = AcpiSleepControlRegister();

    if (!reg->present) {
        kprintf("[ACPI] FADT is %u bytes, too short to carry "
                "SLEEP_CONTROL_REG (offset 244) — hardware-reduced poweroff "
                "has no register to write\n", g_acpi.fadt->header.length);
        return false;
    }
    if (!reg->named) {
        kprintf("[ACPI] FADT names no SLEEP_CONTROL_REG address — "
                "hardware-reduced poweroff cannot proceed\n");
        return false;
    }

    uint8_t value = (uint8_t)(((slp_typ & 0x07u) << 2) | (1u << 5));
    kprintf("[ACPI] hardware-reduced poweroff: SLEEP_CONTROL_REG "
            "addr_space=%u addr=0x%lx value=0x%x\n",
            reg->space, (unsigned long)reg->address, value);

    if (one_way_write_byte(reg, value) != 0) {
        kprintf("[ACPI] SLEEP_CONTROL_REG write failed — this register was "
                "not usable without allocating, and this path allocates "
                "nothing\n");
        return false;
    }
    return true;
}

void acpi_shutdown(void) {
    __asm__ volatile("cli");

    /* Which register the S5 write is about to go to — the two platforms have
     * nothing in common here, and this line used to name PM1 on both. A
     * hardware-reduced platform HAS no PM1 control block (ACPI 6.5 Table
     * 5.10, HW_REDUCED_ACPI), so printing its ports and sleep types was
     * describing a register that does not exist. The rest of the FADT is
     * surveyed once at boot by AcpiFadtSurvey (acpi.c), not again here. */
    if (!g_acpi.initialized) {
        kprintf("[ACPI] not initialised; no soft poweroff available\n");
    } else if (g_acpi.fadt &&
               (g_acpi.fadt->flags & FADT_FLAG_HW_REDUCED_ACPI)) {
        kprintf("[ACPI] poweroff on a hardware-reduced platform: no PM1 "
                "control block exists, so S5 goes to SLEEP_CONTROL_REG "
                "(SLP_TYPa=0x%x)\n", g_acpi.slp_typa);
    } else {
        kprintf("[ACPI] poweroff via PM1 (PM1a=0x%x PM1b=0x%x SLP_TYPa=0x%x "
                "SLP_TYPb=0x%x)\n",
                g_acpi.pm1a_cnt_blk, g_acpi.pm1b_cnt_blk,
                g_acpi.slp_typa, g_acpi.slp_typb);
    }

    /* UEFI 2.10 §8.5.1: EFI ResetSystem(EfiResetShutdown) is the
     * firmware-supported soft-off path on UEFI machines. Prefer it
     * because ACPI PM1 SLP_TYP writes can silently no-op when the SCI
     * is disabled or the FADT is ACPI 1.0b without an _S5 object.
     * Returns only if the firmware refuses; we then fall through to
     * the spec-preferred ACPI PM1 path. */
    if (efi_runtime_available()) {
        kprintf("[ACPI] trying EFI ResetSystem(SHUTDOWN)...\n");
        efi_reset_system(EFI_RESET_SHUTDOWN, EFI_STATUS_SUCCESS, 0, NULL);
    }

    /* ACPI 6.5 §7.1: "If _S5 is not specified, alternative methods are used to
     * turn-off the system." No \_S5 means no sleep vector and no default to
     * put in its place — the 0x05 that used to be invented here is S3 on an
     * Intel PCH, so the machine asked to power off would have suspended to RAM
     * with every core halted. The PM1 write is skipped instead. */
    if (g_acpi.initialized && g_acpi.fadt && !g_acpi.s5_found) {
        kprintf("[ACPI] this firmware published no \\_S5, so there is no sleep "
                "vector to write and none will be guessed\n");
    }

    /* Before the S5 write, not after: either branch below can be the write that
     * takes the machine's power away, and neither returns to flush anything.
     * The window survives a warm reset, not a power cut (klib_logring.h), so
     * what this buys is the account of a poweroff that DIDN'T work and was
     * ended with the reset button. */
    LogKeepFlush();

    /* Through acpi_pm1_request_sleep(), which is the kernel's ONLY resolver
     * of the PM1 control pair and its only writer.
     *
     * A second resolver lived here — resolve_pm1_gas() feeding gas_read_word
     * / gas_write_word — and it was this one, not acpi.c's, that the live S5
     * path used. They agreed on preferring the X_ descriptor and disagreed on
     * everything after that: an X_ descriptor naming an address space OSPM
     * cannot reach was handed straight to gas_read_word here and refused,
     * while acpi.c read the same descriptor as §5.2.9's "non zero value which
     * can be used by the OSPM" failing and went back to the legacy address.
     * So on one board the ACPI-mode handoff found a PM1 register and the
     * shutdown found none. One register, one resolver.
     *
     * It also stops this path from calling vmm_map_mmio: a System Memory PM1
     * register is mapped once, in acpi_init, long before halt_stop_hardware
     * has run and the kernel is asked to allocate on a one-way path. */
    if (g_acpi.initialized && g_acpi.fadt && g_acpi.s5_found) {
        /* A Hardware-Reduced platform (FADT flags bit 20) has no PM1a/b
         * control block for acpi_pm1_request_sleep to resolve at all — going
         * to it anyway would only reproduce, register by register, the
         * "no PM1 block described" refusal acpi_enable_mode() already names
         * during the ACPI-mode handoff. The flag is consulted up front
         * instead, and the S5 write goes to SLEEP_CONTROL_REG in its place. */
        if (g_acpi.fadt->flags & FADT_FLAG_HW_REDUCED_ACPI) {
            if (attempt_sleep_control_reg((uint8_t)g_acpi.slp_typa)) {
                delay_ms(100);
            }
        } else if (acpi_pm1_request_sleep((uint8_t)g_acpi.slp_typa,
                                          (uint8_t)g_acpi.slp_typb)) {
            delay_ms(100);
        }
    }

    /* Soft poweroff exhausted. ACPI S5 is the only standardised way to
     * power down on x86 — there is no second method on real hardware.
     * NEVER reset/triple-fault here: those reboot the machine, which is
     * the wrong semantics for shutdown. Park the CPU and let the operator
     * pull power. */
    /* kprintf, not debug_printf: this is the last thing the machine says
     * before it parks forever, and the operator standing in front of a box
     * with its fans still running is the one person who needs to read it. A
     * shipped build compiles debug_printf into nothing. */
    kprintf("[ACPI] Soft poweroff failed; halting CPU. Power off manually.\n");
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void acpi_reboot(void) {
    __asm__ volatile("cli");

    /* Escalating reboot methods, cleanest first; each is bounded and falls
     * through to the next on failure:
     *   1. EFI ResetSystem(EfiResetWarm) — UEFI 2.10 §8.5.1. Available
     *      only on UEFI boots after efi_runtime_init succeeded. This is
     *      the spec-mandated firmware reset on UEFI machines and works
     *      uniformly across vendors when 0xCF9 is filtered or routed.
     *   2. ACPI reset register — firmware-described. On UEFI the FADT
     *      typically routes this to 0xCF9 itself.
     *   3. 0xCF9 RST_CNT — universal chipset reset (real PCH/ICH, QEMU,
     *      Bochs); the only method that resets legacy BIOS whose FADT
     *      has no reset reg.
     *   4. 8042 pulse (0xFE -> 0x64) — legacy keyboard-controller fallback.
     *   5. triple fault — last resort; real CPUs always reset on it. */
    /* Every rung below gets its own WireForceRelease()+WireDrain() rather
     * than one at the end: this is a one-way ladder, each rung can be the
     * one that actually resets the machine, and a message queued behind a
     * rung that never returns is a message nobody reads. ForceRelease always
     * comes first — g_serial_lock may still be held by an AP this same
     * shutdown parked mid-kprintf in halt_all_ap_cores(), and that core is
     * never coming back to release it.
     *
     * LogKeepFlush() rides beside every one of them for the same reason and a
     * different medium: the wire carries the line to a cable nobody on a bench
     * has, while the carry-over window is what the NEXT boot reads. Rung 1
     * returning is now an ordinary event — Lifeline gives control back after a
     * fault inside the firmware — so rungs 2..5 run, and why rung 1 failed must
     * survive the reset rung 3 causes. A warm reset invalidates the caches
     * without writing them back (Intel SDM Vol 3A §11.5.1), so a window written
     * only through the ordinary write-back path loses exactly that tail. */
    if (efi_runtime_available()) {
        kprintf("[ACPI] Attempting EFI ResetSystem(WARM)...\n");
        efi_reset_system(EFI_RESET_WARM, EFI_STATUS_SUCCESS, 0, NULL);
        /* If the firmware refused (returns), fall through. */
        LogKeepFlush();
        WireForceRelease();
        WireDrain();
    }

    kprintf("[ACPI] Attempting ACPI reset register...\n");
    attempt_acpi_reset();
    LogKeepFlush();
    WireForceRelease();
    WireDrain();

    attempt_cf9_reset();
    LogKeepFlush();
    WireForceRelease();
    WireDrain();

    attempt_keyboard_reset();
    LogKeepFlush();
    WireForceRelease();
    WireDrain();

    triple_fault();

    /* Unreachable: triple_fault() above never returns — the int3 either
     * triple faults the CPU (reset) or falls into its own `while (1) hlt();`
     * — so the debug_printf that used to sit here, right before an identical
     * while/hlt loop, could not have run even in a build where it compiled
     * to something. Removed rather than converted to kprintf: there is
     * nothing this line could ever say that triple_fault() has not already
     * said or done. The loop stays — it is what makes acpi_reboot()'s own
     * noreturn contract hold even though triple_fault() itself is not
     * declared noreturn. */
    while (1) {
        hlt();
    }
}

void acpi_print_info(void) {
    if (!g_acpi.initialized) {
        debug_printf("[ACPI] ACPI not initialized\n");
        return;
    }

    if (g_acpi.rsdp) {
        debug_printf("RSDP:\n");
        debug_printf("  Signature: %.8s\n", g_acpi.rsdp->signature);
        debug_printf("  OEM ID: %.6s\n", g_acpi.rsdp->oem_id);
        debug_printf("  Revision: %u\n", g_acpi.rsdp->revision);
        debug_printf("  RSDT: 0x%x\n", g_acpi.rsdp->rsdt_address);
        if (g_acpi.rsdp->revision >= 2) {
            debug_printf("  XSDT: 0x%lx\n",
                         (unsigned long)g_acpi.rsdp->xsdt_address);
        }
    }

    if (g_acpi.fadt) {
        debug_printf("\nFADT:\n");
        debug_printf("  PM1a_CNT: 0x%x\n", g_acpi.pm1a_cnt_blk);
        debug_printf("  PM1b_CNT: 0x%x\n", g_acpi.pm1b_cnt_blk);
        debug_printf("  DSDT: 0x%x  X_DSDT: 0x%lx\n",
                     g_acpi.fadt->dsdt,
                     (unsigned long)g_acpi.fadt->x_dsdt);
    }

    debug_printf("\n_S5 Object:\n");
    debug_printf("  Found: %s\n",
                 g_acpi.s5_found ? "Yes" : "No (no sleep vector; PM1 skipped)");
    debug_printf("  SLP_TYPa: 0x%x\n", g_acpi.slp_typa);
    debug_printf("  SLP_TYPb: 0x%x\n", g_acpi.slp_typb);

    if (g_acpi.hpet.present) {
        debug_printf("\nHPET:\n");
        debug_printf("  Base: 0x%lx vendor=0x%04x\n",
                     (unsigned long)g_acpi.hpet.base,
                     g_acpi.hpet.vendor_id);
        debug_printf("  Counter: %s, %u comparators, min_tick=%u%s\n",
                     g_acpi.hpet.counter_size_64 ? "64-bit" : "32-bit",
                     g_acpi.hpet.comparator_count,
                     g_acpi.hpet.minimum_tick,
                     g_acpi.hpet.legacy_replacement ? " [LegacyReplace]" : "");
    } else {
        debug_printf("\nHPET: not present\n");
    }

    if (g_acpi.mcfg.present) {
        debug_printf("\nMCFG (PCIe ECAM): %u segment(s)\n", g_acpi.mcfg.count);
        for (uint8_t i = 0; i < g_acpi.mcfg.count; i++) {
            const acpi_mcfg_segment_t* s = &g_acpi.mcfg.segments[i];
            debug_printf("  [%u] group=%u base=0x%lx bus %u..%u\n",
                         i, s->segment_group,
                         (unsigned long)s->base_address,
                         s->start_bus, s->end_bus);
        }
    } else {
        debug_printf("\nMCFG: not present (PCI must use legacy 0xCF8/0xCFC)\n");
    }

    if (g_acpi.numa.present) {
        debug_printf("\nSRAT (NUMA): %u CPU(s), %u memory range(s), %u domain(s)\n",
                     g_acpi.numa.cpu_count,
                     g_acpi.numa.mem_count,
                     g_acpi.numa.domain_count);
    } else {
        debug_printf("\nSRAT: not present (uniform memory)\n");
    }

    if (g_acpi.slit.present) {
        debug_printf("SLIT: %u localities\n", g_acpi.slit.locality_count);
    }

    if (g_acpi.dmar.present) {
        debug_printf("\nDMAR (Intel VT-d): HAW=%u bits, flags=0x%x, %u DRHD\n",
                     g_acpi.dmar.host_address_width_bits,
                     g_acpi.dmar.flags,
                     g_acpi.dmar.drhd_count);
    }

    if (g_acpi.ivrs.present) {
        debug_printf("\nIVRS (AMD-Vi): %u IVHD\n", g_acpi.ivrs.ivhd_count);
    }

    if (g_acpi.apei.hest_present || g_acpi.apei.bert_present ||
        g_acpi.apei.erst_present) {
        debug_printf("\nAPEI: %s%s%s\n",
                     g_acpi.apei.hest_present ? "HEST " : "",
                     g_acpi.apei.bert_present ? "BERT " : "",
                     g_acpi.apei.erst_present ? "ERST " : "");
    }
}
