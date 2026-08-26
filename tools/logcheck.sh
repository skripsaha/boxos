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

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

# Did the machine answer a keystroke? The shell prints its command list in
# response to "help", so one line out of that list is the proof.
typed_ok() {
    grep -q "Show available commands" "$1"
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

    i=0
    while [ $i -lt 30 ]; do
        grep -q "AUTOSTART. Started" build/serial.log 2>/dev/null && break
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
    uefi)     run_uefi ;;
    stranger) run_stranger ;;
    latearrival) run_latearrival ;;
    both)     run_healthy; echo; run_novolume ;;
    all)      run_healthy; echo; run_novolume; echo; run_stranger; echo; run_latearrival; echo; run_uefi ;;
    *) echo "usage: $0 [healthy|novolume|uefi|stranger|latearrival|both|all]"; exit 2 ;;
esac

echo
echo "logcheck: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
