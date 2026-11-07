# shellcheck shell=bash
# Sourced by tools/logcheck.sh: a K-Core's panic beside a BSP that stands with
# its interrupts off.

# ---------------------------------------------------------------------------
# panicstop — the panic stops every other core, whatever its interrupt flag
# ---------------------------------------------------------------------------
#
# The startup tests run on the BSP with interrupts off — everything kernel_main
# does comes before the sti that opens it (main.c) — and a panic stopped the
# other cores by a fixed vector, which a core with interrupts off holds in its
# LAPIC for as long as it keeps them off. Measured on 045e7b5e: a STRICT kernel
# whose K-Core panicked in the self-check during stand_escheat_remounted went
# on on its BSP, failed that test at its guard, entered the next and said
# nothing for sixty seconds — the account of the panic sunk behind a core that
# never stopped (quill's used book, boot 2).
#
# The kernel built PANICSTOP=panic|fault|nested puts that case on purpose
# (panic_stop_proof.c): the BSP takes the console lock and runs on with
# interrupts off, saying nothing; the first K-Core beside it waits until the
# BSP holds the console and calls panic() — or executes ud2, and the exception
# dump is the account, or calls panic() with a string that faults inside the
# first line of its account. Three more legs break panic()'s account inside
# it by a logcheck injection (panicstop_inject): a panic() said after its
# message, the same panic() with its own message the next bug, and the glass's
# own road faulting under the account; and a fourth has the panic stop a core
# inside its #MC handler and puts a second #MC on that core once every core
# stands (QEMU's monitor, mce); a fifth puts a #MC on the owner of the panic
# itself once its account is said whole and every core stands — the account
# no less whole for what came after it — a sixth the same after the dump, and
# a seventh while the owner stands, by an injection, inside the drain at its
# account's end: the account said whole, and on a wire gone over to its
# interrupt not yet out. Four cores, so that a K-Core other than the BSP exists
# (amp_calculate_kcores: log2 of the cores). STRICT, the BIOS legs panic() and
# the breaks inside it and the UEFI legs the dump: the accounts share the one
# claim and the one stop, and either firmware hands the kernel the same LAPIC,
# so each account is put once and each firmware once.
#
# The BSP says nothing after it holds the console, and that is the case and not
# a convenience: a kprintf begun after a panic is claimed stands its own core
# (klib_print.c), so a core that speaks is stopped by speaking whatever the
# panic sent it, and a judge of lines would hold on a panic that never reached
# the BSP — it did, on the first form of this test. So the machine is looked
# at, once a second from the account's first words, by QEMU's monitor (info
# registers -a), and judged by whichever fact comes first: every core halted
# with RFLAGS.IF clear — a core in `hlt` with interrupts off is woken by nothing
# an interrupt controller sends but an NMI, and every core is in one, so the
# machine stands for good — or a core seen running in two looks a second apart,
# its registers changed between them, a second after the panic was said.
#
# A machine that stood is then held to what its account must be (see
# panicstop_account): on the glass where the glass is text (the BIOS legs —
# the screen is what an operator photographs); the dump's count of
# exceptions the one it tells; and a fault or a panic inside the account no
# second account but one line that says the account broke off and by what —
# on the wire whatever else broke, and on the glass where the glass still
# stands; an exception on the owner after its whole account no break of it,
# but one line that says what came after it.
# panicstopmut puts ten mutants of the stop, each turning a leg it weighs.
# ---------------------------------------------------------------------------

PANICSTOP_CORES=4
PANICSTOP_HELD='^\[PANICSTOP\] the BSP takes the console'
PANICSTOP_PANIC='KERNEL PANIC'
# The line the nested case breaks off in, up to the string that faults.
PANICSTOP_NESTED='KERNEL PANIC: core [0-9]+ faults inside its own account:'
# The one line that says it broke off, and by what: the #GP of the string,
# its error code, and a RIP in the kernel's half.
PANICSTOP_BROKE='Account broken off by vector 13, error 0x[0-9a-f]+, RIP 0xffff[0-9a-f]{12}'
PANICSTOP_UNPUT='^\[PANICSTOP\] .* the case is not put'
# panic()'s account as PANICSTOP=panic says it, up to its message's last word.
PANICSTOP_SAID='KERNEL PANIC: core [0-9]+ dies while the BSP holds the console with interrupts off'
# The dump's account as PANICSTOP=fault says it: its banner, and its last line.
PANICSTOP_DUMPED='KERNEL PANIC: #UD \(vector 6\)'
PANICSTOP_HALTED='^System halted\.$'
# Any line that says an account broke off: one where it did, none where not.
PANICSTOP_ANYBREAK='Account broken off by '
# The lines the injected breaks are said by (panicstop_inject).
PANICSTOP_BROKE_PANIC='Account broken off by panic: a panic inside the account of a panic'
PANICSTOP_BROKE_WILD='Account broken off by panic: a panic inside the account, its message the next bug: '
PANICSTOP_BROKE_GLASS='Account broken off by vector 14, error 0x0, RIP 0xffff[0-9a-f]{12}, CR2 0x10$'
# The line a #MC on the owner after its whole account is said by: no break —
# the account above it is whole — and the #MC's vector, its error code (a #MC
# pushes none) and the RIP of the owner's last hlt.
PANICSTOP_AFTER='After the account: vector 18, error 0x0, RIP 0xffff[0-9a-f]{12}$'
# Any line that says what came after a whole account: one where something
# came, none where not.
PANICSTOP_ANYAFTER='After the account: '

