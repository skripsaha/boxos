# logcheck_autostart — what the AUTOSTART lines say and the console a late
# launch leaves behind: a file's tag among those its process wears (healthy,
# uefi), every program of a volume that arrives late held until the kernel has
# said what it started (replugwrite on either format, latelaunchmut), and the
# typer reading a command's echo off a console whose prompt a kernel line may
# have buried, and the shell's prompt read at its end whatever the kernel said
# after it (replugwrite on either format, loggerhour).
#
# SOURCED, NEVER RUN — by logcheck.sh; ok, bad, chk, harness, build, mutate,
# mut_done, restore, type_line, prompt_at_end, wait_for_line, wait_for_quiet,
# mon_raw and SCRATCH are stand.sh's, latearrival_on/off and stick_from_boot_image
# logcheck.sh's. Its own file for the reason logcheck_mkfs.sh has one:
# logcheck.sh is twelve thousand lines.

# autostart_wears LOG FILE TAG — the AUTOSTART line of FILE (a pattern) names
# TAG among the tags its process wears, where the format keeps it: format v1
# keeps a file's tags as create_tagfs gave them, its name's stem first, so it
# is held first there, as it always was; a book keeps them by ascending id
# (spec_v2 §2.8, Born), the well-known tags before the stem — `display.box`
# wears `system,utility,display` off a book — so it is held among.
autostart_wears() {
    local at='([^)]*,)?' end='[,)]'
    [ "$(stand_format)" = v1 ] && { at=''; end=','; }
    grep -qE "\[AUTOSTART\] Started '$2' \(PID [0-9]+, tags: $at$3$end" "$1"
}

# replugwrite_spoke_after LOG — the volume's hub greeted after the kernel said
# what its late launch started: the greeting is not among the kernel's lines
# about the launch. What the greeting is followed by — a kernel line a
# remount prints after the prompt — it does not hold; the typer reads past
# that (type_line, typer_selfcheck).
replugwrite_spoke_after() {
    tr -d '\r' < "$1" | awk '/a volume arrived after the boot had given up/ { h = NR }
        /^BoxOS Hub/ && h { b = NR } END { exit !(h && b > h) }'
}

# ‼ THE ORDER ON THE CONSOLE IS A RACE; THE STATE AS THE LAUNCH IS SAID IS NOT.
#
# A launch that lets each program run the moment it is made buried the volume's
# prompt in its own lines two runs of three on a book; the third came out in
# order, so the banner checked above passes the old launch by luck. A pause
# before the launch's line would not make it certain either: the launch runs
# in a K-Core's service pass, and the shell's pockets may be queued to that
# very core (kcore_find_least_loaded), the pause silencing the shell with it.
# So the kernel is asked, at the very point it says what it started
# (autostart.c, before `a volume arrived after the boot had given up`), what
# state every process is in: one line each, read from the process
# list — the launch's newest first — and printed once the list is let go of.
# A program the launch made and held is still as made, PROC_CREATED; one let
# run is not, whether it has reached its core yet or not. The anchor is the
# summary's own two lines, the same before the hold and after it.
LATELAUNCH_MARK="logcheck injection: the late launch's programs as it is said"
LATELAUNCH_REFUSED_MARK="logcheck mutation: no room to hold"
LATELAUNCH_STOPPED_MARK="logcheck injection: a held program stopped"

