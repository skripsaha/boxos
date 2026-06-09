#!/bin/sh
# 20× UEFI STRICT 16c stress — same per-run burst as stress_matrix.sh
# in full mode. Reports per-run PASS/FAIL + final aggregate.
set -u

POLL_TICKS=480   # 240 seconds @ 0.5 s each (UEFI 16c host contention)

burst="memtest files bench memtest files bench memtest \
       mtest chain htest cow_test lifecycle write_observer \
       write_concurrent write_stress touch_test decks touch_stress"

mkdir -p build/uefi16c_x20_logs

PASS=0
FAIL=0

for run in $(seq 1 20); do
    echo "================================================================"
    echo "  UEFI STRICT 16c RUN ${run}/20"
    echo "================================================================"

    make run-stop >/dev/null 2>&1 || true
    sleep 1

    UEFI=on STRICT=on CORES=16 MEM=8G make run-bg >/dev/null 2>&1
    # Poll for shell prompt (UEFI GOP slow under 16c).
    for i in $(seq 1 240); do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 0.5
    done
    sleep 2

    mt_seen=0; fl_seen=0; bn_seen=0
    for c in $burst; do
        tools/qemu-input.sh type "$c" >/dev/null 2>&1
        tools/qemu-input.sh key ret  >/dev/null 2>&1
        case "$c" in
            memtest)
                mt_seen=$((mt_seen+1))
                pat="All 15 tests passed"; want=$mt_seen ;;
            files)
                fl_seen=$((fl_seen+1))
                pat="^Files:"; want=$fl_seen ;;
            bench)
                bn_seen=$((bn_seen+1))
                pat="create+write64+delete (TagFS+disk)"; want=$bn_seen ;;
            mtest)            pat="\[mtest\] PASS";           want=1 ;;
            chain)            pat="\[chain\] PASS";           want=1 ;;
            htest)            pat="\[htest\] PASS";           want=1 ;;
            cow_test)         pat="\[COW\] PASS";             want=1 ;;
            lifecycle)        pat="\[LIFECYCLE\] PASS";       want=1 ;;
            write_observer)   pat="\[WO\] PASS";              want=1 ;;
            write_concurrent) pat="\[WC\] PASS";              want=1 ;;
            write_stress)     pat="\[WS SUMMARY\] all 3 PASS";want=1 ;;
            touch_test)       pat="\[TT SUMMARY\]";           want=1 ;;
            decks)            pat=", 0 failed";               want=1 ;;
            touch_stress)     pat="\[STRESS\] Done";          want=1 ;;
        esac
        for i in $(seq 1 $POLL_TICKS); do
            [ "$(grep -cE "$pat" build/serial.log 2>/dev/null)" -ge "$want" ] && break
            sleep 0.5
        done
    done
    sleep 2
    make run-stop >/dev/null 2>&1 || true
    sleep 1

    cp build/serial.log "build/uefi16c_x20_logs/run_$(printf '%02d' $run).log" 2>/dev/null || true

    # Aggregate same checks as stress_matrix.sh
    mt=$(grep -c "All 15 tests passed" build/serial.log)
    bn=$(grep -c "create+write64+delete (TagFS+disk)" build/serial.log)
    fl=$(grep -c "^Files:" build/serial.log)
    pn=$(grep -cE "PANIC|^\[EXCEPTION\]" build/serial.log)
    at=$(grep -c "ATRC" build/serial.log)
    un=$(grep -c "Unknown command" build/serial.log)
    tsc_good=$(grep -cE "TSC freq: 1[0-1][0-9]{5}|TSC source:.*— 1[0-1][0-9]{5} kHz" build/serial.log)

    mtest_p=$(grep -c     "\[mtest\] PASS"            build/serial.log)
    chain_p=$(grep -c     "\[chain\] PASS"            build/serial.log)
    cow_p=$(grep -c       "\[COW\] PASS"              build/serial.log)
    lc_p=$(grep -c        "\[LIFECYCLE\] PASS"        build/serial.log)
    wo_p=$(grep -c        "\[WO\] PASS"               build/serial.log)
    wc_p=$(grep -c        "\[WC\] PASS"               build/serial.log)
    ws_p=$(grep -c        "\[WS SUMMARY\] all 3 PASS" build/serial.log)
    tt_p=$(grep -c        "\[TT SUMMARY\]"            build/serial.log)
    decks_p=$(grep -c     ", 0 failed"                build/serial.log)
    ts1_p=$(grep -cE      "\[STRESS S1\].*PASS"       build/serial.log)
    ts2_p=$(grep -cE      "\[STRESS S2\].*PASS"       build/serial.log)
    ts3_p=$(grep -cE      "\[STRESS S3\].*PASS"       build/serial.log)
    app_fail=$(grep -cE  "\[(mtest|chain|COW|LIFECYCLE|WO|WC|WS|TT|decks|STRESS)[^]]*\].*(\bFAIL\b|\bfail\b)" build/serial.log)

    echo "  baseline: memtest=$mt/3 files=$fl/2 bench=$bn/2"
    echo "  apps:     mtest=$mtest_p chain=$chain_p cow=$cow_p lc=$lc_p wo=$wo_p wc=$wc_p ws=$ws_p tt=$tt_p decks=$decks_p ts=$ts1_p/$ts2_p/$ts3_p"
    echo "  negatives: PANIC=$pn ATRC=$at Unknown=$un AppFAIL=$app_fail TSC=~1GHz:$tsc_good/$bn"

    ok=1
    [ "$mt" -lt 3 ] && ok=0
    [ "$fl" -lt 2 ] && ok=0
    [ "$bn" -lt 2 ] && ok=0
    [ "$pn" -gt 0 ] && ok=0
    [ "$at" -gt 0 ] && ok=0
    [ "$un" -gt 0 ] && ok=0
    [ "$tsc_good" -lt "$bn" ] && ok=0
    [ "$mtest_p" -lt 1 ] && ok=0
    [ "$chain_p" -lt 1 ] && ok=0
    [ "$cow_p"   -lt 1 ] && ok=0
    [ "$lc_p"    -lt 1 ] && ok=0
    [ "$wo_p"    -lt 1 ] && ok=0
    [ "$wc_p"    -lt 1 ] && ok=0
    [ "$ws_p"    -lt 1 ] && ok=0
    [ "$tt_p"    -lt 1 ] && ok=0
    [ "$decks_p" -lt 1 ] && ok=0
    [ "$ts1_p"   -lt 1 ] && ok=0
    [ "$ts2_p"   -lt 1 ] && ok=0
    [ "$ts3_p"   -lt 1 ] && ok=0
    [ "$app_fail" -gt 0 ] && ok=0

    if [ $ok -eq 1 ]; then
        echo "  RESULT: PASS"
        PASS=$((PASS+1))
    else
        echo "  RESULT: FAIL"
        FAIL=$((FAIL+1))
    fi
    echo ""
done

echo "================================================================"
echo "  UEFI STRICT 16c × 20: PASS=$PASS  FAIL=$FAIL"
echo "================================================================"
[ $FAIL -eq 0 ] && exit 0 || exit 1
