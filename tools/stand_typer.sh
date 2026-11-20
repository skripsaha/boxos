# stand_typer — the typer: the keystrokes and the monitor (type_cmd,
# key_press, mon_raw, mon_shot), the shell's prompt at the end of the log
# (prompt_at_end, wait_for_prompt) and a command line made sure the machine
# took it (type_echoed, type_line).
#
# SOURCED, NEVER RUN — by stand.sh, whose harness, wait_until, WAIT_LOG,
# WAIT_QUIET_SLOW_S and SCRATCH it uses. Its own file so stand.sh stays under
# a thousand lines; moved out of it as it was.

qi_why() { tr '\n' ' ' < "$SCRATCH/qemu-input.err" 2>/dev/null; }

# ‼ A GUEST THAT WILL NOT ECHO IS NOT A HARNESS FAILURE, AND CALLING IT ONE
#   POINTS THE OPERATOR AT THE WRONG MACHINE.
#
# qemu-input.sh verifies the echo per character itself and gives up with
# "guest echoed N of M characters ... keystrokes are being dropped".
# Everything else it can fail with — no monitor socket, a socket path longer
# than sun_path, no nc, QEMU already gone — is this desk. That one sentence is
# the GUEST: a shell that takes characters and prints nothing is exactly what
# a controller whose rings nobody translates looks like, and it is a verdict
# on BoxOS.
#
# Measured, on a kernel with the write bit taken out of the identity map:
# three correct FAILs about the keyboard and the volume, and beside them a
# yellow HARNESS line saying the run "could not talk to the machine it was
# judging, so its verdicts are not evidence" — the opposite of what happened.
#
# OPT-IN per scenario, and off by default on purpose: a scenario that does not
# go on to judge the keystroke itself would otherwise have NO count for a
# guest that went deaf, and a run with nothing counted must never be green. A
# scenario turns this on exactly when a check of its own answers for it.
TYPE_SILENCE_IS_JUDGED=${TYPE_SILENCE_IS_JUDGED:-0}
type_cmd() {   # the characters of a command line; NOT the Enter after them
    ./tools/qemu-input.sh type "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    if [ "$TYPE_SILENCE_IS_JUDGED" = 1 ] \
       && grep -q "guest echoed" "$SCRATCH/qemu-input.err" 2>/dev/null; then
        echo "  (the machine took '$1' and echoed nothing back: $(qi_why))"
        echo "   That is this kernel, not this desk. The keystroke check below is"
        echo "   where it is counted."
        return 1
    fi
    harness "could not type '$1' into the machine: $(qi_why)"
    return 1
}
key_press() {
    ./tools/qemu-input.sh key "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    harness "could not send the key '$1': $(qi_why)"
    return 1
}
mon_raw() {
    ./tools/qemu-input.sh raw "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    harness "the monitor would not take '$1': $(qi_why)"
    return 1
}
mon_shot() {
    ./tools/qemu-input.sh shot "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    harness "could not screendump into '$1': $(qi_why)"
    return 1
}

# Wait until the shell is READY, which is not the same as the last command
# having printed its answer.
#
# ‼ A COMMAND THAT FINISHES IS NOT A SHELL THAT IS FREE. It still has to reap
# the child and put its own house in order, and that is reads — on a medium
# throttled to a hundred and twenty-eight bytes a second, tens of seconds of
# them. Throttling the medium in that window and typing into it is how four
# runs reported a machine that was simply busy as a machine that was broken.
#
# The prompt is `~ ` with no newline after it — `[tags] ~ ` in a Use Context
# (shell.c ShellUpdatePrompt) — so the fact is the END OF THE FILE rather than
# a line in it, once every kernel line that landed after it is taken out. The
# kernel says its own lines when they fall, not when the shell is done: a
# Tally the logger's last lines owed was written the moment its prompt was
# back (`~ [TagFS] Tally 668 is written over slot 2: ...`), the log ended on
# that line and not on `~ `, and the hour waited out the quiet window at a
# free shell and never typed its `bye` (loggerhour, one run of two). The rule
# is type_echoed's: a kernel line (`[Tag] ` to its end) taken out wherever it
# landed, what stood before it going on with what comes after — and a
# program's line in the kernel's log as well, which the log begins with the
# speaker's pid (`[3] [AUTOSTART] Started 'pulse'`): a program the start set
# launched, said after the hub's prompt, buried it, and three stands of a book
# waited out the quiet window at a free shell (the pid's `[3] ` was taken for
# what stood before the line, not for the line's own). A Use
# Context's `[tags] ` is the prompt's own and never a kernel line, so a tag a
# `~ ` follows is passed over: book_circle and bind type at `[circle,sewn] ~ `
# on a book, where a Tally falls after it as after the plain one. The line no
# newline has ended yet is read as it stands, so a kernel line not yet ended
# keeps the prompt buried until it is.
#
# Read whole — a prompt is free however many lines the kernel said after it —
# and as bytes, by tr as by awk (LC_ALL=C): tr in the host's UTF-8 locale
# stops at the first byte that is not UTF-8, wherever in the boot it fell, and
# the prompt after it was never read. The NULs go with the CRs, as awk ends a
# line at one.
prompt_at_end() {
    { cat build/serial.log 2>/dev/null; echo; } | LC_ALL=C tr -d '\r\000' |
        LC_ALL=C awk '
            function unbury(l,   at, p) {
                l = before l; before = ""; at = 0
                while (match(substr(l, at + 1), /(\[[0-9]+\] )?\[[A-Za-z][^][]*\] /)) {
                    p = at + RSTART; at = p + RLENGTH - 1
                    if (substr(l, at + 1, 2) != "~ ") { before = substr(l, 1, p - 1); return }
                }
            }
            NR > 1 { unbury(held) }
            { held = $0 }
            END { last = before held; exit !(length(last) >= 2 && substr(last, length(last) - 1) == "~ ") }'
}

