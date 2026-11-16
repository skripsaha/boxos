# logcheck_loadersettings — the loadersettings scenario of tools/logcheck.sh:
# gangway.settings, the person's choices for the loader and the kernel
# (src/include/boxos_gangway_settings.h), set in the hub by the utility
# settings and taken at the next start — Gangway finds the file by its name
# and holds it to the book (gangway_lectern_called.c), takes the screen for
# itself (gangway_settings.c, gangway_video.c) and hands the rest on as stamps
# of the Boarding Pass, which the kernel reads first of all (boarding.c): the
# Emergency Screen from the first line, the cores it starts (amp.c), the
# carry-over window (klib_logring.c).
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `loadersettings` to
# run_loadersettings here; everything it calls (build, chk, bad, harness, the
# waits, the typing) is stand.sh's, stand_typer.sh's or logcheck.sh's.

# ---------------------------------------------------------------------------
# loadersettings: a UEFI machine of four cores boots with no gangway.settings
# — Gangway says none is called so, the kernel starts every core and keeps no
# log through a reset. In the hub the person sets the screen to 1024x768,
# startup all, cores 2 and keep-log on, and the panel's look — text size 24,
# text font bold, theme light, colors choice #4060E0, logo off, and colors
# text set and then said back to default — each said to take effect when it
# starts next, and halts the machine through bye. At the next start Gangway
# reads the file, held to the book, says what it sets — the colour said back
# to default not among it, the theme's again — and chooses the 1024x768 mode
# by it; the kernel says what the person asks of it, starts two
# of the four cores, arms the carry-over window, and the display draws on the
# glass Gangway chose. The image is put back as it was after.
# ---------------------------------------------------------------------------

loadersettings_boot() {
    local leg=$1
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI=on CORES=4 MEM=4G >"$SCRATCH/loadersettings.$leg.run-bg" 2>&1; then
        harness "loadersettings ($leg): run-bg would not start: $(tail -1 "$SCRATCH/loadersettings.$leg.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" || return 1
    sleep 3
    return 0
}

# A command typed into the hub, and its answer waited for past the line the
# typing began on — the echo of what was typed is on the serial line too.
loadersettings_say() {
    local from
    from=$(wc -l < build/serial.log | tr -d ' ')
    type_cmd "$1" || return 1
    sleep 1
    key_press ret || return 1
    wait_for_line build/serial.log "$2" "$WAIT_QUIET_S" $((from + 1)) && sleep 1
}

run_loadersettings() {
    echo "== loadersettings: gangway.settings set in the hub, taken by Gangway and the kernel at the next start =="
    build
    cp build/boxos.img "$SCRATCH/loadersettings.before.img"

    local L A
    if loadersettings_boot first; then
        loadersettings_say "settings gangway" "keep-log" &&
            loadersettings_say "settings gangway screen 1024x768" "when it starts next" &&
            loadersettings_say "settings gangway startup all" "when it starts next" &&
            loadersettings_say "settings gangway cores 2" "when it starts next" &&
            loadersettings_say "settings gangway keep-log on" "when it starts next" &&
            loadersettings_say "settings gangway text size 24" "when it starts next" &&
            loadersettings_say "settings gangway text font bold" "when it starts next" &&
            loadersettings_say "settings gangway theme light" "when it starts next" &&
            loadersettings_say "settings gangway colors choice #4060E0" "when it starts next" &&
            loadersettings_say "settings gangway colors text #FFFFFF" "when it starts next" &&
            loadersettings_say "settings gangway colors text default" "when it starts next" &&
            loadersettings_say "settings gangway logo off" "when it starts next"
        type_cmd "bye" && sleep 1 && key_press ret &&
            wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
        sleep 2
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$SCRATCH/serial.loadersettings.first.log"
    fi
    L="$SCRATCH/serial.loadersettings.first.log"

    grep -aq "Gangway: no file on this volume is called gangway.settings" "$L"
    chk $? "loadersettings (first): Gangway finds no gangway.settings, and says so"
    grep -aq "\[AMP\] Detected 4 core(s)" "$L"
    chk $? "loadersettings (first): with none, the kernel starts every core of the four"
    grep -aq "\[BOOT\] LogKeep is not asked for" "$L"
    chk $? "loadersettings (first): and keeps no log through a reset, saying it is not asked for"
    [ "$(grep -ac "when it starts next" "$L")" -ge 11 ]
    chk $? "loadersettings (first): settings takes each of the eleven, to take effect when it starts next"
    grep -aqE "$STAND_HALT_ENDS" "$L"
    chk $? "loadersettings (first): the machine halts through bye, the volume synced"

    if loadersettings_boot second; then
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$SCRATCH/serial.loadersettings.second.log"
    fi
    A="$SCRATCH/serial.loadersettings.second.log"
    cp "$SCRATCH/loadersettings.before.img" build/boxos.img

    grep -aqE "Gangway: gangway.settings \(fid [0-9]+\) is attested by the book" "$A"
    chk $? "loadersettings (second): Gangway reads gangway.settings, held to the book ($(grep -a 'Gangway: gangway.settings (fid' "$A" | tr -d '\r' | head -1 | cut -c10-))"
    grep -aq "Gangway: gangway.settings sets screen 1024x768, startup all, cores 2, keep-log on, text size 24, text font bold, theme light, colors choice #4060E0, logo off" "$A"
    chk $? "loadersettings (second): and says what it sets: screen 1024x768, startup all, cores 2, keep-log on, text size 24, text font bold, theme light, colors choice #4060E0, logo off — colors text, said back to default, the theme's again"
    grep -aq "choice=settings" "$A" && grep -aq "Gangway: GOP 1024x768 " "$A"
    chk $? "loadersettings (second): Gangway chooses the 1024x768 mode by the file ($(grep -a 'Gangway: GOP modes=' "$A" | tr -d '\r' | head -1 | cut -c10-))"
    grep -aq "\[Boarding\]   the person asks of the kernel: startup all cores 2 keep-log on" "$A"
    chk $? "loadersettings (second): the kernel reads the three stamps and says what the person asks of it"
    grep -aq "\[AMP\] the person asks for 2 core(s): 2 of the 4 the machine has are started" "$A" &&
        grep -aq "\[AMP\] Detected 2 core(s)" "$A"
    chk $? "loadersettings (second): it starts two of the four cores"
    grep -aq "\[BOOT\] LogKeep is armed" "$A"
    chk $? "loadersettings (second): it arms the carry-over window — no PRINTTOFILE build asked for it"
    grep -aqE "\[display\] text: .* 146x48 of them" "$A"
    chk $? "loadersettings (second): the display draws on the glass Gangway chose ($(grep -a '\[display\] text:' "$A" | tr -d '\r' | head -1))"
    grep -aq "BoxOS Hub" "$A"
    chk $? "loadersettings (second): and the machine reaches the hub"
}
