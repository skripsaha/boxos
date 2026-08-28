#!/bin/bash
# logcheck — oracle for the Logbook split (kernel occurrence vocabulary vs
# volume tag registry).
#
#   logcheck.sh healthy   boot with a mounted volume; the volume's vocabulary
#                         must be untouched and the kernel's must be complete
#   logcheck.sh novolume  boot with tagfs_recognise() forced false — the board
#                         failure reproduced on the desk. The kernel's
#                         vocabulary must survive having no medium at all.
#   logcheck.sh yank      the stick is pulled WHILE the volume is being read.
#                         The medium is throttled so the window exists at all
#   logcheck.sh twoctrl   two host controllers, stick on the SECOND — the shape
#                         the board has and no scenario could be until now
#   logcheck.sh logsave   the machine writes down what it said. Two kernels:
#                         one built PRINTTOFILE=on, which must produce a
#                         readable account of its own boot on the volume, and
#                         one without, which must SAY it keeps none
#   logcheck.sh both
#
# Every check is a grep against build/serial.log. No timing assertions.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRATCH="${TMPDIR:-/tmp}/boxos-logcheck"
mkdir -p "$SCRATCH"
cd "$ROOT" || exit 1

PASS=0; FAIL=0
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
chk()  { if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi }

build() {
    make >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi
}

# Waits for the SHELL BANNER, not for "userspace is starting".
#
# Those are not the same moment and the gap is not small: a machine with no
# display daemon spends five seconds asking for one before it gives up and
# writes the screen itself, so sampling at "Starting userspace" reports a
# machine that never reached a shell when it reaches one shortly after.
# Measured — it cost a wrong FAIL here first.
boot() {
    make run-stop >/dev/null 2>&1
    make run-bg   >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 2; i=$((i+1))
    done
    sleep 3


    # Type into it. A machine you can look at and not talk to is not a machine
    # that booted — and "can it be typed on" is the only question the board
    # failure was ever really about.
    ./tools/qemu-input.sh type "help" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh key ret >/dev/null 2>&1
    sleep 3

    # And then an EXTERNAL utility, which is a different question entirely.
    #
    # Every scenario in this file typed "help" and nothing else, and "help" is
    # a builtin — it never leaves the shell. So none of them could reach the
    # path where the shell spawns a program and waits for it to die, and that
    # path was broken for as long as the Logbook split has existed: the shell
    # parked on a tag nothing published to, and the prompt never came back.
    # Five green scenarios, and the machine was unusable from its second
    # command onward.
    ./tools/qemu-input.sh type "hw" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh key ret >/dev/null 2>&1
    sleep 4

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

# Did the machine answer a keystroke? The shell prints its command list in
# response to "help", so one line out of that list is the proof.
typed_ok() {
    grep -q "Show available commands" "$1"
}

# Did the shell come BACK after running a program?
#
# The prompt is printed by every pass of the main loop, so a prompt appearing
# after the utility's own output is the proof that the loop went round. A
# machine that runs one command and then ignores the keyboard for ever looks,
# from a photograph, exactly like a machine that is working.
external_ok() {
    sed -n '/CPU features/,$p' "$1" | grep -qE '^~ ?$|^~ '
}

# The probe reports every resolve and which book answered. It is installed
# only for the run and removed after, so the shipped tree never carries it.
probe_on() {
    cp src/kernel/core/touch/logbook.c "$SCRATCH/logbook.c.bak"
    python3 - <<'EOF'
p = "src/kernel/core/touch/logbook.c"
s = open(p).read()
anchor = """void TouchLogbookResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    logbook_resolve(tag, out_full, out_bare, true);
}"""
probed = """void TouchLogbookResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    logbook_resolve(tag, out_full, out_bare, true);
    kprintf("[PROBE] logbook '%s' full=0x%x bare=0x%x\\n", tag, *out_full, *out_bare);
}"""
assert anchor in s, "probe anchor missing"
open(p, "w").write(s.replace(anchor, probed))
EOF
    grep -q "PROBE" src/kernel/core/touch/logbook.c || { echo "probe install FAILED"; exit 1; }
    sleep 1; touch src/kernel/core/touch/logbook.c
}
probe_off() {
    cp "$SCRATCH/logbook.c.bak" src/kernel/core/touch/logbook.c
    sleep 1; touch src/kernel/core/touch/logbook.c
}

mutate_on() {   # reproduce "no volume on this machine"
    cp src/kernel/tagfs/tagfs.c "$SCRATCH/tagfs.c.bak"
    python3 - <<'EOF'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])\n{\n"
assert anchor in s, "mutation anchor missing"
s = s.replace(anchor, anchor + "    return false;   /* logcheck mutation: this machine mounted nothing */\n", 1)
open(p, "w").write(s)
EOF
    grep -q "logcheck mutation" src/kernel/tagfs/tagfs.c || { echo "mutation install FAILED"; exit 1; }
    sleep 1; touch src/kernel/tagfs/tagfs.c
}
mutate_off() {
    cp "$SCRATCH/tagfs.c.bak" src/kernel/tagfs/tagfs.c
    sleep 1; touch src/kernel/tagfs/tagfs.c
}

# The boarding pass names a volume that is not in the room, while another
# perfectly good TagFS volume is. This is the case that used to mount the
# stranger in silence and then ignore the real medium for the rest of the boot.
stranger_on() {
    cp src/kernel/core/boarding/boarding.c "$SCRATCH/boarding.c.bak"
    python3 - <<'EOF'
p = "src/kernel/core/boarding/boarding.c"
s = open(p).read()
anchor = """    if (out_uuid) {
        memcpy(out_uuid, v->uuid, 16);
    }
    return true;
}"""
mutated = """    if (out_uuid) {
        memcpy(out_uuid, v->uuid, 16);
        out_uuid[0] ^= 0xFF;   /* logcheck mutation: a pass naming an absent volume */
    }
    return true;
}"""
assert anchor in s, "stranger anchor missing"
open(p, "w").write(s.replace(anchor, mutated, 1))
EOF
    grep -q "logcheck mutation" src/kernel/core/boarding/boarding.c || { echo "stranger install FAILED"; exit 1; }
    sleep 1; touch src/kernel/core/boarding/boarding.c
}
stranger_off() {
    cp "$SCRATCH/boarding.c.bak" src/kernel/core/boarding/boarding.c
    sleep 1; touch src/kernel/core/boarding/boarding.c
}

# The volume turns up AFTER the boot gave up on it — a stick pushed in while
# the machine is running. Mutation: the internal disk is declared not to be
# this machine's, so the boot finds nothing; then a USB stick carrying the real
# image is hot-plugged through the QEMU monitor and the whole chain has to run
# by itself — enumerate, seat, recognise by boarding pass, mount, start the
# volume's programs, and relieve the stand-in shell.
#
# Four cores, because that chain is driven from the K-Core guide loop. On a
# single core it does not run at all — see the note at the end of this file.
# Wait until the shell is READY, which is not the same as the last command
# having printed its answer.
#
# ‼ A COMMAND THAT FINISHES IS NOT A SHELL THAT IS FREE. It still has to reap
# the child and put its own house in order, and that is reads — on a medium
# throttled to a hundred and twenty-eight bytes a second, tens of seconds of
# them. Throttling the medium in that window and typing into it is how four
# runs reported a machine that was simply busy as a machine that was broken.
#
# The prompt is two bytes, `~ ` with no newline after it, so the fact is the
# END OF THE FILE rather than a line in it — read as hex, because command
# substitution eats the trailing space that is half the evidence.
wait_for_prompt() {
    local i=0
    while [ $i -lt 300 ]; do
        if [ "$(tail -c 2 build/serial.log 2>/dev/null | xxd -p)" = "7e20" ]; then
            sleep 2
            [ "$(tail -c 2 build/serial.log 2>/dev/null | xxd -p)" = "7e20" ] && return 0
        fi
        sleep 1; i=$((i+1))
    done
    echo "  (the shell never came back to its prompt)"
    return 1
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
# The shell echoes what it was given, so the echo is the fact that it landed.
# Tried twice, and said out loud if it still did not, because a scenario that
# quietly tests nothing is worse than one that fails.
type_line() {
    local cmd="$1" attempt=0 i
    while [ $attempt -lt 2 ]; do
        ./tools/qemu-input.sh type "$cmd" >/dev/null 2>&1
        sleep 1
        ./tools/qemu-input.sh key ret >/dev/null 2>&1
        i=0
        while [ $i -lt 10 ]; do
            grep -q "~ $cmd" build/serial.log 2>/dev/null && return 0
            sleep 1; i=$((i+1))
        done
        attempt=$((attempt+1))
    done
    echo "  (the machine never echoed '$cmd' — the keystrokes did not land)"
    return 1
}

latearrival_on() {
    cp src/kernel/tagfs/tagfs.c "$SCRATCH/tagfs.late.bak"
    python3 - <<'EOF'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])\n{\n"
assert anchor in s, "late-arrival anchor missing"
s = s.replace(anchor, anchor + "    if (BoardroomSeatKind(seat) == BOARD_ATA) return false;   /* logcheck mutation: the internal disk is not this machine's */\n", 1)
open(p, "w").write(s)
EOF
    grep -q "logcheck mutation" src/kernel/tagfs/tagfs.c || { echo "late-arrival install FAILED"; exit 1; }
    sleep 1; touch src/kernel/tagfs/tagfs.c
}
latearrival_off() {
    cp "$SCRATCH/tagfs.late.bak" src/kernel/tagfs/tagfs.c
    sleep 1; touch src/kernel/tagfs/tagfs.c
}

