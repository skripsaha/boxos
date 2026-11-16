# logcheck_panel — the panel scenario of tools/logcheck.sh: Gangway's panel
# (gangway_panel.c, gangway_panel_log.c), asked for by holding Space while the
# machine starts — what was found and chosen, what may be changed for one
# start and handed on as the stamps gangway.settings's are, this start's
# journal, the log the kernel kept of the start before (boxos_logkeep.h), and
# Turn off. What the panel says goes down COM1 as it is said, so the wire says
# a machine stands at it.
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `panel` to run_panel
# here; everything it calls (build, chk, harness, the waits, the keys) is
# stand.sh's, stand_typer.sh's or logcheck.sh's.

# ---------------------------------------------------------------------------
# panel: a UEFI machine of four cores starts with no key held and goes its
# way — no panel. It starts again with Space held: the panel opens and says so
# on the wire; startup all and cores 1 are chosen on it, both journals opened —
# no log is kept from before — and Start: the kernel is asked for both, starts
# one core of the four, and the start reaches the hub with the panel among the
# moments of its journey. A warm reset with Space held, keep-log on, Start: the
# window is armed. A warm reset with Space held again: the previous start's
# log is shown out of the window, and Turn off turns the machine off. The
# image is put back as it was after.
# ---------------------------------------------------------------------------

# Space held — sent over and over, as a held key repeats — until the wire says
# once more than it had that the panel is open (the journal the kernel puts
# into its log says it again, after): 0, or 1 when it never does.
panel_hold() {
    local n i
    n=$(grep -ac "Gangway: the panel is open (Space held)" build/serial.log 2>/dev/null)
    n=${n:-0}
    for i in $(seq 1 300); do
        ./tools/qemu-input.sh key spc >/dev/null 2>&1
        [ "$(grep -ac "Gangway: the panel is open (Space held)" build/serial.log 2>/dev/null)" -gt "$n" ] && return 0
        sleep 0.1
    done
    harness "panel: Space held for 30 s, and the wire never said the panel is open"
    return 1
}

# Keys on the panel, each given its moment to be drawn.
panel_keys() {
    local key
    for key in "$@"; do
        key_press "$key" || return 1
        sleep 0.4
    done
}

panel_boot() {
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI=on CORES=4 MEM=4G >"$SCRATCH/panel.run-bg" 2>&1; then
        harness "panel: run-bg would not start: $(tail -1 "$SCRATCH/panel.run-bg")"
        return 1
    fi
}

run_panel() {
    echo "== panel: Space held while the machine starts — Gangway's panel, a start changed on it, the log kept before =="
    build
    cp build/boxos.img "$SCRATCH/panel.before.img"

    local P Q R qemu gone=1
    if panel_boot; then
        wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$SCRATCH/serial.panel.plain.log"
    fi
    P="$SCRATCH/serial.panel.plain.log"
    ! grep -aq "Gangway: the panel" "$P" && grep -aq "BoxOS Hub" "$P"
    chk $? "panel (plain): with no key held there is no panel, and the start reaches the hub"

    # Start (the row chosen) → up twice: Startup → all; up: Cores → 1; down
    # four times: This start's journal; back; down: the previous start's log;
    # back; up twice: Start.
    if panel_boot && panel_hold; then
        qemu=$(cat build/qemu.pid 2>/dev/null)
        sleep 1
        mon_shot "$SCRATCH/panel.ppm"
        panel_keys up up right up right down down down down ret esc down ret esc up up ret
        wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
        sleep 2
        cp build/serial.log "$SCRATCH/serial.panel.start.log"
    fi
    Q="$SCRATCH/serial.panel.start.log"
    grep -aq "Gangway: the panel is open (Space held)" "$Q"
    chk $? "panel (start): Space held, the panel opens, and the wire says so as it does"
    grep -aq "Gangway: the panel shows this start's journal" "$Q"
    chk $? "panel (start): it shows this start's journal"
    grep -aq "Gangway: the panel shows the previous start's log — there is none: nothing is kept there" "$Q"
    chk $? "panel (start): and of the start before, that nothing is kept"
    grep -aq "Gangway: the panel sets for this start: startup all, cores 1" "$Q"
    chk $? "panel (start): Start — it sets startup all and cores 1 for this start, said"
    grep -aq "\[Boarding\]   the person asks of the kernel: startup all cores 1" "$Q"
    chk $? "panel (start): the kernel reads both stamps"
    grep -aq "\[AMP\] the person asks for 1 core(s): 1 of the 4 the machine has are started" "$Q" &&
        grep -aq "\[AMP\] Detected 1 core(s)" "$Q"
    chk $? "panel (start): and starts one core of the four"
    grep -aqE "\[Boarding\]   it marked [0-9]+ moment\(s\) of its journey: .*screen panel memory" "$Q"
    chk $? "panel (start): the panel is a moment of the journey, between the screen and the memory map"
    grep -aq "BoxOS Hub" "$Q"
    chk $? "panel (start): and the start reaches the hub"

    # A warm reset keeps the memory: keep-log on for the start after it, and
    # that start's log read by the panel at the next.
    if [ -s "$Q" ] && mon_raw system_reset && panel_hold; then
        panel_keys up ret down ret
        wait_for_line build/serial.log "LogKeep is armed" "$WAIT_QUIET_S"
        wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" "$(grep -an "LogKeep is armed" build/serial.log | tail -1 | cut -d: -f1)"
        sleep 3
        if mon_raw system_reset && panel_hold; then
            panel_keys down down ret
            sleep 1
            mon_shot "$SCRATCH/panel.previous.ppm"
            panel_keys esc down down ret
            sleep 5
            # QEMU takes its pidfile away as it exits: the pid read at the
            # start is the one asked after.
            [ -n "$qemu" ] && ! kill -0 "$qemu" 2>/dev/null && gone=0
        fi
        cp build/serial.log "$SCRATCH/serial.panel.kept.log"
    fi
    R="$SCRATCH/serial.panel.kept.log"
    make run-stop >/dev/null 2>&1
    cp "$SCRATCH/panel.before.img" build/boxos.img

    grep -aq "Gangway: the panel sets for this start: keep-log on" "$R" && grep -aq "\[BOOT\] LogKeep is armed" "$R"
    chk $? "panel (kept): keep-log on for one start — the kernel arms the carry-over window"
    grep -aqE "Gangway: the panel shows the previous start's log \(start [0-9]+, [0-9]+ bytes said\)" "$R"
    chk $? "panel (kept): after a reset the panel shows the log that start kept ($(grep -a "previous start's log (start" "$R" | tr -d '\r' | tail -1 | cut -c10-))"
    grep -aq "Gangway: the panel turns the machine off" "$R" && [ "$gone" = 0 ]
    chk $? "panel (kept): Turn off — said, and the machine is off"
}