# panicstop_inject HOW — panic()'s account broken inside it, on a kernel built
# PANICSTOP=panic, by a logcheck injection; the file in PANICSTOP_INJECTED:
#   - panicin: a panic() said right after the account's message — a panic
#     inside the account, whose message is the line that says by what;
#   - panicwild: the same panic() with its own message the next bug, a string
#     at a non-canonical address — the line that says by what faults inside
#     itself, the third coming, which stands at once;
#   - glass: the glass's own road faulting under the account from its first
#     colon on — a read at 0x10 on the owner of the panic as the Emergency
#     Screen draws a byte it follows out of the ring, from the ':' of "KERNEL
#     PANIC:" onwards: a #PF, no page there since vmm_init took the identity
#     map down — a framebuffer the bug has broken mid-account, the account's
#     first words on the glass, its line on the wire whatever the glass does,
#     its CR2 with it. Only bytes followed out of the ring: the claim redraws
#     the ring's tail on the owner before the account begins (DrawTail), and a
#     fault there would come before the account's first word;
#   - mcip: the first #MC a core takes stays in its handler, MCG_STATUS.MCIP
#     set, spinning at mce_handle's first line, and the K-Core's panic waits
#     for it — so the NMI that stops that core stops it inside its #MC
#     handler, as it can stop one in the middle of a walk of the banks;
#   - draining: the owner of the panic stands, in a hlt with interrupts off,
#     inside the first drain of the wire it makes (serial.c, WireDrain, the
#     wire's lock in its hand) — the one at its account's end, no line of the
#     account left to say: where a wire gone over to its interrupt holds what
#     the transmitter has not taken of the account, for as long as the drain
#     takes to send it at 115200 baud. Here the wire is pushed by hand and the
#     drain has nothing left to send; the stand holds the owner inside it
#     until the #MC comes.
# PANICSTOP_INJECTED the files, put back by panicstop_case.
panicstop_inject() {
    local f
    case $1 in
        glass) PANICSTOP_INJECTED=src/kernel/drivers/video/emergency_screen.c ;;
        draining) PANICSTOP_INJECTED=src/kernel/drivers/serial/serial.c ;;
        mcip)  PANICSTOP_INJECTED="src/kernel/core/mce/mce.c src/kernel/core/system_halt/panic_stop_proof.c" ;;
        *)     PANICSTOP_INJECTED=src/lib/kernel/klib_print.c ;;
    esac
    for f in $PANICSTOP_INJECTED; do mutate "$f"; done
    PANICSTOP_HOW=$1 python3 - <<'EOF'
import os
how = os.environ["PANICSTOP_HOW"]
if how == "mcip":
    edits = [
        ("src/kernel/core/mce/mce.c",
         "bool mce_handle(interrupt_frame_t *frame, bool *panic_claimed_here) {\n",
         "    {   /* logcheck injection: the first #MC stays in its handler, MCIP set */\n"
         "        extern volatile uint32_t g_panicstop_mc_inside;\n"
         "        static uint32_t once;\n"
         "        if (!__atomic_exchange_n(&once, 1u, __ATOMIC_ACQ_REL)) {\n"
         "            __atomic_store_n(&g_panicstop_mc_inside, 1u, __ATOMIC_RELEASE);\n"
         "            for (;;) __asm__ volatile(\"pause\");\n"
         "        }\n"
         "    }\n"),
        ("src/kernel/core/system_halt/panic_stop_proof.c",
         "static volatile uint32_t s_console_held;\n",
         "extern volatile uint32_t g_panicstop_mc_inside;\n"
         "volatile uint32_t g_panicstop_mc_inside;   /* logcheck injection: a core inside its #MC handler */\n"),
        ("src/kernel/core/system_halt/panic_stop_proof.c",
         "    while (!__atomic_load_n(&s_console_held, __ATOMIC_ACQUIRE))\n        __asm__ volatile(\"pause\");\n",
         "    while (!__atomic_load_n(&g_panicstop_mc_inside, __ATOMIC_ACQUIRE))   /* logcheck injection */\n"
         "        __asm__ volatile(\"pause\");\n"),
    ]
    for p, a, b in edits:
        s = open(p).read()
        assert s.count(a) == 1, "panicstop injection anchor missing in " + p
        open(p, "w").write(s.replace(a, a + b, 1))
    raise SystemExit(0)
if how == "draining":
    p = "src/kernel/drivers/serial/serial.c"
    a = "void WireDrain(void)\n{\n    if (!g_wire_present) return;\n    spin_lock(&g_serial_lock);\n"
    b = a + ("    {   /* logcheck injection: the owner of a panic stands inside the drain at its account's end */\n"
             "        extern bool PanicOwnedHere(void);\n"
             "        static bool stood;\n"
             "        if (PanicOwnedHere() && !stood) { stood = true; for (;;) __asm__ volatile(\"cli; hlt\"); }\n"
             "    }\n")
elif how == "glass":
    p = "src/kernel/drivers/video/emergency_screen.c"
    a = "        uint64_t p = g_glass.drawn++;\n        PenByte(&g_glass.pen, (uint8_t)LogRingByteAt(p), LogRingToneAt(p), true);\n"
    b = ("        {   /* logcheck injection: the glass breaks under the panic's account */\n"
         "            extern bool PanicOwnedHere(void);\n"
         "            static bool broken;\n"
         "            if (PanicOwnedHere() && (broken || LogRingByteAt(p) == ':')) { const volatile char *at = (const volatile char *)0x10; __asm__ volatile(\"\" : \"+r\"(at)); broken = true; (void)*at; }\n"
         "        }\n")
    b = a.replace("        PenByte(", b + "        PenByte(", 1)
else:
    p = "src/lib/kernel/klib_print.c"
    a = '    kprintf("\\n%[E]KERNEL PANIC:%[D] ");\n    kvprintf(message, args);\n'
    inner = ('panic("a panic inside the account of a panic");' if how == "panicin" else
             'panic("a panic inside the account, its message the next bug: %s", (const char *)0x8000000000000000ull);')
    b = a + "    " + inner + "   /* logcheck injection: a panic inside the account */\n"
s = open(p).read()
assert s.count(a) == 1, "panicstop injection anchor missing in " + p
open(p, "w").write(s.replace(a, b, 1))
EOF
    for f in $PANICSTOP_INJECTED; do
        case $f in
            *panic_stop_proof.c) mut_done "$f" "logcheck injection" 2 ;;
            *)                   mut_done "$f" "logcheck injection" 1 ;;
        esac
    done
}

# panicstop_mc — a #MC put by the monitor on the core PANICSTOP_MC_CPU — bank 1,
# VAL|UC|EN|PCC, MCG_STATUS MCIP alone, no restart IP: fatal — the monitor's
# answer in PANICSTOP_MCSAID. 1 when the monitor would not answer.
panicstop_mc() {
    PANICSTOP_MCSAID=$(./tools/qemu-input.sh raw "mce $PANICSTOP_MC_CPU 1 0xb200000000000000 0x4 0 0" 2>"$SCRATCH/qemu-input.err" | tr -d '\r') || return 1
}