run_latearrival() {
    echo "== latearrival: the volume arrives after the boot gave up =="
    # The mutation stays installed until the run is over: `make run-bg` has the
    # image as a prerequisite and will rebuild it, so restoring the source
    # first quietly boots an unmutated kernel. Cost an entire run to learn.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 30 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done

    ./tools/qemu-input.sh raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh raw "device_add usb-storage,drive=stick,id=usbstick" >/dev/null 2>&1

    #
    # Waited out to the END of the chain.
    #
    # This used to break on the FIRST "AUTOSTART Started" — which is
    # display.elf — and then stop the machine two seconds later. Reading the
    # rest of the volume's programs off a USB stick takes longer than that
    # often enough that the two checks below were passing on luck: the machine
    # was killed mid-launch and the log was then searched for a line it had
    # not had time to print. Measured — it went red on a tree whose kernel had
    # nothing wrong with it.
    i=0
    while [ $i -lt 60 ]; do
        grep -q "hands over" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.latearrival.log"
    latearrival_off
    L="$SCRATCH/serial.latearrival.log"

    grep -q "no volume yet" "$L";      chk $? "the boot found no volume of its own"
    grep -q "Fallback shell ready" "$L"; chk $? "and stood a shell in for one"
    grep -q "mass storage" "$L";       chk $? "the stick enumerated after the boot"
    grep -q "arrived after the room was called to order\|seat 1:" "$L"
    chk $? "the room seated it"
    grep -q "seat 1 carries the volume this kernel was read out of" "$L"
    chk $? "and recognised it by the boarding pass, not by a rule"
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "TagFS mounted it"
    grep -q "AUTOSTART. Started .display.elf" "$L"; chk $? "the volume's display daemon started"
    grep -q "AUTOSTART. Started .shell.bin" "$L";   chk $? "the volume's shell started"
    grep -q "the stand-in .PID 1. hands over" "$L"; chk $? "and the stand-in handed over"
}

# ── replug: the stick is pulled out and pushed back in ─────────────────────
#
# The scenario every other one here missed, and the one the board actually
# failed on. Each of the five existing scenarios plugs the stick in exactly
# ONCE, so none of them could ever reach the code that decides what to do with
# a medium arriving in a room that has already seen it.
#
# What went wrong: a seat could never be emptied, so the departing stick left
# its chair behind occupied. It got its unit number straight back on return,
# the room found the chair already holding that number, added nothing, and
# therefore announced nothing — and the volume was never mounted again. On a
# machine that boots from a stick, that is the machine gone until it is
# switched off and on.
#
# Four cores, same as latearrival: the departure is taken down from the idle
# and guide loops, and on one core neither of them runs.
run_replug() {
    echo "== replug: the volume leaves and comes home to the same chair =="
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 30 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done

    # First arrival — the same one latearrival proves.
    ./tools/qemu-input.sh raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh raw "device_add usb-storage,drive=stick,id=usbstick" >/dev/null 2>&1
    # Waited out to the END of the chain, not to its first sign. The stand-in
    # handing over is the last thing the first arrival does; pulling the stick
    # before then tests something else entirely — a medium yanked out from
    # under a program being loaded — and reports it as this.
    i=0
    while [ $i -lt 60 ]; do
        grep -q "hands over" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 2
    # A mark in the log, so the second half is read apart from the first: every
    # line the checks below care about has a twin above it.
    local FIRST_LINES
    FIRST_LINES=$(wc -l < build/serial.log)

    # Pulled out.
    ./tools/qemu-input.sh raw "device_del usbstick" >/dev/null 2>&1
    i=0
    while [ $i -lt 20 ]; do
        grep -q "is empty" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 2

    # And now, with the medium out, ask for a program that IS on it and one
    # that is not. Those are different facts and used to print the same
    # sentence — on a live board the machine told the user that a command
    # which exists does not exist, two lines under the kernel saying why.
    ./tools/qemu-input.sh type "files" >/dev/null 2>&1
    sleep 1; ./tools/qemu-input.sh key ret >/dev/null 2>&1; sleep 3
    ./tools/qemu-input.sh type "nosuchthing" >/dev/null 2>&1
    sleep 1; ./tools/qemu-input.sh key ret >/dev/null 2>&1; sleep 3

    # And pushed back in — the same medium, into the machine it left.
    #
    # The drive is added again first. A drive created through the HMP
    # `drive_add` is auto-delete: QEMU takes it away with the device that was
    # using it, so the second `device_add` referred to a drive that no longer
    # existed and did nothing at all. It cost a run to find, because a monitor
    # command that fails is silent on the guest's serial line.
    ./tools/qemu-input.sh raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh raw "device_add usb-storage,drive=stick,id=usbstick" >/dev/null 2>&1
    i=0
    while [ $i -lt 60 ]; do
        grep -q "the volume is back" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 3

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.replug.log"
    latearrival_off
    L="$SCRATCH/serial.replug.log"
    # Everything after the first arrival. The checks are about the SECOND one.
    tail -n +$((FIRST_LINES + 1)) "$L" > "$SCRATCH/serial.replug.second.log"
    local S="$SCRATCH/serial.replug.second.log"

    # The first arrival happened at all — otherwise the rest is vacuous.
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the stick was mounted on its first arrival"
    grep -q "AUTOSTART. Started .shell.bin" "$L"; chk $? "and its shell started"
    grep -q "the stand-in .PID 1. hands over" "$L"; chk $? "and the stand-in handed over"

    # The departure NAMES the chair. Saying only "something left" is what made
    # the room unable to empty the right one.
    grep -qE "seat [0-9]+ is empty . usb[0-9]+" "$S"
    chk $? "the room emptied the chair and named it"
    grep -q "the medium the volume lives on has left" "$S"
    chk $? "TagFS noticed the medium go"

    # The second arrival is announced. This is the whole repair: it used not to
    # be, because the room only ever spoke when it GREW.
    grep -q "arrived after the room was called to order" "$S"
    chk $? "the return was announced, not swallowed"
    grep -q "the volume is back, in seat" "$S"
    chk $? "TagFS took the volume back up"

    # Into the chair it left, not a new one. The room must not grow a chair
    # per plug cycle — sixteen of them is what a live board showed.
    grep -q "seat 2:" "$S"
    if [ $? -ne 0 ]; then ok "no new chair was added for the returning stick"
    else bad "no new chair was added for the returning stick"; fi
    grep -qE "the volume is back, in seat 1" "$S"
    chk $? "and it is the same seat number it had before"

    # A program that is on the absent volume, and one that is on no volume at
    # all, must not get the same sentence.
    grep -q "files: is there and would not start" "$S"
    chk $? "a real command on an absent medium says so"
    grep -q "Unknown command: nosuchthing" "$S"
    chk $? "and a command that does not exist is still unknown"

    # ONE stick, ONE unit number, for the whole run.
    #
    # Two cores could both attach the same disk — the "is it already attached"
    # walk was outside the lock that the answer depends on — and only one of
    # the two units is ever unlinked when the device leaves. The other keeps
    # its number for the rest of the boot, so the next stick gets a higher one,
    # and a higher number is a chair nobody is sitting in. Measured on a live
    # board: one flash drive, seats usb0, usb1 and usb2.
    local units
    units=$(grep -oE "USB disk [0-9]+\] usb[0-9]+ " "$L" | sort -u | wc -l | tr -d ' ')
    [ "$units" = 1 ]; chk $? "one stick was one disk throughout ($units named)"

    # And the machine is still usable afterwards, which is the point.
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still has a shell"
}

