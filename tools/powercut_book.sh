# powercut_book — what tools/powercut.sh asks of a book (VOLUME_FORMAT=v2):
# the scenarios of format v1's own machinery refused by name, and the
# book's matrix.
#
# SOURCED, NEVER RUN — by powercut.sh; everything it calls (run_selftest,
# run_scenario, run_noanchormut and the verdicts) is powercut.sh's,
# powercut_mut.sh's or stand.sh's. Its own file so powercut.sh stays under a
# thousand lines as the book's scenarios grow.

# VOLUME_FORMAT=v2: every image of the run is a book (the Makefile's key, held
# for every make of the run by stand.sh's make through STAND_FORMAT), and the
# record is judged by the book's census and blklog_book.py — a step committed
# by its stitch (powercut.py COMMIT_KINDS), the slot's counts in the Ledger's
# place. A scenario that stands on format v1's own machinery has no book to
# ask and is refused by name: settlecut's points of the v1 cycle, chaingrow's
# registry and table chains, and the mutations of v1's mount (nextfidmut) and
# of its cycle's pool and Ledger (poolmut, flushmut).
#
# pc_book SCENARIO — 0 once the book's matrix has run (`all`), and the
# caller has nothing left to run; 1 for a scenario the common dispatch runs.
pc_book() {
    case "$PC_FORMAT:${1:-}" in
        v2:settlecut|v2:chaingrow|v2:nextfidmut|v2:poolmut|v2:flushmut)
            echo "powercut: $1 stands on format v1's own machinery and is no scenario of a book (VOLUME_FORMAT=v2)"
            exit 2 ;;
        v2:all)
            run_selftest
            for keys in "" "STRICT=on CORES=4 MEM=4G" "UEFI=on STRICT=on CORES=4 MEM=4G"; do
                # shellcheck disable=SC2086
                run_scenario noanchor $keys; run_scenario nextfid $keys
            done
            run_scenario hog; run_scenario hog UEFI=on STRICT=on
            for keys in "STRICT=on CORES=4 MEM=4G" "UEFI=on STRICT=on CORES=16 MEM=8G"; do
                # shellcheck disable=SC2086
                run_scenario halt $keys; run_scenario race $keys
            done
            run_noanchormut
            return 0 ;;
    esac
    return 1
}
