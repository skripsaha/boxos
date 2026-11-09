# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the power button's halt on one core beside a
# program the halt's own word wakes to compute without end.

# ---------------------------------------------------------------------------
# halthog — on one core the halt the power button begins at rest has its turns
# ---------------------------------------------------------------------------
#
# A one-way pass is taken by a pump at rest (baton.c), and on one core the pump
# at rest that finds the power button's halt is the tick's — only over ring 3,
# its halt then running with interrupts shut — or cpu_idle's: the halt then
# stands in the idle context, which the tick switches away from like any idle,
# and which the scheduler handed the core again only when nothing else was
# ready. The halt publishes system:shutdown and turns its courtesy window
# before its drain (system_halt.c), so a program woken by that word that
# computes without a park kept the core from the halt for good: the machine
# left running, its halt begun and never ended. `quilltest shutdownhog` is that
# program. Measured on 045e7b5e with the word mustered: of four presses two
# were taken there, and both machines ran on a minute later, every look at the
# core in QuillShutdownHog or an interrupt handler, none in the halt. powercut
# race's base met the same with clock.box, which paints every millisecond:
# twice in some 270 boots its halt said "[HALT] Draining async writes..." and
# two lines more, the core at ring 3 in one result_wait of clock.box. And
# before the word was mustered it reached nobody: the kernel named
# system:shutdown outside its muster (logbook.c), and the program that asked
# for it held another id.
#
# One core, BIOS and UEFI, STRICT: the button pressed once shutdownhog's first
# line is said and the machine is quiet, and the machine must switch itself
# off within WAIT_QUIET_S, its halt having said it powers off, with the program
# its word woke computing beside it. A press the tick takes over ring 3 puts
# no case — the program never runs to be told — and with clock.box at boot the
# tick took it in five boots of twelve on BIOS 1c and in all eight on UEFI 1c,
# whose clock paints a framebuffer. So the leg boots an image on which clock.box
# is not started (halthog_quiet): on it twelve boots of twelve, six BIOS 1c and
# six UEFI STRICT 1c, put the case. A boot that still puts none is booted
# again, HALTHOG_ROUNDS at the most, and said as the harness's when none did.
# halthogmut runs the leg on a scheduler that owes idle nothing for the pass,
# and the leg must turn.
# ---------------------------------------------------------------------------

HALTHOG_ROUNDS=3
HALTHOG_WAITS='^\[QUILL\] shutdownhog: pid [0-9]+ waits for the machine to stop'
HALTHOG_TOLD='^\[QUILL\] shutdownhog: the machine is stopping — pid [0-9]+ computes'
HALTHOG_OFF='^\[HALT\] Powering off'
HALTHOG_UNMUSTERED="the kernel named 'system:shutdown', which is not in its muster"
HALTHOG_CLOCK="^\[AUTOSTART\] Started 'clock.box'"

# halthog_quiet KEYS... — $SCRATCH/halthog.quiet.img: a copy of the image as
# built, booted once on one core to take clock.box off its autostart (untag)
# and halted by bye. 0 when it is made; 1 when the guest did not do it, 2 when
# no machine was started or the host could not speak to it — HALTHOG_STOP
# says what, HALTHOG_LAST the last line said.
halthog_quiet() {
    local img="$SCRATCH/halthog.quiet.img" L="$SCRATCH/serial.halthog.quiet.log" pid off=1 h0=$HARNESS
    HALTHOG_STOP= HALTHOG_LAST=
    make run-stop >/dev/null 2>&1
    rm -f "$L"
    cp -c build/boxos.img "$img" 2>/dev/null || cp build/boxos.img "$img"
    if ! make run-bg BOOTDISK="$img" CORES=1 "$@" >"$SCRATCH/runbg.halthog.quiet.log" 2>&1; then
        HALTHOG_STOP="no machine was started to take clock.box off its autostart: $(tail -1 "$SCRATCH/runbg.halthog.quiet.log")"
        rm -f "$img"
        return 2
    fi
    pid=$(cat build/qemu.pid)
    if wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
       type_line "untag clock.box autostart" && wait_for_line build/serial.log "Tag removed" "$WAIT_QUIET_S" &&
       wait_for_prompt && type_line "bye"; then
        wait_until "$WAIT_QUIET_S" "the machine to switch itself off by bye" "! kill -0 $pid 2>/dev/null"
        off=$?
    fi
    cp build/serial.log "$L"
    make run-stop >/dev/null 2>&1
    rm -f build/qemu.pid build/qemu.mon build/qemu.qmp
    [ $off = 0 ] && return 0
    rm -f "$img"
    HALTHOG_LAST=$(tr -d '\r' < "$L" | grep -av '^\s*$' | tail -1 | cut -c1-200)
    if [ $HARNESS -gt $h0 ]; then
        HALTHOG_STOP="the host could not speak to the machine that was to take clock.box off its autostart"
        return 2
    fi
    HALTHOG_STOP="clock.box taken off its autostart and the machine halted by bye — not done"
    return 1
}