# ── nofsgsbase: a processor that cannot write its own TLS base ─────────────
#
# Every configuration in this file runs with +fsgsbase, so ring 3 installs its
# own FS base with WRFSBASE and the kernel's fallback is never exercised. That
# fallback was broken for as long as it has existed, and it takes out EVERY
# C++ binary on such a machine — measured on a Braswell laptop, reproduced here
# byte for byte. Broadwell brought FSGSBASE to the Core line and Goldmont to
# Atom; everything older is this configuration, and BoxOS is meant to run on
# machines people already own.
run_nofsgsbase() {
    echo "== nofsgsbase: the CPU cannot write its own TLS base =="
    build
    make run-stop >/dev/null 2>&1
    make run-bg FSGSBASE=off CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 3
    for c in timezone me; do
        ./tools/qemu-input.sh type "$c" >/dev/null 2>&1
        sleep 1; ./tools/qemu-input.sh key ret >/dev/null 2>&1; sleep 4
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.nofsgsbase.log"
    L="$SCRATCH/serial.nofsgsbase.log"

    grep -q "BoxOS Shell" "$L"; chk $? "boot reaches the shell without fsgsbase"

    # The C++ runtime installs its TLS through the kernel here, and the first
    # fs-relative instruction is two after the request. Nothing may fault.
    ! grep -q "__boxcxx_tls_bootstrap" "$L"
    chk $? "no C++ program died in its TLS bootstrap"
    ! grep -q "EXCEPTION" "$L"; chk $? "no user-mode exception at all"

    grep -q "timezone: not set\|now:" "$L"
    chk $? "a C++ utility ran and printed its answer"
    grep -q "AMP active\|Uptime:" "$L"
    chk $? "and so did the one after it"
}

run_stranger() {
    echo "== stranger: the pass names a volume that is not here =="
    stranger_on; build; boot stranger; stranger_off
    L="$SCRATCH/serial.stranger.log"

    grep -q "no seat is carrying it" "$L"; chk $? "the room says the volume it wants is absent"
    grep -q "none of them is this machine.s . mounting nothing" "$L"
    chk $? "and declines the volume that IS here"

    ! grep -q "TagFS. volume on seat" "$L"; chk $? "no stranger was mounted"
    grep -q "Storage Deck. no volume yet" "$L"; chk $? "the deck says it has no volume"

    # The whole point of declining: the machine must still be usable, because
    # the person who has to re-seat the medium types on it.
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still reaches a shell"
    typed_ok "$L"; chk $? "and answers a keystroke"

    # And it must be LISTENING, not polling on a clock.
    ! grep -q "falling back to the rule" "$L"; chk $? "the guess-rule never ran"
}

# Occurrence names that measurement showed were resolving to INVALID before
# the split, because they are named before any volume mounts.
DEAD_BEFORE="mce:fault:detected mce:fault:fatal cet:fault:cp acpi:ready aml:ready \
display:ready mce:migration:completed apei:memory:error apei:ghes:ready"

run_healthy() {
    echo "== healthy: volume mounted =="
    probe_on; build; boot healthy; probe_off
    L="$SCRATCH/serial.healthy.log"

    grep -q "BoxOS Shell" "$L"; chk $? "boot reaches the shell"
    typed_ok "$L"; chk $? "and answers a keystroke"

    # Running a PROGRAM, and coming back. A builtin never leaves the shell;
    # this is the only check in the file that exercises spawn-and-wait.
    grep -q "CPU features" "$L"; chk $? "an external utility ran"
    external_ok "$L"; chk $? "and the prompt came back after it"

    # (1) every occurrence lands in the kernel half — id has bit 15 set
    local bad_ids
    bad_ids=$(grep '^\[PROBE\] logbook' "$L" | grep -v 'bare=0x8' | grep -v 'bare=0xffff' | head -3)
    [ -z "$bad_ids" ]; chk $? "every occurrence id carries bit 15"
    [ -n "$bad_ids" ] && echo "$bad_ids" | sed 's/^/       /'

    # (2) the names that were dead on every boot now resolve
    local dead=0
    for t in $DEAD_BEFORE; do
        grep -q "logbook '$t' full=0x8" "$L" || { bad "occurrence '$t' still not named"; dead=1; }
    done
    [ $dead = 0 ] && ok "all previously-dead occurrences now resolve"

    # (3) the volume's own vocabulary is untouched — display.elf keeps its tag
    grep -q "AUTOSTART.*display.elf.*tags: display," "$L"; chk $? "volume tags unshifted (display.elf still 'display')"
    grep -q "shell.bin.*tags: shell," "$L"; chk $? "volume tags unshifted (shell.bin still 'shell')"

    # (4) nothing collided in the on-disk registry
    ! grep -q "slot .* already used" "$L"; chk $? "no registry slot collision"

    # Which root ports are one hole in the case. The controller does not say,
    # so this driver pairs them by position and PRINTS that it is doing so —
    # a board wired differently has to be catchable by reading the boot, since
    # nothing else can catch it.
    grep -q "socket(s) have both halves" "$L"
    chk $? "the boot says which of its root ports share a socket"

    # And each port names its own other half, both ways round. A pairing that
    # only holds in one direction is one that will be asked the wrong way.
    local pair_line pair_a pair_b
    pair_line=$(grep -m1 -oE "port [0-9]+: .*same socket as port [0-9]+" "$L")
    pair_a=$(printf '%s' "$pair_line" | sed -nE 's/^port ([0-9]+):.*/\1/p')
    pair_b=$(printf '%s' "$pair_line" | sed -nE 's/.*same socket as port ([0-9]+)$/\1/p')
    [ -n "$pair_a" ] && [ -n "$pair_b" ] &&
        grep -q "port $pair_b: .*same socket as port $pair_a\b" "$L"
    chk $? "the pairing holds both ways (port $pair_a <-> port $pair_b)"

    # (5) the kernel's vocabulary is complete, not a handful of survivors
    local names
    names=$(grep -c '^\[PROBE\] logbook' "$L")
    local distinct
    distinct=$(grep '^\[PROBE\] logbook' "$L" | sed "s/.*logbook //;s/ full.*//" | sort -u | wc -l | tr -d ' ')
    [ "$distinct" -ge 24 ]; chk $? "Logbook holds the full vocabulary ($distinct distinct names, $names resolves)"

    # (6) TagFS itself still healthy
    grep -qE "TagFS: [0-9]+ run, [0-9]+ passed, 0 failed" "$L"; chk $? "TagFS startup tests: 0 failed"

    # (7) the kernel's ear works — mutation-proven harness, see commit
    grep -q "TOUCH WATCH TEST. PASSED: all 20 checks OK" "$L"; chk $? "TouchWatch self-test 20/20"

    # (8) THE MEDIUM IS ASKED ONCE, NOT FIVE TIMES.
    #
    # A boot used to read this seat's partition table five times and its deed
    # four, because five parties that know nothing about each other each asked
    # the same question: the room describing what it seated, the filesystem
    # looking for its volume, the filesystem asking the chosen seat again just
    # for the identity, the mount standing on the ground, and the boot survey.
    # On a flash drive every one of those is a real transfer.
    #
    # The reads themselves are silent, so what is checked here is the symptom
    # that was visible: the volume was DESCRIBED twice, three identical lines
    # each time. One description means one reader reached it.
    local described
    described=$(grep -cE '^\[Deed\] seat [0-9]+: volume [0-9a-f]{32}, ' "$L")
    [ "$described" = 1 ]
    chk $? "the volume is described once, not once per reader ($described)"

    # And the survey says so rather than skipping quietly. A survey that leaves
    # a ground out without naming it is a survey nobody can count.
    grep -q "carries the volume this machine is standing on"  "$L"
    chk $? "the boot survey names the ground already stood on instead of re-reading it"

    # (9) THE MOUNT CHECKS ITS OWN FAR COPY, AND SAYS SO.
    #
    # It always did — and only ever spoke when the far copy did NOT answer, so
    # a volume whose whole extent is present looked exactly like a volume
    # nobody had checked. [TagFS], not [Deed]: the boot survey has its own line
    # with the same words, and that one is about somebody else's ground.
    grep -qE '^\[TagFS\] seat [0-9]+: its far copy agrees' "$L"
    chk $? "the mount checked the far end of the volume it stood on"
}