# panicstop_glass TAG — the BIOS glass as text, 25 rows of 80, into
# $SCRATCH/panicstop.glass.TAG.txt: the VGA text buffer at 0xb8000 read by the
# monitor, a cell's character its even byte (rgbcheck.sh reads the odd ones,
# the attributes, the same way). 1 when the monitor showed no whole screen.
panicstop_glass() {
    local V="$SCRATCH/panicstop.vram.$1.txt" G="$SCRATCH/panicstop.glass.$1.txt"
    rm -f "$G"
    ./tools/qemu-input.sh raw "xp /4000bx 0xb8000" > "$V" 2>>"$SCRATCH/qemu-input.err" || return 1
    python3 -I - "$V" "$G" <<'EOF'
import re, sys
b = []
for ln in open(sys.argv[1], errors="replace"):
    m = re.match(r'^([0-9a-f]{8,}):\s+(.*)$', ln.strip())
    if m:
        b += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(2))]
if len(b) < 4000:
    sys.exit(1)
cells = b[0:4000:2]
with open(sys.argv[2], "w") as g:
    for r in range(25):
        g.write("".join(chr(c) if 32 <= c < 127 else " " for c in cells[r * 80:(r + 1) * 80]).rstrip() + "\n")
EOF
}

# panicstop_look — one look at every core into PANICSTOP_NOW: a line "CPU HLT
# RFL STATE" each, the state its general registers and RIP as the monitor
# printed them. 1 when the monitor would not answer (PANICSTOP_DEAF).
panicstop_look() {
    local out
    if ! out=$(./tools/qemu-input.sh raw "info registers -a" 2>"$SCRATCH/qemu-input.err" | tr -d '\r'); then
        PANICSTOP_DEAF=1
        return 1
    fi
    printf '%s\n' "$out" > "$SCRATCH/panicstop.look"   # the last look, kept for whoever reads a verdict
    PANICSTOP_NOW=$(printf '%s\n' "$out" | LC_ALL=C awk '
        /^CPU#[0-9]+/ { if (cpu != "") print cpu, hlt, rfl, st; cpu = substr($1, 5); hlt = ""; rfl = ""; st = ""; next }
        cpu == "" { next }
        /^(RAX|RSI|R8 |R12)=/ { st = st $0; gsub(/ /, "", st) }
        /^RIP=/ { st = st $1
                  if (match($0, /RFL=[0-9a-fA-F]+/)) rfl = substr($0, RSTART + 4, RLENGTH - 4)
                  if (match($0, /HLT=[01]/)) hlt = substr($0, RSTART + 4, 1) }
        END { if (cpu != "") print cpu, hlt, rfl, st }')
}

# panicstop_seen — the fact the looks have come to, if any: 0 with
# PANICSTOP_VERDICT stood (every core halted with interrupts off) or ran
# (PANICSTOP_RAN names the core); 1 when the look settles nothing yet. The look
# before it is kept in PANICSTOP_BEFORE.
panicstop_seen() {
    local now cpu hlt rfl st n=0 standing=0 was
    panicstop_look || return 1
    now=$PANICSTOP_NOW
    while read -r cpu hlt rfl st; do
        case $rfl in ''|*[!0-9a-fA-F]*) continue ;; esac
        n=$((n + 1))
        if [ "$hlt" = 1 ] && [ $((16#$rfl & 0x200)) = 0 ]; then
            standing=$((standing + 1))
            continue
        fi
        was=$(printf '%s\n' "$PANICSTOP_BEFORE" | awk -v c="$cpu" '$1 == c { print $2, $4 }')
        if [ "$hlt" = 0 ] && [ -n "$was" ] && [ "${was%% *}" = 0 ] && [ "${was#* }" != "$st" ]; then
            PANICSTOP_VERDICT=ran
            PANICSTOP_RAN="core $cpu (RFLAGS $rfl, ${st##*RIP=})"
            return 0
        fi
    done <<<"$now"
    PANICSTOP_LOOKED="$standing of $n"
    PANICSTOP_BEFORE=$now
    if [ "$n" = "$PANICSTOP_CORES" ] && [ "$standing" = "$n" ]; then
        PANICSTOP_VERDICT=stood
        return 0
    fi
    return 1
}

# panicstop_put_back — the files panicstop_inject changed, as they were.
panicstop_put_back() {
    local f
    for f in $PANICSTOP_INJECTED; do restore "$f"; done
    PANICSTOP_INJECTED=
}

# panicstop_case TAG CASE KEYS... — the kernel built PANICSTOP=CASE and KEYS
# booted on PANICSTOP_CORES cores, the log kept as serial.panicstop.TAG.log;
# a CASE panicstop_inject puts is PANICSTOP=panic with that injection, put
# back after the boot. PANICSTOP_CASE how it ended — stood, ran (PANICSTOP_RAN
# the core), fault (PANICSTOP_STOP says where), unput (the kernel never put
# the case: the BSP never held the console, or the proof said why not),
# harness — PANICSTOP_LAST the last line said.
panicstop_case() {
    local tag=$1 kind=$2 L="$SCRATCH/serial.panicstop.$1.log" h0=$HARNESS glass=1 put=$2
    shift 2
    case " $* " in *" UEFI=on "*) glass=0 ;; esac
    PANICSTOP_STOP= PANICSTOP_RAN= PANICSTOP_VERDICT= PANICSTOP_BEFORE= PANICSTOP_NOW= PANICSTOP_LOOKED= PANICSTOP_DEAF=0 PANICSTOP_GLASS= PANICSTOP_INJECTED=
    case $kind in
        panicin|panicwild|glass|mcip|draining) put=panic; panicstop_inject "$kind" ;;
        after) put=panic ;;
        afterdump) put=fault ;;
    esac
    build STRICT=on PANICSTOP="$put"
    make run-stop >/dev/null 2>&1
    rm -f "$L"
    if ! make run-bg CORES=$PANICSTOP_CORES STRICT=on PANICSTOP="$put" "$@" >"$SCRATCH/runbg.panicstop.$tag.log" 2>&1; then
        PANICSTOP_CASE=harness
        PANICSTOP_STOP="no machine was started: $(tail -1 "$SCRATCH/runbg.panicstop.$tag.log")"
        panicstop_put_back
        return
    fi
    PANICSTOP_MCSAID= PANICSTOP_MCSECOND= PANICSTOP_MCAFTER=
    if ! wait_for_line -E build/serial.log "$PANICSTOP_HELD" "$WAIT_QUIET_S"; then
        PANICSTOP_CASE=unput PANICSTOP_STOP="the BSP never said it takes the console"
    elif [ "$kind" = mcip ] && ! panicstop_mc_first; then
        PANICSTOP_CASE=unput PANICSTOP_STOP="the first #MC was not put: ${PANICSTOP_MCSAID:-the monitor would not answer}"
    elif ! wait_for_line -E build/serial.log "$PANICSTOP_PANIC" "$WAIT_QUIET_S"; then
        PANICSTOP_CASE=fault PANICSTOP_STOP="the K-Core's account never reached the wire (/$PANICSTOP_PANIC/)"
    elif wait_until "$WAIT_QUIET_S" "every core to stand, or a core to run past the panic" \
                    "panicstop_seen" build/serial.log; then
        PANICSTOP_CASE=$PANICSTOP_VERDICT
    elif [ "$PANICSTOP_DEAF" = 1 ]; then
        PANICSTOP_CASE=harness PANICSTOP_STOP="the monitor would not answer info registers: $(head -1 "$SCRATCH/qemu-input.err")"
    else
        PANICSTOP_CASE=fault
        PANICSTOP_STOP="no core was seen running past the panic, and QEMU never saw every core halted with interrupts off (the last look: ${PANICSTOP_LOOKED:-none} standing)"
    fi
    if [ "$PANICSTOP_CASE" = stood ] && [ "$kind" = mcip ]; then
        panicstop_mc_second
    fi
    if [ "$PANICSTOP_CASE" = stood ]; then
        case $kind in after|afterdump|draining) panicstop_mc_owner ;; esac
    fi
    if [ "$PANICSTOP_CASE" = stood ] && [ $glass = 1 ]; then
        if panicstop_glass "$tag"; then
            PANICSTOP_GLASS="$SCRATCH/panicstop.glass.$tag.txt"
        else
            PANICSTOP_CASE=harness PANICSTOP_STOP="the monitor showed no whole screen at 0xb8000"
        fi
    fi
    if [ $HARNESS -gt $h0 ]; then
        PANICSTOP_CASE=harness PANICSTOP_STOP="the host could not speak to the machine"
    fi
    if grep -aqE "$PANICSTOP_UNPUT" build/serial.log; then
        PANICSTOP_CASE=unput PANICSTOP_STOP=$(grep -aE "$PANICSTOP_UNPUT" build/serial.log | head -1 | tr -d '\r')
    fi
    cp build/serial.log "$L"
    make run-stop >/dev/null 2>&1
    rm -f build/qemu.pid build/qemu.mon build/qemu.qmp
    panicstop_put_back
    PANICSTOP_LAST=$(tr -d '\r' < "$L" | grep -av '^\s*$' | tail -1 | cut -c1-200)
}

