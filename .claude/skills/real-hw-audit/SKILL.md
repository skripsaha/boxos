---
name: real-hw-audit
description: Use whenever the user asks to audit, verify, harden, or production-ize any BoxOS subsystem for real x86_64 hardware (BIOS or UEFI). Triggers on phrases like "проверь real HW", "audit real HW", "сделай готовым к железу", "подойдёт ли для real PC", "real hardware readiness". Enforces a strict 7-step workflow (sequential-thinking → ask → deep analysis → external docs → re-analysis → full implementation → verify → run) and contains per-subsystem real-HW checklists.
---

# Real-HW Audit (BoxOS)

## Mission

BoxOS must ship as a real OS for real x86_64 machines, both BIOS/CSM legacy
and UEFI. QEMU is a development surface, never the spec. This skill enforces
the workflow that prevents "works on QEMU, dies on hardware" failures.

## The non-negotiable seven steps

You must execute these in order. Never skip. Never collapse two steps into
one. Never defer a step "to later".

### Step 0 — sequential-thinking (always)

Open with `mcp__sequential-thinking__sequentialthinking`. Lay out:
- the targeted subsystem,
- the real-HW failure modes that worry you,
- the spec sections you expect to consult,
- the success criteria for this session.

If the user did not name a subsystem yet, this step ends in an
`AskUserQuestion` (see Step 1).

### Step 1 — Ask which subsystem

Use `AskUserQuestion` with a curated short list. One subsystem per session.
Do not try to audit everything in one pass.

Subsystem menu (pick 3-4 to offer based on prior work, then "Other"):
- ACPI / FADT / MADT / HPET / MCFG / SRAT / SLIT / DMAR / IVRS
- Local APIC / x2APIC, IOAPIC, MSI / MSI-X
- SMP boot (BSP+AP, INIT-SIPI-SIPI, AP trampoline, microcode update)
- CPU caps & CPUID (feature detection, MTRR, PAT, IA32_EFER, CR0/CR4)
- Timers (TSC invariant, HPET, LAPIC-deadline, PIT, RTC, KVM clock)
- PCI / PCIe (legacy 0xCF8/0xCFC, ECAM from MCFG, BAR sizing, caps, ASPM)
- AHCI / SATA / NVMe (queues, MSI-X, BAR mapping, port reset)
- USB (xHCI, legacy BIOS handoff, port routing, HID boot)
- Storage stack (block layer above driver — alignment, flush, FUA)
- Video (UEFI GOP, BIOS VBE/EDID, multiboot framebuffer, MTRR for fb)
- UEFI (memory map, ExitBootServices, SetVirtualAddressMap, runtime svc)
- IOMMU (Intel VT-d / AMD-Vi) — for DMA isolation and remap
- NUMA (SRAT/SLIT, per-node allocators, affinity)
- IRQ infrastructure (IDT gates, IST stacks, PIC remap, IOAPIC routing,
  IRQ stalling, spurious vectors)
- Power / shutdown (ACPI S5, reset register, EFI ResetSystem, triple fault)
- RNG (RDRAND/RDSEED feature gating, retry, TRNG mixing)
- Keyboard / mouse (PS/2 0x60/0x64, scancode set, controller init, USB HID)
- Serial (16550A detection, baud, FIFO, MSR — used as debug spine)
- Boot path (stage1/stage2 BIOS, TagBoot UEFI, E820/EFI mmap parsing)

### Step 2 — Deep code analysis

Read **every file** related to the subsystem. Not diffs. Not summaries. Full
files. Use Grep across `src/` to find every caller, every register touch,
every IRQ vector, every spinlock involved. Map:

- init order (`main.c` and the chain that brings the subsystem up)
- data structures + ownership
- lock graph (note any nesting — feed into the lock-ordering rule)
- IRQ context boundaries (what runs in interrupt context vs. thread)
- teardown / shutdown path (does the subsystem clean up?)
- error paths (what happens when each step fails?)
- hardcoded values (addresses, delays, vector numbers, BAR offsets)

Don't summarize prematurely. Take notes per file. Then compose the picture.