# halthog_round TAG KEYS... — a copy of the quiet image booted with KEYS on one
# core, shutdownhog started and the button pressed; the log kept as
# serial.halthog.TAG.log. 0 when the machine switched itself off, 1 when it did
# not, 2 when no machine was started or the host could not speak to it, 3 when
# the guest never came to the press (HALTHOG_STOP says where).
halthog_round() {
    local tag=$1 img="$SCRATCH/halthog.$1.img" pid off=3 h0=$HARNESS
    shift
    HALTHOG_STOP=
    make run-stop >/dev/null 2>&1
    rm -f "$SCRATCH/serial.halthog.$tag.log"
    cp -c "$SCRATCH/halthog.quiet.img" "$img" 2>/dev/null || cp "$SCRATCH/halthog.quiet.img" "$img"
    if ! make run-bg BOOTDISK="$img" CORES=1 "$@" >"$SCRATCH/runbg.halthog.$tag.log" 2>&1; then
        HALTHOG_STOP="no machine was started: $(tail -1 "$SCRATCH/runbg.halthog.$tag.log")"
        rm -f "$img"
        return 2
    fi
    pid=$(cat build/qemu.pid)
    if ! wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" || ! wait_for_prompt; then
        HALTHOG_STOP="the machine never came to its shell's prompt"
    elif grep -aqE "$HALTHOG_CLOCK" build/serial.log; then
        HALTHOG_STOP="clock.box was started at boot though the machine was halted by bye with its autostart taken off"
    elif ! type_line "quilltest shutdownhog" ||
         ! wait_for_line -E build/serial.log "$HALTHOG_WAITS" "$WAIT_QUIET_S"; then
        HALTHOG_STOP="shutdownhog never said it waits for the machine to stop"
    elif ! wait_for_quiet "$BOOT_BURST_QUIET_S"; then
        HALTHOG_STOP="the machine never went quiet past shutdownhog's first line"
    elif mon_raw system_powerdown; then
        wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
        off=$?
    fi
    [ $HARNESS -gt $h0 ] && off=2 && HALTHOG_STOP="the host could not speak to the machine"
    cp build/serial.log "$SCRATCH/serial.halthog.$tag.log"
    make run-stop >/dev/null 2>&1
    rm -f "$img" build/qemu.pid build/qemu.mon build/qemu.qmp
    return $off
}

# halthog_case TAG KEYS... — rounds of halthog_round until one puts the case:
# the program told the machine is stopping, or the word reaching nobody.
# HALTHOG_CASE how it ended — off (told, and the machine switched itself off
# saying it powers off), up (told, and it did not), deaf (the word reached
# nobody), mute (never told, and the machine did not switch itself off), fault
# (the guest never came to the press: HALTHOG_STOP), unput (no boot put the
# case), harness (HALTHOG_STOP) — HALTHOG_ROUND its boot, HALTHOG_LAST the last
# line said.
halthog_case() {
    local tag=$1 rc L="$SCRATCH/serial.halthog.$1.log"
    HALTHOG_ROUND=1
    while [ $HALTHOG_ROUND -le $HALTHOG_ROUNDS ]; do
        halthog_round "$@"; rc=$?
        HALTHOG_LAST=$([ -f "$L" ] && tr -d '\r' < "$L" | grep -av '^\s*$' | tail -1 | cut -c1-200)
        case $rc in
            2) HALTHOG_CASE=harness; return ;;
            3) HALTHOG_CASE=fault; return ;;
        esac
        if tr -d '\r' < "$L" | grep -aqE "$HALTHOG_TOLD"; then
            if [ $rc = 0 ] && tr -d '\r' < "$L" | grep -aqE "$HALTHOG_OFF"; then HALTHOG_CASE=off; else HALTHOG_CASE=up; fi
            return
        fi
        if tr -d '\r' < "$L" | grep -aqF "$HALTHOG_UNMUSTERED"; then HALTHOG_CASE=deaf; return; fi
        if [ $rc != 0 ]; then HALTHOG_CASE=mute; return; fi
        echo "  (boot $HALTHOG_ROUND: the machine switched itself off and shutdownhog was never heard told the machine was stopping — no case put; booted again)"
        HALTHOG_ROUND=$((HALTHOG_ROUND + 1))
    done
    HALTHOG_ROUND=$HALTHOG_ROUNDS
    HALTHOG_CASE=unput
}

