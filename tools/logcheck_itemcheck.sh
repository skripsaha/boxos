# shellcheck shell=bash
# logcheck_itemcheck.sh — the items' language held on the machine (sourced by
# tools/logcheck.sh; run by the itemcheck scenario).
#
# The image is booted twice: by BIOS on a text page, and by UEFI on a
# framebuffer. Each time `itemcheck` is typed at the hub: it opens an area over
# the screen, says a scene of every kind of item in one step, then runs of
# steps as a program makes them — a word changed, a shape moved, a group
# faded, a cell changed — and says what each run's frames cost as the display
# told it back; then painter::gui as a program writes with it — objects, a
# frame of a list, a canvas, a text measured beforehand, the C face; then
# prints lines into the console and opens an area in its flow after them, and
# says how long until the glass showed it all (src/userspace/utils/itemcheck).
# The gate holds that the area opened on the glass the machine has, that every
# run was shown and said; that objects send only what changed, a frame only
# what differs and nothing when it is as before, a canvas is drawn page after
# page (framebuffer), a text is measured the very size the display lays it at
# (a cell a character on the text page), and the C face holds; that the console's
# lines and the area in its flow were shown, that the last line says PASS —
# every step shown, nothing refused, and on a framebuffer a word and a cell
# changed composing a small part of the area — that the display never closed
# an area for breaking the language, and that the hub still answers after the
# program is gone. The times are kept in the log, a guide only under TCG.

# itemcheck keys: once it is ready, the keys a person would type — Tab to the
# field, h i into it, Tab and → to the second button, Enter, the labels'
# chord, s for the first — and its verdict.
itemcheck_keys() {
    local from key
    from=$(wc -l < build/serial.log | tr -d ' ')
    drawcheck_say "itemcheck keys" 'itemcheck keys: ready' "$WAIT_QUIET_S" || return 1
    sleep 1
    for key in tab h i tab right ret ctrl-dot s; do
        key_press "$key" || return 1
        sleep 0.4
    done
    wait_for_line -E build/serial.log 'itemcheck keys: (PASS|FAIL)' "$WAIT_QUIET_S" $((from + 1))
}

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
        itemcheck_keys && sleep 1 &&
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
        for run in "the scene, 1 steps" "one word changed, 60 steps" "a shape moved, 60 steps" "a group faded, 60 steps" \
                   "one cell changed, 60 steps"; do
            grep -aq "itemcheck: $run — " "$L" || held=0
        done
        [ $held = 1 ]
        chk $? "itemcheck ($name): the scene and four runs of 60 steps each shown, each frame's cost told back"
        grep -aqE "itemcheck: objects — .* — ([0-9]|[1-5][0-9]|6[0-4]) bytes of the ring a step" "$L"
        chk $? "itemcheck ($name): objects told all of themselves each step, only what changed going"
        grep -aqE "itemcheck: a frame of 24 rows in a group — one row changed: [1-3] records; all reordered: [0-9]+ records; drawn again as it was: no step" "$L"
        chk $? "itemcheck ($name): a frame of a list sending only what differs, a frame as before sending nothing"
        if [ "$name" = framebuffer ]; then
            grep -aq "itemcheck: a canvas of 96x64 pixels drawn 6 times, its two pages taken in turn" "$L"
            chk $? "itemcheck ($name): a canvas of the program's own pixels drawn page after page"
            local line measured laid
            line=$(grep -a "itemcheck: a text measured beforehand .* the display laid it at" "$L" | tr -d '\r' | tail -1)
            measured=$(printf '%s' "$line" | sed -E 's/.*beforehand ([0-9.]+x[0-9.]+) points.*/\1/')
            laid=$(printf '%s' "$line" | sed -E 's/.*laid it at ([0-9.]+x[0-9.]+).*/\1/')
            [ -n "$line" ] && [ "$measured" = "$laid" ]
            chk $? "itemcheck ($name): a text measured beforehand the very size the display laid it at ($measured, $laid)"
        else
            grep -aqE "itemcheck: a text measured beforehand [0-9]+x16 points, a cell a character on the text page" "$L"
            chk $? "itemcheck ($name): a text measured on the text page a cell a character"
        fi
        local line fade chain
        line=$(grep -a "itemcheck: in time — " "$L" | tr -d '\r' | tail -1)
        fade=$(printf '%s' "$line" | sed -nE 's/.*its end told after ([0-9]+) ms;.*/\1/p')
        chain=$(printf '%s' "$line" | sed -nE 's/.*both ends told after ([0-9]+) ms;.*/\1/p')
        if [ "$name" = framebuffer ]; then
            [ -n "$fade" ] && [ "$fade" -ge 300 ] && [ "$fade" -le 450 ] && [ "$chain" -ge 400 ] && [ "$chain" -le 550 ] &&
                printf '%s' "$line" | grep -q "said over, told cut: every end as it was"
            chk $? "itemcheck ($name): things in time played by the display, each end told by its time (a fade of 300 ms: $fade; two of 200 one after another: $chain), one shown and said over told cut"
        else
            [ -n "$fade" ] && [ "$fade" -le 100 ] && [ "$chain" -le 100 ] &&
                printf '%s' "$line" | grep -q "said over, told ended: every end as it was"
            chk $? "itemcheck ($name): on the text page every change at once, each end told at once ($fade, $chain ms), one shown and said over told ended, as it went"
        fi
        grep -aqE "itemcheck: from C — .*: every step shown" "$L"
        chk $? "itemcheck ($name): painter::gui's C face held from a program's C"
        grep -aq 'itemcheck keys: the field typed "hi", pressed two by Enter then one by its label, the focus come [0-9]* times, never to the one not to be used, asked back: came' "$L" &&
            grep -aq "itemcheck keys: PASS" "$L"
        chk $? "itemcheck ($name): the keyboard alone reaches a field and types into it, presses one button by Enter and another by its label, never the one not to be used, and the program's focus asked comes back"
        grep -aqE "itemcheck: the console, [0-9]+ lines and an area in its flow — [0-9]+ ms until the glass showed them" "$L"
        chk $? "itemcheck ($name): the console's lines and an area in its flow after them shown"
        grep -aqF "$passed" "$L"
        chk $? "itemcheck ($name): PASS ($(grep -aoE 'itemcheck: (PASS|FAIL)' "$L" | tail -1))"
        ! grep -aq "broke the items' language" "$L"
        chk $? "itemcheck ($name): the display closed no area for breaking the language"
        [ "$said" = 0 ]
        chk $? "itemcheck ($name): the hub answers after itemcheck is gone, and the machine halts"
    done
}