# ── a volume whose metadata will not read ────────────────────────────────
#
# The board failure this reproduces: a flash drive came up once with every
# file on it, and the boot after that had none. The metadata pool had been
# overwritten, and the mount met a block that was not a pool — which it
# answered by "starting fresh" and returning success, silently, because the
# line saying so was a debug_printf and those compile to nothing.
#
# A volume that reads as empty is a volume the next write finishes off. So the
# rule is: metadata that cannot be read means the volume does NOT mount, the
# machine says why, and nothing is written to the medium.
#
# The damage is done to the IMAGE, not to the code — the same way the deed
# checks are proven. Nothing here depends on a mutation being installed.
badpool_on() {
    cp build/boxos.img "$SCRATCH/boxos.img.bak"
    python3 - <<'EOF'
import struct
# ground at 2048, data run begins at volume block 134, the pool is data block 2
off = (2048 + 136 * 8) * 512
f = open("build/boxos.img", "r+b")
f.seek(off)
was = f.read(4)
f.seek(off)
f.write(b"\xAA\xBB\xCC\xDD")
f.close()
print("logcheck: metadata pool magic %s -> aabbccdd" % was.hex())
EOF
}
badpool_off() {
    cp "$SCRATCH/boxos.img.bak" build/boxos.img
}

run_badpool() {
    echo "== badpool: the metadata pool holds something that is not a pool =="
    build
    badpool_on
    # boot() rebuilds nothing — it runs what is on disk now.
    make run-stop >/dev/null 2>&1
    make run-bg   >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 2; i=$((i+1))
    done
    sleep 3
    ./tools/qemu-input.sh type "help" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh key ret >/dev/null 2>&1
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.badpool.log"
    badpool_off
    L="$SCRATCH/serial.badpool.log"

    grep -q "where a metadata pool should be" "$L"
    chk $? "the mount says what it found instead of a pool"

    ! grep -q "starting fresh" "$L"
    chk $? "and does not decide the volume is empty"

    ! grep -qE "AUTOSTART. Started" "$L"
    chk $? "nothing from the volume was started off it"

    grep -q "BoxOS Shell" "$L"
    chk $? "the machine still reaches a shell"

    typed_ok "$L"
    chk $? "and answers a keystroke"
}

run_novolume() {
    echo "== novolume: tagfs_recognise() forced false =="
    probe_on; mutate_on; build; boot novolume; mutate_off; probe_off
    L="$SCRATCH/serial.novolume.log"

    grep -q "no volume\|No autostart" "$L"; chk $? "boot proceeds with no volume"
    grep -q "BoxOS Shell" "$L"; chk $? "a machine with no medium still reaches a shell"
    typed_ok "$L"; chk $? "and answers a keystroke"

    # THE point of the whole change: the console tag survives having no medium
    grep -q "logbook 'keyboard'.*bare=0x8" "$L"; chk $? "'keyboard' resolves with NO volume"

    grep -q "logbook 'usb:arrived' full=0x8" "$L"; chk $? "'usb:arrived' resolves with NO volume"

    local dead=0
    for t in $DEAD_BEFORE; do
        grep -q "logbook '$t' full=0x8" "$L" || { bad "occurrence '$t' missing with no volume"; dead=1; }
    done
    [ $dead = 0 ] && ok "full occurrence vocabulary present with no volume"

    ! grep -q "logbook .* bare=0xffff" "$L"; chk $? "no occurrence resolved to INVALID"
}