# latelaunch_on [refused|stopped] — latearrival's machine, its late launch
# saying the state of its programs; `refused`: no program can be held
# (AutostartHold answers no room), so each runs as it is made, as before the
# hold; `stopped`: the newest held program stopped as a person stops one
# (SysTagAdd: the `stopped` tag worn, PROC_STOPPED) before the launch lets
# them run.
latelaunch_on() {
    latearrival_on
    mutate src/kernel/core/process/autostart.c
    LATELAUNCH_MODE=${1:-} python3 - <<'EOF'
import os
p = "src/kernel/core/process/autostart.c"
s = open(p).read()
a = ('            __atomic_store_n(&g_volume_launched, 1u, __ATOMIC_RELEASE);\n'
     '            kprintf("[AUTOSTART] a volume arrived after the boot had given up "\n')
assert s.count(a) == 1, "late-launch said anchor missing"
said = """            {   /* logcheck injection: the late launch's programs as it is said */
                uint32_t said_pid[16], said_n = 0;
                bool     said_held[16];
                process_list_lock();
                for (process_t *q = process_get_first(); q && said_n < 16; q = q->next) {
                    said_pid[said_n]    = q->pid;
                    said_held[said_n++] = q->state == PROC_CREATED;
                }
                process_list_unlock();
                for (uint32_t i = 0; i < said_n; i++)
                    kprintf("[AUTOSTART] logcheck injection: as the launch is said, PID %u %s\\n",
                            said_pid[i], said_held[i] ? "is held" : "was let run");
            }
"""
s = s.replace(a, said + a, 1)
if os.environ.get("LATELAUNCH_MODE") == "refused":
    a = "static bool AutostartHold(AutostartHeld *held, process_t *proc)\n{\n"
    assert s.count(a) == 1, "late-launch hold anchor missing"
    s = s.replace(a, a + "    (void)held; (void)proc; return false;   /* logcheck mutation: no room to hold */\n", 1)
if os.environ.get("LATELAUNCH_MODE") == "stopped":
    a = "static void AutostartLetRun(AutostartHeld *held)\n{\n"
    assert s.count(a) == 1, "late-launch let-run anchor missing"
    s = s.replace(a, a + """    if (held->count) {   /* logcheck injection: a held program stopped */
        process_t *stop = held->procs[held->count - 1];
        process_add_tag(stop, "stopped");
        process_set_state(stop, PROC_STOPPED);
        kprintf("[AUTOSTART] logcheck injection: PID %u stopped while it was held\\n", stop->pid);
    }
""", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/core/process/autostart.c "$LATELAUNCH_MARK" 1
    case "${1:-}" in
        refused) mut_done src/kernel/core/process/autostart.c "$LATELAUNCH_REFUSED_MARK" 1 ;;
        stopped) mut_done src/kernel/core/process/autostart.c "$LATELAUNCH_STOPPED_MARK" 1 ;;
    esac
}
latelaunch_off() {
    restore src/kernel/core/process/autostart.c
    latearrival_off
}