# panicstop_dies — the core the proof says is to die: the owner of its panic.
panicstop_dies() {
    tr -d '\r' < build/serial.log | sed -n 's/^\[PANICSTOP\] core \([0-9][0-9]*\) is to .*/\1/p' | head -1
}

# panicstop_mc_first — the core the #MC goes to chosen, and the #MC put on it.
# 1 when it was not: the monitor would not answer, or said why not (a "CPU n:"
# line — reporting disabled, no MCE capability). The core is neither the BSP
# nor the K-Core the proof says is to die: the last, or the one before it when
# that is the K-Core.
panicstop_mc_first() {
    local dies
    dies=$(panicstop_dies)
    PANICSTOP_MC_CPU=$((PANICSTOP_CORES - 1))
    [ "$dies" = "$PANICSTOP_MC_CPU" ] && PANICSTOP_MC_CPU=$((PANICSTOP_MC_CPU - 1))
    panicstop_mc || return 1
    ! printf '%s\n' "$PANICSTOP_MCSAID" | grep -q 'CPU [0-9]*:'
}

# panicstop_mc_second — a second #MC on the same core, once the machine stood,
# the monitor's answer in PANICSTOP_MCSECOND, and how the machine was a
# second or more after it in PANICSTOP_MCAFTER: stood again, ran (a core
# running — a reset that rebooted it), or none.
panicstop_mc_second() {
    if ! panicstop_mc; then
        PANICSTOP_CASE=harness PANICSTOP_STOP="the monitor would not put the second #MC"
        return
    fi
    PANICSTOP_MCSECOND=$PANICSTOP_MCSAID
    PANICSTOP_BEFORE= PANICSTOP_VERDICT= PANICSTOP_RAN=
    sleep 1
    if wait_until "$WAIT_QUIET_S" "every core to stand again past a second #MC" panicstop_seen build/serial.log; then
        PANICSTOP_MCAFTER=$PANICSTOP_VERDICT
    else
        PANICSTOP_MCAFTER=none
    fi
}

