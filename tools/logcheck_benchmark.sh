# shellcheck shell=bash
# logcheck_benchmark.sh — `benchmark graphics` run to its end on the machine
# (sourced by tools/logcheck.sh; run by the benchmark scenario).
#
# The image is booted on a framebuffer and `benchmark graphics` typed at the
# hub (src/userspace/utils/benchmark): the drawing engine measured every way
# the processor offers, then eight scenes the display shows, each measured for
# five seconds, and the file of it all written to the volume. Under TCG the
# numbers are a guide only and none is held; the gate holds that every part
# was measured and said — every way, the text, all eight scenes — that the
# display refused nothing and closed no area, that the file reads back off
# the image as the console said it, and that the hub answers after.

run_benchmark() {
    echo "== benchmark: the drawing engine and eight scenes the display shows, measured on the machine =="
    if ! make >"$SCRATCH/build.log" 2>&1; then
        bad "benchmark: make does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI=on >"$SCRATCH/benchmark.run-bg" 2>&1; then
        harness "benchmark: run-bg would not start: $(tail -1 "$SCRATCH/benchmark.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && sleep 3 &&
        drawcheck_say "benchmark graphics" 'benchmark: (written to|.* could not be written|.* stopped taking)' "$WAIT_QUIET_SLOW_S" &&
        sleep 1 && drawcheck_say "bye" "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    local said=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    local L="$SCRATCH/serial.benchmark.log"
    tr -d '\r' < build/serial.log > "$L"
    grep -aE "^(benchmark|engine|scene)" "$L" | sed 's/^/  /'

    grep -aqE "^engine plain, ns a pixel: clear [0-9.]+, .* out nearest [0-9.]+" "$L" &&
        grep -aqE "^engine SSE2, ns a pixel: clear [0-9.]+, .* out nearest [0-9.]+" "$L"
    chk $? "benchmark: the engine measured every way the processor offers — plain and SSE2"
    grep -aqE "^engine: text in Inter — [0-9]+ characters set in [0-9]+ us" "$L"
    chk $? "benchmark: a page of text set and drawn in the system's font, and how long it took said"
    local scene measured=1
    for scene in quiet shapes panels text fades tiles canvas everything; do
        grep -aqE "^scene $scene — .*: [0-9.]+ frames a second; a frame every [0-9.]+ ms \(95% within [0-9.]+, 99% within [0-9.]+, worst [0-9.]+\), [0-9]+ late; the display [0-9.]+ ms a frame .* processor: the display [0-9.]+ cores, benchmark [0-9.]+ — (smooth|uneven|slideshow)$" "$L" ||
            measured=0
    done
    [ $measured = 1 ]
    chk $? "benchmark: all eight scenes measured — quiet, shapes, panels, text, fades, tiles, canvas, everything"
    grep -aqE "^benchmark graphics: 8 of 8 scenes measured — [0-9]+ smooth, [0-9]+ uneven, [0-9]+ a slideshow; 0 change\(s\) refused$" "$L"
    chk $? "benchmark: the summary of all eight, the display refusing nothing"
    ! grep -aq "broke the items' language" "$L"
    chk $? "benchmark: the display closed no area for breaking the language"
    local got="$SCRATCH/benchmark-graphics.log"
    rm -f "$got"
    python3 tools/volume-read.py build/boxos.img get benchmark-graphics.log -o "$got" >/dev/null 2>&1 &&
        [ "$(grep -c "^scene " "$got")" = 8 ] &&
        diff <(grep -aE "^(benchmark graphics|engine|scene)" "$L") "$got" >/dev/null
    chk $? "benchmark: benchmark-graphics.log read off the image, every line as the console said it"
    [ "$said" = 0 ]
    chk $? "benchmark: the hub answers after benchmark is gone, and the machine halts"
}