# halthog_leg TAG WHAT KEYS... — the case put to WHAT and judged.
halthog_leg() {
    local tag=$1 what=$2
    shift 2
    halthog_case "$tag" "$@"
    case $HALTHOG_CASE in
        off)  ok "halthog: $what switched itself off by its power button with the program its halt woke computing without end beside it, boot $HALTHOG_ROUND (the last said: $HALTHOG_LAST)" ;;
        up)   bad "halthog: $what would not switch itself off by its power button: its halt woke the program, which computes without end beside it, boot $HALTHOG_ROUND (the last said: $HALTHOG_LAST)" ;;
        deaf) bad "halthog: $what — the halt's system:shutdown reached nobody: the kernel named it outside its muster, and the program that asked for it holds another id (logbook.c)" ;;
        mute) bad "halthog: $what would not switch itself off by its power button, and the program was never told the machine was stopping, boot $HALTHOG_ROUND (the last said: $HALTHOG_LAST)" ;;
        fault) bad "halthog: $what — $HALTHOG_STOP, boot $HALTHOG_ROUND (the last said: $HALTHOG_LAST)" ;;
        unput) harness "halthog: in $HALTHOG_ROUNDS boots of $what shutdownhog was never heard told the machine was stopping" ;;
        *)    harness "halthog: the case could not be put to $what — $HALTHOG_STOP (the last said: $HALTHOG_LAST)" ;;
    esac
}

run_halthog() {
    echo "== halthog: the power button on one core while the program its halt wakes computes without end, BIOS and UEFI =="
    build STRICT=on
    halthog_quiet STRICT=on MEM=2G
    case $? in
        0) halthog_leg bios1 "BIOS STRICT 1c" STRICT=on MEM=2G
           halthog_leg uefi1 "UEFI STRICT 1c" UEFI=on STRICT=on MEM=2G
           lockrank_judge "$SCRATCH/serial.halthog.quiet.log $SCRATCH/serial.halthog.bios1.log $SCRATCH/serial.halthog.uefi1.log" "halthog" ;;
        1) bad "halthog: $HALTHOG_STOP (the last said: $HALTHOG_LAST)" ;;
        *) harness "halthog: $HALTHOG_STOP" ;;
    esac
    rm -f "$SCRATCH/halthog.quiet.img"
    build   # leave the tree built plain, as every scenario here does
}

# ---------------------------------------------------------------------------
# halthogmut — the scheduler owes idle nothing for the pass: halthog turns
# ---------------------------------------------------------------------------

halthogmut_idle_on() {
    mutate src/kernel/core/scheduler/idle.c
    python3 - <<'EOF'
p = "src/kernel/core/scheduler/idle.c"
s = open(p).read()
a = "    return TagFSIdleOwed() || BatonOneWayIdleOwed();\n"
assert s.count(a) == 1, "halthogmut anchor missing"
s = s.replace(a, "    return TagFSIdleOwed();   /* logcheck mutation: idle owed nothing for a one-way pass */\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/core/scheduler/idle.c "logcheck mutation" 1
}

# halthogmut_leg TAG WHAT KEYS... — the case put to WHAT on the mutant: the
# machine must still be running past its press, its halt beside the program.
halthogmut_leg() {
    local tag=$1 what=$2
    shift 2
    halthog_case "$tag" "$@"
    case $HALTHOG_CASE in
        up)  ok "halthogmut: $what — the leg turned: told, the program computes and the machine runs on past its power button, boot $HALTHOG_ROUND (the last said: $HALTHOG_LAST)" ;;
        off) bad "halthogmut: $what — the leg held on a scheduler that owes idle nothing for the pass: the machine switched itself off with the program told, boot $HALTHOG_ROUND" ;;
        *)   harness "halthogmut: $what — the case the leg weighs was not put ($HALTHOG_CASE${HALTHOG_STOP:+: $HALTHOG_STOP}; the last said: $HALTHOG_LAST)" ;;
    esac
}

run_halthogmut() {
    echo "== halthogmut: a scheduler that owes idle nothing for a one-way pass — every halthog leg must turn =="
    halthogmut_idle_on
    build STRICT=on
    if halthog_quiet STRICT=on MEM=2G; then
        halthogmut_leg mutbios1 "BIOS STRICT 1c" STRICT=on MEM=2G
        halthogmut_leg mutuefi1 "UEFI STRICT 1c" UEFI=on STRICT=on MEM=2G
    else
        harness "halthogmut: the case the leg weighs was not put — $HALTHOG_STOP${HALTHOG_LAST:+ (the last said: $HALTHOG_LAST)}"
    fi
    rm -f "$SCRATCH/halthog.quiet.img"
    restore src/kernel/core/scheduler/idle.c
    build   # leave the tree built from clean sources
}
