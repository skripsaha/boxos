#!/bin/bash
# logcheck — oracle for the Logbook split (kernel occurrence vocabulary vs
# volume tag registry).
#
#   logcheck.sh healthy   boot with a mounted volume; the volume's vocabulary
#                         must be untouched and the kernel's must be complete
#   logcheck.sh novolume  boot with tagfs_recognise() forced false — the board
#                         failure reproduced on the desk. The kernel's
#                         vocabulary must survive having no medium at all.
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
}

case "${1:-both}" in
    healthy)  run_healthy ;;
    novolume) run_novolume ;;
    badpool)  run_badpool ;;
    uefi)     run_uefi ;;
    stranger) run_stranger ;;
    latearrival) run_latearrival ;;
    replug)   run_replug ;;
    both)     run_healthy; echo; run_novolume ;;
    all)      run_healthy; echo; run_novolume; echo; run_stranger; echo; run_latearrival; echo; run_replug; echo; run_uefi; echo; run_badpool ;;
    *) echo "usage: $0 [healthy|novolume|uefi|stranger|latearrival|replug|badpool|both|all]"; exit 2 ;;
esac

echo
echo "logcheck: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
