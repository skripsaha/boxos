#!/usr/bin/env bash
# tools/check_endbr64.sh
#
# Post-link audit: verify every globally-visible function in the kernel
# ELF begins with ENDBR64 (F3 0F 1E FA). Required for IBT under
# CR4.CET=1 + IA32_S_CET.ENDBR_EN=1 (Intel Tiger Lake+, AMD Zen 4+).
#
# Without ENDBR64 at the start of an indirect-call target, the CPU raises
# #CP with error_code.endbranch_target=1. On real silicon any function-
# pointer dispatch, IDT-vectored handler, or SYSCALL landing pad without
# the marker would trigger a non-recoverable kernel fault.
#
# The script exempts a small allowlist of symbols that are NEVER reached
# through an indirect branch: pre-CET boot code, direct-jmp local labels,
# GCC's own isra/constprop/part/cold clones, and asm data sections.
#
# Spec refs:
#   Intel SDM Vol 3D §17.3.3 / §17.3.4 — operations that set
#   WAIT_FOR_ENDBRANCH and the ENDBR64 contract.
#
# Usage:
#   tools/check_endbr64.sh [path/to/kernel.elf]
#
# Exit 0 if all required functions start with ENDBR64; exit 1 if any
# violations are found.

set -euo pipefail

KERNEL_ELF=${1:-build/kernel.elf}
OBJDUMP=${OBJDUMP:-objdump}

if [[ ! -f "$KERNEL_ELF" ]]; then
    printf '%s: %s not found — run `make` first\n' "$0" "$KERNEL_ELF" >&2
    exit 1
fi

# ─── Allowlist ─────────────────────────────────────────────────────────
# These symbols are NOT expected to begin with ENDBR64:
#
#  1. Boot / pre-CET path: _start, kernel_entry helpers (write_port,
#     read_port, etc.), AP trampoline. CR4.CET=0 throughout this window
#     so IBT does not enforce. ENDBR64 was added to most of these as
#     defence-in-depth, but anything still missing it is exempted here.
#  2. Direct-jmp local labels: isr_common / paranoid_isr_common are
#     reached via a `jmp` from ENDBR'd ISR stubs (relative branch, not
#     an indirect transfer).
#  3. GCC internal clones: ".isra.0", ".constprop.0", ".part.0",
#     ".cold". These are interprocedural optimization variants — they
#     are never the target of an indirect call (GCC -fcf-protection
#     deliberately omits ENDBR for them).
#  4. LA57 dance asm internals (.low32, .low_64_trampoline, .high64,
#     .reload_cs, .bail) execute with CR4.CET=0 during paging-mode flip.
#  5. ASM data labels (vmm_la57_dance_gdt etc.) — not code.
#  6. TagBoot UEFI bootloader symbols — runs before kernel CR4.CET=1.
#  7. NASM-generated local labels containing "." (sub-labels like
#     "_start.past_header" or "load_gdt.reload_cs") — these are
#     internal jump targets, never independent functions.
#  8. NASM macro-generated local labels (the "..@N.skip" shape used by
#     %%skip and %if expansions in context_switch.asm macros).
#  9. Static C helpers GCC can prove are never indirect targets
#     (whole-program isolated) — IBT-safe by analysis. We trust GCC's
#     -fcf-protection=full output for C; this script audits the
#     boundary between asm and C only.
SKIP_RE='^(_start|stage[12]_.*|ap_trampoline_(start|end|data)|ap_protected|ap_long_mode|ap_gdt|ap_gdt_ptr|isr_common|paranoid_isr_common|low_64_trampoline|vmm_la57_(dance_gdt|dance_gdtr|saved_gdtr|saved_idtr|jmp32|jmp64).*|.*\.(isra|constprop|part|cold)(\.[0-9]+)?.*|__bss_(start|end)|_kernel_(phys_)?(start|end)|_kernel_start|_text_(start|end)|gdt_(start|end|pointer|null|kernel_code|kernel_data|user_code|user_data|tss)|__init_array_(start|end)|__fini_array_(start|end)|TagBootJump)$'

