#include "acpi_internal.h"
#include "klib.h"
#include "efi.h"

/*
 * WSMT — the Windows SMM Security Mitigations Table.
 *
 * Microsoft, "Windows SMM Security Mitigations Table", version 1.0: the OS
 * "must rely on SMM firmware to accurately self-report which of the
 * Microsoft recommended security best practices it has implemented. To
 * accomplish this, Microsoft has defined the Windows SMM Security
 * Mitigations Table (WSMT)." ACPI 6.5 Table 5.6 reserves the signature for
 * that specification and says nothing else about it. The whole table is a
 * header and one 32-bit word:
 *
 *   FIXED_COMM_BUFFERS (bit 0, 0x1) — "SMM will validate that input and
 *     output buffers lie entirely within the expected fixed memory regions".
 *   COMM_BUFFER_NESTED_PTR_PROTECTION (bit 1, 0x2) — "SMM will validate that
 *     input and output pointers embedded within the fixed communication
 *     buffer only refer to address ranges that lie entirely within the
 *     expected fixed memory regions"; the "WSMT Protections Flags Details"
 *     section below that table adds that "Firmware setting this bit must
 *     also set the FIXED_COMM_BUFFERS bit".
 *   SYSTEM_RESOURCE_PROTECTION (bit 2, 0x4) — the firmware states that "it
 *     will not allow reconfiguration of system resources via non-
 *     architectural mechanisms".
 *
 * Why a kernel that never asks to enter SMM should care: this one calls EFI
 * runtime services. That means jumping into firmware code which stayed
 * resident past ExitBootServices and whose pages this kernel keeps mapped.
 * The flags are what that firmware says it checks when something calls into
 * SMM, and the flags it does NOT set are precisely the classes of attack it
 * does not defend against.
 *
 * The ceiling is named by Microsoft's companion article, "Understanding the
 * Windows SMM Security Mitigation Table (WSMT)" — not by the specification
 * itself, which is why it is attributed separately here: "From the operating
 * system, the only check that is possible is to look for the presence of the
 * WSMT, and check the state of all defined Protection Flags." So this file
 * reads the word and says what it found, and claims nothing beyond that —
 * the flags remain the firmware's statement about itself, never a property
 * anyone here verified.
 *
 * Ordering, said plainly because the log is read top to bottom: on a UEFI
 * boot efi_runtime_init() has already run by the time the ACPI tables are
 * parsed, so this line arrives after the first runtime call rather than
 * before it. It is a fact for the owner and for any later policy about how
 * far to trust this firmware — it is not a gate, and it does not pretend to
 * be one.
 */

void acpi_parse_wsmt(void) {
    memset(&g_acpi.wsmt, 0, sizeof(g_acpi.wsmt));

    acpi_wsmt_t* wsmt = (acpi_wsmt_t*)acpi_find_table("WSMT");
    if (!wsmt) {
        /* Silent on purpose. A board that publishes no WSMT is already named
         * by the inventory's verdict line, which lists every signature this
         * kernel can read and this firmware did not publish; repeating it
         * here would be a second line answering a question already answered. */
        return;
    }

    if (wsmt->header.length < sizeof(acpi_wsmt_t)) {
        kprintf("[WSMT] this firmware published a WSMT of %u byte(s), too "
                "short to hold the protection flags the table exists for — "
                "nothing can be read out of it, so what this machine's SMM "
                "checks stays unknown\n", wsmt->header.length);
        return;
    }

    uint32_t flags = wsmt->protection_flags;
    g_acpi.wsmt.protection_flags = flags;
    g_acpi.wsmt.present          = true;

    bool fixed  = (flags & ACPI_WSMT_FIXED_COMM_BUFFERS) != 0;
    bool nested = (flags & ACPI_WSMT_COMM_BUFFER_NESTED_PTR_PROTECTION) != 0;
    bool sysres = (flags & ACPI_WSMT_SYSTEM_RESOURCE_PROTECTION) != 0;

    kprintf("[WSMT] what this firmware claims about its own SMM (protection "
            "flags 0x%x): ", (unsigned)flags);
    if (fixed)
        kprintf("a buffer handed to an SMI handler is checked to lie inside "
                "the fixed region SMM expects; ");
    else
        kprintf("nothing says a buffer handed to an SMI handler is checked "
                "against the fixed region SMM expects; ");
    if (nested)
        kprintf("pointers nested inside that buffer are checked the same "
                "way; ");
    else
        kprintf("pointers nested inside that buffer are not claimed to be "
                "checked at all; ");
    if (sysres)
        kprintf("and system resources cannot be reconfigured by "
                "non-architectural means.");
    else
        kprintf("and nothing says system resources are held safe from "
                "reconfiguration by non-architectural means.");
    if (efi_runtime_available())
        kprintf(" This kernel calls that firmware's runtime services and "
                "keeps its code mapped, so whatever it does not claim to "
                "check is what this machine is exposed to. The claim is the "
                "firmware's own and no one here verified it.\n");
    else
        kprintf(" This kernel entered no runtime service on this boot, so "
                "nothing here is running that firmware's code.\n");

    if (nested && !fixed)
        kprintf("[WSMT] %[E]this firmware sets the nested-pointer bit without "
                "the fixed-buffer bit it depends on%[D] — the specification "
                "requires firmware setting the one to set the other, so the "
                "declaration contradicts itself and cannot be taken at face "
                "value\n");
}
