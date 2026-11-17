# shellcheck shell=bash
# logcheck_drawcheck.sh — the drawing engine held to itself on the machine
# (sourced by tools/logcheck.sh; run by the drawcheck scenario).
#
# The image is booted twice: on a processor of SSE2 alone (qemu64, the
# default) and on one with AVX2 (STRICT=on, -cpu max). Each time `drawcheck`
# is typed at the hub; it draws the same scenes of every kind of thing every
# way the processor offers, and each must make the plain way's very pixels and
# write the very numbers to the glass, near and dithered (src/userspace/utils/
# drawcheck). The gate holds that the engine chose the way the processor has,
# that every way below it was held, and that the last line says PASS; the
# times each way took are kept in the log, a guide only under TCG.

drawcheck_boot() {   # drawcheck_boot NAME [make-args…]
    local name=$1
    shift
    make run-stop >/dev/null 2>&1
    if ! make run-bg "$@" >"$SCRATCH/drawcheck.$name.run-bg" 2>&1; then
        harness "drawcheck ($name): run-bg would not start: $(tail -1 "$SCRATCH/drawcheck.$name.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "drawcheck" &&
        wait_for_line -E build/serial.log 'drawcheck: (PASS|FAIL)' "$WAIT_QUIET_SLOW_S" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    local rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.drawcheck.$name.log"
    return $rc
}

run_drawcheck() {
    echo "== drawcheck: every way the processor offers draws the plain way's very pixels, on the machine =="
    if ! make >"$SCRATCH/build.log" 2>&1; then
        bad "drawcheck: make does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    local name want ways L
    for name in sse2 avx2; do
        if [ $name = sse2 ]; then
            drawcheck_boot $name
            want=SSE2
            ways="SSE2"
        else
            drawcheck_boot $name STRICT=on
            want=AVX2
            ways="SSE2 AVX2"
        fi
        L="$SCRATCH/serial.drawcheck.$name.log"
        grep -a "^drawcheck: " "$L" | tr -d '\r' | sed 's/^/  /'
        grep -aq "drawcheck: the processor offers $want — the engine works it so" "$L"
        chk $? "drawcheck ($name): the engine works the way the processor offers — $want"
        local way held=1
        for way in $ways; do
            grep -aq "drawcheck: $way — 24 scenes, every pixel the plain way's, near and dithered" "$L" || held=0
        done
        [ $held = 1 ]
        chk $? "drawcheck ($name): $ways, each the plain way's very pixels in all 24 scenes, near and dithered"
        grep -aq "drawcheck: PASS — every way the processor offers draws the plain way's very pixels" "$L"
        chk $? "drawcheck ($name): PASS ($(grep -aoE 'drawcheck: (PASS|FAIL)' "$L" | tail -1))"
    done
}
