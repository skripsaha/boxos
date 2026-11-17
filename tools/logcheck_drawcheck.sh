# shellcheck shell=bash
# logcheck_drawcheck.sh — the drawing engine held to itself on the machine
# (sourced by tools/logcheck.sh; run by the drawcheck scenario).
#
# The image is booted twice: on a processor of SSE2 alone (qemu64, the plain
# build) and on one with AVX2 (-cpu max, the tree built STRICT=on). Each time
# `drawcheck` is typed at the hub; it draws the same scenes of every kind of
# thing, and of text in the system's fonts, every way the processor offers,
# and each must make the plain way's very pixels and write the very numbers
# to the glass, near and dithered (src/userspace/utils/drawcheck). The gate
# holds that the engine chose the way the processor has, that the system's
# fonts were found on the volume and text set in them, that every way below
# it was held, and that the last line says PASS; the times each way took, and
# setting and drawing text, are kept in the log, a guide only under TCG.

# drawcheck_say COMMAND LINE WAIT — COMMAND typed and entered, then LINE (a
# pattern) waited for past where the log stood: the hub's prompt is not
# looked for, programs it started saying their lines after it.
drawcheck_say() {
    local from
    from=$(wc -l < build/serial.log | tr -d ' ')
    type_cmd "$1" || return 1
    sleep 1
    key_press ret || return 1
    wait_for_line -E build/serial.log "$2" "$3" $((from + 1))
}

drawcheck_boot() {   # drawcheck_boot NAME [make-args…]
    local name=$1
    shift
    make run-stop >/dev/null 2>&1
    if ! make run-bg "$@" >"$SCRATCH/drawcheck.$name.run-bg" 2>&1; then
        harness "drawcheck ($name): run-bg would not start: $(tail -1 "$SCRATCH/drawcheck.$name.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && sleep 3 &&
        drawcheck_say "drawcheck" 'drawcheck: (PASS|FAIL)' "$WAIT_QUIET_SLOW_S" && sleep 1 &&
        drawcheck_say "bye" "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    local rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.drawcheck.$name.log"
    return $rc
}

run_drawcheck() {
    echo "== drawcheck: every way the processor offers draws the plain way's very pixels, on the machine =="
    local name want ways L
    for name in sse2 avx2; do
        if [ $name = sse2 ]; then
            if ! make >"$SCRATCH/build.log" 2>&1; then
                bad "drawcheck: make does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
                return
            fi
            drawcheck_boot $name
            want=SSE2
            ways="SSE2"
        else
            if ! build_with_key STRICT=on; then
                bad "drawcheck: make STRICT=on does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
                return
            fi
            drawcheck_boot $name STRICT=on
            want=AVX2
            ways="SSE2 AVX2"
        fi
        L="$SCRATCH/serial.drawcheck.$name.log"
        grep -a "^drawcheck: " "$L" | tr -d '\r' | sed 's/^/  /'
        grep -aq "drawcheck: the processor offers $want — the engine works it so" "$L"
        chk $? "drawcheck ($name): the engine works the way the processor offers — $want"
        grep -aq "drawcheck: the system's fonts — JetBrains Mono NL; text set in JetBrains Mono NL" "$L"
        chk $? "drawcheck ($name): the system's fonts read from the volume, text set in them"
        local way held=1
        for way in $ways; do
            grep -aq "drawcheck: $way — 32 scenes, every pixel the plain way's, near and dithered" "$L" || held=0
        done
        [ $held = 1 ]
        chk $? "drawcheck ($name): $ways, each the plain way's very pixels in all 32 scenes of shapes and text, near and dithered"
        grep -aq "drawcheck: text, " "$L"
        chk $? "drawcheck ($name): a page of text set and drawn, and how long it took said"
        grep -aq "drawcheck: PASS — every way the processor offers draws the plain way's very pixels" "$L"
        chk $? "drawcheck ($name): PASS ($(grep -aoE 'drawcheck: (PASS|FAIL)' "$L" | tail -1))"
    done
}
