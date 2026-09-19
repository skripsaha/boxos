#include "acpi_internal.h"
#include "io.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "vmm.h"
#include "pci.h"
#include "efi.h"
#include <stddef.h>

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

static void delay_ms(uint32_t ms) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while (rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
}

static void triple_fault(void) {
    debug_printf("[ACPI] Triggering triple fault...\n");

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
    debug_printf("[ACPI] Attempting keyboard controller reset...\n");

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
    debug_printf("[ACPI] Attempting 0xCF9 reset control register...\n");

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

/* gas_write_word — write a 16-bit value through a Generic Address Structure.
 *
 * GAS address_space dispatch matches the ACPI spec rather than assuming I/O.
 * On consumer/desktop x86 PM1 CNT lives at I/O 0x604-class ports; on some
 * server boards and embedded SoCs (ACPI 2.0+) it can be memory-mapped, where
 * `outw` would silently miss the register. UEFI machines occasionally route
 * the reset register via PCI config space. Handle all three.
 *
 * For MMIO we map the register UC via vmm_map_mmio so the access is
 * uncached + write-through (PCD=1, PWT=1) and visible to the chipset
 * regardless of MTRR state — same correctness path as the VGA framebuffer.
 */
static int gas_write_word(const acpi_gas_t *gas, uint16_t value) {
    if (!gas || gas->address == 0) return -1;

    switch (gas->address_space) {
        case GAS_AS_SYSTEM_IO:
            outw((uint16_t)gas->address, value);
            return 0;

        case GAS_AS_SYSTEM_MEMORY: {
            volatile void *vmap = vmm_map_mmio((uintptr_t)gas->address,
                                               sizeof(uint16_t),
                                               VMM_FLAGS_KERNEL_RW);
            if (!vmap) {
                debug_printf("[ACPI] GAS MMIO map failed for 0x%lx\n",
                             (unsigned long)gas->address);
                return -1;
            }
            *(volatile uint16_t *)vmap = value;
            /* Don't unmap — we're on a one-way path to power off / reset. */
            return 0;
        }

        case GAS_AS_PCI_CONFIG: {
            /* Segment and bus are not fields of the address: Table 5.2 fixes
             * both at zero for this address space. ECAM unlocks the extended
             * (12-bit) offsets; the 0xCF8/0xCFC fallback stops at 0xFE for a
             * word. */
            GasPciAddress pci;
            if (!gas_pci_decode(gas->address, &pci)) return -1;

            debug_printf("[ACPI] GAS PCI_CONFIG 0000:00:%02x.%x off=0x%x val=0x%x\n",
                         pci.device, pci.function, pci.offset, value);

            if (pci_has_ecam()) {
                pci_ecam_write_word(0, 0, pci.device, pci.function,
                                    pci.offset, value);
                return 0;
            }
            if (pci.offset <= 0xFEu) {
                pci_config_write_word(0, pci.device, pci.function,
                                      (uint8_t)pci.offset, value);
                return 0;
            }
            kprintf("[ACPI] GAS PCI_CONFIG offset 0x%x needs extended config "
                    "space and this machine published no MCFG\n", pci.offset);
            return -1;
        }

        default:
            debug_printf("[ACPI] GAS unknown address_space=%u\n", gas->address_space);
            return -1;
    }
}

static int gas_write_byte(const acpi_gas_t *gas, uint8_t value) {
    if (!gas || gas->address == 0) return -1;

    switch (gas->address_space) {
        case GAS_AS_SYSTEM_IO:
            outb((uint16_t)gas->address, value);
            return 0;

        case GAS_AS_SYSTEM_MEMORY: {
            volatile void *vmap = vmm_map_mmio((uintptr_t)gas->address,
                                               sizeof(uint8_t),
                                               VMM_FLAGS_KERNEL_RW);
            if (!vmap) return -1;
            *(volatile uint8_t *)vmap = value;
            return 0;
        }

        case GAS_AS_PCI_CONFIG: {
            GasPciAddress pci;
            if (!gas_pci_decode(gas->address, &pci)) return -1;

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

/* Read a 16-bit register through the same GAS its write will use.
 *
 * PM1_CNT is a control register, and ACPI 6.5 §4.6.1 is explicit about those:
 * "OSPM implementations must write zeros to reserved bits in enable and status
 * registers and preserve bits in control registers"; §4.6.2 says the same for
 * ignored bit fields. A sleep request is therefore a read-modify-write, and
 * this is the read half of it. */
static int gas_read_word(const acpi_gas_t *gas, uint16_t *out) {
    if (!gas || !out || gas->address == 0) return -1;

    switch (gas->address_space) {
        case GAS_AS_SYSTEM_IO:
            *out = inw((uint16_t)gas->address);
            return 0;

        case GAS_AS_SYSTEM_MEMORY: {
            volatile void *vmap = vmm_map_mmio((uintptr_t)gas->address,
                                               sizeof(uint16_t),
                                               VMM_FLAGS_KERNEL_RW);
            if (!vmap) {
                debug_printf("[ACPI] GAS MMIO map failed for 0x%lx\n",
                             (unsigned long)gas->address);
                return -1;
            }
            *out = *(volatile uint16_t *)vmap;
            return 0;
        }

        case GAS_AS_PCI_CONFIG: {
            GasPciAddress pci;
            if (!gas_pci_decode(gas->address, &pci)) return -1;

            if (pci_has_ecam()) {
                *out = pci_ecam_read_word(0, 0, pci.device, pci.function,
                                          pci.offset);
                return 0;
            }
            if (pci.offset <= 0xFEu) {
                *out = pci_config_read_word(0, pci.device, pci.function,
                                            (uint8_t)pci.offset);
                return 0;
            }
            return -1;
        }

        default:
            debug_printf("[ACPI] GAS unknown address_space=%u\n",
                         gas->address_space);
            return -1;
    }
}

/* Pick the live PM1 control GAS:
 *   1. ACPI 2.0+ X_ variant if the FADT is long enough to actually contain
 *      it AND it has a non-zero address — spec-preferred descriptor.
 *   2. Synthesise a legacy I/O GAS from the 32-bit FADT field otherwise.
 *
 * The length check matters on real hardware: every ACPI 1.0b firmware (and
 * QEMU's default i440fx FADT) ships rev=1, len=116, where `x_pm1a_*` lives
 * past EOT and `*x_gas` reads garbage. Trusting it sends PM1 writes to a
 * random MMIO address and shutdown silently fails.
 */
static acpi_gas_t resolve_pm1_gas(const acpi_gas_t *x_gas, uint32_t legacy_port,
                                  uint32_t fadt_len, size_t x_field_end) {
    acpi_gas_t out = {0};
    if (x_gas && fadt_field_present(fadt_len, x_field_end) &&
        x_gas->address != 0) {
        out = *x_gas;
        return out;
    }
    if (legacy_port != 0) {
        out.address_space = GAS_AS_SYSTEM_IO;
        out.bit_width     = 16;
        out.bit_offset    = 0;
        out.access_size   = 2;
        out.address       = legacy_port;
    }
    return out;
}

/* PM1 control register fields — ACPI 6.5 §4.8.3.2.1, Table 4.13. */
#define PM1_CNT_SLP_TYP_SHIFT  10
#define PM1_CNT_SLP_TYP_MASK   0x1C00u   /* bits 12:10 */
#define PM1_CNT_SLP_EN         0x2000u   /* bit 13     */

/* Ask the platform for a sleep state through one PM1 control register.
 *
 * Everything but SLP_TYP and SLP_EN is preserved, because §4.6.1 has OSPM
 * preserve the bits of a control register and Table 4.13 names one of them in
 * particular: bit 0 is SCI_EN, "the responsibility of the hardware to set or
 * reset", which OSPM always preserves.
 *
 * The word written here used to be built from nothing — (SLP_TYP << 10) |
 * SLP_EN — so it drove SCI_EN and BM_RLD to zero along with every reserved
 * bit, handing the PM event decoder back to the SMI arbiter that
 * acpi_enable_mode() had just spent up to three seconds taking it from, on
 * exactly the legacy-BIOS boards that need that handoff. Nothing in the tree
 * ever put SCI_EN back, so on every path where the write does not immediately
 * kill power — the PM1b retry, the halt below — PM1 events and GPEs were
 * routed to SMI for the rest of the machine's life. ACPICA masks off the same
 * two fields and no others (acpi_hw_legacy_sleep). */
static void attempt_acpi_pm1(const acpi_gas_t *gas, uint16_t slp_typ, const char *tag) {
    if (gas->address == 0) return;

    uint16_t cur = 0;
    if (gas_read_word(gas, &cur) != 0) {
        kprintf("[ACPI] %s control register cannot be read; a sleep vector "
                "written blind would clear SCI_EN with it, so it is not "
                "written\n", tag);
        return;
    }
    if (cur == 0xFFFFu) {
        /* Table 4.13 gives PM1_CNT reserved bits that read zero, so an
         * all-ones word is a port answering nothing, not a register. */
        kprintf("[ACPI] %s control register reads 0xFFFF — nothing is decoding "
                "that address, so there is no register to ask for sleep\n",
                tag);
        return;
    }

    uint16_t keep = (uint16_t)(cur & ~(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN));
    uint16_t typ  = (uint16_t)(keep |
                               (((uint16_t)(slp_typ << PM1_CNT_SLP_TYP_SHIFT)) &
                                PM1_CNT_SLP_TYP_MASK));

    debug_printf("[ACPI] %s shutdown via GAS as=%u addr=0x%lx cur=0x%04x "
                 "SLP_TYP=0x%x -> 0x%04x then 0x%04x\n",
                 tag, gas->address_space, (unsigned long)gas->address,
                 cur, slp_typ, typ, (uint16_t)(typ | PM1_CNT_SLP_EN));

    /* Two writes, as ACPICA does: the sleep type first, then the same word
     * with SLP_EN set. The chipset sequences on the SLP_EN transition, and a
     * separate write means the type is already standing when it arrives. */
    if (gas_write_word(gas, typ) != 0) {
        kprintf("[ACPI] %s SLP_TYP write failed\n", tag);
        return;
    }
    if (gas_write_word(gas, (uint16_t)(typ | PM1_CNT_SLP_EN)) != 0) {
        kprintf("[ACPI] %s SLP_EN write failed\n", tag);
        return;
    }
    delay_ms(100);
}

static void attempt_acpi_reset(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return;

    /* reset_reg + reset_value were added in ACPI 2.0 (FADT rev 3, length
     * grew to 244). On a rev 1 FADT (len=116) those bytes don't exist —
     * accessing them reads adjacent table data and would issue a wild
     * write to a random MMIO/IO address. Guard with a length check. */
    uint32_t flen = g_acpi.fadt->header.length;
    if (!fadt_field_present(flen, offsetof(acpi_fadt_t, reset_value) + 1)) {
        debug_printf("[ACPI] FADT too short for reset register (len=%u)\n", flen);
        return;
    }

    if (!(g_acpi.fadt->flags & (1 << 10))) {
        debug_printf("[ACPI] FADT does not support reset register\n");
        return;
    }

    const acpi_gas_t *reg = &g_acpi.fadt->reset_reg;
    uint8_t val           = g_acpi.fadt->reset_value;

    debug_printf("[ACPI] reset: addr_space=%u addr=0x%lx value=0x%x\n",
                 reg->address_space, (unsigned long)reg->address, val);

    if (gas_write_byte(reg, val) != 0) {
        debug_printf("[ACPI] reset write failed (unsupported address space)\n");
        return;
    }
    delay_ms(100);
}

void acpi_shutdown(void) {
    __asm__ volatile("cli");

    if (g_acpi.initialized) {
        debug_printf("[ACPI] poweroff via PM1 (PM1a=0x%x PM1b=0x%x SLP_TYPa=0x%x SLP_TYPb=0x%x rev=%u len=%u)\n",
                     g_acpi.pm1a_cnt_blk, g_acpi.pm1b_cnt_blk,
                     g_acpi.slp_typa, g_acpi.slp_typb,
                     g_acpi.fadt ? g_acpi.fadt->header.revision : 0,
                     g_acpi.fadt ? g_acpi.fadt->header.length   : 0);
    } else {
        debug_printf("[ACPI] not initialised; no soft poweroff available\n");
    }

    /* UEFI 2.10 §8.5.1: EFI ResetSystem(EfiResetShutdown) is the
     * firmware-supported soft-off path on UEFI machines. Prefer it
     * because ACPI PM1 SLP_TYP writes can silently no-op when the SCI
     * is disabled or the FADT is ACPI 1.0b without an _S5 object.
     * Returns only if the firmware refuses; we then fall through to
     * the spec-preferred ACPI PM1 path. */
    if (efi_runtime_available()) {
        debug_printf("[ACPI] trying EFI ResetSystem(SHUTDOWN)...\n");
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

    if (g_acpi.initialized && g_acpi.fadt && g_acpi.s5_found) {
        uint32_t flen = g_acpi.fadt->header.length;
        acpi_gas_t pm1a = resolve_pm1_gas(&g_acpi.fadt->x_pm1a_control_block,
                                          g_acpi.pm1a_cnt_blk,
                                          flen,
                                          offsetof(acpi_fadt_t, x_pm1a_control_block) +
                                              sizeof(acpi_gas_t));
        acpi_gas_t pm1b = resolve_pm1_gas(&g_acpi.fadt->x_pm1b_control_block,
                                          g_acpi.pm1b_cnt_blk,
                                          flen,
                                          offsetof(acpi_fadt_t, x_pm1b_control_block) +
                                              sizeof(acpi_gas_t));
        if (pm1a.address) attempt_acpi_pm1(&pm1a, g_acpi.slp_typa, "PM1a");
        if (pm1b.address) attempt_acpi_pm1(&pm1b, g_acpi.slp_typb, "PM1b");
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
    if (efi_runtime_available()) {
        debug_printf("[ACPI] Attempting EFI ResetSystem(WARM)...\n");
        efi_reset_system(EFI_RESET_WARM, EFI_STATUS_SUCCESS, 0, NULL);
        /* If the firmware refused (returns), fall through. */
    }

    debug_printf("[ACPI] Attempting ACPI reset register...\n");
    attempt_acpi_reset();

    attempt_cf9_reset();

    attempt_keyboard_reset();

    triple_fault();

    debug_printf("[ACPI] All reboot methods failed, entering HLT loop\n");
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