# latelaunch_held LOG — every PID the late launch's Started lines name was
# held, not let run, when the launch was said; none named is no launch held.
latelaunch_held() {
    tr -d '\r' < "$1" | awk '
        /^\[AUTOSTART\] Started .* \(PID [0-9]+, tags: / { p = $0; sub(/.*\(PID /, "", p); sub(/,.*/, "", p); made[p] = 1; n++ }
        /^\[AUTOSTART\] logcheck injection: as the launch is said, PID [0-9]+ is held$/ { held[$(NF - 2)] = 1 }
        END { if (!n) exit 1; for (p in made) if (!(p in held)) exit 1 }'
}

# latelaunch_unheld_said LOG — every PID the late launch's Started lines name
# has its line that no room could be had to hold it, and runs at once.
latelaunch_unheld_said() {
    tr -d '\r' < "$1" | awk '
        /^\[AUTOSTART\] Started .* \(PID [0-9]+, tags: / { p = $0; sub(/.*\(PID /, "", p); sub(/,.*/, "", p); made[p] = 1; n++ }
        /^\[AUTOSTART\] no memory to hold PID [0-9]+ back until the launch is said — it runs at once$/ { said[$7] = 1 }
        END { if (!n) exit 1; for (p in made) if (!(p in said)) exit 1 }'
}

# ‼ THE TYPER TAKES WHAT THE MACHINE ECHOED, WHEREVER THE KERNEL PUT ITS LINES.
#
# type_line (stand.sh) once looked for `~ COMMAND` anywhere in the log: a
# command echoed on a line of its own — the prompt buried under a kernel line,
# `~ [Boardroom] seat 1 carries the volume ...` as a stick comes back — was
# never its echo, so it typed the command a second time into the program
# already running it (awaycheck ran twice in three runs of four; with a
# quilltest that took no keys, `0 of 27` and a HARNESS verdict on a machine
# that had taken every key). And a command typed earlier in the log, or the
# characters echoed with no Enter after them, answered for a line the machine
# never took. typer_case plays the machine on a log of the host's making —
# the characters echoed as the case lays them, the Enter as the line's end or
# not at all — and holds what type_line answers and how often it typed.
TYPER_CMD="quilltest awaycheck _qtyper 3 32"

# typer_case NAME LOG ECHO ENTER WANT — type_line run on LOG (printf %b), the
# machine echoing ECHO for the characters and ENTER for the Enter: its answer
# and the times it typed, "RC TYPED", held to WANT.
typer_case() {
    local name=$1 log=$2 echoed=$3 enter=$4 want=$5 dir="$SCRATCH/typer" got
    rm -rf "$dir"; mkdir -p "$dir/build"
    printf '%b' "$log" > "$dir/build/serial.log"
    got=$(cd "$dir" && {
        typed=0
        type_cmd()  { typed=$((typed + 1)); printf '%b' "$echoed" >> build/serial.log; }
        key_press() { printf '%b' "$enter" >> build/serial.log; }
        TYPE_ECHO_QUIET_S=2
        type_line "$TYPER_CMD" >/dev/null
        echo "$? $typed"; })
    [ "$got" = "$want" ]
    chk $? "typer: $name — type_line answers ${want% *} and types once (answered and typed: ${got:-nothing})"
}

typer_selfcheck() {
    local c=$TYPER_CMD
    typer_case "the prompt last in the log, the line taken" \
        "[TagFS] the volume on seat 1 is MOUNTED\r\n~ " "$c" "\r\n" "0 1"
    typer_case "the prompt buried under the kernel's lines, the echo on a line of its own" \
        "~ [Boardroom] seat 1 carries the volume this kernel was read out of\r\n[TagFS] the volume on seat 1 is MOUNTED\r\n" \
        "$c" "\r\n" "0 1"
    typer_case "a kernel line printed inside the echo" \
        "~ " "quilltest aw[TagFS] a whole Tally of the book is written\r\naycheck _qtyper 3 32" "\r\n" "0 1"
    typer_case "the Enter lost, the same line typed and answered before" \
        "~ $c\r\n[QUILL] awaycheck: PASS\r\n~ " "$c" "" "1 1"
    typer_case "the Enter lost after a prompt" "~ " "$c" "" "1 1"
    typer_case "a byte no locale reads in a kernel line inside the echo" \
        "~ " "quilltest aw[TagFS] \xff\xfe\r\naycheck _qtyper 3 32" "\r\n" "0 1"
    prompt_selfcheck
}

# ‼ A FREE SHELL IS ITS PROMPT AT THE END OF THE LOG, WHATEVER THE KERNEL SAID
# AFTER IT. The logger's last lines owed a Tally, written the moment its prompt
# was back (`~ [TagFS] Tally 668 is written over slot 2: ...`): the log ended
# on the kernel's line, wait_for_prompt (stand_typer.sh) held a free shell for
# a busy one through its quiet window, `bye` was never typed, and the hour
# said the machine never stopped (loggerhour). The prompt of a Use Context,
# `[circle,sewn] ~ `, is the prompt and not a kernel line, and a byte the
# host's locale does not read is a byte, wherever in the boot it fell.
# prompt_case plays the log on the host and holds what prompt_at_end answers:
# 0 at the prompt, 1 while the shell is not there yet.
prompt_case() {
    local name=$1 log=$2 want=$3 dir="$SCRATCH/prompt" got
    rm -rf "$dir"; mkdir -p "$dir/build"
    printf '%b' "$log" > "$dir/build/serial.log"
    got=$(cd "$dir" && { prompt_at_end; echo $?; })
    [ "$got" = "$want" ]
    chk $? "prompt: $name — the shell's prompt at the end of the log answers $want (answered: ${got:-nothing})"
}

prompt_selfcheck() {
    local ended="[QUILL] logger: PASS — loggerh-g.log (fid 81): 3600 lines in 3599003 ms\r\n"
    local tally="[TagFS] Tally 668 is written over slot 2: the state after stitch 667"
    local running="~ quilltest logger loggerh 3600 600\r\n" used="[circle,sewn] ~ "
    prompt_case "the prompt last in the log" "$ended~ " 0
    prompt_case "a Tally said after the prompt" "$ended~ $tally\r\n" 0
    prompt_case "two kernel lines said after it" "$ended~ $tally\r\n[KLIB] a\r\n" 0
    prompt_case "a kernel line after it not yet ended" "$ended~ $tally" 1
    prompt_case "a key typed after it" "$ended~ b" 1
    prompt_case "the logger running, its progress said" "$running[QUILL] logger: 30 of 3600 lines appended, 0 anchor(s)\r\n" 1
    prompt_case "the logger running, a Tally said under it" "$running$tally\r\n" 1
    prompt_case "a Use Context's prompt last in the log" "$ended$used" 0
    prompt_case "a Tally said after a Use Context's prompt" "$ended$used$tally\r\n" 0
    prompt_case "a kernel line after a Use Context's prompt not yet ended" "$ended$used$tally" 1
    prompt_case "a command typed at a Use Context's prompt" "$ended${used}quilltest names circle" 1
    prompt_case "a byte no locale reads early in the boot" "boot \xff\xfe\r\n$ended~ " 0
    prompt_case "a byte 1 and a NUL in the log" "\001boot\0000\r\n$ended\0000~ " 0
}

# latelaunch_leg MODE LOG — latearrival's machine with its late launch said
# (latelaunch_on MODE), the stick plugged in, and LOG once the volume's hub
# has greeted and the console gone quiet; 1, said, when that never came.
latelaunch_leg() {
    local mode=$1 L=$2 rc
    latelaunch_on "$mode"; build
    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latelaunch_off; return 1; }
    wait_for_line build/serial.log "$NO_VOLUME_YET" "$WAIT_QUIET_S"
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" &&
        wait_for_quiet "$BOOT_BURST_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$L"
    latelaunch_off
    [ $rc = 0 ] || harness "latelaunchmut ($mode): the stick never became this machine's volume"
    return $rc
}

# latelaunchmut — two turns of the late launch. The hold taken away
# (AutostartHold answers no room): each program says it runs at once and does,
# and the oracle that holds every program to the launch's line,
# latelaunch_held, turns. A held program stopped before the launch lets it
# run, as a person stops one: it stays stopped, said, and the others run.
run_latelaunchmut() {
    echo "== latelaunchmut: a late launch that cannot hold its programs says so and the oracle turns; a program stopped while held stays stopped =="
    local L="$SCRATCH/serial.latelaunchmut" stopped
    if latelaunch_leg refused "$L.refused.log"; then
        latelaunch_unheld_said "$L.refused.log"
        chk $? "latelaunchmut: every program the late launch made says no room held it and that it runs at once"
        ! latelaunch_held "$L.refused.log"
        chk $? "latelaunchmut: the oracle turns — $(grep -ac 'as the launch is said, PID [0-9]* was let run' "$L.refused.log") program(s) said let run before the launch is said"
        ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$L.refused.log"
        chk $? "latelaunchmut: no panic, no exception, nothing held"
    fi
    if latelaunch_leg stopped "$L.stopped.log"; then
        latelaunch_held "$L.stopped.log"
        chk $? "latelaunchmut: every program held when the launch is said"
        stopped=$(tr -d '\r' < "$L.stopped.log" | sed -nE 's/^\[AUTOSTART\] logcheck injection: PID ([0-9]+) stopped while it was held$/\1/p')
        [ -n "$stopped" ] && tr -d '\r' < "$L.stopped.log" |
            grep -qx "\[AUTOSTART\] PID $stopped was stopped or ended before the launch let it run — left as it stands"
        chk $? "latelaunchmut: the program stopped while held stays stopped, said (PID ${stopped:-none stopped})"
        [ "$(grep -ac 'before the launch let it run' "$L.stopped.log")" = 1 ]
        chk $? "latelaunchmut: and only it — every other held program let run"
        ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$L.stopped.log"
        chk $? "latelaunchmut: no panic, no exception, a program stopped while held"
    fi
}