# panicstop_mc_owner — a #MC on the owner of the panic, the core the proof says
# is to die, once every core stood: its account said whole, the owner in its
# last hlt — or, put by the draining injection, in a hlt inside the drain at
# its account's end. A "CPU n:" line in the monitor's answer says the #MC was
# not put. How the machine was after it in PANICSTOP_MCAFTER: stood again once
# the owner said a line of it — a line that says what came after the account,
# or one that says it broke off — silent (no such line, the wire quiet), ran,
# or none. Waited for by that line, not by a second's sleep: the owner stands
# only after it.
panicstop_mc_owner() {
    PANICSTOP_MC_CPU=$(panicstop_dies)
    if [ -z "$PANICSTOP_MC_CPU" ] || ! panicstop_mc; then
        PANICSTOP_CASE=harness PANICSTOP_STOP="the monitor would not put a #MC on the owner of the panic"
        return
    fi
    if printf '%s\n' "$PANICSTOP_MCSAID" | grep -q 'CPU [0-9]*:'; then
        PANICSTOP_CASE=unput PANICSTOP_STOP="the #MC on the owner of the panic was not put: $(printf '%s\n' "$PANICSTOP_MCSAID" | grep -m1 'CPU [0-9]*:')"
        return
    fi
    if ! wait_for_line -E build/serial.log "$PANICSTOP_ANYAFTER|$PANICSTOP_ANYBREAK" "$WAIT_QUIET_S"; then
        PANICSTOP_MCAFTER=silent
        return
    fi
    PANICSTOP_BEFORE= PANICSTOP_VERDICT= PANICSTOP_RAN=
    if wait_until "$WAIT_QUIET_S" "every core to stand again past the #MC on the owner" panicstop_seen build/serial.log; then
        PANICSTOP_MCAFTER=$PANICSTOP_VERDICT
    else
        PANICSTOP_MCAFTER=none
    fi
}

# panicstop_counted_one LOG — the dump's count of exceptions the one fault it
# tells (panicstop_account says why); PANICSTOP_WRONG says what it counts
# where it does not.
panicstop_counted_one() {
    local n
    n=$(tr -d '\r' < "$1" | sed -n 's/^Exception count: \([0-9][0-9]*\).*/\1/p' | head -1)
    [ "$n" = 1 ] && return 0
    PANICSTOP_WRONG="the dump counts ${n:-no} exceptions where one was raised — the NMIs that stopped its peers counted as faults"
    return 1
}

# panicstop_after_stood LOG — the #MC panicstop_mc_owner put on the owner said
# by a line on the wire, and every core standing again after it;
# PANICSTOP_WRONG says which is not so.
panicstop_after_stood() {
    if [ "$PANICSTOP_MCAFTER" = silent ]; then
        PANICSTOP_WRONG="a #MC on the owner of the panic, core $PANICSTOP_MC_CPU, after its whole account, and no line on the wire says it came (the last said: $(tr -d '\r' < "$1" | grep -av '^\s*$' | tail -1 | cut -c1-120))"
        return 1
    fi
    if [ "$PANICSTOP_MCAFTER" != stood ]; then
        PANICSTOP_WRONG="past a #MC on the owner of the panic, core $PANICSTOP_MC_CPU, after its whole account, the machine did not stand again (${PANICSTOP_MCAFTER:-none}${PANICSTOP_RAN:+: $PANICSTOP_RAN})"
        return 1
    fi
}

# panicstop_account TAG CASE — what the account of a machine that stood must
# also be. PANICSTOP_WRONG the first fact it is not (empty when it is all of
# them), PANICSTOP_FACTS what it is:
#   - every case: one KERNEL PANIC — a fault or a panic inside the account is
#     no account of its own — a line that says the account broke off only
#     where it did, and then one, and a line that says what came after a whole
#     account only where something did, and then one;
#   - fault: the dump counts the one exception it tells. The NMIs that stop its
#     peers are not faults, and a count that grows with the cores reads as a
#     cascade that never was;
#   - nested: the line the account broke off in on the wire whole, and the line
#     that says by what: vector 13, its error code and the RIP it faulted at, a
#     kernel address — both on the glass too;
#   - panicin: panic()'s message and then "Account broken off by panic:" and
#     the message of the panic inside it, on the wire and the glass;
#   - panicwild: that line on the wire up to the string that faults inside it,
#     and nothing after it — the third coming says nothing;
#   - glass: the line that says by what — the #PF on the glass's road, vector
#     14 and the address it read, CR2 0x10 — on the wire whole, though the
#     glass faults at its first letter;
#   - mcip: a second #MC on the core the panic stopped inside its #MC handler
#     is taken, not a shutdown — QEMU raises a triple fault for a #MC that
#     meets MCIP set ("Previous MCE still in progress"), as the processor
#     enters the shutdown state (SDM Vol 3B, IA32_MCG_STATUS) — and every
#     core stands again after it;
#   - after: a #MC on the owner of the panic once its account is said whole is
#     no break of it — "After the account:" and the #MC's vector 18, its error
#     code and RIP, on the wire after panic()'s message and on the glass — and
#     every core stands again after it;
#   - draining: the same, the #MC taken inside the drain at the account's end;
#   - afterdump: the same after the dump, on the wire after its banner and its
#     last line, the dump counting the one fault it tells;
#   - a BIOS leg: the account's first words are on the glass, though the BSP
#     stopped with the console in its hand.
# Not a weight on what the drain at the account's end sends, nor on the drain
# before and after the line that says by what: the wire is pushed by hand at
# this point of the boot — it goes over to its interrupt once one arrives with
# the UART's own word on it, and none has before the BSP opens interrupts
# (serial.c, serial_com1_irq and WireKick) — so every byte is on the wire as
# it is put in the ring. The draining leg weighs only the time the drain takes:
# what comes in it comes after the account.
panicstop_account() {
    local L="$SCRATCH/serial.panicstop.$1.log" kind=$2 n want breaks=1 afters=0 wire= glass
    glass=$PANICSTOP_PANIC
    PANICSTOP_WRONG= PANICSTOP_FACTS=
    case $kind in
        panic) breaks=0 ;;
        fault)
            breaks=0
            panicstop_counted_one "$L" || return
            PANICSTOP_FACTS="; the dump counts the one fault it tells" ;;
        nested)    wire="$PANICSTOP_NESTED
$PANICSTOP_BROKE" glass=$wire ;;
        panicin)   wire="$PANICSTOP_SAID
$PANICSTOP_BROKE_PANIC" glass=$wire ;;
        panicwild) wire="$PANICSTOP_SAID