wait_for_prompt() {
    # The quiet window is the SLOW one on purpose: every caller of this has
    # throttled the medium first, and a shell reading at 64 KiB/s is honestly
    # silent for a long time while it is working. Silence is the giving-up
    # fact, so it has to be long enough to mean what it says.
    wait_until "$WAIT_QUIET_SLOW_S" "the shell's prompt at the end of the log" \
        'prompt_at_end && { sleep 2; prompt_at_end; }' \
        || { echo "  (the shell never came back to its prompt)"; return 1; }
    return 0
}

# Type a command line and make sure the machine TOOK it.
#
# ‼ A KEYSTROKE THAT DOES NOT LAND IS SILENT, AND THAT COST FOUR RUNS.
#
# `qemu-input.sh type` talks to the monitor, and a monitor command that fails
# says nothing at all on the guest's serial line — the replug scenario already
# has that written down for `drive_add`. The same is true of the keystrokes
# themselves: type into a shell that is still working through the last command
# and the characters go nowhere, the scenario waits out its ceiling, and it
# reports a machine that never had a chance as a machine that failed.
#
# The characters are type_cmd's to prove — it reads each back off the echo
# and retypes the line, erased, when one did not land — so only a type_cmd
# that could not is tried again. What is left is the Enter: past the byte the
# typing began at, a line that ends on the command, and is itself ended.
#
# ‼ A LINE THE MACHINE TOOK IS NEVER TYPED AGAIN. `~ COMMAND` looked for
# anywhere in the log missed an echo whose prompt a kernel line buried
# (`~ [Boardroom] seat 1 carries ...` as a stick comes back): the second
# typing went into the program already running the first — awaycheck twice in
# three runs of four, `0 of 27` and a HARNESS verdict for a quilltest that
# took no keys — and an earlier line, or an echo with no Enter, answered for
# one never taken (typer_selfcheck, logcheck_autostart.sh).
TYPE_ECHO_QUIET_S=${TYPE_ECHO_QUIET_S:-10}

# type_echoed FROM CMD — the line, read past byte FROM with every kernel line
# (`[Tag] ` to its end, a program's `[pid] [Tag] ` too) taken out wherever it landed and what stood before it
# going on with the line after; the log's last byte marked, so a line not yet
# ended is not one. Read as bytes, as prompt_at_end reads the log.
type_echoed() {
    { tail -c "+$(($1 + 1))" "$WAIT_LOG" 2>/dev/null; printf '\001'; } | LC_ALL=C tr -d '\r\000' |
        TYPE_ECHOED=$2 LC_ALL=C awk '
            BEGIN { c = ENVIRON["TYPE_ECHOED"] }
            index($0, "\001") { exit }
            {
                l = before $0; before = ""
                if (match(l, /(\[[0-9]+\] )?\[[A-Za-z][^][]*\] /)) { before = substr(l, 1, RSTART - 1); next }
                if (length(l) >= length(c) && substr(l, length(l) - length(c) + 1) == c) { found = 1; exit }
            }
            END { exit !found }'
}

type_line() {
    local cmd="$1" attempt=0 from
    while [ $attempt -lt 2 ]; do
        from=$(wc -c 2>/dev/null < "$WAIT_LOG" | tr -d ' ')
        if type_cmd "$cmd"; then
            sleep 1
            key_press ret
            wait_until "$TYPE_ECHO_QUIET_S" "the line '$cmd' ended past the typing" \
                'type_echoed "${from:-0}" "$cmd"' && return 0
            echo "  (the machine echoed every character of '$cmd' and no line since ends on it"
            echo "   — the Enter did not land; not typed again: the machine may be running it)"
            return 1
        fi
        attempt=$((attempt+1))
    done
    echo "  (the machine never echoed '$cmd' — the keystrokes did not land)"
    return 1
}
