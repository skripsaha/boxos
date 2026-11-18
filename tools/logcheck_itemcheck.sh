# shellcheck shell=bash
# logcheck_itemcheck.sh — the items' language held on the machine (sourced by
# tools/logcheck.sh; run by the itemcheck scenario).
#
# The image is booted twice: by BIOS on a text page, and by UEFI on a
# framebuffer. Each time `itemcheck` is typed at the hub: it opens an area over
# the screen, says a scene of every kind of item in one step, then runs of
# steps as a program makes them — a word changed, a shape moved, a group
# faded — and says what each run's frames cost as the display told it back
# (src/userspace/utils/itemcheck). The gate holds that the area opened on the
# glass the machine has, that every run was shown and said, that the last
# line says PASS — every step shown, nothing refused, and on a framebuffer a
# word changed composing a small part of the area — that the display never
# closed an area for breaking the language, and that the hub still answers
# after the program is gone. The times are kept in the log, a guide only
# under TCG.

itemcheck_boot() {   # itemcheck_boot NAME [make-args…]
    local name=$1
    shift
    make run-stop >/dev/null 2>&1
    if ! make run-bg "$@" >"$SCRATCH/itemcheck.$name.run-bg" 2>&1; then
        harness "itemcheck ($name): run-bg would not start: $(tail -1 "$SCRATCH/itemcheck.$name.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && sleep 3 &&
        drawcheck_say "itemcheck" 'itemcheck: (PASS|FAIL)' "$WAIT_QUIET_SLOW_S" && sleep 1 &&
        drawcheck_say "bye" "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    local rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.itemcheck.$name.log"
    return $rc
}

run_itemcheck() {
    echo "== itemcheck: a program's items said, shown a step at a time, and what each frame cost, on the machine =="
    if ! make >"$SCRATCH/build.log" 2>&1; then
        bad "itemcheck: make does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    local name glass passed said L run held
    for name in text framebuffer; do
        if [ $name = text ]; then
            itemcheck_boot $name
            said=$?
            glass="a text page"
            passed="itemcheck: PASS — every step shown, nothing refused; a text page composes no pixels"
        else
            itemcheck_boot $name UEFI=on
            said=$?
            glass="a framebuffer"
            passed="itemcheck: PASS — every step shown, nothing refused, a word changed composing a small part of the area"
        fi
        L="$SCRATCH/serial.itemcheck.$name.log"
        grep -a "^itemcheck: " "$L" | tr -d '\r' | sed 's/^/  /'
        grep -aqE "itemcheck: an area of [0-9]+x[0-9]+ points at [0-9.]+ pixels a point, on $glass" "$L"
        chk $? "itemcheck ($name): an area opened over the screen, on $glass, its size and scale told"
        held=1
        for run in "the scene, 1 steps" "one word changed, 60 steps" "a shape moved, 60 steps" "a group faded, 60 steps"; do
            grep -aq "itemcheck: $run — " "$L" || held=0
        done
        [ $held = 1 ]
        chk $? "itemcheck ($name): the scene and three runs of 60 steps each shown, each frame's cost told back"
        grep -aqF "$passed" "$L"
        chk $? "itemcheck ($name): PASS ($(grep -aoE 'itemcheck: (PASS|FAIL)' "$L" | tail -1))"
        ! grep -aq "broke the items' language" "$L"
        chk $? "itemcheck ($name): the display closed no area for breaking the language"
        [ "$said" = 0 ]
        chk $? "itemcheck ($name): the hub answers after itemcheck is gone, and the machine halts"
    done
}