run_uefi() {
    # q35 + OVMF is the only configuration here that exposes MCFG, so it is
    # the only one that produces the data-driven pci:vendor:* names at all.
    # That is exactly the traffic that used to be interned into the volume's
    # registry and flushed to the medium.
    echo "== uefi/q35: data-driven names =="
    probe_on; build
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on >/dev/null 2>&1
    local i=0
    while [ $i -lt 45 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 2; i=$((i+1))
    done
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.uefi.log"
    probe_off
    L="$SCRATCH/serial.uefi.log"

    grep -q "BoxOS Shell" "$L"; chk $? "uefi boot reaches the shell"

    local pci_n
    pci_n=$(grep -c "logbook 'pci:" "$L")
    [ "$pci_n" -gt 0 ]; chk $? "pci:* hardware inventory named in the Logbook ($pci_n)"

    local pci_low
    pci_low=$(grep "logbook 'pci:" "$L" | grep -vc 'bare=0x8')
    [ "$pci_low" = 0 ]; chk $? "no pci:* name took a volume id"

    grep -q "AUTOSTART.*display.elf.*tags: display," "$L"; chk $? "uefi: volume tags unshifted"

    # A family the muster DECLARED must not be reported as a stranger. It was:
    # the warning judged the full name, which is new for every device that has
    # ever existed, so "pci" being in the muster bought nothing and eight lines
    # were printed on every boot of a machine with MCFG. Measured here, on this
    # scenario, before and after: 8 lines -> 0. Removing X("pci") from the
    # muster prints exactly one line naming the family, which is the whole
    # point of the warning and is what must survive.
    local strangers
    strangers=$(grep -c "not in its muster" "$L")
    [ "$strangers" = 0 ]
    chk $? "no name the muster declared was called a stranger ($strangers)"
}

# Does the medium actually carry this text? Read off the image rather than
# believed from what the command printed. Every needle carries a
# runtime-formatted digit or a typed prompt, so none of them can match a format
# string sitting in some binary's rodata — the trap a plain grep walks into.
img_carries() {
    python3 - "$1" "$2" <<'PY'
import sys, re
img = open(sys.argv[1], 'rb').read()
sys.exit(0 if re.search(sys.argv[2].encode(), img) else 1)
PY
}

run_logsave() {
    echo "== logsave: what the kernel said, written down where it can be read =="

    # Two kernels, because the switch is a BUILD switch. The machine that keeps
    # its log and the machine that does not are different kernels, and both
    # halves of the promise have to hold: the one that keeps it must produce a
    # readable account, and the one that does not must SAY so rather than hand
    # back an empty file that reads like a machine which never spoke.
    make PRINTTOFILE=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (PRINTTOFILE=on) — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi

    make run-stop >/dev/null 2>&1
    make run-bg PRINTTOFILE=on >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 3

    # A command FIRST, so the file is asked to carry both what a photograph of
    # the screen would have shown and what it would not: the command that was
    # typed. Userspace output reaches the ring through the Manifest VGA ops,
    # deliberately not through the serial mirror a board build turns off.
    ./tools/qemu-input.sh type "hw" >/dev/null 2>&1
    sleep 1; ./tools/qemu-input.sh key ret >/dev/null 2>&1; sleep 4
    ./tools/qemu-input.sh type "logsave" >/dev/null 2>&1
    sleep 1; ./tools/qemu-input.sh key ret >/dev/null 2>&1; sleep 8
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.logsave.log"
    cp build/boxos.img  "$SCRATCH/logsave.img"
    L="$SCRATCH/serial.logsave.log"
    I="$SCRATCH/logsave.img"

    grep -q "BoxOS Shell" "$L"; chk $? "boot reaches the shell with the log kept"

    grep -qE "[0-9]+ byte\(s\) written to watch.log" "$L"
    chk $? "logsave says how much it wrote, and where"

    ! grep -qE "^0 byte\(s\) written" "$L"
    chk $? "and it is not nothing"

    ! grep -q "byte(s) were said before this" "$L"
    chk $? "one boot does not overflow the ring"

    sed -n '/~ logsave/,$p' "$L" | grep -qE '^~ ?$|^~ '
    chk $? "the shell came back after it"

    img_carries "$I" '\[Logbook\] muster: \d+ name\(s\) seated'
    chk $? "the file carries a line from before the screen could scroll"

    img_carries "$I" '~ hw\n'
    chk $? "the file carries the command that was typed"

    # A RUNTIME value, not a label. The first form of this check looked for
    # "invariant TSC : " and passed with the ring deliberately broken — that
    # string is the format literal in hw.elf's rodata, sitting in the same
    # image, and the check was reading the binary rather than the log. The
    # number here is printed through %u and exists nowhere but in the account.
    img_carries "$I" 'TSC freq      : \d+ kHz'
    chk $? "and the answer that command produced"

    # Now the same command on a kernel that keeps nothing.
    build
    make run-stop >/dev/null 2>&1
    make run-bg >/dev/null 2>&1
    i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 3
    ./tools/qemu-input.sh type "logsave" >/dev/null 2>&1
    sleep 1; ./tools/qemu-input.sh key ret >/dev/null 2>&1; sleep 6
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.logsave-off.log"
    O="$SCRATCH/serial.logsave-off.log"

    grep -q "does not keep what it says" "$O"
    chk $? "a kernel without the ring refuses by name"

    ! grep -qE "byte\(s\) written to" "$O"
    chk $? "and claims nothing it did not do"
}

# ---------------------------------------------------------------------------
# manyports — every port the controller says it has is a port this driver uses
#
# MaxPorts is an eight-bit field. This driver surveyed the first thirty-one
# root ports and kept a thirty-two-bit note of which it had done, so a device
# in any socket above that was invisible to the boot: not reported, not
# enumerated, not mentioned. A machine booting off a stick in one of them
# would not have booted, and nothing anywhere would have said why.
#
# ‼ WHAT THIS CANNOT DO. QEMU's xHCI allows fifteen ports per protocol and no
# more, so thirty is the widest controller that can stand on this desk and the
# cliff at thirty-one cannot be reached. What is checked instead is the
# INVARIANT the cliff broke — every port the controller reports is looked at
# and described — which is worth something at any width and which goes red the
# moment a bound of any size comes back. The bound is what the mutation below
# puts back, at fifteen instead of thirty-one, and the keyboard sitting on
# port 16 is what notices.
# ---------------------------------------------------------------------------
run_manyports() {
    echo "== manyports: thirty root ports, and none of them ignored =="
    build

    make run-stop >/dev/null 2>&1
    make run-bg USB=on XHCIPORTS=15 >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.manyports.log"
    L="$SCRATCH/serial.manyports.log"

    grep -q "BoxOS Shell" "$L"
    chk $? "boot reaches the shell with thirty root ports"

    # What the controller says it has, taken from its own summary line rather
    # than from the number this scenario asked for — the emulator is entitled
    # to give fewer than requested and a check against our own wish would then
    # be a check against nothing.
    local claimed described
    claimed=$(grep -oE "controller ready: [0-9]+ port" "$L" | head -1 |
              sed -nE 's/.*: ([0-9]+) port/\1/p')
    described=$(grep -cE "^\[xHCI [0-9a-f:.]+\] port [0-9]+: (powered|NOT powered)" "$L")

    [ -n "$claimed" ] && [ "$claimed" -gt 16 ]
    chk $? "the controller really has more than sixteen ports ($claimed)"

    [ "$claimed" = "$described" ]
    chk $? "every port it says it has is described ($described of $claimed)"

    # The survey, as opposed to the description: the keyboard sits on the first
    # USB 2 port, which with fifteen SuperSpeed ports in front of it is port 16.
    # A survey that stops short never finds it, and the machine boots without a
    # keyboard rather than saying anything.
    grep -qE "port ([2-9][0-9]|1[6-9]): device attached at boot" "$L"
    chk $? "the boot survey reached a port above the sixteenth"

    grep -qE "port ([2-9][0-9]|1[6-9]): .*keyboard .* is live" "$L"
    chk $? "and the device there came up"
}

run_usbrecover() {
    echo "== usbrecover: the deck is asked to put a controller back in service =="

    # Four cores, for the reason latearrival and replug already give above: the
    # deferred work of this kernel runs from the K-Core guide loop and from
    # cpu_idle, and on one core neither of them runs. Measured again here by
    # probe while this scenario was written — with CORES=1 the proof below is
    # never reached at all.
    #
    # The build key exists because this costs the machine every USB device it
    # has. A shipped build does not do that to itself.
    make USBRECOVER=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi

    make run-stop >/dev/null 2>&1
    make run-bg USBRECOVER=on CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 45 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 2; i=$((i+1))
    done
    sleep 3

    # Typed AFTER the controller has been taken down and brought back. The
    # keyboard is on that controller: this is the whole point of the scenario.
    ./tools/qemu-input.sh type "help" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh key ret >/dev/null 2>&1
    sleep 3
    ./tools/qemu-input.sh type "hw" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh key ret >/dev/null 2>&1
    sleep 4

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.usbrecover.log"
    L="$SCRATCH/serial.usbrecover.log"

    # The surface first: the two opcodes that used to break the machine are
    # withdrawn, and their numbers answer to nothing rather than to something
    # else. hw.usb.reset is still there and still system-only.
    grep -q "the two withdrawn opcodes answer to nothing" "$L"
    chk $? "hw.usb.init and hw.usb.stop are gone and their numbers are spent"

    grep -q "hw.usb.reset is registered, system-only" "$L"
    chk $? "hw.usb.reset is still there, and still system-only"

    # Then the op itself, reached through the registry the way the dispatcher
    # reaches it — so what is proved is the surface and not the driver behind.
    grep -q "asked for the controller on .* to be put back in service" "$L"
    chk $? "the deck was asked, by name, through the op registry"

    # And what it did. A bare xhci_reset — which is what this op used to be —
    # prints the first of these and none of the rest, and leaves the machine
    # with no USB and no line saying so.
    grep -q "putting the controller back in service" "$L"
    chk $? "the driver began the whole repair, not a bare reset"

    grep -q "back in service — looking again at what is plugged in" "$L"
    chk $? "and finished it: the controller is running and the bus resurveyed"

    # THE check. The keyboard lives on the controller that was just reset, so
    # it went away with it. A repair that stops halfway leaves this line absent
    # and the machine unusable from that moment on.
    #
    # ‼ The anchor must EXIST, and this check said so only after a mutation
    # proved it did not. Written as `awk -v n="${began:-0}"`, a missing anchor
    # became line zero and the check then matched the keyboard's ORIGINAL boot
    # enumeration — it passed with the repair reverted to the bare reset it
    # replaced, which is the one case it exists to catch.
    local began came_back
    began=$(grep -n "putting the controller back in service" "$L" | head -1 | cut -d: -f1)
    came_back=""
    if [ -n "$began" ]; then
        came_back=$(awk -v n="$began" 'NR>n && /keyboard .* is live/ {print NR; exit}' "$L")
    fi
    [ -n "$came_back" ]
    chk $? "the keyboard came back after the controller did"

    grep -q "USB RECOVER TEST. PASSED" "$L"
    chk $? "the op answered success"

    ! grep -q "USB RECOVER TEST. FAIL" "$L"
    chk $? "nothing in the proof failed"

    # And the machine, afterwards. Typing is the only question the board
    # failure was ever really about, and here it is asked of a keyboard that
    # has been through a controller reset.
    typed_ok "$L";    chk $? "and the machine answers a keystroke after all that"
    external_ok "$L"; chk $? "and the prompt came back after an external utility"

    # The volume was not on that controller in this configuration, and must not
    # have noticed anything at all.
    ! grep -q "the medium the volume lives on has left" "$L"
    chk $? "the volume on the other seat was untouched by any of it"
}

run_yank() {
    echo "== yank: the stick is pulled while the volume is being read =="

    # replug pulls the stick when the machine is idle, so nothing is in flight
    # and the recovery path is never entered. The window this driver used to
    # spend tens of seconds in is the other one: a transfer outstanding at the
    # moment the device leaves. Two things are needed to reach it.
    #
    # The stick must BE the volume, or nothing reads from it — the boot image
    # is on ATA. Same mutation late-arrival uses, and it stays installed for
    # the whole run, for the reason written there.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 2
    local MARK
    MARK=$(wc -l < build/serial.log)

    ./tools/qemu-input.sh raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh raw "device_add usb-storage,drive=stick,id=usbstick" >/dev/null 2>&1

    # And the MEDIUM must be slow. QEMU answers a read the instant it is asked,
    # so the whole mount burst is over before a poll of the log can see it
    # begin — there is no window to aim at, and a fixed delay lands either side
    # of it. Held to 64 KiB/s the same burst takes seconds, which is also what
    # a flash drive on a real bus looks like. Nothing in the guest is touched:
    # this is the emulator being told to behave less like a RAM disk.
    ./tools/qemu-input.sh raw "block_set_io_throttle stick 0 65536 0 0 0 0" >/dev/null 2>&1

    # Fired off the first sign of the volume being read, not off a clock.
    i=0
    while [ $i -lt 400 ]; do
        tail -n +$((MARK+1)) build/serial.log 2>/dev/null | grep -q "Deed] seat" && break
        python3 -c "import time; time.sleep(0.02)"
        i=$((i+1))
    done
    sleep 1
    ./tools/qemu-input.sh raw "device_del usbstick" >/dev/null 2>&1

    # Waited out past XHCI_RETIRE_PATIENCE_MS (15 s), not merely past the
    # departure. The last check below is on the slot service saying it has been
    # trying to take a slot down and cannot — and a window shorter than the
    # patience makes that check green on a machine where it is false, which is
    # decoration rather than a guard. Measured: at fourteen seconds it passed
    # against the very code it exists to catch.
    sleep 20
    make run-stop >/dev/null 2>&1
    tail -n +$((MARK+1)) build/serial.log > "$SCRATCH/serial.yank.log"
    latearrival_off
    L="$SCRATCH/serial.yank.log"

    grep -q "mounted from seat" "$L"
    chk $? "the stick became this machine's volume"

    # The point of the whole scenario. Measured against the code before this
    # change, same script, same throttle: 4 transport resets, 2 endpoint
    # clears, no departure and NO EMPTY SEAT within fourteen seconds.
    grep -q "the device has left, so it is not being asked for anything more" "$L"
    chk $? "the driver stopped because the device left, not because a clock ran out"

    ! grep -q "resetting the transport" "$L"
    chk $? "nothing was spent resetting a transport that is not there"

    ! grep -q "halted — clearing it" "$L"
    chk $? "no endpoint of a departed device was reset"

    ! grep -q "no answer in .* ms at stage" "$L"
    chk $? "no budget was waited out for an answer nobody could give"

    # ‼ THE SOCKET, NOT THE PORT.
    #
    # A USB 3 socket is two root ports, and a device whose SuperSpeed link does
    # not train leaves one of them and appears on the other — which on the port
    # it left is bit-for-bit what a hand pulling it out looks like. Here the
    # emulator really does take the device away, so BOTH halves are empty, and
    # the driver has to say so rather than say something that would be equally
    # true of a link falling back.
    grep -q "unplugged — the socket is empty" "$L"
    chk $? "the departure says the SOCKET is empty, not just the port"

    ! grep -q "the other half of the same socket — has something in it" "$L"
    chk $? "and does not claim the device moved to the socket's other half"

    grep -q "is gone" "$L"
    chk $? "the unit was released"

    grep -q "seat .* is empty" "$L"
    chk $? "and its chair was emptied"

    # The slot service says this when a slot has been leaving for longer than
    # its patience because somebody is still inside it. That somebody was the
    # recovery grind.
    ! grep -q "has been leaving for" "$L"
    chk $? "no caller was still inside the slot when it wanted to leave"
}

run_stillthere() {
    echo "== stillthere: the read was slow, and the device never went anywhere =="

    # yank    pulls the stick while it is being read  -> the answer NEVER comes
    # replug  pulls it while the machine is idle      -> nothing is in flight
    #
    # This is the third thing, and it is the one a real flash drive does: it
    # answers LATE. A device doing its own garbage collection stops replying
    # for seconds and then carries on, and from the host's side that is
    # indistinguishable from silence — on BULK it is not even silence, it is
    # the device NAKing, which the controller retries for as long as it takes.
    #
    # Nothing here is unplugged. The stick is in the machine from the first
    # line to the last, and every check is about what the driver did to a
    # device that was there the whole time.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done

    ./tools/qemu-input.sh raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh raw "device_add usb-storage,drive=stick,id=usbstick" >/dev/null 2>&1
    i=0
    while [ $i -lt 60 ]; do
        grep -q "hands over" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 2

    # ── the control is the boot itself ──────────────────────────────────────
    #
    # Everything below is "and now it does not work", which is worth nothing
    # unless the machine has been shown to work first. It has: reaching a shell
    # at all means display.elf and shell.bin were read off this stick and run.
    #
    # ‼ IT USED TO BE A `files` TYPED HERE, AND THAT MADE THE RUN UNRELIABLE.
    # TagFS reads ahead, so running the program once put it in memory and the
    # throttled read below was then served without touching the medium — the
    # scenario passed by never reaching the situation it exists for. Measured:
    # green on its own, red in the matrix, same tree. A control that warms what
    # the test is about is not a control.
    cp build/serial.log "$SCRATCH/serial.stillthere.control.log"
    local MARK
    MARK=$(wc -l < build/serial.log)

    # ── PART ONE: slow, and that is ALL it is ──────────────────────────────
    #
    # 512 bytes a second puts one 4 KiB filesystem block at eight seconds —
    # longer than the five this driver used to allow a transfer, and well
    # inside what a flash drive doing its own housekeeping takes, but nowhere
    # near what the class gives a whole command. NOTHING may be given up on
    # here. This is the check the whole change is for, and on the old tree it
    # goes red within five seconds.
    #
    # ‼ THE PROGRAM MATTERS AS MUCH AS THE RATE. TagFS reads ahead, and
    # measurement says some programs are already in memory by the time a shell
    # appears: `files` and `bench` came back in two seconds at SIXTY-FOUR bytes
    # a second, while `today` and `mtest` took a hundred and twenty-two at five
    # hundred and twelve. A scenario aimed at a program that is already in
    # memory tests nothing and says PASS. Both programs used here were measured
    # cold, and neither is used twice.
    local GAVE_UP_BEFORE GAVE_UP_AFTER
    GAVE_UP_BEFORE=$(grep -cE "giving up on the command|no answer in .* at stage" build/serial.log)
    wait_for_prompt
    ./tools/qemu-input.sh raw "block_set_io_throttle stick 0 512 0 0 0 0" >/dev/null 2>&1
    sleep 1
    type_line "today"

    # ‼ WAITED ON THE PROGRAM'S OWN ANSWER, AND NOTHING IS TYPED UNTIL IT COMES.
    #
    # Fixed pauses here cost two whole runs. The shell is one thing doing one
    # thing at a time: a command typed while the last one is still reading is a
    # command that lands in a buffer, and the scenario then reports a machine
    # that was never given a chance as a machine that failed. `today` prints a
    # date, and the date arriving is the fact that it was read.
    i=0
    while [ $i -lt 240 ]; do
        tail -n +$((MARK + 1)) build/serial.log 2>/dev/null | \
            grep -qE "^20[0-9][0-9]-[0-9][0-9]-[0-9][0-9] " && break
        sleep 1; i=$((i+1))
    done
    GAVE_UP_AFTER=$(grep -cE "giving up on the command|no answer in .* at stage" build/serial.log)
    ./tools/qemu-input.sh raw "block_set_io_throttle stick 0 0 0 0 0 0" >/dev/null 2>&1
    sleep 2

    # ── ‼ WHAT IS NOT TESTED HERE, AND WHY IT IS SAID OUT LOUD ──────────────
    #
    # The other half of the change is the LAST RESORT: a device slower than the
    # class allows is given up on, the transfer is taken off the endpoint with
    # the command the controller accepts, and the disk still works afterwards.
    # That was OBSERVED WORKING — a medium held to 128 bytes a second produced
    #
    #   [USB disk 0] the device has been asking for more time for 30000 ms at
    #                stage 2 — giving up on the command
    #   [USB disk 0] resetting the transport
    #   [mtest] HW VGA via Manifest works
    #
    # — the give-up, the reset, and then the same program arriving anyway once
    # the medium was quick again.
    #
    # It is not a CHECK here because reaching it needs a SECOND typed command,
    # and a second command does not reliably land: when the first one has just
    # finished, the shell is still doing its own reads, and on a medium this
    # slow those take tens of seconds. Five runs went into that, and a check
    # that is red for the instrument's reasons is worse than no check — it
    # teaches everyone to ignore the matrix.
    #
    # The shape that has never once failed is ONE typed command per run, which
    # means the slow-but-tolerable half above has to be proved by the BOOT
    # rather than by typing. That is a scenario of its own and it is written
    # down as owed, not forgotten.

    make run-stop >/dev/null 2>&1
    tail -n +$((MARK + 1)) build/serial.log > "$SCRATCH/serial.stillthere.log"
    latearrival_off

    local C="$SCRATCH/serial.stillthere.control.log"
    local L="$SCRATCH/serial.stillthere.log"

    grep -q "AUTOSTART. Started .shell.bin" "$C"
    chk $? "the machine reads programs off this stick at full speed"

    # ‼ THE POINT OF THE WHOLE CHANGE. On bulk, a device that is busy NAKs, and
    # the controller retries for as long as it takes; trouble arrives as an
    # EVENT and busy arrives as nothing at all. A driver that gives up on
    # "nothing" gives up on a healthy device that is doing its housekeeping.
    grep -qE "^20[0-9][0-9]-[0-9][0-9]-[0-9][0-9] " "$L"
    chk $? "a program read off a medium at 512 bytes a second still runs"

    [ "$GAVE_UP_AFTER" = "$GAVE_UP_BEFORE" ]
    chk $? "and nothing was given up on while it was being read ($GAVE_UP_BEFORE -> $GAVE_UP_AFTER)"

    # The device is in the machine. Anything that says otherwise is the driver
    # inventing a departure out of a delay.
    ! grep -q "the device has left" "$L"
    chk $? "nothing claimed a device had left that was still plugged in"

    # ‼ Reset Endpoint is defined for a HALTED endpoint (xHCI 1.2 4.6.8) and a
    # merely slow one is Running, so the controller refuses it — and says so, in
    # its own words, once per attempt. A refused command is a repair that did
    # not happen.
    ! grep -qE "Reset Endpoint on slot .* refused: Context State Error" "$L"
    chk $? "no endpoint was reset that the controller did not consider halted"

    ! grep -qE "Set TR Dequeue Pointer on slot .* refused: Context State Error" "$L"
    chk $? "and no dequeue pointer was moved on a ring still being read"

    ! grep -q "has been leaving for" "$L"
    chk $? "no slot was left with somebody stuck inside it"
}

run_slowdisk() {
    echo "== slowdisk: the disk was slow, and that is all it was =="

    # A drive that meets a marginal sector retries the head inside itself before
    # it answers. Seven seconds is ordinary on a desktop disk without
    # configurable error recovery and the standard sets no ceiling at all — so a
    # two-second clock on a command is not patience, it is a driver deciding a
    # healthy disk is broken, recovering the port, retrying three times and
    # reporting a failure on a read that would have arrived.
    #
    # ‼ THE DRIVER ALREADY HELD THE RIGHT NUMBER IN ONE HALF OF ITSELF. The
    # ASYNCHRONOUS path has always given a command CONFIG_AHCI_IO_TIMEOUT_MS
    # (thirty seconds, through ahci_watchdog_scan); only the SYNCHRONOUS one —
    # every metadata read a filesystem makes — used two.
    #
    # ‼ WHAT THIS DOES NOT COVER, SAID RATHER THAN LEFT TO BE FOUND. The legacy
    # IDE channels take the same fix (CONFIG_ATA_IO_TIMEOUT_MS) and are NOT
    # tested here: ATA PIO waits for DRQ once per 512-byte SECTOR, so making one
    # of those outlast the old five seconds means a hundred bytes a second, and
    # reading a program at that rate is a quarter of an hour. It is exercised by
    # every other scenario in the ordinary way, and the change to it is a
    # ceiling nothing in an emulator ever reaches.
    build

    make run-stop >/dev/null 2>&1
    make run-bg AHCI=on CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    wait_for_prompt

    # 1536 bytes a second puts ONE 4 KiB filesystem block at not quite three
    # seconds — past the two this driver used to allow a whole command, and
    # nowhere near the thirty the class gives one. Everything below must simply
    # work, slowly.
    #
    # The volume is already mounted, so this is aimed at what a filesystem does
    # afterwards: `files` is a tag query, and a tag query is metadata reads,
    # and metadata reads are the synchronous path.
    ./tools/qemu-input.sh raw "block_set_io_throttle disk0 0 1536 0 0 0 0" >/dev/null 2>&1
    sleep 1
    type_line "files"

    i=0
    while [ $i -lt 180 ]; do
        grep -q "^shell.bin " build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done

    ./tools/qemu-input.sh raw "block_set_io_throttle disk0 0 0 0 0 0 0" >/dev/null 2>&1
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.slowdisk.log"
    L="$SCRATCH/serial.slowdisk.log"

    # The control, and it is the boot: reaching a shell means the machine read
    # its programs off this disk at full speed.
    grep -q "AUTOSTART. Started" "$L"
    chk $? "the machine reads programs off the AHCI disk at full speed"

    grep -q "seat 0: AHCI port" "$L"
    chk $? "and the volume really is behind the AHCI controller"

    # The point. A disk that takes seconds is a disk, and the machine has to go
    # on using it.
    grep -q "^shell.bin " "$L"
    chk $? "the filesystem still answers with the disk at 1536 bytes a second"

    ! grep -q "did not answer a" "$L"
    chk $? "and nothing was given up on while it was being read"

    ! grep -qE "a (read|write|cache flush) failed after" "$L"
    chk $? "no command was abandoned after retries on a disk that was answering"

    ! grep -q "the link is no longer established" "$L"
    chk $? "and the cable was never called unplugged"
}

run_gpt() {
    echo "== gpt: the table is kept at both ends, and the far one is read =="

    # A GPT is written TWICE — the primary at LBA 1 and a copy in the last
    # sector of the medium — and UEFI 2.10 §5.3.2 requires a reader whose
    # primary does not check out to use the other one. This kernel did not: it
    # returned nothing, and every caller read that as "there is no BoxOS ground
    # on this disk". One dead erase block under sector 1 — the commonest way a
    # GPT is lost — and a perfectly good disk did not mount, without a word.
    #
    # Reaching the far copy needs one fact no driver would hand out until now:
    # how far the medium runs. BoardroomSeatSectors is that door.
    #
    # The disk under test arrives as a SECOND medium rather than as the one the
    # machine booted from, deliberately: the loaders read a partition table
    # too, and a scenario that damages the disk it is booting from measures
    # them as much as the kernel. Here the machine boots the ordinary way and
    # the damaged disk is handed to it afterwards, so what is measured is the
    # Ground survey and nothing else.
    build
    ./tools/make_gpt_boot.py build/boxos.img "$SCRATCH/gpt-good.img" >/dev/null 2>&1
    cp "$SCRATCH/gpt-good.img" "$SCRATCH/gpt-bad.img"
    python3 - "$SCRATCH/gpt-bad.img" <<'EOF'
import sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    f.seek(512)              # LBA 1: the primary GPT header
    f.write(b'\x00' * 512)   # one dead erase block, which is how they go
EOF
    local GOOD_LOG="$SCRATCH/serial.gpt.good.log"
    local BAD_LOG="$SCRATCH/serial.gpt.bad.log"

    local which img wait_for log
    for which in good bad; do
        img="$SCRATCH/gpt-$which.img"
        if [ "$which" = good ]; then
            wait_for="from GPT entry"; log="$GOOD_LOG"
        else
            wait_for="the copy at the far end"; log="$BAD_LOG"
        fi

        make run-stop >/dev/null 2>&1
        make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
        local i=0
        while [ $i -lt 40 ]; do
            grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
            sleep 1; i=$((i+1))
        done

        local MARK
        MARK=$(wc -l < build/serial.log)
        ./tools/qemu-input.sh raw "drive_add 0 if=none,id=gptdisk,file=$img,format=raw" >/dev/null 2>&1
        sleep 1
        ./tools/qemu-input.sh raw "device_add usb-storage,drive=gptdisk,id=gptstick" >/dev/null 2>&1

        # Waited on the survey having spoken, not on a clock.
        i=0
        while [ $i -lt 60 ]; do
            tail -n +$((MARK + 1)) build/serial.log 2>/dev/null | \
                grep -q "$wait_for" && break
            sleep 1; i=$((i+1))
        done
        sleep 2
        make run-stop >/dev/null 2>&1
        tail -n +$((MARK + 1)) build/serial.log > "$log"
    done

    # ── the disk with both copies intact ────────────────────────────────────
    grep -q "from GPT entry" "$GOOD_LOG"
    chk $? "a healthy GPT disk gives up its BoxOS ground"

    ! grep -qE "its GPT (is damaged|is not where)" "$GOOD_LOG"
    chk $? "and nothing called a healthy table damaged"

    ! grep -q "made for a smaller disk" "$GOOD_LOG"
    chk $? "and the table matches the medium it is on"

    # ── and the same disk with its primary header gone ──────────────────────
    grep -q "its GPT is not where the disk says it is" "$BAD_LOG"
    chk $? "a lost primary header is called damage, not an empty disk"

    grep -q "reading the copy at the far end, sector 122325" "$BAD_LOG"
    chk $? "and the far copy is looked for in the last sector of the medium"

    grep -q "the copy at the far end is good" "$BAD_LOG"
    chk $? "and it is read"

    grep -q "from GPT entry" "$BAD_LOG"
    chk $? "and the BoxOS ground on that disk is found after all"

    # And the machine that was handed a damaged table finished the pass it was
    # in rather than stopping inside it — the room says so on its way out.
    grep -q "arrived after the room was called to order" "$BAD_LOG"
    chk $? "and the room finished seating it"

    ! grep -qiE "panic|fault at" "$BAD_LOG"
    chk $? "and nothing fell over reading a broken table"
}

run_twoctrl() {
    echo "== twoctrl: two host controllers, and the stick on the SECOND one =="

    # The board has two: an Intel 00:14.0 and an NVIDIA 01:00.2, and the driver
    # registers the NVIDIA one FIRST — so the flash drive lives on controller
    # INDEX 1. Every scenario before this one ran on a machine with exactly one
    # controller, where index 1 does not exist, so nothing here could ever be
    # seen: the retire flag is ONE flag for the whole machine, and the pass for
    # controller 0 used to consume it and walk away, leaving controller 1's
    # departed devices standing for ever.
    #
    # Measured on the board: fifteen chairs from one flash drive, slot ids to
    # 34, and not one "is gone" in a 977-line log. Measured here, same script,
    # with and without the fix: five replugs give usb0 five times, or usb0
    # through usb4 and zero releases.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on XHCI2=on CORES=4 MEM=4G >/dev/null 2>&1
    local i=0
    while [ $i -lt 40 ]; do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 1; i=$((i+1))
    done
    sleep 2
    local MARK
    MARK=$(wc -l < build/serial.log)

    # Five arrivals and five departures on the second controller. A fresh
    # drive_add each time: HMP marks the drive auto-del, so device_del takes it
    # with it and the next device_add silently does nothing — one cycle
    # pretending to be ten.
    for n in 1 2 3 4 5; do
        ./tools/qemu-input.sh raw "drive_add 0 if=none,id=st$n,file=$SCRATCH/stick.img,format=raw" >/dev/null 2>&1
        sleep 1
        ./tools/qemu-input.sh raw "device_add usb-storage,bus=xhci2.0,drive=st$n,id=us" >/dev/null 2>&1
        sleep 4
        ./tools/qemu-input.sh raw "device_del us" >/dev/null 2>&1
        sleep 4
    done
    sleep 4
    make run-stop >/dev/null 2>&1
    tail -n +$((MARK+1)) build/serial.log > "$SCRATCH/serial.twoctrl.log"
    latearrival_off
    L="$SCRATCH/serial.twoctrl.log"

    grep -q "2 controller(s) in service" build/serial.log
    chk $? "the machine really has two host controllers"

    local arrivals
    arrivals=$(grep -c "usb0 QEMU" "$L")
    [ "$arrivals" -ge 2 ]
    chk $? "the stick arrived on the second controller more than once ($arrivals)"

    # THE check. A unit number is handed back when its device leaves, so the
    # same stick coming home takes the same number. A second number means the
    # first was never given back.
    local numbers count
    numbers=$(grep -oE "\[USB disk [0-9]+\] usb[0-9]+ " "$L" | grep -oE "usb[0-9]+" | sort -u | tr '\n' ' ')
    count=$(printf '%s' "$numbers" | wc -w | tr -d ' ')
    [ "$count" = "1" ]
    chk $? "one stick kept one unit number across every replug ($numbers)"

    grep -q "is gone" "$L"
    chk $? "the unit on controller 1 was released at all"

    grep -q "is empty" "$L"
    chk $? "and its chair was emptied"

    ! grep -q "has been leaving for" "$L"
    chk $? "no slot was left standing because nobody came to take it down"
}

case "${1:-both}" in
    healthy)  run_healthy ;;
    novolume) run_novolume ;;
    badpool)  run_badpool ;;
    uefi)     run_uefi ;;
    stranger) run_stranger ;;
    latearrival) run_latearrival ;;
    replug)   run_replug ;;
    nofsgsbase) run_nofsgsbase ;;
    logsave)  run_logsave ;;
    yank)     run_yank ;;
    slowdisk) run_slowdisk ;;
    gpt)      run_gpt ;;
    stillthere) run_stillthere ;;
    twoctrl)  run_twoctrl ;;
    manyports) run_manyports ;;
    usbrecover) run_usbrecover ;;
    both)     run_healthy; echo; run_novolume ;;
    all)      run_healthy; echo; run_novolume; echo; run_stranger; echo; run_latearrival; echo; run_replug; echo; run_nofsgsbase; echo; run_logsave; echo; run_yank; echo; run_stillthere; echo; run_slowdisk; echo; run_gpt; echo; run_twoctrl; echo; run_manyports; echo; run_usbrecover; echo; run_uefi; echo; run_badpool ;;
    *) echo "usage: $0 [healthy|novolume|uefi|stranger|latearrival|replug|nofsgsbase|badpool|manyports|usbrecover|stillthere|slowdisk|gpt|both|all]"; exit 2 ;;
esac

echo
echo "logcheck: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