# nm-derived function symbols (lowercase t = local FUNC, uppercase T =
# global FUNC). Extract address + name, skip local labels and macro-
# generated noise. Then for each, fetch 4 bytes at the address and
# check for F3 0F 1E FA (endbr64).

total=0
violations=0
skipped=0
viol_list=()

# Build address→is_endbr map via objdump once. Endbr64 disassembles to
# the literal mnemonic "endbr64" so we recognise it regardless of how
# many raw-byte columns objdump prints. Other instructions go in with
# their first byte for diagnostic context.
declare -A FUNC_FIRST
while IFS= read -r line; do
    if [[ "$line" =~ ^([0-9a-f]+):[[:space:]]+([0-9a-f].*) ]]; then
        addr="${BASH_REMATCH[1]}"
        # Only the FIRST instruction line per function — skip if we've
        # already seen this address (objdump may show data dumps after
        # a function ends, but with new addresses, so this is just
        # idempotent).
        if [[ -z "${FUNC_FIRST[$addr]:-}" ]]; then
            if [[ "$line" == *"endbr64"* ]]; then
                FUNC_FIRST[$addr]="ENDBR64"
            else
                # Capture the first ~12 chars after the address for
                # diagnostic output.
                FUNC_FIRST[$addr]="${BASH_REMATCH[2]:0:24}"
            fi
        fi
    fi
done < <("$OBJDUMP" -d "$KERNEL_ELF" 2>/dev/null)

while IFS= read -r line; do
    # nm "address type name". Only GLOBAL (T) function symbols are
    # candidate IBT targets across translation units. Local (t)
    # functions are:
    #   - C statics where GCC's -fcf-protection=full handles ENDBR
    #     emission per its own whole-TU address-take analysis. Trust
    #     the compiler.
    #   - ASM local labels (e.g. `print_string_vga` in kernel_entry.asm
    #     when not declared `global`). They are not exported, cannot
    #     be reached from another TU via a function-pointer cast, and
    #     the asm author is expected to know if any local address-take
    #     happens within the same file.
    if [[ "$line" =~ ^([0-9a-f]+)[[:space:]]+T[[:space:]]+(.+)$ ]]; then
        addr="${BASH_REMATCH[1]}"
        sym="${BASH_REMATCH[2]}"

        # Skip NASM-style local labels (parent.label, parent..@label).
        if [[ "$sym" == *"."* ]]; then
            # Exception: GCC's "name.isra.0" / "name.constprop.0" are
            # caught by SKIP_RE — they're not real local labels.
            if [[ "$sym" =~ \.(isra|constprop|part|cold)(\.[0-9]+)? ]]; then
                : # fall through to SKIP_RE handling
            else
                ((skipped++)) || true
                continue
            fi
        fi

        if [[ "$sym" =~ $SKIP_RE ]]; then
            ((skipped++)) || true
            continue
        fi

        ((total++)) || true

        # nm and objdump emit identical 16-digit hex addresses for
        # absolute-load kernels — no normalization needed.
        first="${FUNC_FIRST[$addr]:-}"

        if [[ "$first" == "ENDBR64" ]]; then
            : # OK
        else
            ((violations++)) || true
            viol_list+=("$sym @ 0x$addr: ${first:-<no instructions>}")
        fi
    fi
done < <(nm "$KERNEL_ELF" 2>/dev/null | sort)

printf '[CET audit] checked=%d violations=%d skipped=%d\n' \
    "$total" "$violations" "$skipped" >&2

if (( violations > 0 )); then
    printf '\nFunctions missing ENDBR64:\n' >&2
    for s in "${viol_list[@]}"; do
        printf '  %s\n' "$s" >&2
    done
    printf '\nAdd `endbr64` as the first instruction in each asm function\n' >&2
    printf 'above, or extend SKIP_RE in %s with a justified entry.\n' "$0" >&2
    exit 1
fi

printf '[CET audit] OK — all %d functions start with ENDBR64\n' "$total" >&2
exit 0