$PANICSTOP_BROKE_WILD" ;;
        glass)     wire=$PANICSTOP_BROKE_GLASS ;;
        mcip)
            breaks=0
            if printf '%s\n' "$PANICSTOP_MCSECOND" | grep -q 'Previous MCE still in progress'; then
                PANICSTOP_WRONG="the core the panic stopped inside its #MC handler stood with MCIP set, and a second #MC on it shut the machine down (the monitor: $(printf '%s\n' "$PANICSTOP_MCSECOND" | grep -o 'CPU [0-9]*: Previous MCE still in progress, raising triple fault' | head -1))"
                return
            fi
            if [ "$PANICSTOP_MCAFTER" != stood ]; then
                PANICSTOP_WRONG="past a second #MC on the core the panic stopped inside its #MC handler, the machine did not stand again (${PANICSTOP_MCAFTER:-none}${PANICSTOP_RAN:+: $PANICSTOP_RAN})"
                return
            fi
            PANICSTOP_FACTS="; a second #MC on core $PANICSTOP_MC_CPU, stopped inside its #MC handler, was taken and stood again, no shutdown" ;;
        after|draining)
            breaks=0 afters=1
            panicstop_after_stood "$L" || return
            wire="$PANICSTOP_SAID
$PANICSTOP_AFTER" glass="$PANICSTOP_PANIC
$PANICSTOP_AFTER" ;;
        afterdump)
            breaks=0 afters=1
            panicstop_counted_one "$L" || return
            panicstop_after_stood "$L" || return
            PANICSTOP_FACTS="; the dump counts the one fault it tells"
            wire="$PANICSTOP_DUMPED
$PANICSTOP_HALTED
$PANICSTOP_AFTER" glass=$PANICSTOP_AFTER ;;
    esac
    n=$(grep -ac "$PANICSTOP_PANIC" "$L")
    if [ "$n" != 1 ]; then
        PANICSTOP_WRONG="the account is said $n times where it is one, a fault or a panic inside it said as an account of its own: $(grep -a "$PANICSTOP_PANIC" "$L" | sed -n 2p | tr -d '\r' | cut -c1-120)"
        return
    fi
    n=$(grep -ac "$PANICSTOP_ANYBREAK" "$L")
    if [ "$n" != "$breaks" ]; then
        if [ "$breaks" = 1 ] && [ "$n" = 0 ]; then
            PANICSTOP_WRONG="the account broke off and no line says by what (the last said: $(tr -d '\r' < "$L" | grep -av '^\s*$' | tail -1 | cut -c1-120))"
        else
            PANICSTOP_WRONG="$n lines say the account broke off where $breaks must: $(grep -a "$PANICSTOP_ANYBREAK" "$L" | tr -d '\r' | cut -c1-100 | paste -sd'|' -)"
        fi
        return
    fi
    n=$(grep -ac "$PANICSTOP_ANYAFTER" "$L")
    if [ "$n" != "$afters" ]; then
        PANICSTOP_WRONG="$n lines say what came after a whole account where $afters must: $(grep -a "$PANICSTOP_ANYAFTER" "$L" | tr -d '\r' | cut -c1-100 | paste -sd'|' -)"
        return
    fi
    while IFS= read -r want; do
        [ -n "$want" ] || continue
        if ! tr -d '\r' < "$L" | grep -aqE -- "$want"; then
            PANICSTOP_WRONG="/$want/ is not on the wire (the last said: $(tr -d '\r' < "$L" | grep -av '^\s*$' | tail -1 | cut -c1-120))"
            return
        fi
    done <<<"$wire"
    [ "$breaks" = 1 ] && PANICSTOP_FACTS="; one line says by what ($(tr -d '\r' < "$L" | grep -ao "$PANICSTOP_ANYBREAK.*" | head -1 | cut -c1-100)), on the wire with what it must, and no second account was said"
    [ "$afters" = 1 ] && PANICSTOP_FACTS="$PANICSTOP_FACTS; a #MC on the owner, core $PANICSTOP_MC_CPU, after its whole account was taken and stood again, one line says it came ($(tr -d '\r' < "$L" | grep -ao "$PANICSTOP_ANYAFTER.*" | head -1 | cut -c1-100)), no line says the account broke off, and no second account was said"
    if [ -n "$PANICSTOP_GLASS" ]; then
        while IFS= read -r want; do
            if ! grep -qE -- "$want" "$PANICSTOP_GLASS"; then
                PANICSTOP_WRONG="the account never reached the glass: /$want/ is not on the screen (the screen: $(basename "$PANICSTOP_GLASS"))"
                return
            fi
        done <<<"$glass"
        PANICSTOP_FACTS="$PANICSTOP_FACTS; the account is on the glass"
    fi
}

# panicstop_leg TAG WHAT CASE KEYS... — the case put to WHAT and judged.
panicstop_leg() {
    local tag=$1 what=$2 kind=$3
    shift 3
    panicstop_case "$tag" "$kind" "$@"
    [ "$PANICSTOP_CASE" = stood ] && panicstop_account "$tag" "$kind"
    case $PANICSTOP_CASE in
        stood) if [ -n "$PANICSTOP_WRONG" ]; then
                   bad "panicstop: $what — every core stood, but $PANICSTOP_WRONG"
               else
                   ok "panicstop: $what — every core stood, the BSP among them with interrupts off, the console in its hand$PANICSTOP_FACTS"
               fi ;;
        ran)   bad "panicstop: $what — the K-Core's $kind did not stop every core: $PANICSTOP_RAN ran on a second past it" ;;
        fault|unput) bad "panicstop: $what — $PANICSTOP_STOP (the last said: $PANICSTOP_LAST)" ;;
        *)     harness "panicstop: the case could not be put to $what — $PANICSTOP_STOP" ;;
    esac
}

run_panicstop() {
    echo "== panicstop: a K-Core's panic beside a BSP with interrupts off stops every core and is said whole, BIOS and UEFI =="
    panicstop_leg bios4 "BIOS STRICT 4c, panic()" panic MEM=2G
    panicstop_leg uefi4 "UEFI STRICT 4c, the exception dump" fault UEFI=on MEM=2G
    panicstop_leg bios4n "BIOS STRICT 4c, a fault inside the account" nested MEM=2G
    panicstop_leg bios4p "BIOS STRICT 4c, a panic inside the account" panicin MEM=2G
    panicstop_leg bios4w "BIOS STRICT 4c, a panic inside the account whose message faults" panicwild MEM=2G
    panicstop_leg bios4g "BIOS STRICT 4c, the glass's road faulting under the account" glass MEM=2G
    panicstop_leg bios4m "BIOS STRICT 4c, a core stopped inside its #MC handler" mcip MEM=2G
    panicstop_leg bios4a "BIOS STRICT 4c, a #MC on the owner after its whole account" after MEM=2G
    panicstop_leg bios4d "BIOS STRICT 4c, a #MC on the owner inside the drain at its account's end" draining MEM=2G
    panicstop_leg uefi4a "UEFI STRICT 4c, a #MC on the owner after its whole dump" afterdump UEFI=on MEM=2G
    build   # leave the tree built plain, as every scenario here does
}