### Step 3 — Authoritative external documentation

Fetch real specifications. Cite **section/paragraph numbers** in your report.
Required references depending on subsystem:

- Intel SDM Volume 3 (System Programming) for APIC, paging, IRQ, MSR, MTRR
- Intel SDM Volume 4 (Model-Specific Registers)
- AMD APM Volume 2 (System Programming)
- ACPI Specification (current rev — verify what's current at audit time)
- UEFI Specification + Platform Init Spec
- PCI Local Bus 3.0 + PCI Express Base + the ECN you rely on
- xHCI Specification 1.2
- AHCI 1.3.1
- NVMe Base + Command Set
- Intel VT-d Spec / AMD I/O Virtualization Tech (Vol 2)
- IA32 / Intel 64 Errata documents for target CPUs
- OSDev Wiki — only as a starting pointer; always cross-reference with the
  primary spec
- Vendor microcode update docs when SMP/microcode is in scope

Use `WebFetch` or `WebSearch` (or context7 for SDK / library docs). Save the
exact URL + section in your notes — these become the citations in code
comments where invariants matter.

### Step 4 — Re-analysis: current vs. required (delta report)

Produce a table or bulleted delta:

| Aspect | Current code (file:line) | Spec requirement | Severity |
|---|---|---|---|
| … | … | … | MUST / SHOULD / NICE |

Severity rubric:
- **MUST-FIX** — will break / corrupt / hang on at least one class of real
  PC (legacy chipset, modern UEFI box, AMD vs Intel, BIOS vs UEFI, etc.).
- **SHOULD-FIX** — works in QEMU but fragile, against spec, or relies on
  undefined behavior. Will bite on field hardware variance.
- **NICE-TO-HAVE** — quality, observability, or future-proofing.

Then audit **hardcode → dynamics**. Every constant must justify itself:
- Frozen PC/AT spec (0xCF8/0xCFC, 0x1F0, 0x60/0x64, 0x3F8) — OK to hardcode.
- Anything else (PCI BAR, APIC base, HPET base, IRQ count, MMIO size,
  vector assignments, timing delays) — must come from detection (CPUID,
  ACPI table, PCI config, fwcfg, capability probe).

If real HW requires a feature BoxOS does not have, **add it to the
delta as MUST-FIX even if the user didn't ask**. The user has explicitly
authorized this.

### Step 5 — Implement, fully and in stages

Now and only now: code. Rules:

- **C, project style** — CamelCase types/funcs, snake_case locals.
  See `.claude/skills/c-style/SKILL.md`.
- **No stubs, no TODOs, no half-measures**. Production-ready or don't ship.
- **No `#ifdef QEMU`** paths. No emulator-specific shortcuts.
- **Replace hardcode with detection**. Every magic number that lost its
  spec justification gets a `Detect…()` helper.
- **Cite the spec** in comments where invariants live. "Intel SDM Vol 3
  §10.5.1: SVR bit 8 must be set before writing ICR" — that's the bar.
- **Timeouts everywhere**. Every `while (! ready) {}` becomes a bounded
  loop with `pit_uptime_us()` or `tsc_now()` watchdog and a real error
  return.
- **Barriers / MMIO ordering**. `mb()`, `wmb()`, `rmb()` where the spec
  demands. Use `volatile` for MMIO pointers.
- **Cacheability**. MMIO regions must end up UC or WC per spec; verify
  MTRR/PAT setup matches.
- **AMP/SMP safety**. Anything touched in IRQ + thread context: atomic
  or guarded by interrupt-safe spinlock with documented ordering. Honor
  the project's lock-ordering rule from CLAUDE.md.
- **Both boot paths**. If the change affects boot or runtime tables (ACPI,
  framebuffer, memory map), verify BIOS path **and** UEFI path.
- **Errata workarounds** when the targeted CPU/chipset families need them.
  Family/model/stepping detection via CPUID leaf 1.
- **Stage commits**. Logical units. Each commit builds + boots.

### Step 6 — Self-verify

After implementing, before running:

- Re-read every changed file. Look for:
  - dropped error returns
  - missing barriers
  - lock held across sleep/yield
  - assumption that "always succeeds"
  - register write before required setup bit
  - cleanup omitted on error path
  - hardcode that crept back in
- Check the lock graph hasn't introduced a cycle.
- Check IRQ handlers haven't gained a sleeping/blocking call.
- Check CLAUDE.md rules and the per-subsystem rules in this file.

### Step 7 — Build and run

```
make clean && make && make run
```

Then SMP smoke:

```
make run CORES=4 MEM=4G
```

If the change touches storage or boot:

```
make run BOOT=uefi   # if applicable
```

If a regression appears in unrelated areas (keyboard, TagFS, VMM —
historical landmines per CLAUDE.md), stop and investigate before
declaring success. Document what passed in your end-of-session report
and update the right memory file.

## Per-subsystem real-HW checklists

These are the spec-grounded checklists. Tailor depth to the subsystem the
user picked. **Cite the spec** when you apply them.

### ACPI

- RSDP search: EBDA (first 1 KB) + 0xE0000-0xFFFFF; for UEFI use the
  ACPI 2.0 / ACPI 1.0 GUID config tables — do **not** scan low memory
  on UEFI.
- Validate RSDP rev + checksum (V1 = first 20 bytes, V2 = full length).
- Walk XSDT on V2, fall back to RSDT on V1. Never trust a length without
  bounds-check.
- Validate every table: signature, length, checksum (sum mod 256 == 0).
- FADT: read SCI_INT, SMI_CMD, ACPI_ENABLE/DISABLE, PM1a/b control,
  RESET_REG (with RESET_VALUE), preferred PM profile.
- MADT: enumerate Local APIC entries (flag bit 0 = enabled OR online-
  capable), IOAPIC entries, Interrupt Source Overrides, NMI sources, LAPIC
  NMI. Don't assume IRQ→GSI is identity — apply ISO entries.
- HPET via HPET table — BAR, comparator count, counter size.
- MCFG (ECAM) — per-segment base address, start/end bus. Use ECAM if
  present, fall back to 0xCF8/0xCFC.
- SRAT/SLIT for NUMA topology.
- DMAR (Intel) / IVRS (AMD) for IOMMU.
- AML: at minimum, parse OEM static tables we need (FADT-mandated). For
  full PM, an interpreter is required eventually — call out as roadmap.

### Local APIC / x2APIC

- Detect via CPUID.01H:ECX[21] (x2APIC) and EDX[9] (xAPIC).
- Enable: set IA32_APIC_BASE.EN (bit 11). For x2APIC, also bit 10.
- xAPIC: MMIO via APIC base (24-bit ID). x2APIC: MSR (0x800-0x83F).
- SVR (offset 0xF0): set spurious vector + bit 8 enable.
- Mask LVT entries before reconfiguring.
- ICR write path: xAPIC writes ICR_HI then ICR_LO; x2APIC writes the
  combined MSR 0x830 (not 0x831 — that's reserved). See memory
  `x2apic_msi_fixes_2026_05_09.md`.
- EOI: write 0 to LVT EOI register; in x2APIC use MSR 0x80B.
- For SMP startup: INIT (vector field ignored) → 10 ms wait → SIPI →
  200 µs wait → SIPI again. AP trampoline must be in <1 MB, page-aligned
  for the 8-bit vector field.
- LAPIC timer modes: one-shot, periodic, **TSC-deadline** if
  CPUID.01H:ECX[24] — prefer TSC-deadline on modern HW.

### IOAPIC / MSI / MSI-X

- One IOAPIC per board commonly; multiple on server. Enumerate via MADT.
- Redirection table entries: vector, delivery mode, dest, polarity, level.
- Apply Interrupt Source Overrides from MADT before programming.
- MSI capability (PCI cap ID 0x05): Message Address = 0xFEE_xxxxx with
  destination ID in bits 19:12 (xAPIC) or full 32-bit dest (x2APIC).
  Message Data carries vector + delivery mode.
- MSI-X (cap 0x11): table in a BAR, mask bits per entry, PBA table.
- After programming MSI/MSI-X, **clear the function's INTx** (PCI cmd
  reg bit 10 = 1).

### MTRR / PAT

- Detect MTRR via CPUID.01H:EDX[12]. Read IA32_MTRRCAP.
- Variable MTRRs: count, types (UC, WC, WT, WP, WB). Cover MMIO with UC
  or WC as appropriate.
- Fixed MTRRs cover legacy 0-1 MB ranges.
- PAT (CPUID.01H:EDX[16]) — re-program IA32_PAT to add WC for FB, etc.
- Sequence: disable cache (CR0.CD=1, CWB), flush TLB, write MTRRs, enable.

### CPU detection / CPUID

- Always test the leaf before reading it (CPUID(0) max basic, CPUID(0x80000000) max extended).
- Vendor string from CPUID(0).
- Family/Model/Stepping from CPUID(1).EAX; apply the extended family/model
  formula (effective family = base family + extended when base == 0xF; etc.).
- Feature flags: CPUID(1).EDX/ECX, CPUID(7,0).EBX/ECX/EDX, CPUID(0x80000001).
- TSC invariant: CPUID(0x80000007).EDX[8].
- RDRAND CPUID(1).ECX[30]; RDSEED CPUID(7,0).EBX[18].
- Apply microcode updates for the BSP **before** AP boot; reapply per-AP.
- Honor errata: family/model/stepping → workaround table.

### SMP boot

- Parse MADT for AP LAPIC IDs.
- Allocate per-CPU stacks/GS bases.
- Trampoline at < 1 MB, page-aligned.
- INIT-SIPI-SIPI per spec (above).
- AP enables paging, loads GDT/IDT, sets MSRs (EFER.LME, EFER.NXE, CR4
  bits), then jumps to high-half kernel.
- Synchronization: BSP waits on per-AP "alive" flag with timeout. On
  timeout, log and mark AP dead — don't hang the system.
- Microcode update on AP before enabling local features.

### Timers

- Calibrate: HPET (preferred) → PM Timer → PIT (last resort).
- TSC frequency from CPUID(0x15)/CPUID(0x16) when present; else
  calibrate against HPET/PM.
- PIT init: mode 3 (square wave) for 0x40-0x43; channel 0 to IRQ 0
  via legacy path.
- HPET: enable counter, set comparators; periodic vs one-shot. Use
  64-bit main counter when supported (HPET_CAP bit 13).
- LAPIC TSC-deadline: program MSR_IA32_TSC_DEADLINE. Beats one-shot on
  jitter.

### PCI / PCIe

- ECAM via MCFG when available; legacy 0xCF8/0xCFC otherwise (and only
  for buses 0-255, function 0-7, dev 0-31).
- Bus enumeration: header type 1 (bridge) recurses; secondary/sub bus.
- BAR sizing: write all-1s, read back, mask, compute size. Be careful
  with 64-bit BARs (low half indicates 64-bit memory).
- Capability list (status reg bit 4 → cap pointer at 0x34).
- Express cap (0x10) gives link width/speed; ASPM control if needed.
- AER (advanced error reporting) on PCIe — set up to log fatal/non-fatal.

### AHCI / SATA / NVMe

- AHCI: take ownership from BIOS via BOH handshake (HBA cap bit 0).
- Reset HBA (GHC.HR=1), wait for clear, set GHC.AE.
- Per-port: stop FRE/ST, allocate CL/FB, set CLB/FB, start.
- Command issue, await CI clear, parse PxIS for errors. Always timeout.
- MSI/MSI-X over INTx where supported.
- NVMe: identify controller, identify namespace, create IO queues
  according to CAP.MQES, doorbell stride (CAP.DSTRD).
- Pay attention to MPSMIN/MPSMAX vs system page size; alignment of PRP
  entries.

### USB / xHCI

- BIOS handoff: read xECP for "USB Legacy Support" cap, set OS-owned bit,
  spin for BIOS-owned clear (with timeout).
- Reset controller (USBCMD.HCRST), wait CNR clear.
- Configure DCBAA, command ring, event ring with ERSTSZ/ERST entries.
- Doorbell ringing per-slot.
- Detect port-routing for USB2/USB3 super-speed split.
- HID boot protocol for keyboard/mouse before USBHID class drivers.

### Video (GOP / VBE / FB)

- UEFI: GOP gives FB base, pitch, format. Save before ExitBootServices.
- BIOS: VBE 2.0 / 3.0, parse EDID for modes. Multiboot framebuffer
  tag if loaded under multiboot.
- Map FB as WC (PAT) for performance; never UC (huge perf hit) and
  never WB (gets corrupted writes).
- Pitch ≠ width × bpp in general — always use pitch.
- See memory `vga_fb_fixes.md` for the shadow-buffer pattern.

### UEFI

- After ExitBootServices, no boot services allowed.
- Runtime services need SetVirtualAddressMap to be remapped if your
  kernel isn't identity-mapped where firmware put them.
- Preserve ACPI tables (V2 GUID), SMBIOS, memory map.
- Beware of NVRAM size limits when storing variables.
- Always exit with the **latest** memory map key.

### IOMMU (VT-d / AMD-Vi)

- Detect via DMAR (Intel) or IVRS (AMD).
- Map drhd structures (Intel): per-segment, register base, included
  scope.
- For DMA isolation: build IOMMU page tables, program context entries,
  enable translation.
- Required for safety even on bare-metal once we add untrusted PCIe
  devices (Thunderbolt, etc.).

### IRQ infrastructure

- IDT: 256 gates. Set DPL=3 only for syscall vectors and INT 3 (debug).
- IST for double fault, NMI, MCE, page fault on faulty stack.
- 8259 PIC remap (master 0x20, slave 0x28) even if you mask it — must
  not collide with CPU exceptions.
- IOAPIC vector range 0x30-0xEF typically.
- 0xFF reserved as spurious; set in SVR.
- MSI vectors must be **edge-triggered** in IDT type.

### Power / shutdown

- ACPI S5: parse _S5 from DSDT (AML) or static fallback; write SLP_TYPx
  + SLP_EN to PM1a/PM1b control.
- Reset register from FADT.
- UEFI ResetSystem at runtime.
- Triple fault as last resort.

### RNG

- Detect RDRAND/RDSEED before use.
- Both have explicit failure (CF=0 → no entropy this iteration); spec
  says retry up to 10 times then fall back.
- Mix with other entropy sources (HPET jitter, TSC at IRQ, MAC, etc.).

### Keyboard / mouse

- PS/2: controller commands via 0x64, data 0x60. Wait OBF/IBF with
  timeouts. Drain output buffer before init.
- Translate scancode set 2 → set 1 if controller translates; else
  decode set 2 directly.
- USB HID boot keyboard handed off after xHCI is up.

### Serial

- 16550A detect: write 0xE7 to FCR, read back IIR — FIFO bits indicate
  16550A/16550AF/16750.
- Set divisor latch, baud, line/control regs.
- Don't trust FCR if older 8250 — works without FIFO.

## Output template

When you finish a session, produce this report:

### Subsystem audited
- Name + scope.

### Files read
- List with line counts.

### Spec references consulted
- Section + URL.

### Delta (current vs spec)
- MUST-FIX list
- SHOULD-FIX list
- NICE-TO-HAVE list

### Changes implemented
- Per-commit summary.

### Verification
- Build result, run results (single-core + SMP), regressions checked.

### Outstanding
- What was not fixed and why (with severity).

### Memory updates
- Files added / edited under
  `/Users/skripsaha/.claude/projects/-Users-skripsaha-Projects-boxos/memory/`.

## Rules layered on top

- Always start with `mcp__sequential-thinking__sequentialthinking`.
- Always end with build + run.
- Never skip a step. Never collapse two. Never defer.
- Never insert `#ifdef QEMU`. Never special-case the emulator.
- Always cite the spec where the invariant lives.
- Hardcode requires a frozen-spec citation, or it gets replaced with
  detection. No exceptions.
- The user has authorized you to add missing capabilities the real HW
  requires, even when not asked. Use that authority.
- Update memory at session end so the next session inherits context.