# ---------------------------------------------------------------------------
# panicstopmut — ten mutants of the panic's stop: each turns a panicstop leg
# ---------------------------------------------------------------------------
#
# One build per mutant, and the legs it weighs put on it:
#   - the fixed vector sent again, where the NMI is: the BSP, its interrupts
#     off, runs on past the panic — on both firmwares, panic() and the dump;
#   - the owner of a panic waiting on a held lock like any core (klib_lock.c):
#     the console the BSP holds is never given back, and panic() says nothing;
#   - the break of an exception said by no line (klib_panic.c): the fault
#     inside the account ends it at the line it broke off in, nothing says by
#     what;
#   - the break of a panic said by no line: the panic inside the account ends
#     it, nothing says by what;
#   - the third coming saying a line as the second does: the panic whose
#     message faults is said broken off twice;
#   - a #PF's line said without its CR2: the glass's #PF says no address;
#   - the stand leaving MCIP as it finds it: the core stopped inside its #MC
#     handler stands with it set, and a second #MC on it shuts the machine
#     down;
#   - the end of the account marked by nothing (klib_panic.c,
#     PanicEndAccount): a #MC on the owner after its whole account is said as
#     a break of it;
#   - the mark made after the drain, not before it: a #MC taken inside the
#     drain at the account's end is said as a break of it;
#   - the dump ending by its own flush, drain and stand (idt.c), as it did
#     before it ended in PanicEndAccount: a #MC on the owner after the whole
#     dump is said as a break of it.
# The last eight are a leg each, the one their defect shows on; a mutant that
# leaves its leg green is a mechanism no test holds. Not among them: the drain
# and the flush around the line that says by what. The wire of every leg is
# pushed by hand (panicstop_account), and QEMU keeps no cache a warm reset
# could take the memory the next boot reads out of — a mutant of either
# leaves every leg as it is, so neither is put. Nor the glass said before the
# ring, or a batch it was left holding: the glass draws only what the ring
# holds and keeps no batch (emergency_screen.c), so neither defect can be put.

panicstopmut_fixed_on() {
    mutate src/lib/kernel/klib_panic.c
    python3 - <<'EOF'
p = "src/lib/kernel/klib_panic.c"
s = open(p).read()
a = "            LapicSendNmi(g_amp.cores[c].lapic_id);\n"
assert s.count(a) == 1, "panicstopmut anchor missing"
s = s.replace(a, "            lapic_send_ipi(g_amp.cores[c].lapic_id, IPI_HALT_VECTOR);   /* logcheck mutation: the fixed vector */\n", 1)
b = '#include "lapic.h"        /* lapic_is_mapped, LapicSendNmi */\n'
assert s.count(b) == 1, "panicstopmut include anchor missing"
s = s.replace(b, b + '#include "irqchip.h"      /* logcheck mutation: IPI_HALT_VECTOR */\n', 1)
open(p, "w").write(s)
EOF
    mut_done src/lib/kernel/klib_panic.c "logcheck mutation" 2
}

# panicstopmut_cut FILE ANCHOR WHY — FILE with the one place ANCHOR names cut
# out, a comment saying WHY in its place.
panicstopmut_cut() {
    mutate "$1"
    PANICSTOP_MUT_FILE=$1 PANICSTOP_MUT_ANCHOR=$2 PANICSTOP_MUT_WHY=$3 python3 - <<'EOF'
import os
p, a = os.environ["PANICSTOP_MUT_FILE"], os.environ["PANICSTOP_MUT_ANCHOR"]
s = open(p).read()
assert s.count(a) == 1, "panicstopmut anchor missing in " + p
pad = a[:len(a) - len(a.lstrip(" "))]
s = s.replace(a, pad + "/* logcheck mutation: " + os.environ["PANICSTOP_MUT_WHY"] + " */\n", 1)
open(p, "w").write(s)
EOF
    mut_done "$1" "logcheck mutation" 1
}

# panicstopmut_mcip_kept — the stand clearing no MCIP; the claim's winner still
# clears its own.
panicstopmut_mcip_kept() {
    mutate src/lib/kernel/klib_panic.c
    python3 - <<'EOF'
p = "src/lib/kernel/klib_panic.c"
s = open(p).read()
a = "static __attribute__((noreturn)) void PanicStand(void)\n{\n    PanicLeaveMachineCheck();\n"
b = "static __attribute__((noreturn)) void PanicStand(void)\n{\n    /* logcheck mutation: the stand leaves MCIP as it finds it */\n"
assert s.count(a) == 1, "panicstopmut stand anchor missing"
open(p, "w").write(s.replace(a, b, 1))
EOF
    mut_done src/lib/kernel/klib_panic.c "logcheck mutation" 1
}

# panicstopmut_mark_late — the account's end marked after its drain, as it was
# before a #MC taken in the drain was weighed.
panicstopmut_mark_late() {
    mutate src/lib/kernel/klib_panic.c
    python3 - <<'EOF'
p = "src/lib/kernel/klib_panic.c"
s = open(p).read()
a = "    g_panic_account_said = true;\n"
b = "    WireDrain();\n\n    /* And the whole account drawn once more"
assert s.count(a) == 1 and s.count(b) == 1, "panicstopmut mark anchors missing"
s = s.replace(a, "    /* logcheck mutation: the mark made after the drain */\n", 1)
s = s.replace(b, "    WireDrain();\n    g_panic_account_said = true;   /* logcheck mutation: the mark made after the drain */\n\n    /* And the whole account drawn once more", 1)
open(p, "w").write(s)
EOF
    mut_done src/lib/kernel/klib_panic.c "logcheck mutation" 2
}

# panicstopmut_dump_own_end — the dump ending by its own flush, drain and stand
# of `cli; hlt`, as it did before PanicEndAccount, its end marked by nothing.
panicstopmut_dump_own_end() {
    mutate src/kernel/arch/x86-64/idt/idt.c
    python3 - <<'EOF'
p = "src/kernel/arch/x86-64/idt/idt.c"
s = open(p).read()
a = "    PanicEndAccount();\n"
b = ("    {   /* logcheck mutation: the dump ends by its own flush, drain and stand */\n"
     "        extern void LogKeepFlush(void);\n"
     "        extern void WireDrain(void);\n"
     "        LogKeepFlush();\n"
     "        WireDrain();\n"
     "        for (;;) __asm__ volatile(\"cli; hlt\");\n"
     "    }\n")
assert s.count(a) == 1, "panicstopmut dump-end anchor missing"
open(p, "w").write(s.replace(a, b, 1))
EOF
    mut_done src/kernel/arch/x86-64/idt/idt.c "logcheck mutation" 1
}

# panicstopmut_leg TAG WHAT CASE KEYS... — the case put to WHAT on the fixed
# vector: a core must run on past the panic.
panicstopmut_leg() {
    local tag=$1 what=$2 kind=$3
    shift 3
    panicstop_case "$tag" "$kind" "$@"
    case $PANICSTOP_CASE in
        ran)   ok "panicstopmut: $what — the leg turned: $PANICSTOP_RAN ran on a second past the K-Core's $kind" ;;
        stood) bad "panicstopmut: $what — the leg held on a panic that stops its peers by the fixed vector: every core stood" ;;
        *)     harness "panicstopmut: $what — the case the leg weighs was not put ($PANICSTOP_CASE${PANICSTOP_STOP:+: $PANICSTOP_STOP}; the last said: $PANICSTOP_LAST)" ;;
    esac
}

# panicstopmut_turns MUTANT TAG WHAT CASE KEYS... — the case put to WHAT on the
# kernel MUTANT names: the leg must turn — a core run on, an account never
# said, or a machine that stood with an account not what it must be.
panicstopmut_turns() {
    local mutant=$1 tag=$2 what=$3 kind=$4
    shift 4
    panicstop_case "$tag" "$kind" "$@"
    [ "$PANICSTOP_CASE" = stood ] && panicstop_account "$tag" "$kind"
    case $PANICSTOP_CASE in
        ran)   ok "panicstopmut: $mutant — $what turned: $PANICSTOP_RAN ran on a second past the K-Core's $kind" ;;
        fault) ok "panicstopmut: $mutant — $what turned: $PANICSTOP_STOP" ;;
        stood) if [ -n "$PANICSTOP_WRONG" ]; then
                   ok "panicstopmut: $mutant — $what turned: every core stood, but $PANICSTOP_WRONG"
               else
                   bad "panicstopmut: $mutant — $what held: every core stood and the account is all it must be"
               fi ;;
        *)     harness "panicstopmut: $mutant — the case $what weighs was not put ($PANICSTOP_CASE${PANICSTOP_STOP:+: $PANICSTOP_STOP}; the last said: $PANICSTOP_LAST)" ;;
    esac
}

run_panicstopmut() {
    echo "== panicstopmut: ten mutants of the panic's stop — each must turn the panicstop legs it weighs =="
    panicstopmut_fixed_on
    panicstopmut_leg mutbios4 "BIOS STRICT 4c, panic()" panic MEM=2G
    panicstopmut_leg mutuefi4 "UEFI STRICT 4c, the exception dump" fault UEFI=on MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_cut src/lib/kernel/klib_lock.c $'            if (PanicOwnedHere())\n                break;\n' \
        "the owner of a panic waits on a held lock"
    panicstopmut_turns "the owner waits on a held lock" mutbust "BIOS STRICT 4c, panic()" panic MEM=2G
    restore src/lib/kernel/klib_lock.c

    panicstopmut_cut src/lib/kernel/klib_panic.c $'    else\n        PanicSayBreakf("vector %lu, error 0x%lx, RIP 0x%lx",\n                       frame->vector, frame->error_code, frame->rip);\n' \
        "the break of an exception said by no line"
    panicstopmut_turns "the break said by no line" mutbroke "BIOS STRICT 4c, a fault inside the account" nested MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_cut src/lib/kernel/klib_panic.c $'    PanicSayBreak("panic: ", message, args);\n' \
        "the break of a panic said by no line"
    panicstopmut_turns "a panic's break said by no line" mutpline "BIOS STRICT 4c, a panic inside the account" panicin MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_cut src/lib/kernel/klib_panic.c $'    if (++g_panic_depth != 2)\n        PanicStand();\n' \
        "the third coming says a line as the second does"
    panicstopmut_turns "the third coming says a line" mutdepth "BIOS STRICT 4c, a panic inside the account whose message faults" panicwild MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_cut src/lib/kernel/klib_panic.c $'    if (frame->vector == 14)\n        PanicSayBreakf("vector %lu, error 0x%lx, RIP 0x%lx, CR2 0x%lx",\n                       frame->vector, frame->error_code, frame->rip, cr2);\n    else\n' \
        "a #PF's line said without its CR2"
    panicstopmut_turns "a #PF's line without its CR2" mutcr2 "BIOS STRICT 4c, the glass's road faulting under the account" glass MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_mcip_kept
    panicstopmut_turns "the stand keeps MCIP" mutmcip "BIOS STRICT 4c, a core stopped inside its #MC handler" mcip MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_cut src/lib/kernel/klib_panic.c $'    g_panic_account_said = true;\n' \
        "the end of the account marked by nothing"
    panicstopmut_turns "the account's end unmarked" mutsaid "BIOS STRICT 4c, a #MC on the owner after its whole account" after MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_mark_late
    panicstopmut_turns "the mark after the drain" mutlate "BIOS STRICT 4c, a #MC on the owner inside the drain at its account's end" draining MEM=2G
    restore src/lib/kernel/klib_panic.c

    panicstopmut_dump_own_end
    panicstopmut_turns "the dump's own end, unmarked" mutdumpend "UEFI STRICT 4c, a #MC on the owner after its whole dump" afterdump UEFI=on MEM=2G
    restore src/kernel/arch/x86-64/idt/idt.c
    build   # leave the tree built from clean sources
}
