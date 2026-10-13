#!/bin/bash
# logcheck — oracle for the Logbook split (kernel occurrence vocabulary vs
# volume tag registry).
#
#   logcheck.sh healthy   boot with a mounted volume; the volume's vocabulary
#                         must be untouched and the kernel's must be complete
#   logcheck.sh novolume  boot with tagfs_recognise() forced false — the board
#                         failure reproduced on the desk. The kernel's
#                         vocabulary must survive having no medium at all.
#   logcheck.sh framerefuse a format-v2 volume (create_tagfs --frame-only) on
#                         BIOS q35 with every write recorded: refused in so
#                         many words, and not one write lands in its ground
#   logcheck.sh yank      the stick is pulled WHILE the volume is being read.
#                         The medium is throttled so the window exists at all
#   logcheck.sh twoctrl   two host controllers, stick on the SECOND — the shape
#                         the board has and no scenario could be until now
#   logcheck.sh logsave   the machine writes down what it said. Two kernels:
#                         one built PRINTTOFILE=on, which must produce a
#                         readable account of its own boot on the volume, and
#                         one without, which must SAY it keeps none
#   logcheck.sh footing  a UEFI boot is told to reboot: the kernel's own map
#                         goes under the firmware (Footing), the reset says
#                         which map it stood on, the EFI descriptor list is
#                         on the record, and the Lifeline stays silent
#   logcheck.sh census    the host's roll call (tools/volume-read.py check) and
#                         the kernel's fsck name the same runs, counts and free
#                         figure, and the same seal, on planted volumes; and a
#                         qemu-io blklogwrites log replays onto its base byte
#                         for byte
#   logcheck.sh censusmut either half of that reconcile broken on purpose — the
#                         kernel's pool walk, the census's retired-record rule —
#                         must turn it red
#   logcheck.sh stand     the startup tests that write (VOLUME_TESTS=on), on the
#                         legacy channel and on AHCI, each image booted twice:
#                         0 failed, the caveats held with their numbers, the
#                         faults fired where aimed, no Bitmap mismatch after
#   logcheck.sh standmut  each caveat's real fix put in must lift it early, and
#                         each fault mechanism broken must fail its self-test;
#                         a muted assertion, a judge that always holds and the
#                         old CR-12 body must each be caught
#   logcheck.sh quill     quilltest's red tests of the Quill write core on a grown
#                         volume, three boots: each case held under the step that
#                         discharges it, judged with the kernel's lines and the
#                         host's listing (QUILL_STAND picks the machine)
#   logcheck.sh quillmut  every quill oracle shown the behaviour it waits for —
#                         fixes emulated, sizes shrunk, the slab count cut, a
#                         copy's first touch taken away — must turn; and the
#                         caveat judge's three answers, word for word
#   logcheck.sh stackguard no boot: a block put on a TagFS stack, a held frame
#                         grown or lifted, the reach grown or its caveat gone —
#                         each must stop a plain make before the link, and the
#                         harness must still link a grown frame, saying so
#   logcheck.sh lockrankmut no boot: the LockRank judge shown a place no row
#                         holds, a row whose step was lifted, a place named
#                         twice, the table full and a self-deadlock — each a
#                         FAIL — and a bare spinlock put into TagFS, found
#   logcheck.sh abba      quilltest abba on UEFI STRICT 16c: the tag registry and
#                         the file table grown while three other cabins rename,
#                         create and tag; the run must reach its verdict (a stall
#                         judged against its own pace), the chains must grow, and
#                         the two order places are demanded as their rows say
#   logcheck.sh abbamut   a block taken under the registry's lock at every flush,
#                         its row's step lifted: abba's demand must say the
#                         defect is back
#   logcheck.sh writers   UEFI STRICT 16c: sixteen cabins each write and rewrite
#                         a file of their own; halted and booted again, every
#                         file is its last round byte for byte, on the medium
#                         and through the kernel, and the volume is whole
#   logcheck.sh appenders two cabins append to one file beside a spanner (16c
#                         AHCI), and two on a USB stick: every append where its
#                         answer put it, nothing else but the spans' zeros
#   logcheck.sh readdelete a big file read in windows while another cabin
#                         deletes it, cycles and writes a filler: the window
#                         read across it all is the file's own bytes
#   logcheck.sh defragdelete defrag and delete in a loop on 16c, the power
#                         button, a reboot: nothing deleted comes back, every
#                         slot whole, no block owned twice
#   logcheck.sh cores     the four of them
#   logcheck.sh both
#
# Every check is a grep against build/serial.log. No timing assertions.

set -u
STAND_NAME=logcheck
# ROOT, SCRATCH, the verdict counters, the mutation manifest and its
# preflights, the way out, the waits and the typer: tools/stand.sh.
. "$(dirname "$0")/stand.sh"

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
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3


    # Type into it. A machine you can look at and not talk to is not a machine
    # that booted — and "can it be typed on" is the only question the board
    # failure was ever really about.
    type_cmd "help"
    sleep 1
    key_press ret
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
    type_cmd "hw"
    sleep 1
    key_press ret
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
    mutate src/kernel/core/touch/logbook.c
    python3 - <<'EOF'
p = "src/kernel/core/touch/logbook.c"
s = open(p).read()
anchor = """    logbook_resolve(&spelled, out_full, out_bare, true);
}"""
probed = """    logbook_resolve(&spelled, out_full, out_bare, true);
    /* logcheck injection: the probe says which book answered each resolve */
    kprintf("[PROBE] logbook '%s' full=0x%x bare=0x%x\\n", tag, *out_full, *out_bare);
}"""
assert s.count(anchor) == 1, "probe anchor missing"
open(p, "w").write(s.replace(anchor, probed))
EOF
    mut_done src/kernel/core/touch/logbook.c "logcheck injection"
}
probe_off() {
    restore src/kernel/core/touch/logbook.c
}

mutate_on() {   # reproduce "no volume on this machine"
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])\n{\n"
assert anchor in s, "mutation anchor missing"
s = s.replace(anchor, anchor + "    return false;   /* logcheck mutation: this machine mounted nothing */\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation"
}
mutate_off() {
    restore src/kernel/tagfs/tagfs.c
}

# The boarding pass names a volume that is not in the room, while another
# perfectly good TagFS volume is. This is the case that used to mount the
# stranger in silence and then ignore the real medium for the rest of the boot.
stranger_on() {
    mutate src/kernel/core/boarding/boarding.c
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
    mut_done src/kernel/core/boarding/boarding.c "logcheck mutation"
}
stranger_off() {
    restore src/kernel/core/boarding/boarding.c
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

# The one string that says the late-arrival mutation is standing in the tree.
# mut_done proves it landed right after the edit; stick_from_boot_image asks the
# same question much later, and one variable is what keeps the two from drifting
# apart into a guard that has quietly stopped guarding.
LATEARRIVAL_MARK="logcheck mutation: the internal disk is not this machine's"

# ‼ AND ONLY WHERE AN ARRIVAL ASKS: A RETURN NEEDS NO SAYING.
#
# The stick is a copy of the boot image, so the internal disk carries the very
# same volume — the same sixteen bytes — for the whole run. tagfs_recognise is
# where an ARRIVAL asks whose volume a medium carries, and the mutation stands
# there. A RETURN asks somewhere else — the open book looks for the volume
# that left (volume_ground_on) — and the mutation used to stand there too:
# every return was judged against the internal disk, a medium that never left,
# measured so on 1.12 ("the volume came back in seat 0 with its Ledger as this
# kernel left it", the stick sitting in seat 1). The kernel now judges only a
# seat taken after the book opened (tagfs.c, g_book_seatings), and the
# internal disk was seated at boot, so that half of the mutation is gone: a
# return judged against the disk is the kernel's defect again, and red.
latearrival_on() {
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])\n{\n"
assert anchor in s, "late-arrival anchor missing"
s = s.replace(anchor, anchor + "    if (BoardroomSeatKind(seat) == BOARD_ATA) return false;   /* logcheck mutation: the internal disk is not this machine's */\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/tagfs/tagfs.c "$LATEARRIVAL_MARK" 1
}
latearrival_off() {
    restore src/kernel/tagfs/tagfs.c
}

# ── the stick has to BE this machine's volume, and that is sixteen bytes ──
#
# ‼ TAKEN AFTER `make run-bg`, NEVER BEFORE IT, AND THAT IS THE WHOLE POINT.
#
# create_tagfs stamps a FRESH identity into every volume it lays down
# (tools/create_tagfs.c, generate_uuid), so laying the same content down twice
# makes two DIFFERENT volumes. Take the stick off an image that is about to be
# laid down again and the copy is a volume that exists nowhere: the loader
# reads the new one and stamps THAT on the boarding pass, the Boardroom looks
# at the stick, sees sixteen bytes belonging to somebody else, and declines it
# — correctly, and out loud:
#
#   [Boardroom] the loader came from the volume <uuid>, and no seat is carrying it
#   [Boardroom] 1 volume(s) are here and none of them is this machine's
#
# The machine then has no filesystem at all, and every scenario below that
# needed one on the stick was judging a machine that never mounted anything.
# Six of them were, silently, for as long as the copy was taken where it was.
#
# WHAT LAYS THE IMAGE DOWN AGAIN IS THE MUTATION, NOT `make`.
#
# An idle `make` does NOT re-lay it, and the two ways of asking that suggest
# otherwise are both wrong. $(SHELL_BIN) has a .PHONY prerequisite, so its
# recipe does run on every make — but the sub-make writes nothing, shell.bin's
# mtime does not move, make re-stats it, and $(IMAGE) is left alone. And
# `make -n` / `make -q` are not evidence at all: under both, make still runs
# the $(MAKE) lines of a recipe and then counts that target remade, so a dry
# run always draws the whole chain. Measured on this make (GNU Make 3.81) with
# that exact shape in isolation — the recipe ran three times, the image was
# laid down once.
#
# The real churn is mut_bump, and the comment on mut_bump names its own price:
# the mutated source is stamped five seconds into the FUTURE, so "the next make
# rebuilds that ONE file a second time". Here that one file is the kernel's,
# and the second rebuild walks the whole way down:
#
#   latearrival_on   src/kernel/tagfs/tagfs.c := now + 5s
#   build            the tree is already built, so that one file is all there
#                    is to compile and tagfs.o lands inside those five seconds
#                    — finished, and already older than its own source
#   make run-bg      tagfs.o -> kernel.bin -> $(IMAGE) -> create_tagfs writes
#                    a NEW identity, and any copy taken before this is stale
#
# Read off the wreckage rather than argued: the stick left behind by the
# failing yank run carries the Deed uuid 890dcdee75f04e6babb9349821944ffe,
# while that same run's boarding pass named c1969b4289324e618829f74488a5e73b.
# One `cp`, two volumes — the image WAS laid down again in between, and this
# is the only thing in the run that lays it down. Which is also why the
# scenarios that mutate no source (grow, growcut, ctrlgiveup) were never hit.
#
# So the placement is the fix, and the three things it depends on are checked
# here rather than believed: the machine is already up (or the copy is being
# taken too early again), the late-arrival mutation is standing (or the guest
# mounts the internal disk and writes to the image while it is being copied,
# and the stick is torn), and the copy came out carrying a readable Deed whose
# sixteen bytes are this machine's.

stick_from_boot_image() {
    local stick boot
    if ! grep -q "$LATEARRIVAL_MARK" src/kernel/tagfs/tagfs.c; then
        bad "stick_from_boot_image: the late-arrival mutation is not installed, so this machine mounts its internal disk and writes to build/boxos.img while it is being copied"
        return 1
    fi
    if [ ! -f build/qemu.pid ] || ! kill -0 "$(cat build/qemu.pid)" 2>/dev/null; then
        bad "stick_from_boot_image: no machine is running — the copy has to be taken AFTER 'make run-bg', which is the one thing this helper exists to get right"
        return 1
    fi
    if ! cp build/boxos.img "$SCRATCH/stick.img"; then
        bad "stick_from_boot_image: could not copy build/boxos.img to $SCRATCH/stick.img"
        return 1
    fi
    stick=$(deed_uuid "$SCRATCH/stick.img") || stick=""
    boot=$(deed_uuid build/boxos.img)       || boot=""
    if [ -z "$stick" ] || [ "$stick" != "$boot" ]; then
        bad "stick_from_boot_image: the copy is not this machine's volume (stick '${stick:-no readable Deed}', image '${boot:-no readable Deed}')"
        return 1
    fi
    echo "  (the stick carries this machine's volume $stick)"
    return 0
}

# ── the stick's Ledger moved on while the stick was out of the machine ─────
#
# A medium that comes back is taken up with its open book only when its Ledger
# is the one this kernel left on it — the seq, and the seal's {settle,
# closed_crc32, tail_crc32} (tagfs/mount/away.h). Another BoxOS kernel that
# settled a cycle on the stick meanwhile wrote the next seq into the OTHER copy
# and sealed it as that cycle. This writes exactly that from the host, between
# the pull and the push: the newest copy laid into the other slot, its seq one
# higher, its seal naming that seq, its checksum made again — and nothing else.
# No table, no pool, no block of any file: the volume it describes is the same
# volume, and only the record of who settled it last has moved.
#
# ‼ ONLY THE LEDGER, AND ON PURPOSE. A host that changes a file's bytes and
# leaves the Ledger alone moves no seq, and the kernel does not see it — it
# says so itself, in the line that takes the book up — so that is not a change
# this can stand for.
#
# The image is the host's while this runs: a drive added over HMP goes with its
# device, so between the pull's `device_del` and the next `drive_add` nothing
# in QEMU holds the file.
ledger_moved_on_host() {
    python3 - "$1" <<'PY'
import struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk

path = sys.argv[1]
stand = medium.StandOnDeed(medium.Medium(path), None)
state = stand.deed["layout"]["state_block"]
lba = [stand.start + (state + c) * ondisk.SECTORS_PER_BLOCK for c in range(ondisk.LEDGER_COPIES)]
at = {name: off for name, off, _n in ondisk.VOLUME_LEDGER.Offsets()}
settle_at = ondisk.LEDGER_SEAL_AT + dict((n, o) for n, o, _s in ondisk.SETTLE_MARK.Offsets())["settle"]

with open(path, "r+b") as f:
    def read(c):
        f.seek(lba[c] * ondisk.SECTOR)
        return f.read(ondisk.SECTOR)
    was = [ondisk.ReadLedgerCopy(read(c), c) for c in range(ondisk.LEDGER_COPIES)]
    newest = ondisk.LedgerChoice(was)
    if newest is None:
        sys.exit("ledger_moved_on_host: no copy of the stick's Ledger reads intact")
    if not newest["sealed"]:
        sys.exit("ledger_moved_on_host: the newest copy (seq %u) carries no seal — "
                 "this kernel always seals, so the stick is not what it should be" % newest["seq"])
    src = newest["copy"]
    dst = (src + 1) % ondisk.LEDGER_COPIES
    seq = newest["seq"] + 1
    rec = bytearray(read(src))
    struct.pack_into("<Q", rec, at["seq"], seq)
    struct.pack_into("<Q", rec, settle_at, seq)
    struct.pack_into("<I", rec, at["crc32"], 0)
    struct.pack_into("<I", rec, at["crc32"], ondisk.Crc32(bytes(rec[:newest["bytes"]])))
    f.seek(lba[dst] * ondisk.SECTOR)
    f.write(rec)
    f.flush()
    now = [ondisk.ReadLedgerCopy(read(c), c) for c in range(ondisk.LEDGER_COPIES)]

stands = ondisk.LedgerChoice(now)
if (stands is None or stands["copy"] != dst or stands["seq"] != seq or not stands["sealed"]
        or not now[src]["ok"] or now[src]["seq"] != newest["seq"]):
    sys.exit("ledger_moved_on_host: the stick did not take the moved Ledger as written")
print("  (the stick's Ledger moved on the host: copy %u at seq %u, copy %u now seq %u, "
      "sealed as that cycle)" % (src, newest["seq"], dst, seq))
PY
}

run_latearrival() {
    echo "== latearrival: the volume arrives after the boot gave up =="
    # The mutation stays installed until the run is over: `make run-bg` has the
    # image as a prerequisite and will rebuild it, so restoring the source
    # first quietly boots an unmutated kernel. Cost an entire run to learn.
    latearrival_on; build
    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latearrival_off; return; }
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"

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
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    #
    # ‼ IF IT DID NOT HAND OVER, FIND OUT WHETHER THE MACHINE IS STILL ALIVE.
    #
    # Measured 2026-08-29: this scenario failed once, inside a forty-minute
    # matrix, with the log ending at `[AUTOSTART] Started 'display.elf'` and
    # nothing after it for sixty seconds. Standing alone it then passed three
    # times on that tree and three times on the tree before it — so it is not a
    # regression, and it is not reproducible on demand either.
    #
    # What was missing was not another run. It was the one fact that separates
    # the two things this can be: a machine WEDGED between two autostart
    # launches, and a machine perfectly alive and merely slow reading a
    # 772 KB program off a stick. The stand-in shell is still on the keyboard
    # at that moment, so asking it is free — and a `help` that answers proves
    # the cores are running, which moves the hunt onto the volume read and off
    # the scheduler entirely.
    #
    # Costs four seconds on a run that fails and nothing at all on one that
    # does not.
    if ! grep -q "hands over" build/serial.log 2>/dev/null; then
        echo "-- it did not hand over; asking whether the machine is alive --"
        type_cmd "help"
        sleep 1
        key_press ret
        sleep 3
    fi

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.latearrival.log"
    latearrival_off
    L="$SCRATCH/serial.latearrival.log"

    grep -q "no volume yet" "$L";      chk $? "the boot found no volume of its own"
    grep -q "Fallback shell ready" "$L"; chk $? "and stood a shell in for one"
    grep -q "mass storage" "$L";       chk $? "the stick enumerated after the boot"
    grep -q "took a seat while the machine was\|seat 1:" "$L"
    chk $? "the room seated it"

    # ‼ AND IT IS ASKED WHAT IT TAKES, and answers as itself rather than as the
    # slowest medium in the machine. A flash drive swallows its whole bounce
    # buffer in one command, so its neighbouring blocks are fetched together —
    # measured on this path: 216 commands to mount and start a volume became
    # 66, and the wall time from plugging in to a shell halved.
    # 128 is this drive's bounce buffer in sectors (MSD_BOUNCE_BYTES / 512) —
    # a number that comes from the DEVICE. Named exactly rather than loosely,
    # so that going back to one figure imposed on every medium reddens this
    # whichever figure is chosen.
    grep -q "this medium hands over 128 sector(s) in one go" "$L"
    chk $? "the flash drive is asked what IT takes, and answers as itself"
    grep -q "seat 1 carries the volume this kernel was read out of" "$L"
    chk $? "and recognised it by the boarding pass, not by a rule"
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "TagFS mounted it"
    grep -q "AUTOSTART. Started .display.elf" "$L"; chk $? "the volume's display daemon started"
    grep -q "AUTOSTART. Started .shell.bin" "$L";   chk $? "the volume's shell started"
    grep -q "the stand-in .PID 1. hands over" "$L"; chk $? "and the stand-in handed over"

    # Only meaningful when the line above went red — and then it is the whole
    # of what the next session needs. Not a check: a fact, printed.
    if ! grep -q "the stand-in .PID 1. hands over" "$L"; then
        if typed_ok "$L"; then
            echo "       the machine was ALIVE and answering the keyboard"
            echo "       => the stall is in reading the volume, not in the scheduler"
        else
            echo "       the machine did NOT answer the keyboard"
            echo "       => it was wedged, and the last line it printed says where"
        fi
        echo "       last line: $(tail -1 "$L")"
    fi
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
# ‼ SINCE 1.12 A RETURN IS JUDGED, AND THERE ARE TWO RETURNS HERE.
#
# A medium that leaves takes nothing down: the volume's book is held open, and
# the return decides it (tagfs/mount/away.h). The SAME stick, as this kernel
# left it — its Ledger's seq and seal the ones a cycle left there, every write
# the volume saw kept by a flush — is TAKEN UP: no second mount, nothing read
# again, the files it held still the files it holds. A stick whose Ledger moved
# while it was away is not the book this kernel holds, and is GIVEN UP: the
# volume is let go of and mounted from the medium as it now is. So the stick
# goes out twice. The first time it comes back as it left; the second time the
# host moves its Ledger on, as another BoxOS kernel settling a cycle on it
# would (ledger_moved_on_host), and it must not be taken for the same book.
#
# The file that proves the first return is `anchor_test`'s, written and
# anchored before the stick goes: anchored, so the volume's every write is
# covered by a flush the medium kept, which is what "as this kernel left it"
# asks — the anchor's line is the event the pull waits for, not a clock. After
# the return the same program reads it back and checks it, deletes it, and
# writes and anchors it again: the volume reads, and it takes a write. The
# second return is proved by the same file read off the volume mounted anew.
#
# ‼ RED ON 1.12, FOUR CHECKS, AND THE KERNEL IS WHY. The first return is never
# taken up. The anchor's cycle ends by laying again the Spares a delete stands
# on (SettleRunEnd, settle.c: PoolSpareRefill after the cycle's flush), and
# every Spare is stamped on the medium — a write the volume saw complete that
# no kept flush covers until the next cycle. VolumeWritesKept() answers no, the
# stick that left exactly as it was is judged AWAY_IN_DOUBT, and its book is
# given up and read again; a volume mounted and never written is judged the
# same, by the mount's own refill (tagfs_init). Measured: with one flush after
# that refill — not in the tree — this is 23 passed, 0 failed. The four are the
# claim, so they stay red until the kernel keeps it.
#
# ‼ THE MUTATIONS IT EXISTS FOR, one per return, each measured on 1.12:
#
#   take the take-up out of volume_book_decided (tagfs.c: `if (verdict ==
#      AWAY_AS_LEFT) { volume_book_taken_up(...); return true; }`) — every
#      return is given up and read again, and the first one mounts a second
#      time. MEASURED with the flush above: 19 passed, 4 failed — exactly the
#      four the kernel reddens by itself today, so until it is mended this
#      mutation and that defect read as one result;
#   take `if (as == LEDGER_OTHER) return AWAY_OTHER;` out of AwayJudge
#      (away.c) — the stick whose Ledger moved is taken up as the book this
#      kernel holds, and nothing is read again. MEASURED: 17 passed, 6 failed
#      (the doubt above still gives that book up, so its judgement and its
#      order are what go red); with the flush, 18 passed, 5 failed.
#
# Four cores, same as latearrival: the departure is taken down from the idle
# and guide loops, and on one core neither of them runs.
run_replug() {
    echo "== replug: the volume leaves and comes home to the same chair =="
    latearrival_on; build
    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latearrival_off; return; }
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    # First arrival — the same one latearrival proves.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    # Waited out to the END of the chain, not to its first sign. The stand-in
    # handing over is the last thing the first arrival does; pulling the stick
    # before then tests something else entirely — a medium yanked out from
    # under a program being loaded — and reports it as this.
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    # A file on the stick before anything leaves, and every write under a
    # flush the medium kept: anchor_test writes and anchors it.
    type_cmd "anchor_test"
    sleep 1; key_press ret
    wait_for_line build/serial.log "\[ANCHOR\] WROTE+ANCHORED" "$WAIT_QUIET_S"
    sleep 2
    # A mark in the log, so the returns are read apart from the first arrival:
    # every line the checks below care about has a twin above it.
    local FIRST_LINES SECOND_LINES AT
    FIRST_LINES=$(wc -l < build/serial.log)

    # ── the first return: the stick as it left ─────────────────────────────
    mon_raw "device_del usbstick"
    wait_for_line build/serial.log "is empty" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 2

    # And now, with the medium out, ask for a program that IS on it and one
    # that is not. Those are different facts and used to print the same
    # sentence — on a live board the machine told the user that a command
    # which exists does not exist, two lines under the kernel saying why.
    type_cmd "files"
    sleep 1; key_press ret; sleep 3
    type_cmd "nosuchthing"
    sleep 1; key_press ret; sleep 3

    # And pushed back in — the same medium, into the machine it left.
    #
    # The drive is added again first. A drive created through the HMP
    # `drive_add` is auto-delete: QEMU takes it away with the device that was
    # using it, so the second `device_add` referred to a drive that no longer
    # existed and did nothing at all. It cost a run to find, because a monitor
    # command that fails is silent on the guest's serial line.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "the volume is back\|its book is given up" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 3

    # The file from before the pull, read back and checked; then written and
    # anchored again, which is the volume taking a write after the return.
    AT=$(wc -l < build/serial.log)
    type_cmd "anchor_test"
    sleep 1; key_press ret
    wait_for_line build/serial.log "\[ANCHOR\] PASS\|\[ANCHOR\] FAIL\|\[ANCHOR\] read rc" "$WAIT_QUIET_S" $((AT + 1))
    sleep 2
    AT=$(wc -l < build/serial.log)
    type_cmd "anchor_test"
    sleep 1; key_press ret
    wait_for_line build/serial.log "\[ANCHOR\] WROTE+ANCHORED\|\[ANCHOR\] create rc\|\[ANCHOR\] fwrite rc\|\[ANCHOR\] anchor rc" "$WAIT_QUIET_S" $((AT + 1))
    sleep 2
    SECOND_LINES=$(wc -l < build/serial.log)

    # ── the second return: the stick's Ledger moved while it was away ──────
    mon_raw "device_del usbstick"
    wait_for_line build/serial.log "the medium the volume lives on has left" "$WAIT_QUIET_S" $((SECOND_LINES + 1))
    sleep 2
    local MOVED=1
    ledger_moved_on_host "$SCRATCH/stick.img" && MOVED=0
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "the volume is back\|is MOUNTED\|would not mount" "$WAIT_QUIET_S" $((SECOND_LINES + 1))
    sleep 3
    AT=$(wc -l < build/serial.log)
    type_cmd "anchor_test"
    sleep 1; key_press ret
    wait_for_line build/serial.log "\[ANCHOR\] PASS\|\[ANCHOR\] FAIL\|\[ANCHOR\] read rc\|\[ANCHOR\] WROTE" "$WAIT_QUIET_S" $((AT + 1))
    sleep 2

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.replug.log"
    latearrival_off
    L="$SCRATCH/serial.replug.log"
    # The first return, and the second, each read alone.
    sed -n "$((FIRST_LINES + 1)),${SECOND_LINES}p" "$L" > "$SCRATCH/serial.replug.second.log"
    tail -n +$((SECOND_LINES + 1)) "$L" > "$SCRATCH/serial.replug.third.log"
    local S="$SCRATCH/serial.replug.second.log" T="$SCRATCH/serial.replug.third.log"

    # The first arrival happened at all — otherwise the rest is vacuous.
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the stick was mounted on its first arrival"
    grep -q "AUTOSTART. Started .shell.bin" "$L"; chk $? "and its shell started"
    grep -q "the stand-in .PID 1. hands over" "$L"; chk $? "and the stand-in handed over"
    head -n "$FIRST_LINES" "$L" | grep -q "\[ANCHOR\] WROTE+ANCHORED"
    chk $? "a file was written and anchored on it before it left"

    # ── the first return ───────────────────────────────────────────────────
    # The departure NAMES the chair. Saying only "something left" is what made
    # the room unable to empty the right one.
    grep -qE "seat [0-9]+ is empty . usb[0-9]+" "$S"
    chk $? "the room emptied the chair and named it"
    grep -q "the medium the volume lives on has left — its book is held open" "$S"
    chk $? "TagFS noticed the medium go, and held its book open"

    # A program that is on the absent volume, and one that is on no volume at
    # all, must not get the same sentence.
    grep -q "files: is there and would not start" "$S"
    chk $? "a real command on an absent medium says so"
    grep -q "Unknown command: nosuchthing" "$S"
    chk $? "and a command that does not exist is still unknown"

    # The second arrival is announced. This is the whole repair: it used not to
    # be, because the room only ever spoke when it GREW.
    grep -q "took a seat while the machine was" "$S"
    chk $? "the return was announced, not swallowed"

    # ‼ TAKEN UP, in the chair it left, and not read again.
    grep -qE "the volume is back, in seat 1: .* as this kernel left it" "$S"
    chk $? "the stick as it left was taken back up, in the seat it had"
    ! grep -q "its book is given up" "$S"
    chk $? "and its book was not given up"
    ! grep -q "\[TagFS\] the volume on seat .* is MOUNTED" "$S"
    chk $? "and nothing was mounted a second time"

    # Into the chair it left, not a new one. The room must not grow a chair
    # per plug cycle — sixteen of them is what a live board showed.
    ! grep -q "seat 2:" "$L"
    chk $? "no new chair was added for the returning stick"

    # The file from before the pull is there and reads back whole, and the
    # volume takes a write after the return.
    grep -q "\[ANCHOR\] PASS" "$S"
    chk $? "the file written before the pull reads back intact"
    grep -q "\[ANCHOR\] WROTE+ANCHORED" "$S"
    chk $? "and the volume takes a write and anchors it after the return"

    # ── the second return: the stick's Ledger moved while it was away ──────
    [ "$MOVED" = 0 ]
    chk $? "the host moved the stick's Ledger on while it was out"
    grep -q "came back in seat 1 with a Ledger or a Deed this kernel did not leave on it" "$T"
    chk $? "the stick whose Ledger moved was judged not to be the book this kernel holds"
    ! grep -q "the volume is back, in seat" "$T"
    chk $? "and was not taken up as that book"
    grep -q "\[TagFS\] the volume on seat 1 is MOUNTED" "$T"
    chk $? "the volume was mounted anew from the medium as it now is"
    local GIVEN_AT UP_AT READ_AT
    GIVEN_AT=$(grep -n "did not leave on it" "$T" | head -1 | cut -d: -f1)
    UP_AT=$(grep -n "is MOUNTED" "$T" | head -1 | cut -d: -f1)
    READ_AT=$(grep -n "\[ANCHOR\] PASS" "$T" | head -1 | cut -d: -f1)
    [ -n "$GIVEN_AT" ] && [ -n "$UP_AT" ] && [ -n "$READ_AT" ] && \
        [ "$GIVEN_AT" -lt "$UP_AT" ] && [ "$UP_AT" -lt "$READ_AT" ]
    chk $? "given up, then mounted, then the file read back off it — in that order (${GIVEN_AT:-?}, ${UP_AT:-?}, ${READ_AT:-?})"

    # One arrival mounts; the unchanged return mounts nothing; the changed one
    # mounts once. Two mounts, and one of them is the first.
    local mounts
    mounts=$(grep -c "\[TagFS\] the volume on seat .* is MOUNTED" "$L")
    [ "$mounts" = 2 ]
    chk $? "two mounts in the whole run: the arrival and the changed return ($mounts)"

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

# ── replugwrite: the stick leaves while a write is in flight ───────────────
#
# replug pulls the stick from an idle machine. This pulls it from under a
# write going through the port, with writers queued behind it for the file's
# Helm (RT-13): `quilltest away` appends with REPLUGWRITE_COUNT writers of
# 4 MiB each to a file of its own on the stick, the stick's writes held to
# REPLUGWRITE_BPS so one of them is still in the medium when the stick goes —
# once every writer has asked and the stick has taken a write since, counted
# off QEMU's own block statistics, never off a clock. Two rounds on one boot
# (USB, four cores, the stick the volume as in replug):
#
#   same   the stick comes back as it left. The book is taken up — every
#          waiter handed the Helm, its write whole — or, a write the volume saw
#          complete that no kept flush covered (the holder's last blocks), given
#          up as AWAY_IN_DOUBT and the volume read again; any other verdict is a
#          FAIL for a stick nobody touched;
#   moved  the stick's Ledger is moved on from the host while it is out
#          (ledger_moved_on_host): AWAY_OTHER, the book given up, and nothing of
#          the old book reaches the changed medium — no waiter's byte on it.
#
# Either way: the holder answers ERR_DEVICE_NOT_READY (203) with the bytes the
# file holds of its write, every waiter is answered (a book given up: 203 and
# nothing of it done; taken up: its write whole), away hears every writer, the
# volume is up again, and `quilltest awaycheck` reads the file back through it:
# its first block, then one chunk per writer that kept anything, each its
# writer's stream from its first byte — none longer than its writer was told
# the file holds, and, taken up, each exactly that. After the second round the
# machine halts through bye ("Storage sync complete"), volume-read finds the
# stick consistent, reads both files off it and cuts them into the same chunks
# awaycheck said, and finds no block of a waiter's stream anywhere on it. A
# pull that takes a cycle's block map with it and not its Ledger leaves blocks
# the map marks used and no settled record names: the ORPHAN PowerCut holds
# under 1.6 (B1, stand_caveat.sh), held here too — only beside a cycle the
# departure refused, and a FAIL anywhere else.
#
# ‼ THE MUTATIONS IT EXISTS FOR, MEASURED:
#   HelmBookGivenUp answering nobody (helm.c: its walk taken out) — the
#      waiters wait for good, the take-down waits for them ("2 caller(s) are
#      still inside it"), nothing is mounted again and away never says: 0
#      passed, 1 failed, the first round's return gone quiet;
#   the changed stick taken up as the book this kernel holds (tagfs.c
#      volume_book_decided: `verdict == AWAY_AS_LEFT` true for AWAY_OTHER
#      too) — the waiters are handed the Helm and their 4 MiB each go to a
#      medium another kernel settled: 15 passed, 3 failed — the judgement, the
#      waiters' answers, and 2048 blocks of their streams found on the stick.
# Where a round's return is IN_DOUBT the book is given up whatever AwayJudge
# makes of the Ledger, so taking `if (as == LEDGER_OTHER) return AWAY_OTHER;`
# out of it reddens only the judgement line, as replug says.
REPLUGWRITE_BPS=262144     # a quarter of a MiB a second: one writer's 4 MiB takes sixteen
REPLUGWRITE_COUNT=3
REPLUGWRITE_BYTES=4194304  # quilltest_away.c QUILL_AWAY_MIB
REPLUGWRITE_ROUNDS="same:_qaway1:16 moved:_qaway2:32"   # CHANGE:NAME:SEED

replugwrite_wrops() {   # the stick's writes so far, as QEMU counts them
    ./tools/qemu-input.sh raw "info blockstats" 2>/dev/null | tr -d '\r' |
        grep -aoE '^stick: .* wr_operations=[0-9]+' | sed 's/.*wr_operations=//'
}

replugwrite_drive_gone() {   # QEMU has let go of the stick's drive, as its own listing says
    wait_until "$WAIT_QUIET_S" "QEMU to let go of the stick's drive" \
        '! ./tools/qemu-input.sh raw "info block" 2>/dev/null | tr -d "\r" | grep -aq "^stick "'
}

# replugwrite_round CHANGE NAME SEED — one round on the running machine: the
# stick's writes slowed, away typed, its writers seen asking and the stick seen
# taking a write since, the stick pulled, changed on the host for `moved`,
# pushed back, away's verdict and the volume up again waited for, awaycheck
# typed and answered. Leaves REPLUGWRITE_MOVED (0 when the host moved the
# Ledger) and REPLUGWRITE_SILENT (what never came once the stick was back).
# Answers 1 when the round could not be put to the machine, 2 when it was and
# the machine went quiet without what the return owes — the verdict, not the
# harness.
replugwrite_round() {
    local change=$1 name=$2 seed=$3 from w0
    REPLUGWRITE_MOVED=1
    REPLUGWRITE_SILENT=""
    from=$(wc -l < build/serial.log)
    mon_raw "block_set_io_throttle stick 0 0 $REPLUGWRITE_BPS 0 0 0" || return 1
    type_line "quilltest away $name $REPLUGWRITE_COUNT $seed" || return 1
    wait_until "$WAIT_QUIET_S" "$REPLUGWRITE_COUNT writers of $name to ask" \
        '[ "$(tail -n +$((from + 1)) build/serial.log | grep -acE "awaywriter: $name fid=[0-9]+ seed [0-9]+ pid [0-9]+ appends")" -ge "$REPLUGWRITE_COUNT" ]' ||
        return 1
    w0=$(replugwrite_wrops)
    [ -n "$w0" ] || return 1
    wait_until "$WAIT_QUIET_S" "the stick to take a write of theirs" '[ "$(replugwrite_wrops)" -gt "$w0" ]' ||
        return 1
    mon_raw "device_del usbstick" || return 1
    wait_for_line build/serial.log "the medium the volume lives on has left" "$WAIT_QUIET_S" $((from + 1)) ||
        return 1
    # The guest's word that the stick left is not QEMU's that its drive is
    # gone: a drive_add in between is refused as a duplicate, in silence, and
    # the stick never comes back. So QEMU is asked, and the image is touched
    # only once nothing in it holds the file.
    replugwrite_drive_gone || return 1
    if [ "$change" = moved ]; then
        ledger_moved_on_host "$SCRATCH/stick.img" && REPLUGWRITE_MOVED=0
    fi
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw" || return 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick" || return 1
    wait_for_line build/serial.log "took a seat while the machine was" "$WAIT_QUIET_S" $((from + 1)) ||
        return 1
    REPLUGWRITE_SILENT="away's word that every writer was answered"
    wait_for_line -E build/serial.log "\[QUILL\] away: (PASS|FAIL) — $name " "$WAIT_QUIET_S" $((from + 1)) ||
        return 2
    REPLUGWRITE_SILENT="the volume up again"
    wait_for_line -E build/serial.log "the volume is back, in seat|\[TagFS\] the volume on seat [0-9]+ is MOUNTED|would not mount" \
        "$WAIT_QUIET_S" $((from + 1)) || return 2
    REPLUGWRITE_SILENT="awaycheck's answer"
    from=$(wc -l < build/serial.log)
    type_line "quilltest awaycheck $name $REPLUGWRITE_COUNT $seed" &&
        wait_for_line -E build/serial.log "\[QUILL\] awaycheck: (PASS|FAIL) — $name " "$WAIT_QUIET_S" $((from + 1)) ||
        return 2
    REPLUGWRITE_SILENT=""
}

# replugwrite_answers LOG NAME — "SEED WRITTEN ERROR AFTER" for every writer of
# NAME that said its answer; AFTER is 1 when it said it after the stick took
# its seat again — a writer that waited through the return — and 0 before.
replugwrite_answers() {
    tr -d '\r' < "$1" | awk -v name="$2" '
        /took a seat while the machine was/ { back = 1 }
        $0 ~ "^\\[QUILL\\] awaywriter: " name " seed [0-9]+ answered [0-9]+ of [0-9]+ byte\\(s\\) the file.s \\(error -?[0-9]+\\)$" {
            e = $NF; sub(/\)$/, "", e)
            print $5, $7, e, back + 0
        }'
}

# replugwrite_chunks LOG NAME — awaycheck's chunks of NAME as "SEED BYTES" lines
# (SEED 0 for the base), and nothing when it did not pass.
replugwrite_chunks() {
    tr -d '\r' < "$1" | sed -nE "s/^\[QUILL\] awaycheck: PASS — $2 fid=[0-9]+ holds [0-9]+ bytes: (.*) \(the first byte no chunk's .*/\1/p" |
        tail -1 | tr ',' '\n' | sed -E 's/^ *//; s/^base /0 /; s/^seed //'
}

# replugwrite_judge LOG CHANGE NAME SEED — one round, read off its slice of the
# log. Adds to REPLUGWRITE_ABSENT the seeds whose bytes must be nowhere on the
# stick: every writer that waited through a return its book was given up at —
# and, for the moved stick, through its return whatever it was judged — and
# every writer answered that the file holds none of its write.
replugwrite_judge() {
    local L=$1 change=$2 name=$3 seed=$4 what="the $2 stick" verdict up s w e a c held=0 bad_held="" fid
    fid=$(tr -d '\r' < "$L" | sed -nE "s/^\[QUILL\] away: $name fid=([0-9]+) laid down.*/\1/p" | head -1)
    grep -aqE "^\[QUILL\] away: PASS — $name fid=[0-9]+: $REPLUGWRITE_COUNT of $REPLUGWRITE_COUNT writer\(s\) started and $REPLUGWRITE_COUNT answered" "$L"
    chk $? "replugwrite: $what — every one of the $REPLUGWRITE_COUNT writers was answered ($(tr -d '\r' < "$L" | grep -aoE "\[QUILL\] away: (PASS|FAIL) — .*" | tail -1))"
    if ! grep -aqE "^\[Storage\] write to file ${fid:-x} refused \(error 203\): [0-9]+ of its $REPLUGWRITE_BYTES byte\(s\) are the file's" "$L"; then
        harness "replugwrite: $what — no write of $name was in the medium when it left, so the case was not asked"
        return
    fi
    ok "replugwrite: $what — the write in the medium answered ERR_DEVICE_NOT_READY with the bytes the file holds of it: $(tr -d '\r' < "$L" | grep -aoE "write to file $fid refused \(error 203\): [0-9]+ of its [0-9]+ byte\(s\) are the file's" | head -1)"
    verdict=$(tr -d '\r' < "$L" | grep -aE '^\[TagFS\] (the volume is back, in seat|the volume came back in seat|another volume sits in seat)' | head -1)
    case "$change:$verdict" in
        same:*"the volume is back, in seat"*) up=taken ;;
        same:*"with its Ledger as this kernel left it"*"was never covered by a flush"*) up=given ;;
        moved:*"with a Ledger or a Deed this kernel did not leave on it"*) up=given ;;
        *) up="" ;;
    esac
    [ -n "$up" ]
    chk $? "replugwrite: $what — the return was judged as such a stick must be: ${verdict:-no verdict at all}"
    [ "$change" = moved ] && { [ "$REPLUGWRITE_MOVED" = 0 ]; chk $? "replugwrite: $what — the host moved its Ledger on while it was out"; }
    if [ "$up" = given ]; then
        grep -aqE '^\[TagFS\] the volume on seat [0-9]+ is MOUNTED' "$L"
        chk $? "replugwrite: $what — the book given up, the volume was mounted again from the medium as it now is"
    fi
    # Every writer's answer, as the book's fate makes it, and its chunk: one
    # that waited through the return is handed the Helm and writes whole when
    # the book is taken up, and is answered 203 with nothing done when it is
    # given up; one answered before it finished whole, or held the Helm when
    # the stick left and is answered 203 with what the file holds of it — all
    # of which a book taken up keeps, and one given up keeps only what a cycle
    # settled.
    while read -r s w e a; do
        [ -n "$s" ] || continue
        c=$(replugwrite_chunks "$L" "$name" | awk -v s="$s" '$1 == s { print $2 }')
        c=${c:-0}
        case "$up:$a:$e" in
            taken:1:0)   [ "$w" = "$REPLUGWRITE_BYTES" ] && [ "$c" = "$w" ] ;;
            given:1:203) [ "$w" = 0 ] && [ "$c" = 0 ] ;;
            taken:0:0)   [ "$w" = "$REPLUGWRITE_BYTES" ] && [ "$c" = "$w" ] ;;
            taken:0:203) [ "$c" = "$w" ] ;;
            given:0:0)   [ "$w" = "$REPLUGWRITE_BYTES" ] && [ "$c" -le "$w" ] ;;
            given:0:203) [ "$c" -le "$w" ] ;;
            *)           false ;;
        esac || bad_held="$bad_held seed $s answered $w (error $e, $([ "$a" = 1 ] && echo after || echo before) the return) and the file keeps $c of it;"
        if [ "$w" = 0 ] || { [ "$a" = 1 ] && { [ "$up" = given ] || [ "$change" = moved ]; }; }; then
            REPLUGWRITE_ABSENT="$REPLUGWRITE_ABSENT $s"
        fi
        held=$((held + 1))
    done <<REPLUGWRITE_ANSWERS
$(replugwrite_answers "$L" "$name")
REPLUGWRITE_ANSWERS
    [ "$held" = "$REPLUGWRITE_COUNT" ] && [ -n "$up" ] && [ -z "$bad_held" ]
    chk $? "replugwrite: $what — $held writer answer(s), each as a book ${up:-neither taken nor given} up makes it, and the file keeps of each what it should${bad_held:+ —$bad_held}"
    grep -aqE "^\[QUILL\] awaycheck: PASS — $name " "$L"
    chk $? "replugwrite: $what — read back through the volume up now, every byte of the file is its first block or one writer's stream from its first byte ($(tr -d '\r' < "$L" | grep -aoE "\[QUILL\] awaycheck: (PASS|FAIL) — .*" | tail -1))"
}

# replugwrite_medium IMG ROUNDS LOG — off the stick the machine left: each
# round's file cut into the same chunks awaycheck said, and no block anywhere
# on it that begins a block of a stream REPLUGWRITE_ABSENT names — what a
# waiter would have written had the old book handed it the Helm.
replugwrite_medium() {
    local img=$1 rounds=$2 log=$3 r change name seed out rc
    for r in $rounds; do
        IFS=: read -r change name seed <<REPLUGWRITE_ROUND
$r
REPLUGWRITE_ROUND
        out="$SCRATCH/replugwrite.$name.bin"
        rm -f "$out"
        python3 tools/volume-read.py "$img" get "$name" -o "$out" >/dev/null 2>&1
        # Into a file, not a $( ): bash 3.2 reads a quote inside a here-document
        # inside a command substitution as the shell's own.
        python3 - "$out" "$seed" "$REPLUGWRITE_COUNT" "$REPLUGWRITE_BYTES" \
            "$(replugwrite_chunks "$log.$name" "$name" | tr '\n' ',')" >"$out.said" 2>&1 <<'REPLUGWRITE_EOF'
import struct, sys
path, first, count, cap, said = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
M = (1 << 64) - 1
def word(seed, off):     # quilltest.c QuillWord, the stream QuillFill lays
    x = (off ^ ((seed << 56) & M)) & M
    x = (x * 0x9E3779B97F4A7C15) & M
    return x ^ (x >> 29)
try:
    data = open(path, "rb").read()
except OSError:
    sys.exit("volume-read found no such file on the stick")
chunks = []
for i in range(0, len(data) - len(data) % 8, 8):
    w = struct.unpack_from("<Q", data, i)[0]
    cur = chunks[-1] if chunks else None
    if cur and i - cur[1] < (4096 if cur[0] == first else cap) and w == word(cur[0], i - cur[1]):
        cur[2] += 8
        continue
    if i == 0:
        seed = first if w == word(first, 0) else None
    else:
        seed = next((first + k for k in range(1, count + 1)
                     if all(c[0] != first + k for c in chunks) and w == word(first + k, 0)), None)
    if seed is None:
        sys.exit("byte %d of its %d is nobody's" % (i, len(data)))
    chunks.append([seed, i, 8])
got = ",".join("%d %d" % (0 if s == first else s, n) for s, _a, n in chunks) + ","
if got != said:
    sys.exit("the stick holds %s where awaycheck said %s" % (got.rstrip(","), said.rstrip(",") or "nothing"))
print("%d bytes: %s" % (len(data), got.rstrip(",")))
REPLUGWRITE_EOF
        rc=$?
        chk $rc "replugwrite: the $change stick's $name, read off the medium by volume-read, is cut into the very chunks awaycheck said — $(tail -1 "$out.said")"
        rm -f "$out" "$out.said"
    done
    out="$SCRATCH/replugwrite.scan"
    # shellcheck disable=SC2086
    python3 - "$img" "$REPLUGWRITE_BYTES" $REPLUGWRITE_ABSENT >"$out" 2>&1 <<'REPLUGWRITE_EOF'
import struct, sys
img, cap, seeds = sys.argv[1], int(sys.argv[2]), [int(s) for s in sys.argv[3:]]
M = (1 << 64) - 1
def word(seed, off):     # quilltest.c QuillWord, the stream QuillFill lays
    x = (off ^ ((seed << 56) & M)) & M
    x = (x * 0x9E3779B97F4A7C15) & M
    return x ^ (x >> 29)
heads = {word(s, k): (s, k) for s in seeds for k in range(0, cap, 4096)}
found = []
with open(img, "rb") as f:
    at = 0
    while True:
        b = f.read(4096)
        if len(b) < 8:
            break
        w = struct.unpack_from("<Q", b, 0)[0]
        if w in heads:
            found.append((at, heads[w]))
        at += len(b)
if found:
    sys.exit("%d block(s) of them lie on the stick, the first at byte %d (seed %d, its byte %d)"
             % (len(found), found[0][0], found[0][1][0], found[0][1][1]))
print("none of the %d block-heads of seeds %s lies on it" % (len(heads), seeds))
REPLUGWRITE_EOF
    rc=$?
    chk $rc "replugwrite: no block of a writer that waited through a book given up, or through the moved stick's return, or was answered that the file holds none of its write, is anywhere on the stick — $(tail -1 "$out")"
}

# replugwrite_census JSON LOG — volume-read's roll call of the stick the
# machine left: consistent, or every finding a leak of ORPHAN blocks a pull
# left by cutting a cycle between its map and its Ledger — held under its row
# (stand_caveat.sh, B1) only when LOG shows a cycle the departure refused —
# and anything else a FAIL with its words.
replugwrite_census() {
    local json=$1 log=$2 kind text
    while IFS="$(printf '\t')" read -r kind text; do
        case "$kind" in
            consistent) ok "replugwrite: volume-read finds the stick consistent — $text" ;;
            orphan)
                if grep -aqE '^\[Settle\] the cycle that would have settled [0-9]+ change\(s\) was refused at ' "$log"; then
                    caveat_judge replugwrite:ORPHAN defect "the stick after its returns: $text — a cycle the departure refused ($(tr -d '\r' < "$log" | grep -aoE 'the cycle that would have settled [0-9]+ change\(s\) was refused at [a-z ]+' | head -1))"
                else
                    bad "replugwrite: the stick after its returns holds an ORPHAN no refused cycle explains — $text"
                fi ;;
            *) bad "replugwrite: volume-read finds the stick ${kind:-unread}: $text" ;;
        esac
    done <<REPLUGWRITE_CENSUS
$(python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except (OSError, ValueError) as e:
    print("unreadable\tno roll call: %s" % e); sys.exit()
bad = [f for f in d.get("findings", []) if f.get("severity") != "note"]
if not bad:
    print("consistent\t%s, %d note(s)" % (d.get("verdict"), len(d.get("findings", []))))
for f in bad:
    print("%s\t%s: %s" % ("orphan" if (f.get("code"), f.get("severity")) == ("ORPHAN", "leak") else f.get("severity"),
                          f.get("code"), f.get("text")))
' "$json")
REPLUGWRITE_CENSUS
}

run_replugwrite() {
    echo "== replugwrite: the stick leaves while a write is in flight and writers wait for its Helm, and comes back as it left, then changed =="
    local r change name seed from rc last pid L="$SCRATCH/serial.replugwrite.log"
    REPLUGWRITE_ABSENT=""
    latearrival_on; build
    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latearrival_off; return; }
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    if ! wait_for_line build/serial.log "the stand-in .PID 1. hands over" "$WAIT_QUIET_S"; then
        make run-stop >/dev/null 2>&1; cp build/serial.log "$L"; latearrival_off
        harness "replugwrite: the stick never became this machine's volume"
        return
    fi
    wait_for_quiet "$BOOT_BURST_QUIET_S"
    for r in $REPLUGWRITE_ROUNDS; do
        IFS=: read -r change name seed <<REPLUGWRITE_ROUND
$r
REPLUGWRITE_ROUND
        from=$(wc -l < build/serial.log)
        replugwrite_round "$change" "$name" "$seed"; rc=$?
        if [ "$rc" != 0 ]; then
            make run-stop >/dev/null 2>&1; cp build/serial.log "$L"; latearrival_off
            last=$(tr -d '\r' < "$L" | grep -aE '^\[(QUILL|TagFS|Storage)\]' | tail -1 | cut -c1-160)
            if [ "$rc" = 1 ]; then
                harness "replugwrite: the $change round could not be put to the machine (last: $last)"
            else
                bad "replugwrite: the $change stick came back and the machine went quiet without $REPLUGWRITE_SILENT (last: $last)"
            fi
            return
        fi
        tail -n +$((from + 1)) build/serial.log > "$L.$name"
        replugwrite_judge "$L.$name" "$change" "$name" "$seed"
    done
    from=$(wc -l < build/serial.log)
    pid=$(cat build/qemu.pid)
    type_line bye && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" $((from + 1))
    wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$L"
    latearrival_off
    grep -aq '^\[HALT\] Storage sync complete' "$L"
    chk $? "replugwrite: the machine halted through bye and its sync wrote the volume ($(grep -aE "$STAND_HALT_ENDS" "$L" | tr -d '\r' | tail -1 | cut -c1-100))"
    python3 tools/volume-read.py "$SCRATCH/stick.img" check --json >"$SCRATCH/replugwrite.check.json" 2>/dev/null
    replugwrite_census "$SCRATCH/replugwrite.check.json" "$L"
    replugwrite_medium "$SCRATCH/stick.img" "$REPLUGWRITE_ROUNDS" "$L"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|^\[NIGHTWATCH\] VERDICT: .* a defect' "$L"
    chk $? "replugwrite: no panic, no exception, nothing owed proved unserved"
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
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    for c in timezone me; do
        type_cmd "$c"
        sleep 1; key_press ret; sleep 4
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

    # ‼ THE MOUNT FINISHED, WHICH IS NOT WHAT ANY OTHER LINE HERE SAYS.
    #
    # Everything TagFS prints while mounting — the deed, the far copy, the
    # file count — is printed just as readily by a mount that then fails eight
    # steps later. On the owner's board that is exactly what happened, twice,
    # and it was read off a photograph as a healthy volume while the shell
    # answered `Unknown command` to every file on it. One line closes it.
    grep -q "\[TagFS\] the volume on seat .* is MOUNTED" "$L"
    chk $? "and the volume actually finished mounting"
    ! grep -q "\[TagFS\] this volume is NOT mounted" "$L"
    chk $? "with nothing refused along the way"

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

    # (6) TagFS itself still healthy — and every test it counts is one this
    # tree registers. The numbers are the code's, not a run's (K9): a build
    # without VOLUME_TESTS walks the seven suites of tests.h TEST_SUITES not
    # under VOLUME_TESTS_ONLY — core 3, stress 2, braid 3, cow 1, boxhash 5,
    # integrity 3, use_context 5 TestCases, 22 run; the 11 whose reach is
    # TEST_WRITES_TO_VOLUME are skipped (tests.c, g_volume_tests_allowed),
    # and the 11 that only read pass — none of them stands aside on this
    # stand's fresh image, whose first file wears a tag (use_context_narrows).
    # A test added or taken out moves this line in the same commit.
    grep -qE "TagFS: [0-9]+ run, [0-9]+ passed, 0 failed" "$L"; chk $? "TagFS startup tests: 0 failed"
    local said
    said=$(stand_summary "$L")
    [ "$said" = "TagFS: 22 run, 11 passed, 0 failed, 11 skipped, 0 held under a caveat" ]
    chk $? "TagFS startup tests: the 22 this tree registers ran, 11 reads passed and 11 writes stood down (${said:-no summary})"
    lockrank_judge "$L" "healthy 1c"

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

    # (10) THE PASS ARRIVED THE WAY THE LOADER WROTE IT.
    #
    # The volume identity the whole boot leans on is read out of a block in
    # low memory that nothing defends. Until the seal it was believed because
    # its magic looked right — which a block half-overwritten by firmware also
    # manages. Both loaders seal it now; the kernel says which of the two
    # things it has: a block that adds up, or a block it had to take on trust.
    grep -q "the loader left a pass with 4 stamp(s)" "$L"
    chk $? "the pass carries its seal as well as its three facts"
    grep -q "sealed, and the seal agrees" "$L"
    chk $? "and the block adds up to what the loader sealed it with"

    # (11) THE MEDIUM SAYS HOW MUCH IT TAKES, AND IT IS ASKED.
    #
    # The room used to hand every medium the same 64 sectors. That is above the
    # eight past which this channel gives up its bus-master DMA path and moves
    # the bytes with the processor — so the one number was not merely a poor
    # fit, it took the fast path away from the disk this machine boots from.
    # Asked instead, the channel says eight and keeps it.
    grep -q "this medium hands over 8 sector(s) in one go" "$L"
    chk $? "the legacy channel keeps the width its DMA path works at"

    # (12) THE TWO ENDPOINT-CONTEXT FIELDS NO BOOT EVER COMPUTES.
    #
    # Nothing in this driver prepares an isochronous endpoint, so the
    # isochronous half of the interval and error-count fields is unreachable —
    # it can be neither wrong in a way a running kernel shows nor right in a
    # way anybody can check. It is checked against the specification's own
    # table instead, where being unreachable does not matter.
    grep -q "endpoint-context self-test 15/15" "$L"
    chk $? "the endpoint-context fields agree with xHCI Table 6-45"
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
# ‼ THE IMAGE IS A BUILD PRODUCT, AND A MUTATED PRODUCT IS ANSWERED BY
# DELETING IT. There is no marker to write into a disk image and the tree-wide
# preflight cannot see one, so the only thing holding this straight is the
# manifest and the trap — which is exactly what `mutate_product` puts it under.
# Restoring it is `rm -f`: the next `build` makes it again from clean sources,
# which is both cheaper and more honest than keeping a hundred and thirty
# megabyte copy of something a Makefile can produce on demand.
badpool_on() {
    mutate_product build/boxos.img
    # ‼ AN IMAGE CARRIES NO COMMENT, so there is no marker for `mut_done` to
    # find and no preflight that could see this one left behind. The proof it
    # can give is its own bytes — and the proof it used to give was not enough.
    #
    # THIS OFFSET WAS HARDWIRED AND IT ROTTED. It read
    # `(2048 + 136 * 8) * 512` with a comment saying "the data run begins at
    # volume block 134, the pool is data block 2". Then the volume moved onto
    # its own ground and its parts moved with it: the pool now begins at volume
    # block 647. The write landed on empty ground, the four bytes it reported
    # replacing were 00000000 rather than a pool magic, and the only check on
    # them was "they must not ALREADY be the corruption" — which empty ground
    # passes. So the scenario booted a perfectly healthy volume, watched it
    # mount, and reported that the kernel had failed to notice a corruption
    # nobody had made. Two red checks for a year, both of them the stand's.
    #
    # So the offset is DERIVED from the image, and by the reader the format has
    # on the host (tools/volumeread): the ground the kernel would stand on, the
    # Deed there, and the block its LAYOUT says the metadata pool begins at. A
    # parser of its own here was one more reader of the Deed kept in step with
    # the format by hand, which the self-test's X1 never sees. And the guard is
    # the real one: the four bytes must BE the pool magic before the write. A
    # scenario that cannot find a pool says so and stops, instead of testing
    # nothing in silence.
    if ! python3 - <<'EOF'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk

TMPL = ondisk.MPOOL_MAGIC.to_bytes(4, "little")
BAD  = b"\xAA\xBB\xCC\xDD"

try:
    stand = medium.StandOnDeed(medium.Medium("build/boxos.img"), None)
except medium.Unreadable as e:
    sys.exit("logcheck: no deed in this image the kernel could stand on (%s) — "
             "nothing here can say where the pool is" % e)
if stand.format != "v1":
    sys.exit("logcheck: this volume's book is format %s — there is no metadata pool "
             "to corrupt" % stand.format)
pool_block = stand.deed["layout"]["metadata_pool_block"]
at = (stand.start + pool_block * ondisk.SECTORS_PER_BLOCK) * ondisk.SECTOR

img = open("build/boxos.img", "r+b")
img.seek(at)
was = img.read(4)
if was == BAD:
    sys.exit("logcheck: that offset ALREADY holds the corruption — this image is not clean")
if was != TMPL:
    sys.exit("logcheck: volume block %d holds %s, not a metadata pool — the "
             "scenario refuses to corrupt something it cannot identify"
             % (pool_block, was.hex()))
img.seek(at)
img.write(BAD)
img.flush()
img.seek(at)
now = img.read(4)
img.close()
if now != BAD:
    sys.exit("logcheck: the image did not take the corruption")
print("logcheck: metadata pool at volume block %d (byte %d): %s -> aabbccdd"
      % (pool_block, at, was.hex()))
EOF
    then
        echo "badpool: the image could not be corrupted where the pool should be"
        exit 1
    fi
}
badpool_off() {
    restore build/boxos.img
}

run_badpool() {
    echo "== badpool: the metadata pool holds something that is not a pool =="
    build
    badpool_on
    # boot() rebuilds nothing — it runs what is on disk now.
    make run-stop >/dev/null 2>&1
    make run-bg   >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "help"
    sleep 1
    key_press ret
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.badpool.log"
    badpool_off
    build   # leave the tree with an image built from clean sources
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

# ---------------------------------------------------------------------------
# framerefuse: a format-v2 volume, met by a kernel that cannot read its book,
# is turned away whole — not a byte of its ground written (step 2.1)
#
# A Deed says what its book is written in by the stamp it carries: LAYOUT for
# format v1, FRAME for v2 (volume_deed.h). A kernel that meets a book it
# cannot read has one thing to do, at its format check (tagfs.c,
# volume_take_ground): refuse the volume before the mount's first write, and
# say so in words a person at the machine can act on.
#
# The image is this tree's, its volume made again by `create_tagfs
# --frame-only`: GEOMETRY · FRAME · BORN · BOOT · PACKED, the Spine's slots
# blank, the kernel at the data run's first block where BOOT names it — listed
# after the shell, so the image also shows the kernel is put first anyway —
# and stage2 boots it the way it boots any volume. It is booted on BIOS, q35
# (STRICT), every write it makes on the record (POWERCUT=on), typed at, and
# switched off by its power button, so the halt meets the refused volume too.
# The record is then read against the image as it stood before the boot:
# base + record must BE the image the machine left (cut --against), so the
# record holds every write; and not one of them may land in the volume's
# ground — counted by where each write lands, and again by what the
# classifier names it (cut --list), which has to be standing on the volume as
# format v2 to name anything at all.
#
# ‼ THE KERNEL BEFORE FORMAT v2 IS ASKED THE SAME, ONCE. FRAMEREFUSE_KERNEL=
# <kernel.bin> puts that kernel in the same image instead, booted the same
# way, and holds it to the refusal kernels before format v2 give ("carries no
# layout this kernel understands") and to the same silence on the medium. A
# receipt of step 2.1 taken by hand, not a gate: a tree of another day, built
# on the desk of this one, goes stale with the tools that build it.
# ---------------------------------------------------------------------------

FRAMEREFUSE_V2_SAYS="carries a format-v2 volume (its deed bears FRAME); this kernel cannot read a format-v2 book — boot a newer kernel"
FRAMEREFUSE_V1_SAYS="carries no layout this kernel understands"

# framerefuse_judge BEFORE RECORD FINAL — what the record says the machine
# wrote, against the image before the boot; one line, FRAMEREFUSE k=v ...
framerefuse_judge() {
    local list="$SCRATCH/framerefuse.list.json" against
    against=$(python3 tools/volume-read.py "$1" cut "$2" --against "$3" 2>&1)
    echo "  (cut --against: ${against#cut: })"
    case "$against" in "cut: base + "*) ;; *) echo "FRAMEREFUSE record=incomplete"; return ;; esac
    if ! python3 tools/volume-read.py "$1" cut "$2" --list --json >"$list" 2>"$list.err"; then
        echo "FRAMEREFUSE record=unreadable $(tail -1 "$list.err")"; return
    fi
    python3 - "$1" "$list" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium

stand = medium.StandOnDeed(medium.Medium(sys.argv[1]), None)
lo, hi = stand.start, stand.start + stand.deed["sectors"]
writes = json.load(open(sys.argv[2]))["writes"]
inside = [w for w in writes if w["lba"] < hi and w["lba"] + w["sectors"] > lo]
named = [(w, t) for w in writes for t in w["touches"] if t["kind"] != "outside"]
for w in inside[:5]:
    print("  write %u.%u, LBA %u+%u, lands in the volume's ground %u..%u"
          % (w["epoch"], w["index"], w["lba"], w["sectors"], lo, hi - 1))
for w, t in named[:5]:
    print("  write %u.%u is named %s: %s" % (w["epoch"], w["index"], t["kind"], t["what"]))
print("FRAMEREFUSE record=whole format=%s writes=%u inside=%u named=%u"
      % (stand.format, len(writes), len(inside), len(named)))
PY
}

# framerefuse_leg KERNEL SAYS — the image made around KERNEL, booted, switched
# off, and judged: SAYS is the refusal its log must carry.
framerefuse_leg() {
    local kernel=$1 says=$2 img="$SCRATCH/framerefuse.img" before="$SCRATCH/framerefuse.before.img"
    local rec="$SCRATCH/framerefuse.record" L="$SCRATCH/serial.framerefuse.log" pid verdict uuid
    rm -f "$img" "$before" "$rec" "$L"
    cp build/boxos.img "$img" || { harness "framerefuse: could not copy build/boxos.img"; return; }
    if ! tools/create_tagfs --frame-only "$img" src/userspace/shell/shell.bin "utility,system" \
            "$kernel" "system" >"$SCRATCH/framerefuse.mkfs.log" 2>&1; then
        bad "framerefuse: create_tagfs --frame-only would not make the image: $(tail -1 "$SCRATCH/framerefuse.mkfs.log")"
        return
    fi
    python3 tools/volume-read.py "$img" deed >"$SCRATCH/framerefuse.deed" 2>&1
    grep -q "^format      : v2" "$SCRATCH/framerefuse.deed"
    chk $? "framerefuse: the image's Deed is format v2 ($(grep '^spine' "$SCRATCH/framerefuse.deed"))"
    python3 - "$img" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk
m = medium.Medium(sys.argv[1])
ground, _how = medium.find_ground(m, None)
d = ondisk.ReadDeed(m.sectors(ground, ondisk.DEED_SECTORS))
sys.exit(0 if d["ok"] and d["frame"] and d["boot"] and
         d["boot"]["kernel_block"] == d["frame"]["data_block"] else 1)
PY
    chk $? "framerefuse: the kernel sits at the data run's first block, where BOOT names it, though listed second"
    cp "$img" "$before" || { harness "framerefuse: could not keep the image as it stood"; return; }

    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" POWERCUT=on POWERCUT_LOG="$rec" STRICT=on \
            >"$SCRATCH/framerefuse.run-bg" 2>&1; then
        harness "framerefuse: run-bg would not start the image: $(tail -1 "$SCRATCH/framerefuse.run-bg")"
        return
    fi
    pid=$(cat build/qemu.pid 2>/dev/null)
    case "$(ps -o command= -p "$pid" 2>/dev/null)" in
        *"filename=$img,"*"filename=$rec,"*) ;;
        *) harness "framerefuse: the machine (pid $pid) was not started on $img with its record at $rec"
           make run-stop >/dev/null 2>&1
           return ;;
    esac
    if wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"; then
        sleep 2
        type_cmd "help"
        sleep 1
        key_press ret
        sleep 2
    fi
    mon_raw system_powerdown
    wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$L"

    uuid=$(deed_uuid "$before") || uuid=""
    [ -n "$uuid" ] && grep -aqE "^\[Deed\] seat [0-9]+: volume $uuid, " "$L"
    chk $? "framerefuse: the machine read the Deed of this very image (volume ${uuid:-unreadable})"
    grep -qF "$says" "$L"
    chk $? "framerefuse: the volume is refused in so many words — $(grep -oF "$says" "$L" | head -1)"
    ! grep -qE "MOUNTED — [0-9]+ file" "$L"
    chk $? "framerefuse: nothing is mounted off it"
    grep -q "BoxOS Shell" "$L" && typed_ok "$L"
    chk $? "framerefuse: the machine still reaches a shell, and answers a keystroke"
    ! grep -qE "PANIC|\[EXCEPTION\]" "$L"
    chk $? "framerefuse: no panic, no fault"

    verdict=$(framerefuse_judge "$before" "$rec" "$img")
    echo "$verdict" | grep -v "^FRAMEREFUSE "
    verdict=$(echo "$verdict" | grep "^FRAMEREFUSE ")
    echo "$verdict" | grep -q "record=whole"
    chk $? "framerefuse: base + the record is the image the machine left — every write it made is on the record (${verdict:-no verdict})"
    echo "$verdict" | grep -q "format=v2 .* inside=0 named=0$"
    chk $? "framerefuse: not one write lands in the volume's ground, by place and by name ($verdict)"
    rm -f "$img" "$before"
}

run_framerefuse() {
    echo "== framerefuse: a format-v2 volume is refused whole by a kernel that cannot read its book =="
    build STRICT=on
    if [ -n "${FRAMEREFUSE_KERNEL:-}" ]; then
        echo "  (the A/B receipt: $FRAMEREFUSE_KERNEL — $(tools/buildplate say "$FRAMEREFUSE_KERNEL" 2>&1 | head -1))"
        framerefuse_leg "$FRAMEREFUSE_KERNEL" "$FRAMEREFUSE_V1_SAYS"
        return
    fi
    framerefuse_leg build/kernel.bin "$FRAMEREFUSE_V2_SAYS"
    local L="$SCRATCH/serial.framerefuse.log"
    ! grep -qF "$FRAMEREFUSE_V1_SAYS" "$L"
    chk $? "framerefuse: and not as a volume that says where nothing is"
    grep -qE "^\[Deed\] seat [0-9]+: format v2 — a Spine ring of 3 slot\(s\) 256 block\(s\) apart from block 256, and its data run from block 1024" "$L"
    chk $? "framerefuse: the deed is described with its FRAME"
    ! grep -q "carries the volume this machine is standing on" "$L"
    chk $? "framerefuse: the boot survey does not take the refused volume for the one the machine stands on"
    ! grep -qE "would not flush its write cache|WAS NOT MADE DURABLE" "$L"
    chk $? "framerefuse: the halt raises no durability alarm over a volume it never mounted"
    grep -aq '^\[HALT\] no volume is named on this machine — there is no medium to make durable' "$L"
    chk $? "framerefuse: and says instead that there is no medium to make durable"
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
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
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

    # ── what the loader hands over, and where it puts it ───────────────────
    #
    # TagBoot writes the E820 map at 0x500, boot_info at 0xA000 and the pass
    # at 0xA600 — three fixed addresses it had never asked the firmware for,
    # while calling that same firmware's allocator several more times before
    # ExitBootServices. Two of those calls are AllocateAnyPages, which lets
    # the firmware pick, and low conventional memory is exactly what it may
    # pick. Now the pages are claimed first, and the loader SAYS how many it
    # got — a board whose firmware refuses one names it instead of booting
    # strangely.
    grep -qE "TagBoot: set aside [0-9]+ of 3 handoff page\(s\)" "$L"
    chk $? "the loader asked for the pages it writes the handoff into"
    grep -q "TagBoot: set aside 3 of 3 handoff page(s)" "$L"
    chk $? "and this firmware gave it all three"
    # ‼ This one cannot go red here and is kept anyway: OVMF gives all three,
    # so there is no refusal to report and removing the claim entirely leaves
    # it green. Its value is on a board whose firmware says no — which is the
    # only place the question was ever open.
    ! grep -q "would not set aside" "$L"
    chk $? "no handoff page was written into unclaimed"

    # The UEFI half of the seal — a different loader, a different CRC, the
    # same block and the same kernel check.
    grep -q "sealed, and the seal agrees" "$L"
    chk $? "the pass TagBoot wrote adds up too"
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

# ── noexec: bit 63 is nobody's business until somebody takes it up ──────────
#
# This is the scenario for the failure that kept BoxOS off two real machines,
# and it exists because the emulator cannot show it by accident: OVMF leaves
# IA32_EFER.NXE set, so the UEFI path always found the bit already legal here
# while both boards found it clear.
#
# What went wrong there: the kernel wrote bit 63 into the page table entries
# for EFI runtime data, then called firmware through those pages — while NXE
# was still 0 and bit 63 was therefore a RESERVED bit (Intel SDM Vol 3A §4.5).
# Every access through such an entry faults, so the firmware's first read of
# its own data was #PF err=0x9 = P|RSVD, inside SetVirtualAddressMap. The BIOS
# path never showed it because stage2 sets NXE on its way into long mode.
#
# Two halves, and the second is the one the emulator could never give:
#
#   1. On an ordinary UEFI boot the kernel must take no-execute up, and must
#      do it BEFORE the two things that used to come first — the EFI runtime
#      mapping, and per_core_init_bsp, where the write used to live. Order in
#      the log IS the property; a check that only asked "did it happen" would
#      have passed on the broken kernel too.
#   2. `make NOEXEC=off` reproduces the boards' machine state — NXE clear —
#      and the machine must still BOOT. That is the whole claim of the fix:
#      not "we set the bit", but "the kernel knows whether the bit is legal
#      and never writes one that is not".
run_noexec() {
    echo "== noexec: bit 63 is taken up before anything writes it =="

    # ── half one: the ordinary UEFI boot, where the panic happened ──────────
    build
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.noexec.log"
    L="$SCRATCH/serial.noexec.log"

    grep -q "BoxOS Shell" "$L"; chk $? "uefi boot reaches the shell"

    grep -q "\[CPU\] no-execute taken up" "$L"
    chk $? "the kernel says it took no-execute up"

    # The ordering, by line number, because that is the defect: both of these
    # were AFTER the mapping that used the bit.
    local nx_line rt_line pc_line
    nx_line=$(grep -n "\[CPU\] no-execute taken up" "$L" | head -1 | cut -d: -f1)
    rt_line=$(grep -n "\[EFI\] SetVirtualAddressMap" "$L" | head -1 | cut -d: -f1)
    pc_line=$(grep -n "\[PER_CORE\] Initializing BSP" "$L" | head -1 | cut -d: -f1)

    [ -n "$nx_line" ] && [ -n "$rt_line" ] && [ "$nx_line" -lt "$rt_line" ]
    chk $? "no-execute is taken up before EFI runtime is mapped (${nx_line:-?} < ${rt_line:-?})"

    [ -n "$nx_line" ] && [ -n "$pc_line" ] && [ "$nx_line" -lt "$pc_line" ]
    chk $? "and before per-core init, where the write used to live (${nx_line:-?} < ${pc_line:-?})"

    # The fault this whole change is about, named by the KIND of fault and not
    # by the error code one board happened to print.
    #
    # ‼ It was written the other way first — `err=0x9`, straight off the photo
    # — and that check stayed GREEN under a mutation that panicked the machine,
    # because the emulator reports the same reserved-bit fault as err=0x8
    # (P=0 where the boards had P=1). An oracle keyed to one machine's error
    # code is an oracle for that machine.
    ! grep -q "reserved bit set in a paging entry" "$L"
    chk $? "no reserved-bit page fault"
    ! grep -q "KERNEL PANIC" "$L"
    chk $? "the boot does not panic"

    # The firmware's own memory never enters the allocator, and the machine
    # says how much of itself it gave up. UEFI only — on a BIOS boot there is
    # no EFI memory map and the line is correctly absent, which is why this
    # sits in the UEFI half of this scenario.
    #
    # ‼ The count is asserted NON-ZERO, and that is the whole point of reading
    # it out instead of grepping the sentence: the reservation is taken twice,
    # once at pmm_init and once after the spans above 4 GiB reach the buddy,
    # and a zero here would mean both passes found nothing to hold.
    local fw_pages
    fw_pages=$(sed -n 's/.*\[PMM\] \([0-9][0-9]*\) page(s), [0-9][0-9]* MB, belong to the firmware.*/\1/p' "$L" | head -1)
    [ -n "$fw_pages" ] && [ "$fw_pages" -gt 0 ]
    chk $? "the firmware's own memory is held out of the allocator (${fw_pages:-no line} page(s))"

    # And the kernel's own audit of that claim — it re-reads the buddy's
    # alloc_map after both passes and speaks only when a held range is still
    # free. A line here means the count above under-reports.
    ! grep -q "firmware page(s) are still free in this allocator" "$L"
    chk $? "and the allocator's own bitmap agrees that none of them are free"

    # The E820 the kernel divides memory by is the one ExitBootServices
    # accepted, not the one built several allocations earlier.
    #
    # ‼ This one cannot go red here and is kept anyway. Measured with the
    # rebuild disabled: OVMF produces a BYTE-IDENTICAL table either way,
    # because it answers the EfiACPIMemoryNVS request out of a region that was
    # already NVS before the snapshot. The hazard is on firmware whose NVS
    # pool has to grow into conventional memory — there the staged map lands
    # in pages this allocator would hand out, and only this line would say so.
    ! grep -q "the loader's E820 is older than its own allocations" "$L"
    chk $? "the staged EFI map is not memory the allocator calls free"

    # The firmware's own declaration of how tightly its runtime regions may be
    # mapped (UEFI 2.10 §4.6.4), honoured whole. Applied only after SVAM,
    # because the firmware writes to its own code during that call.
    grep -q "\[EFI\] memory attributes: [0-9]* runtime region(s) tightened" "$L"
    chk $? "runtime regions are tightened to what the firmware declared"
    grep -q "tightened to what the firmware declared, 0 left as they were" "$L"
    chk $? "and every entry in the table checked out"

    # ‼ The only check here that is EVIDENCE rather than arrangement: a real
    # runtime service, dispatched through the rebased pointer, into the
    # relocated firmware code, off the pages just made read-only. Everything
    # above it describes what was set up; this is the machine using it.
    grep -q "\[EFI\] runtime services answer:" "$L"
    chk $? "and a runtime service still answers afterwards"
    grep -qE "firmware clock reads [0-9]{4}-[0-9]{2}-[0-9]{2}" "$L"
    chk $? "with a date off the firmware's own clock"

    # ── half two: the boards' state, reproduced ─────────────────────────────
    echo "-- and again with NXE clear, the way both boards booted --"
    make NOEXEC=off >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (NOEXEC=off) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "NOEXEC=off build"; return
    fi
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on NOEXEC=off >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.noexec_off.log"
    local N="$SCRATCH/serial.noexec_off.log"

    grep -q "no-execute refused by this build" "$N"
    chk $? "the kernel says bit 63 stays out of every entry"

    grep -q "BoxOS Shell" "$N"
    chk $? "a machine with NXE clear still boots to a shell"

    ! grep -q "reserved bit set in a paging entry" "$N"
    chk $? "and takes no reserved-bit fault doing it"

    ! grep -q "KERNEL PANIC" "$N"
    chk $? "and does not panic"

    # Restore the tree's ordinary kernel so a later scenario does not inherit
    # a NOEXEC=off build.
    make >"$SCRATCH/build.log" 2>&1
}

# ── earlyirq: an interrupt that arrives before the kernel opened the door ───
#
# The second thing that kept BoxOS off a real machine, and the emulator could
# not show it either: on an i5-9400F booting UEFI, RFLAGS.IF was set by
# something OUTSIDE this kernel — stage2.asm clears it ten times over on the
# BIOS path, TagBootJump clears it nowhere, and UEFI 2.10 §8.1 obliges
# firmware to restore it after a runtime call, which is the kind of obligation
# firmware breaks. The first HPET tick then landed inside cpu_calibrate_tsc,
# thirty-six lines of kernel_main before scheduler_init() had allocated
# anything, and irq_handler wrote through the NULL it got back: #PF at 0x18.
#
# Three things are asked here:
#   1. On an ordinary boot NOTHING arrives early — the kernel owns the flag
#      from _start and takes it back after every firmware call.
#   2. The state the loader handed over is SAID, both paths, so a machine that
#      inherits an open flag names it instead of behaving strangely.
#   3. `make EARLYIRQ=on` reproduces the board's window exactly — the flag is
#      opened for the length of TSC calibration and closed again — and the
#      machine must survive it, say so, and boot.
#
# ‼ The narrow window is deliberate. Measured: leaving interrupts on for the
# REST of early init deadlocks the kernel later in TouchLogbookResolve,
# because early init is not written to be re-entered from an interrupt. That
# is why the real fix is ownership of the flag, not surviving one tick.
run_earlyirq() {
    echo "== earlyirq: the kernel opens its own interrupt door =="

    build
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.earlyirq.log"
    L="$SCRATCH/serial.earlyirq.log"

    grep -q "BoxOS Shell" "$L"; chk $? "uefi boot reaches the shell"

    grep -q "\[BOOT\] the loader handed over RFLAGS=" "$L"
    chk $? "the kernel says what interrupt state the loader handed it"

    ! grep -q "arrived before this kernel was ready to be interrupted" "$L"
    chk $? "and nothing was delivered before the kernel opened the door"

    ! grep -q "Unhandled kernel #PF at 0x18" "$L"
    chk $? "no fault at 0x18 — the address a NULL scheduler state faults at"

    # The BIOS half, because the two loaders disagreed and only one was right.
    make run-stop >/dev/null 2>&1
    make run-bg >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.earlyirq_bios.log"
    local B="$SCRATCH/serial.earlyirq_bios.log"

    grep -q "\[BOOT\] the loader handed over RFLAGS=" "$B"
    chk $? "the bios path says it too"
    ! grep -q "arrived before this kernel was ready to be interrupted" "$B"
    chk $? "and nothing arrives early there either"

    # ── the board'"'"'s window, reproduced ───────────────────────────────────────
    echo "-- and again with the flag opened where the board took its tick --"
    make EARLYIRQ=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (EARLYIRQ=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "EARLYIRQ=on build"; return
    fi
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on EARLYIRQ=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell\|System halted" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.earlyirq_on.log"
    local E="$SCRATCH/serial.earlyirq_on.log"

    grep -q "arrived before this kernel was ready to be interrupted" "$E"
    chk $? "an interrupt that slips in early is named, with its vector"

    ! grep -q "Unhandled kernel #PF at 0x18" "$E"
    chk $? "and does not write through a scheduler that does not exist yet"

    ! grep -q "KERNEL PANIC" "$E"
    chk $? "and does not panic"

    grep -q "BoxOS Shell" "$E"
    chk $? "and the machine boots through it to a shell"

    make >"$SCRATCH/build.log" 2>&1
}

# ── iommu: the machine runs its DMA through tables this kernel wrote ───────
#
# DMA translation is ON by default since dd871b4: straight after pci_init
# every PCI function on the machine is given an identity-map entry and then
# every remap unit is switched on, or none of them is and the kernel says
# why. Not one line of that had ever executed. QEMU publishes no DMAR unless
# it is asked to, so every run this tree has ever done ended at
#
#   [IOMMU] ACPI describes neither DMAR nor IVRS
#
# and the identity tables, the seating of every function, the switch itself
# and the fault channel were all shipped unexecuted towards a board on which
# the failure mode is a screen that never comes back.
#
# `-device intel-iommu` on a q35 machine is what asks QEMU for the hardware.
# It goes in through QEMUEXTRA rather than a key of its own — see the
# Makefile for why that hook exists.
#
# ‼ WHY UEFI AND NOT THE ORDINARY BIOS BOOT. Not because the MBR path cannot
#   stand on q35. This paragraph said it could not ("no volume found on any
#   of the 1 seated medium(s)", written at bf2fb8aa, 2026-09-20), and at
#   56f9871c that is false: measured 2026-09-27, SeaBIOS on q35 boots the
#   image off ICH9 AHCI port 0, TagFS mounts its 69 files and the unattended
#   read PASSES on 4 and 16 cores — every BIOS `STRICT=on` run-bg is that
#   machine now. What this scenario needs is a machine that has a DMAR and
#   mounts; it stays on UEFI+q35 because that is how the owner's board boots.
#   BIOS on q35 with the remap unit has not been measured.
#
# ‼ intremap IS DELIBERATELY NOT ASKED FOR. QEMU's interrupt remapping wants
#   kernel-irqchip=split, which is a KVM setting and means nothing under the
#   TCG this desk runs, and this kernel writes no interrupt-remap table: it
#   prints "unit at 0x... arrived REMAPPING INTERRUPTS" and takes the unit
#   off that mode. The claim here is about DMA. DMA is what is switched on.
#
# What is asked, and what each answer is worth:
#
#   1. THE MACHINE REALLY HAS THE HARDWARE. Without this one line every check
#      below is a check on a machine with no IOMMU — the kernel prints "ACPI
#      describes neither DMAR nor IVRS", takes no wheel, boots, mounts, types,
#      and this scenario goes green having proved nothing whatever. It is the
#      first check for exactly that reason.
#   2. The units came up.
#   3. The contexts were populated BEFORE the switch. That is an ORDER, and
#      the log carries it: iommu_init prints "translation still OFF at this
#      line", the switch prints how many functions it seated and how many
#      units of how many are translating, and every driver on the machine
#      prints after it.
#   4. The machine survives its own switch. It reaches a shell, it answers a
#      keystroke — xHCI's event and transfer rings are reached by DMA — and
#      it mounts its volume, which is AHCI's command list and FIS reached by
#      DMA. THIS IS THE SCENARIO. Everything above it is arrangement.
#   5. Nothing in the fault channel — asked of the units AFTER the drivers
#      have run, which is the only moment at which the answer is worth
#      anything. It was not, once: see the note over that check.
#
# And then the way back: `make IOMMU=off` on the same machine, with the same
# DMAR in front of it, must build, boot, mount, and SAY that it is not
# translating.

# The WHOLE boot log, because every fact judged here is printed long before
# the shell exists. util_boot keeps only the tail after the banner, which is
# why it is not used.
iommu_boot() {
    local name=$1 args=$2 extra=$3
    make run-stop >/dev/null 2>&1
    # ‼ THE LOG OF THE RUN BEFORE IS A TRAP, AND IT SPRANG ONCE HERE.
    #
    # run-bg removes build/serial.log itself — but only if it gets that far.
    # When it did not (a build that failed underneath it, a machine that was
    # already running), the previous half's log was still on disk with its
    # banner and its mount in it, wait_for_line returned instantly, and this
    # scenario judged the SECOND kernel by the FIRST one's boot: four greens
    # from a machine that was never started. Measured, first try.
    #
    # So the log is taken away here, and the machine's existence is a fact
    # this function checks rather than assumes. Neither is a verdict on
    # BoxOS, which is why the second one is a harness failure: a run that
    # could not start the machine it was judging judged nothing.
    rm -f build/serial.log
    # shellcheck disable=SC2086
    make run-bg $args QEMUEXTRA="$extra" >"$SCRATCH/runbg.log" 2>&1
    if [ ! -S build/qemu.mon ]; then
        harness "no machine was started for '$name' ($args QEMUEXTRA=$extra): $(tail -3 "$SCRATCH/runbg.log" | tr '\n' ' ')"
        make run-stop >/dev/null 2>&1
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    # The banner is not the prompt — the boot goes on printing after it. The
    # fact worth waiting on is that the machine has STOPPED talking; the
    # window is named where the other windows of its kind are named.
    wait_for_quiet "$BOOT_BURST_QUIET_S"
    local MARK
    MARK=$(wc -l < build/serial.log)
    # type_line, not a copy of it. What stood here was its three lines written
    # out again — type, pause, Enter — which threw away the two things it is
    # for: the retry when a shell at its prompt has echoed nothing, and the
    # plain sentence saying the keystrokes did not land. The comment beside
    # the copy claimed the shell does not echo what it is typed. It does: the
    # log of every run here carries `~ help`, and qemu-input.sh checks that
    # echo character by character.
    #
    # And a shell that stays silent is BoxOS's answer, not this desk's, which
    # is what the flag says — see type_cmd. It is turned off again straight
    # after, so nothing outside this function inherits it.
    TYPE_SILENCE_IS_JUDGED=1
    type_line "help"
    TYPE_SILENCE_IS_JUDGED=0
    wait_for_line build/serial.log "Show available commands" "$REACT_QUIET_S" $((MARK + 1))
    tr -d '\r' < build/serial.log > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_iommu() {
    echo "== iommu: every DMA on this machine goes through tables this kernel wrote =="

    local DEV="-device intel-iommu"
    local MACHINE="UEFI=on STRICT=on CORES=4 MEM=4G"

    # ── half one: the wheel is taken ───────────────────────────────────────
    build
    if ! iommu_boot iommu "$MACHINE" "$DEV"; then
        bad "iommu: never reached a shell with an IOMMU on the machine"; return
    fi
    local L="$SCRATCH/serial.iommu.log"

    # (1) The hardware is there. Red the moment the device key stops reaching
    #     QEMU — a typo in QEMUEXTRA, the hook dropped out of the run recipe,
    #     a machine type without a DMAR — which is the one way this whole
    #     scenario could be green and worthless.
    grep -qE '^\[ACPI\] tables:.*DMAR' "$L"
    chk $? "the firmware publishes a DMAR — there is remap hardware to drive"

    # (2) Red if the DMAR is there and every unit refused: a register window
    #     the kernel cannot map, a SAGAW it does not speak, a root table the
    #     unit would not take. Then the backend is never registered and the
    #     audit says so instead.
    grep -q "^\[IOMMU\] backend=Intel VT-d, units up, translation still OFF at this line" "$L"
    chk $? "the remap unit(s) came up under the VT-d backend"
    ! grep -q "not one unit came online" "$L"
    chk $? "and not one of them refused on the way up"

    local on_line init_line drv_line mount_line units_on units_all seated
    on_line=$(grep -n "^\[IOMMU\] translation ON:" "$L" | head -1 | cut -d: -f1)
    [ -n "$on_line" ]
    chk $? "translation is switched ON"

    # (3) Both counts, because "some units translating" is the state that
    #     stops the DMA of everything behind the ones that are not, and the
    #     kernel's own contract is all-or-nothing. Red if take_the_wheel ever
    #     returns a partial success instead of refusing.
    units_on=$(sed -nE 's/^\[IOMMU\] translation ON:.*, ([0-9]+) of ([0-9]+) unit\(s\) translating.*/\1/p' "$L" | head -1)
    units_all=$(sed -nE 's/^\[IOMMU\] translation ON:.*, ([0-9]+) of ([0-9]+) unit\(s\) translating.*/\2/p' "$L" | head -1)
    [ -n "$units_all" ] && [ "$units_all" -ge 1 ] && [ "$units_on" = "$units_all" ]
    chk $? "every unit on the machine is translating (${units_on:-?} of ${units_all:-?})"

    # And the table is not empty. A unit switched on over a context table
    # with no present entries translates every request to nothing, which is
    # the exact shape of "the board never came back".
    seated=$(sed -nE 's/^\[IOMMU\] translation ON:.*translating, ([0-9]+) PCI function\(s\) seated.*/\1/p' "$L" | head -1)
    [ -n "$seated" ] && [ "$seated" -gt 0 ]
    chk $? "with ${seated:-0} PCI function(s) seated in the identity domain, not an empty table"

    # (4) THE ORDER, by line number, because the order IS the property. The
    #     seating happens inside the same call that throws the switch, so
    #     what the log can show is the bracket around it: the units were up
    #     and NOT translating at iommu_init, and the switch came later. Red
    #     if anybody ever moves the switch up into iommu_init, where there is
    #     no PCI enumeration yet and every context table is empty.
    init_line=$(grep -n "^\[IOMMU\] backend=.*translation still OFF at this line" "$L" | head -1 | cut -d: -f1)
    [ -n "$init_line" ] && [ -n "$on_line" ] && [ "$init_line" -lt "$on_line" ]
    chk $? "the units were up and not translating first (${init_line:-?} < ${on_line:-?})"

    # And red the other way: if the switch is ever moved to the end of the
    # boot, after the drivers have already done their DMA, then "the machine
    # reached a shell" says nothing about surviving translation. Every
    # controller on this machine must come up behind the tables.
    drv_line=$(grep -n "^\[xHCI [0-9a-f:.]*\] register space:" "$L" | head -1 | cut -d: -f1)
    [ -n "$drv_line" ] && [ -n "$on_line" ] && [ "$on_line" -lt "$drv_line" ]
    chk $? "and the drivers start their DMA behind the tables (${on_line:-?} < ${drv_line:-?})"

    # (5) The scenario itself. A controller whose rings the unit does not
    #     translate is a controller that does nothing and says nothing.
    grep -q "BoxOS Shell" "$L"
    chk $? "the machine reaches a shell with translation on"
    typed_ok "$L"
    chk $? "and answers a keystroke — xHCI reaches its rings through the tables"

    mount_line=$(grep -n "^\[TagFS\] the volume on seat .* is MOUNTED" "$L" | head -1 | cut -d: -f1)
    [ -n "$mount_line" ]
    chk $? "and mounts its volume — AHCI reaches its command list through them too"
    [ -n "$mount_line" ] && [ -n "$on_line" ] && [ "$on_line" -lt "$mount_line" ]
    chk $? "and the volume was read after the switch, not before it (${on_line:-?} < ${mount_line:-?})"

    # (6) The units' own account of what they refused — and FIRST, that
    #     anybody asked them after the drivers ran.
    #
    # ‼ THIS PAIR OF CHECKS WAS DECORATIVE, AND IT WAS PROVED SO BY MUTATION.
    #   The write bit was taken out of both leaves of the identity map: every
    #   device write to memory faulted, the keyboard died, TagFS found no
    #   volume at all — and "not one I/O page fault recorded by any unit (0)"
    #   went GREEN, with zero lines matching in the whole log. Nothing had
    #   looked. The last read of the fault channel was inside
    #   iommu_take_the_wheel itself, one line after "translation ON", and
    #   every driver on the machine starts AFTER it; no Fault Event vector is
    #   armed, so the records sat in the units' registers, unsaid.
    #
    #   A count of zero from a channel nobody reads is not evidence, it is
    #   false comfort, and false comfort in front of a board run is worse than
    #   no check at all. So the kernel now reads the channel a third time, at
    #   the line where the last driver has come up (iommu_sweep_faults, called
    #   from kernel_main), and it SAYS that it read even when it finds
    #   nothing — because a silent reader cannot be told from no reader, which
    #   is the whole of how this check came to lie.
    #
    #   The marker is therefore checked BEFORE the count, and the count means
    #   nothing without it. Red if anybody removes that call, or moves it back
    #   above the drivers.
    local sweep_line
    sweep_line=$(grep -n "^\[IOMMU\] fault channel read again with every driver up" "$L" | head -1 | cut -d: -f1)
    [ -n "$sweep_line" ]
    chk $? "the fault channel was read again with the drivers up — somebody looked"
    [ -n "$sweep_line" ] && [ -n "$mount_line" ] && [ "$mount_line" -gt 0 ] && [ "$mount_line" -lt "$sweep_line" ]
    chk $? "and looked AFTER the mount, so a fault the mount caused is inside the window (${mount_line:-?} < ${sweep_line:-?})"

    #     Now the count is worth taking. Red if the identity map has a hole in
    #     it — a region above the ceiling it covers, an RMRR the DMAR named
    #     and the map left out — because a device DMA to an address with no
    #     entry is recorded there and nowhere else.
    #
    # ‼ Anchored at the start of the line on purpose. The audit line at
    #   iommu_init QUOTES the words "[VT-d] fault" and "[AMD-Vi] event" while
    #   explaining where they would appear, and an unanchored pattern matches
    #   that sentence on every single boot.
    local faults
    faults=$(grep -cE '^\[VT-d\] DRHD\[[0-9]+\] fault |^\[AMD-Vi\] event' "$L")
    [ "$faults" = 0 ]
    chk $? "not one I/O page fault recorded by any unit ($faults)"
    [ "$faults" != 0 ] && grep -E '^\[VT-d\] DRHD\[[0-9]+\] fault |^\[AMD-Vi\] event' "$L" \
        | head -3 | sed 's/^/       /'

    ! grep -q "fault log OVERFLOWED" "$L"
    chk $? "and no unit's fault log overflowed, so that count is the whole count"
    ! grep -q "KERNEL PANIC" "$L"
    chk $? "and the machine did not panic"

    # ── half two: the way back ─────────────────────────────────────────────
    #
    # The owner's escape hatch, on the SAME machine with the SAME DMAR in
    # front of it — otherwise "it is not translating" is true of any machine
    # that has no IOMMU and the half proves nothing.
    #
    # ‼ AND THE TREE IS PUT BACK EVEN IF THIS HALF NEVER FINISHES. The key
    #   leaves no trace in git — every file is clean and only build/.iommu.off
    #   says which kernel is on the disk — so it is taken under the EXIT trap
    #   the same way a mutated source file is. See build_with_key.
    echo "-- and the way back: IOMMU=off, same machine, same DMAR --"
    if ! build_with_key IOMMU=off; then
        echo "BUILD FAILED (IOMMU=off) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "iommu: IOMMU=off does not build"
        buildkey_restore; return
    fi
    # IOMMU=off is passed to run-bg as well, and it has to be: the key owns a
    # rebuild marker, so a run-bg without it names a marker that does not
    # exist, rebuilds every object, and boots the translating kernel while
    # the scenario believes it is judging the other one.
    if ! iommu_boot iommu_off "$MACHINE IOMMU=off" "$DEV"; then
        bad "iommu: IOMMU=off never reached a shell"
        buildkey_restore; return
    fi
    local N="$SCRATCH/serial.iommu_off.log"

    grep -qE '^\[ACPI\] tables:.*DMAR' "$N"
    chk $? "off: the machine still has the remap hardware in front of it"
    grep -q "^\[IOMMU\] built with IOMMU=off" "$N"
    chk $? "off: and the kernel says out loud which build this is"
    ! grep -q "^\[IOMMU\] translation ON:" "$N"
    chk $? "off: nothing was switched on"
    grep -q "BoxOS Shell" "$N"
    chk $? "off: the machine boots"
    typed_ok "$N"
    chk $? "off: and answers a keystroke"
    grep -q "^\[TagFS\] the volume on seat .* is MOUNTED" "$N"
    chk $? "off: and mounts its volume untranslated"
    ! grep -q "KERNEL PANIC" "$N"
    chk $? "off: and does not panic"

    # Give the tree back the kernel it had. The trap would do it anyway; doing
    # it here means the rest of an `all` run builds on the ordinary tree
    # instead of discovering the key one object at a time.
    buildkey_restore
}

# ── intx: the interrupt arrives on a WIRE, and the wire is described ───────
#
# ‼ THE WHOLE Summons IS DEAD CODE ON THIS DESK, AND THAT IS WHY THIS EXISTS.
#
# A line that arrives described — its trigger, its polarity, whether anything
# else may answer it, and WHERE each of those four facts came from — is a
# statement about PCI INTx and about nothing else. MSI carries its vector in
# the message; there is no pin to describe and no roster to keep.
#
# And no test on this desk has ever reached that code. QEMU's xHCI offers
# MSI-X, QEMU's AHCI offers MSI, and both drivers take what they are offered.
# Measured on the `dd871b4` run, which is the run that shipped the Summons:
# exactly ONE line was armed in the whole boot — the ACPI SCI on GSI 9 — and
# not one PCI function ever reached PciSummons. Four defects were closed in
# that commit and all four could have been left standing, with every scenario
# in this file green:
#
#   - a PCI line programmed edge/active-high from the ISA default, on a wire
#     PCI 3.0 §2.2.6 defines as open-drain, level sensitive and asserted low
#   - one answerer per line, on a bus where §6.5 makes sharing mandatory
#   - the GSI taken from config byte 0x3C, which footnote 43 defines for the
#     8259 pair, on a machine running its IOAPIC
#   - the EOI broadcast suppressed in the LAPIC SVR, which would let a level
#     line fire exactly ONCE and then hold Remote IRR for the rest of the boot
#
# `make INTX=on` makes the xHCI driver decline both message-signalled paths,
# which is not a fault injected but a machine this kernel must work on: a
# controller behind a bridge that does not forward MSI, a function with the
# capability fused off, a firmware that left it unusable. Everything after
# that point runs exactly as it would there.
#
# ‼ AND THE KEYBOARD IS THE ORACLE FOR THE FOURTH DEFECT. It is on the first
# controller, it is answered from the wire, and it is asked TWICE, minutes of
# machine time apart. A level line that fires once and never again types the
# first thing and then nothing — which is the one failure a boot log full of
# correct-looking description cannot show.
#
# The second half is the roster. Two controllers are put in PCI slots four
# apart, which on this machine type is what puts two functions' INTA# on one
# interrupt link, so the line really is shared and the registry really is
# asked to hold two answerers for one pin. Measured: 00:04.0 and 00:08.0 both
# arm GSI 11 on vector 43, and the keyboard on 00:04.0 still types afterwards
# — which it would not, if the newcomer had rewritten the pin's entry.
run_intx() {
    echo "== intx: the interrupt arrives on a wire, and the wire is described =="

    # build_with_key, not a bare make: the tree is left carrying a kernel no
    # file in git names, and only the record this writes puts it back when this
    # function returns early, is killed, or the build itself fails.
    if ! build_with_key INTX=on; then
        echo "BUILD FAILED (INTX=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "INTX=on build"; return
    fi

    # Four cores for the reason latearrival and replug give: this kernel's
    # deferred work runs from the K-Core guide loop and from cpu_idle, and on
    # one core neither runs. STRICT=on for the reason every other scenario
    # that programs hardware has it: it is what puts `-d guest_errors,unimp`
    # on the command line, so QEMU's own complaint about a redirection entry
    # or an INTx routing this kernel got wrong is kept instead of thrown away.
    #
    # XHCI2ADDR=0x08 is not a decoration. QEMU puts the first controller at
    # slot 4; four slots on is the next slot whose INTA# the board's PIRQ
    # router sends to the same link, and without naming it the second
    # controller lands on a line of its own and the roster is never asked a
    # question it could get wrong.
    local MACHINE="INTX=on USB=on XHCI2=on XHCI2ADDR=0x08 STRICT=on CORES=4 MEM=4G"

    # ‼ THE PS/2 CHIP IS TAKEN OUT OF THIS MACHINE, AND THAT IS THE WHOLE
    #   REASON THE KEYSTROKE CHECKS BELOW ARE WORTH ANYTHING.
    #
    # `qemu-input.sh type` sends `sendkey` to the monitor, and QEMU hands
    # those keys to the emulated i8042 — ISA IRQ1, an edge line with nothing
    # whatever to do with the PCI wire this scenario is about. BoxOS feeds one
    # key ring from two transports (keyboard.c says so in as many words), so
    # the shell echoes either way. Measured by a reviewer: with xhci's
    # interrupt handler stubbed out entirely, and again with the USB keyboard
    # removed from the machine, `help` still printed its command list. Three
    # checks here were green on a kernel that took no USB interrupt at all.
    #
    # With the chip gone the only keyboard on the machine is the USB one.
    #
    # STRICT puts `-machine q35` on the line first; this `-machine pc` comes
    # after it and wins, because QEMU keeps the last machine type — measured:
    # the volume stays on the ATA channel and the 8042 line checked below is
    # printed. It has to: the slot arithmetic above is i440fx's PIRQ router,
    # and it holds on that board and no other.
    local EXTRA="-machine pc,i8042=off"

    make run-stop >/dev/null 2>&1
    # ‼ THE LOG OF THE RUN BEFORE IS A TRAP, AND IT SPRANG ONCE IN THIS FILE
    # ALREADY — see iommu_boot, which has this paragraph for the same reason.
    # run-bg removes build/serial.log itself, but only after two guards it can
    # exit on: an image that would not build, and a QEMU already running. When
    # it takes either, the PREVIOUS intx run's log is still on disk, with its
    # banner and its armed line and its wire fires in it, and every check below
    # reads that instead — a green scenario over a machine nobody started.
    rm -f build/serial.log
    # shellcheck disable=SC2086
    make run-bg $MACHINE QEMUEXTRA="$EXTRA" >"$SCRATCH/runbg.log" 2>&1
    if [ ! -S build/qemu.mon ]; then
        harness "no machine was started for intx ($MACHINE QEMUEXTRA=$EXTRA): $(tail -3 "$SCRATCH/runbg.log" | tr '\n' ' ')"
        make run-stop >/dev/null 2>&1; buildkey_restore; return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        bad "intx: never reached a shell with its interrupts on a wire"
        make run-stop >/dev/null 2>&1; buildkey_restore; return
    fi

    # The banner is not the prompt — a boot goes on printing after it. MARK is
    # taken once the machine has stopped talking, so everything after it in the
    # log was caused by a keystroke and by nothing else. That division is what
    # makes "the line delivered AGAIN" a fact rather than a wish.
    wait_for_quiet "$BOOT_BURST_QUIET_S"
    local MARK
    MARK=$(wc -l < build/serial.log | tr -d ' ')

    # Two commands, not one, and the second only after the first has been
    # answered. A shell that goes deaf is BoxOS's answer here, not this desk's
    # — the keystroke checks below are where it is counted.
    #
    # ‼ type_cmd and not type_line, and the reason is this boot in particular.
    # type_line confirms a command landed by waiting for the contiguous string
    # "~ help" on one line, and under the proof key the wire prints a sentence
    # of its own between every two characters the shell echoes — the echo is
    # real and complete, but it is never contiguous, so that wait can only
    # ever time out and type the command a second time. qemu-input.sh's own
    # check, which type_cmd already makes, matches the echo as a SUBSEQUENCE
    # of what the guest wrote and is unaffected; what is waited on here is the
    # command's output, which is the fact the checks below read anyway.
    TYPE_SILENCE_IS_JUDGED=1
    type_cmd "help" && key_press ret
    wait_for_line build/serial.log "Show available commands" "$REACT_QUIET_S" $((MARK + 1))
    type_cmd "hw" && key_press ret
    TYPE_SILENCE_IS_JUDGED=0
    wait_for_line build/serial.log "CPU features" "$REACT_QUIET_S" $((MARK + 1))
    wait_for_prompt

    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log > "$SCRATCH/serial.intx.log"
    local L="$SCRATCH/serial.intx.log"

    # ── the key did what it says, and nothing below judges an MSI machine ──
    #
    # Without this the whole scenario is decoration: a stale object file, a
    # marker not wired into the rebuild, a key renamed — every one of those
    # leaves a kernel that takes MSI-X, arms no wire at all, and still types.
    grep -q "INTX=on: MSI-X and MSI declined on purpose" "$L"
    chk $? "the driver declined the message-signalled paths on purpose"
    ! grep -qE "MSI-X enabled|MSI enabled \(vector" "$L"
    chk $? "and took neither of them behind its own back"

    # ── the line was ARMED, and armed as PCI rather than as ISA ───────────
    #
    # This is the first defect. ioapic_enable_irq used to ask the MADT
    # override table — which by construction describes ISA sources — find
    # nothing for a PCI line, and program the redirection entry with neither
    # POLARITY_LOW nor TRIGGER_LEVEL: edge triggered, active high, and a
    # single handler. Put that back and this line reads "edge, active high,
    # exclusive", or the bus reads ISA, and all three go red.
    grep -qE "\[IRQCHIP\] PCI GSI [0-9]+ armed on vector [0-9]+: level, " "$L"
    chk $? "the wire is level-triggered, because PCI 3.0 §2.2.6 says it is"
    grep -qE "\[IRQCHIP\] PCI GSI [0-9]+ armed on vector [0-9]+: [a-z]+, [a-z ]+, shareable" "$L"
    chk $? "and shareable, because §6.5 makes every INTx line shareable"
    ! grep -qE "\[IRQCHIP\] PCI GSI [0-9]+ armed on vector [0-9]+: edge" "$L"
    chk $? "and no PCI line was armed as an edge"

    # The provenance is SPOKEN, not merely stored. It is printed by kprintf
    # for exactly this reason: moved back under debug_printf — where the old
    # form of this sentence lived — it compiles to nothing and a board that
    # took its pin from the wrong place says so nowhere at all.
    grep -qE "\[IRQCHIP\] PCI GSI [0-9]+ armed .*; wiring from .*, pin from " "$L"
    chk $? "and the line names where each half of its description came from"

    # ── ‼ THE TWO CHECKS THAT ARE THE POINT, AND THEY JUDGE _PRT ──────────
    #
    # _PRT is the only source written about THIS machine in THIS interrupt
    # mode, and it outranks the byte at 0x3C for the pin and the bus rule for
    # the wiring. QEMU's board publishes one, so on this desk the pin must
    # come from the routing table.
    #
    # ‼ THE SECOND CHECK USED TO DEMAND "active low" AND THAT WAS WRONG — it
    # is recorded here because the mistake is an easy one to make twice. It
    # read: a PCI interrupt link's _CRS says IRQ(Level, ActiveLow, Shared)
    # and nothing else does. It does not. PCI 3.0 §2.2.6 makes INTx# active
    # low on the DEVICE side of the PIRQ router; the link describes the
    # router's OUTPUT, which is the pin the interrupt controller actually
    # sees, and QEMU's own link says level, active HIGH. Demanding the bus
    # rule there demanded that the kernel ignore the firmware — the exact
    # opposite of the order of trust this whole mechanism is built on.
    #
    # What has discriminating power is AGREEMENT: whatever the link says the
    # wire is, the entry the kernel programmed must say the same. A kernel
    # that read _CRS and then armed the line by the bus rule anyway would
    # show the two disagreeing, and that is the defect worth catching. The
    # floor the bus rule still owns on its own is "shareable", which no
    # firmware statement here contradicts and which a shared INTx line must
    # have.
    grep -qE "\[IRQCHIP\] PCI GSI [0-9]+ armed .*pin from the ACPI routing table" "$L"
    chk $? "the pin came from the ACPI routing table, not from the 8259 byte"

    LINK_WIRE=$(grep -oE "describes that wire as (level|edge), active (low|high)" "$L" \
                | head -1 | sed 's/describes that wire as //')
    ARMED_WIRE=$(grep -oE "\[IRQCHIP\] PCI GSI [0-9]+ armed on vector [0-9]+: (level|edge), active (low|high)" "$L" \
                 | head -1 | sed 's/.*: //')
    [ -n "$LINK_WIRE" ] && [ "$LINK_WIRE" = "$ARMED_WIRE" ]
    chk $? "and the entry was programmed as the link describes it (link: ${LINK_WIRE:-none}, armed: ${ARMED_WIRE:-none})"

    grep -qE "\[IRQCHIP\] PCI GSI [0-9]+ armed on vector [0-9]+: .*shareable" "$L"
    chk $? "and shareable, which is the bus rule's own and which no firmware here contradicts"
    if grep -q "pin from the byte the firmware wrote for the 8259" "$L"; then
        echo "       ‼ the pin came from config byte 0x3C. _PRT did not answer —"
        echo "         see the [AML] lines in $L for which step declined."
        grep -m3 "^\[AML\]" "$L" | sed 's/^/         /'
    fi

    # ── it was not quietly dropped to polling ─────────────────────────────
    ! grep -q "events will be polled from the timer tick" "$L"
    chk $? "no controller fell back to polling the timer tick"
    ! grep -q "INTx line refused" "$L"
    chk $? "and no INTx line was refused"
    grep -q "controller ready: .* INTx interrupts" "$L"
    chk $? "the driver says it is running on INTx"

    # ── ‼ THE WIRE DELIVERED, AND DELIVERED AGAIN ─────────────────────────
    #
    # This is the block that judges delivery, and it judges it by counting the
    # answerers the LINE called — not by watching the machine behave, which it
    # does either way. xhci_tick drains every event ring once per timer tick
    # whether or not the controller has an interrupt, so a device on a line
    # that fired exactly once and was never re-armed still enumerates and still
    # types. That is the EOI-broadcast defect's whole disguise, and the only
    # thing that sees through it is a count read at two moments.
    #
    # MARK divides the log at "the boot stopped talking". Fires before it are
    # the line working at all; fires after it were caused by keystrokes and by
    # nothing else, so they are the line having been re-armed after its EOI,
    # over and over, long after the first time.
    local fired_boot fired_after
    fired_boot=$(head -n "$MARK" "$L" | grep -c "INTx wire fired")
    fired_after=$(tail -n +$((MARK + 1)) "$L" | grep -c "INTx wire fired")
    [ "$fired_boot" -ge 1 ]
    chk $? "the wire delivered while the machine was booting ($fired_boot call(s))"
    [ "$fired_after" -ge 1 ]
    chk $? "and delivered again after the boot went quiet ($fired_after more) — so the line was re-armed after its EOI"

    # ‼ AND THE PIN'S CHAIN HELD BOTH, AND CALLED BOTH.
    #
    # Each controller answers through a thunk of its own under the proof key,
    # for this check and no other reason: an irq_callback_t carries no
    # argument, so one shared function could say the line fired but never say
    # WHICH node on the chain was called. Rewriting the pin's redirection
    # entry instead of joining its chain — what happened before
    # SummonsSettleOnPin — leaves exactly one name here for the rest of the
    # boot, and the machine goes on working, because either xHCI answerer
    # services every controller. Nothing else in this scenario can tell those
    # two apart.
    local answerers arrived
    answerers=$(tail -n +$((MARK + 1)) "$L" \
                | grep -oE "\[xHCI [0-9a-f:.]+\] INTx wire fired" | sort -u | wc -l | tr -d ' ')
    arrived=$(grep -oE "\[xHCI [0-9a-f:.]+\] INTx INT[A-D]# on GSI" "$L" | sort -u | wc -l | tr -d ' ')
    [ "$answerers" -ge 2 ] && [ "$answerers" = "$arrived" ]
    chk $? "and every answerer on the pin was called, not just the first ($answerers of $arrived)"

    # ── the device WORKS, and the keyboard IS the USB one ─────────────────
    #
    # The guard first, because without it this block is back to judging a PS/2
    # chip. BoxOS says out loud when the 8042 is not there — this line is the
    # kernel's own, through kprintf — so a QEMUEXTRA that stopped reaching
    # QEMU, a machine type that ignores the property, a QEMU too old for it:
    # all of them turn up here rather than quietly restoring the second
    # keyboard nobody wanted.
    grep -q "\[KEYBOARD\] 8042 .*PS/2 port left untouched" "$L"
    chk $? "there is no working PS/2 keyboard in this machine to answer for the USB one"
    typed_ok "$L"
    chk $? "the USB keyboard answers a keystroke"
    grep -q "CPU features" "$L"
    chk $? "and answers a second command, after the first was served"
    external_ok "$L"
    chk $? "and the prompt came back after the utility it ran"

    # ── two devices, one wire, one roster ─────────────────────────────────
    local intx_lines intx_gsis
    intx_lines=$(grep -cE "\] INTx INT[A-D]# on GSI [0-9]+" "$L")
    intx_gsis=$(grep -oE "\] INTx INT[A-D]# on GSI [0-9]+" "$L" \
                | grep -oE "GSI [0-9]+" | sort -u | wc -l | tr -d ' ')
    [ "$intx_lines" = 2 ]
    chk $? "two controllers came up on INTx ($intx_lines)"
    [ "$intx_lines" = 2 ] && [ "$intx_gsis" = 1 ]
    chk $? "and both are on the SAME wire ($intx_gsis distinct GSI)"

    # A second answerer on a line the description calls exclusive is refused
    # aloud, and that refusal is what the old ISA-default wiring would have
    # produced here.
    ! grep -q "and this line is exclusive" "$L"
    chk $? "the second answerer was not refused as if the line were exclusive"

    grep -q "BoxOS Shell" "$L"
    chk $? "the machine reaches a shell with its interrupts on a wire"
    grep -q "\[TagFS\] the volume on seat .* is MOUNTED" "$L"
    chk $? "and mounts its volume"
    ! grep -q "KERNEL PANIC" "$L"
    chk $? "and does not panic"

    # Give the tree back the kernel it had. The EXIT trap does this too — this
    # is only so that a suite does not carry INTX=on into the next scenario.
    buildkey_restore
}

# ── lastsaid: the account survives the reset that ended the run ─────────────
#
# `logsave` writes THIS boot's log to the volume, and that is the right tool
# right up to the moment it is needed most. A board wedged itself — a storming
# interrupt and a host controller out of slots — and the volume was exactly
# what had stopped answering. The only account of it was four photographs.
#
# So the kernel carries its log through a warm reset in a fixed window it does
# not clear at boot, and `lastsaid` reads it. Both tools exist; neither
# replaces the other.
#
# ‼ What is asked here is the WHOLE claim: a cold start must say nothing came
# through (so an empty window is never dressed up as a log), a warm reset must
# carry the bytes, and the count the kernel announces must be the count the
# tool pours. QEMU's system_reset is a warm reset — it resets devices and CPU
# and leaves guest RAM alone — which is the same thing the RESET button on a
# case does, and NOT what holding the power button does.
run_lastsaid() {
    echo "== lastsaid: what the machine said before the reset =="

    make PRINTTOFILE=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (PRINTTOFILE=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "PRINTTOFILE=on build"; return
    fi

    make run-stop >/dev/null 2>&1
    make run-bg PRINTTOFILE=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    grep -q "nothing came through the last reset" build/serial.log
    chk $? "a cold start says nothing came through"

    # Warm reset: devices and CPU reset, guest RAM untouched.
    mon_raw "system_reset"
    wait_until "$WAIT_QUIET_S" "a SECOND shell banner after the warm reset" \
        '[ "$(grep -c "BoxOS Shell" build/serial.log 2>/dev/null)" = "2" ]'
    sleep 4

    grep -qE "the previous run left [0-9]+ byte\(s\) behind" build/serial.log
    chk $? "and the run after a warm reset finds what the one before it said"

    local announced
    announced=$(grep -oE "the previous run left [0-9]+ byte" build/serial.log \
                | tail -1 | grep -oE "[0-9]+")
    [ -n "$announced" ] && [ "$announced" -gt 0 ]
    chk $? "and it is not an empty window dressed up as a log ($announced bytes)"

    type_cmd "lastsaid"
    key_press ret
    # Waited on the sentence each check is about, not on a number of seconds.
    # Every one of these commands reads a volume, and a volume read is as long
    # as the medium takes.
    wait_for_line -E build/serial.log "^-- [0-9]+ byte\(s\) from the previous run --" "$WAIT_QUIET_S"

    grep -qE "^-- [0-9]+ byte\(s\) from the previous run --" build/serial.log
    chk $? "lastsaid prints it, which is what a machine with no volume needs"

    local poured
    poured=$(grep -oE "^-- [0-9]+ byte" build/serial.log | tail -1 | grep -oE "[0-9]+")
    [ -n "$poured" ] && [ "$poured" = "$announced" ]
    chk $? "and pours exactly what the kernel announced ($poured of $announced)"

    # Both tools, still. The point was never to replace one with the other.
    type_cmd "lastsaid prev.log"
    key_press ret
    wait_for_line -E build/serial.log "byte\(s\) of the previous run written to prev.log" "$WAIT_QUIET_S"
    grep -qE "byte\(s\) of the previous run written to prev.log" build/serial.log
    chk $? "and writes a file when it is given a name"

    type_cmd "logsave now.log"
    key_press ret
    wait_for_line -E build/serial.log "byte\(s\) written to now.log" "$WAIT_QUIET_S"
    grep -qE "byte\(s\) written to now.log" build/serial.log
    chk $? "and logsave still writes this boot's log, untouched"

    # ── the copy that cannot be taken away ──────────────────────────────────
    #
    # Both tools above are ELF files ON the volume. The board failure being
    # chased is one where the volume mounts, reports its files, and the shell
    # can find none of them — so at the moment the log is worth having,
    # nothing that could write it can be started. `said` is compiled into
    # shell.bin and needs no lookup and no spawn.
    type_cmd "said"
    key_press ret
    wait_for_line -E build/serial.log "^-- [0-9]+ byte\(s\): what this boot has said --" "$WAIT_QUIET_S"
    grep -qE "^-- [0-9]+ byte\(s\): what this boot has said --" build/serial.log
    chk $? "the built-in prints this boot's log without loading anything"

    type_cmd "said before"
    key_press ret
    wait_for_line -E build/serial.log "^-- [0-9]+ byte\(s\): what the run before this one said --" "$WAIT_QUIET_S"
    grep -qE "^-- [0-9]+ byte\(s\): what the run before this one said --" build/serial.log
    chk $? "and the previous run's, through the same door"

    type_cmd "said kept.log"
    key_press ret
    wait_for_line -E build/serial.log "written to kept.log" "$WAIT_QUIET_S"
    grep -qE "written to kept.log" build/serial.log
    chk $? "and files it when given a name"

    grep -q "said \[before\]" build/serial.log || {
        type_cmd "help"
        key_press ret
        wait_for_line build/serial.log "said \[before\]" "$REACT_QUIET_S"
    }
    grep -q "said \[before\]" build/serial.log
    chk $? "and help names it, so it can be found without being known"

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.lastsaid.log"
    make >"$SCRATCH/build.log" 2>&1
}

# ── mountfail: a mount that fails is a mount, not the end of the volume ─────
#
# The board failure this closes: the room seats the medium, the deed is read,
# "6009 data blocks, 410 free, 58 files" is printed — and the shell answers
# `Unknown command` to every one of those files. Two photographs of it, and
# the decisive fact was a line that WAS NOT THERE.
#
# tagfs_init brings up eight things in order. A mount that failed partway used
# to leave the first five standing and nothing ever took them down, so every
# LATER mount died at TagFS_CowInit with ERR_ALREADY_INITIALIZED — reported by
# a debug_printf, which compiles to nothing in a shipped build. One transient
# read error cost the machine its filesystem for the rest of its boot, silently.
#
# Nothing in QEMU ever fails a mount, so this whole path was unreachable from
# the desk and the repair would have been green by construction. MOUNTFAIL=on
# fails the FIRST mount at the worst point there is, with every subsystem
# before it up, and this scenario checks the whole story: the failure is SAID,
# the catch-up mount SUCCEEDS, and the shell can then find and run a program
# that lives on the volume.
#
# ‼ MEASURED AGAINST THE MUTATION IT EXISTS FOR. With the clearing taken out of
# mount_refused, the second mount answers "its copy-on-write layer would not
# start" and the machine falls back to its embedded shell with no files — the
# board, reproduced on the desk. Two other mutations were tried first and both
# stayed green, which is why this one is written against the line that does the
# work rather than the one that reads as though it does.
run_mountfail() {
    echo "== mountfail: one failed mount does not cost the volume for good =="

    make MOUNTFAIL=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (MOUNTFAIL=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "MOUNTFAIL=on build"; return
    fi

    make run-stop >/dev/null 2>&1
    # ‼ The key goes on run-bg TOO. It decides which mark file the tree carries,
    # and a bare `make run-bg` rebuilds the whole kernel without it — measured,
    # and it cost a run that looked like the reproduction had failed.
    make run-bg MOUNTFAIL=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    type_cmd "hw"
    sleep 1
    key_press ret
    sleep 4

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.mountfail.log"
    local L="$SCRATCH/serial.mountfail.log"

    # ── the failure happened, and it SAID so ────────────────────────────────
    grep -q "\[TagFS\] this volume is NOT mounted: MOUNTFAIL=on asked this one to fail" "$L"
    chk $? "the first mount fails where it was told to, and says so out loud"

    grep -q "\[Storage Deck\] no volume yet" "$L"
    chk $? "and the deck carries the refusal up rather than swallowing it"

    # ‼ The line that was missing on the board. Everything printed during a
    # mount — the deed, the far copy, the file count — is printed by a mount
    # that then fails, so none of it answers "is there a filesystem". This does.
    local said mounted
    said=$(grep -c "\[TagFS\] this volume is NOT mounted" "$L")
    mounted=$(grep -c "\[TagFS\] the volume on seat .* is MOUNTED" "$L")
    [ "$said" = 1 ] && [ "$mounted" = 1 ]
    chk $? "exactly one mount refused and exactly one mounted (said $said, mounted $mounted)"

    # ── and the SECOND mount, on the same medium, works ─────────────────────
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the catch-up mount takes the volume up after the failure"

    # The one that goes red under the mutation: with the ground left uncleared,
    # this line reads "its copy-on-write layer would not start" instead.
    ! grep -q "copy-on-write layer would not start" "$L"
    chk $? "the second mount is not poisoned by the first one's remains"

    # ── and the machine can actually USE it, which is the whole point ────────
    #
    # Autostart reads its files off the volume. A machine whose volume did not
    # come back says "No autostart files found" and falls back to the embedded
    # shell — which is exactly what the board did, with a stick in the socket.
    grep -q "\[AUTOSTART\] Started 'shell.bin'" "$L"
    chk $? "shell.bin is found on the volume and started"
    ! grep -q "No autostart files found" "$L"
    chk $? "and the machine does not fall back to the shell built into it"

    grep -q "CPU features" "$L"
    chk $? "an external utility off the volume runs"
    external_ok "$L"
    chk $? "and the prompt comes back after it"

    make >"$SCRATCH/build.log" 2>&1
}

run_logsave() {
    echo "== logsave: what the kernel said, written down where it can be read =="

    # Two kernels, because the switch is a BUILD switch. Every kernel keeps the
    # ring of its own run (it is the serial line's source); PRINTTOFILE adds
    # the window that carries it through a reset. Both halves have to hold:
    # both kernels must produce a readable account of their run, and the one
    # without the window must SAY it carries nothing through a reset rather
    # than hand back an empty file that reads like a machine which never spoke.
    make PRINTTOFILE=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (PRINTTOFILE=on) — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi

    make run-stop >/dev/null 2>&1
    make run-bg PRINTTOFILE=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    # A command FIRST, so the file is asked to carry both what a photograph of
    # the screen would have shown and what it would not: the command that was
    # typed. Userspace output reaches the ring through the Manifest VGA ops,
    # deliberately not through the serial mirror a board build turns off.
    type_cmd "hw"
    sleep 1; key_press ret; sleep 4
    type_cmd "logsave"
    sleep 1; key_press ret; sleep 8
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

    # Now the same on a kernel without the carry-over window.
    build
    make run-stop >/dev/null 2>&1
    make run-bg >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "logsave"
    sleep 1; key_press ret; sleep 8
    type_cmd "lastsaid"
    sleep 1; key_press ret; sleep 4
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.logsave-off.log"
    O="$SCRATCH/serial.logsave-off.log"

    grep -qE "[0-9]+ byte\(s\) written to watch.log" "$O"
    chk $? "a kernel without the window still writes down what it said"

    grep -q "carries nothing through a reset" "$O"
    chk $? "and says by name that it carries nothing through a reset"
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
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
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
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    # Typed AFTER the controller has been taken down and brought back. The
    # keyboard is on that controller: this is the whole point of the scenario.
    type_cmd "help"
    sleep 1
    key_press ret
    sleep 3
    type_cmd "hw"
    sleep 1
    key_press ret
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
    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latearrival_off; return; }
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    local MARK
    MARK=$(wc -l < build/serial.log)

    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"

    # And the MEDIUM must be slow. QEMU answers a read the instant it is asked,
    # so the whole mount burst is over before a poll of the log can see it
    # begin — there is no window to aim at, and a fixed delay lands either side
    # of it. Held to 64 KiB/s the same burst takes seconds, which is also what
    # a flash drive on a real bus looks like. Nothing in the guest is touched:
    # this is the emulator being told to behave less like a RAM disk.
    mon_raw "block_set_io_throttle stick 0 65536 0 0 0 0"

    # Fired off the first sign of the volume being read, not off a clock.
    #
    # ‼ THE ONE POLL IN THIS FILE THAT KEEPS ITS CEILING, AND ON PURPOSE.
    # It is not waiting for the machine to finish something — it is aiming at a
    # WINDOW: the stick has to be pulled while the read is in flight, so the
    # poll runs at twenty milliseconds and the ceiling is eight seconds of that
    # window. It also does not fail anything when it expires; the pull happens
    # either way and the checks below judge what the machine did. A silence
    # watchdog is the wrong instrument for a window: the machine is at its most
    # talkative exactly when this is waiting.
    i=0
    while [ $i -lt 400 ]; do
        tail -n +$((MARK+1)) build/serial.log 2>/dev/null | grep -q "Deed] seat" && break
        python3 -c "import time; time.sleep(0.02)"
        i=$((i+1))
    done
    sleep 1
    mon_raw "device_del usbstick"

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

# ── holdground: the volume comes back while somebody is still inside it ────
#
# replug proves a volume that leaves and comes home is judged, and one read
# again is read again. It proves it on an IDLE machine, which is the one case
# where letting go is free: nobody is in the filesystem, so the memory it hands
# back — the tag registry, the tag index, the block map, the file table, the
# metadata pool — is memory nobody is holding. That is not the machine this
# has to survive.
#
# `make HOLDGROUND=on` puts somebody there. At the moment the medium is noticed
# gone, one caller steps into the volume and does not step out until the volume
# is back; and a file is held OPEN across the whole departure.
#
# ‼ SINCE 1.12 ONLY A BOOK GIVEN UP TAKES ANYTHING DOWN. A stick that comes
# back as it left is taken up with its book open (tagfs/mount/away.h) — the
# caller steps out, the handle reads on, nothing is let go of — so the
# situation this exists for needs a stick that is NOT the book this kernel
# holds: its Ledger is moved on from the host while it is out, as another BoxOS
# kernel settling a cycle on it would move it (ledger_moved_on_host). The book
# is given up, and the take-down has to wait for the caller.
#
# What each party hears, derived from the code rather than guessed:
#   a program that asks the volume while its medium is away — a block read of
#      the absent medium answers ERR_DEVICE_NOT_READY (203, said by
#      TagFSReadStream with the bytes that had landed), and the program is
#      refused by name: "is there and would not start";
#   the caller inside — nothing is let go of until it is out: the take-down
#      says it waits, the caller steps out, and only then is the old volume let
#      go of and the new one mounted (tagfs_abandon, the service pass);
#   the handle held from before — its mounting is not the one up, so a read
#      through it is refused by name and answers ERR_TAGFS_INVALID_HANDLE (855,
#      handle_belongs_here), never a block of somebody else's file; its entry
#      in the open-file table is still the one the table hands out.
#
# ‼ THE MUTATIONS IT EXISTS FOR, each measured against this scenario on 1.12:
#
#   take `if (as == LEDGER_OTHER) return AWAY_OTHER;` out of AwayJudge
#      (away.c) -> the stick whose Ledger moved is taken up as the book this
#      kernel holds, and nothing below happens at all. MEASURED: 17 passed,
#      2 failed — the doubt replug names (a volume mounted and never written
#      is already judged AWAY_IN_DOUBT) still gives that book up, so only its
#      judgement and its order go red; with one flush after each Spare refill,
#      10 passed, 9 failed;
#   take the "somebody is inside" refusal out of tagfs_abandon      -> the
#      take-down says nothing and frees the volume under the caller.
#      MEASURED: 11 passed, 8 failed;
#   take the mounting stamp off the handle (tagfs_handle_is_of_this_mount
#      answers yes)                                                 -> the old
#      handle reads block numbers that may now belong to another file, and
#      says nothing. MEASURED: 17 passed, 2 failed — and exactly the two;
#   empty the open-file table at the mount again — what memset(g_open_files)
#      did in tagfs_init before the table moved to helm.c           -> the
#      entry a live handle owns is no longer the one the table hands out, so
#      two writers to one file take two different locks. MEASURED: 17 passed,
#      2 failed — and exactly the other two.
run_holdground() {
    echo "== holdground: the volume comes back while somebody is still in it =="

    # The stick must BE the volume — the same mutation replug and yank use, and
    # it stays installed for the whole run for the reason written there.
    latearrival_on
    make HOLDGROUND=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (HOLDGROUND=on) — tail:"; tail -25 "$SCRATCH/build.log"
        latearrival_off; bad "HOLDGROUND=on build"; return
    fi
    make run-stop >/dev/null 2>&1
    # ‼ The key goes on run-bg TOO: it decides which mark file the tree carries,
    # and a bare `make run-bg` rebuilds the kernel without it. Same trap that
    # cost mountfail a whole run.
    make run-bg HOLDGROUND=on USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latearrival_off; return; }
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    # First arrival: the stick becomes this machine's volume, and the probe
    # opens a file on it.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    local FIRST_LINES AT
    FIRST_LINES=$(wc -l < build/serial.log)

    # Pulled out — and waited on the PROBE's own line, not on a clock: it is
    # the fact that there is somebody inside the volume to protect.
    mon_raw "device_del usbstick"
    wait_for_line build/serial.log "HOLDGROUND. a caller was inside the volume" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 2

    # A program asked of the volume while its medium is away.
    type_cmd "files"
    sleep 1; key_press ret; sleep 3

    # The stick is changed while it is out, and pushed back in. drive_add
    # first: an HMP drive is auto-delete and went with the device.
    local MOVED=1
    ledger_moved_on_host "$SCRATCH/stick.img" && MOVED=0
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "HOLDGROUND. the open-file entry" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 3

    # And the volume mounted anew takes a write and anchors it.
    AT=$(wc -l < build/serial.log)
    type_cmd "anchor_test"
    sleep 1; key_press ret
    wait_for_line build/serial.log "\[ANCHOR\] WROTE+ANCHORED\|\[ANCHOR\] create rc\|\[ANCHOR\] fwrite rc\|\[ANCHOR\] anchor rc" "$WAIT_QUIET_S" $((AT + 1))
    sleep 2

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.holdground.log"
    latearrival_off
    L="$SCRATCH/serial.holdground.log"
    tail -n +$((FIRST_LINES + 1)) "$L" > "$SCRATCH/serial.holdground.second.log"
    local S="$SCRATCH/serial.holdground.second.log"

    # ── the situation was actually reached ──────────────────────────────────
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the stick was mounted on its first arrival"
    grep -q "HOLDGROUND. holding file .* open across whatever happens" "$L"
    chk $? "and a file was held open on it"
    grep -q "HOLDGROUND. a caller was inside the volume when its medium left" "$S"
    chk $? "somebody was inside the volume when the medium left"
    [ "$MOVED" = 0 ]
    chk $? "the host moved the stick's Ledger on while it was out"

    # ── a program asking the absent medium hears it is absent ──────────────
    grep -q "answered 203 — 0 of .* bytes had landed" "$S"
    chk $? "a read of the absent medium answered ERR_DEVICE_NOT_READY (203)"
    grep -q "files: is there and would not start" "$S"
    chk $? "and the program was refused by name, not called unknown"

    # ── ‼ NOT THE BOOK THIS KERNEL HOLDS ───────────────────────────────────
    grep -q "came back in seat 1 with a Ledger or a Deed this kernel did not leave on it" "$S"
    chk $? "the stick whose Ledger moved was judged not to be the book this kernel holds"
    ! grep -q "the volume is back, in seat" "$S"
    chk $? "and was not taken up as that book"

    # ── ‼ THE BARRIER. Nothing is let go of while somebody is on it ─────────
    grep -qE "the volume is on its way out and [1-9][0-9]* caller\(s\) are still inside it" "$S"
    chk $? "the take-down found a caller inside and let go of nothing"

    grep -q "HOLDGROUND. the caller has stepped out of the old volume" "$S"
    chk $? "the caller stepped out of the old volume"

    grep -q "the last caller is out and the old volume has been let go of" "$S"
    chk $? "and the take-down finished only once that caller was out"

    # In that ORDER. A "let go of" line that precedes the "still inside" line
    # would mean the memory went back first and the sentence came after.
    local GIVEN_AT WAIT_AT OUT_AT FREE_AT MOUNT_AT
    GIVEN_AT=$(grep -n "did not leave on it" "$S" | head -1 | cut -d: -f1)
    WAIT_AT=$(grep -nE "the volume is on its way out and [1-9]" "$S" | head -1 | cut -d: -f1)
    OUT_AT=$(grep -n "HOLDGROUND. the caller has stepped out" "$S" | head -1 | cut -d: -f1)
    FREE_AT=$(grep -n  "the last caller is out and the old volume" "$S" | head -1 | cut -d: -f1)
    MOUNT_AT=$(grep -n "the volume on seat .* is MOUNTED" "$S" | tail -1 | cut -d: -f1)
    [ -n "$GIVEN_AT" ] && [ -n "$WAIT_AT" ] && [ -n "$OUT_AT" ] && [ -n "$FREE_AT" ] && \
        [ -n "$MOUNT_AT" ] && [ "$GIVEN_AT" -lt "$WAIT_AT" ] && [ "$WAIT_AT" -lt "$OUT_AT" ] && \
        [ "$OUT_AT" -lt "$FREE_AT" ] && [ "$FREE_AT" -lt "$MOUNT_AT" ]
    chk $? "given up, waited, stepped out, let go, then mounted — in that order (${GIVEN_AT:-?}, ${WAIT_AT:-?}, ${OUT_AT:-?}, ${FREE_AT:-?}, ${MOUNT_AT:-?})"

    # ── ‼ THE HANDLE FROM BEFORE IS REFUSED, NOT FOLLOWED ──────────────────
    grep -q "was opened on an earlier mounting of this volume" "$S"
    chk $? "a read through the handle from the previous mounting is refused"

    # 855 is ERR_TAGFS_INVALID_HANDLE: the read's own code, never folded (S3b).
    grep -q "HOLDGROUND. a read through the handle from before answered 855" "$S"
    chk $? "and the refusal reaches the caller as its own code, ERR_TAGFS_INVALID_HANDLE"

    # ── ‼ THE OPEN-FILE TABLE BELONGS TO THE MACHINE ───────────────────────
    grep -q "HOLDGROUND. the open-file entry that handle owns is still in the table" "$S"
    chk $? "the entry a live handle owns survived the re-mount"

    grep -q "HOLDGROUND. opening the same file again found the same entry" "$S"
    chk $? "and one file still has exactly one entry, so its writers share one lock"

    # ── and the machine is usable afterwards, which is the point ───────────
    grep -q "\[TagFS\] the volume on seat 1 is MOUNTED" "$S"
    chk $? "the volume was mounted anew from the medium as it now is"
    grep -q "\[ANCHOR\] WROTE+ANCHORED" "$S"
    chk $? "and it takes a write and anchors it"
    grep -q "BoxOS Shell" "$L"
    chk $? "the machine still has a shell"
}

# ── returnfail: a mount that did not finish is asked again by the machine ──
#
# Mounting is driven by ARRIVALS. A medium that is already seated does not
# arrive twice, so a mount that begins on a volume's RETURN and fails used to
# leave the machine with no filesystem and nothing that would ever ask again —
# the stick in its socket, answering, ignored for the rest of the boot.
#
# ‼ MEASURED ON THE OWNER'S BOARD, 2026-08-30, BEFORE THIS EXISTED: three
# returns out of fifty-five failed their mount, and all three recovered ONLY
# because the hand at the machine kept replugging. A read that fails on a
# medium which STAYS PUT had no way back at all.
#
# `make RETURNFAIL=on` fails the first re-mount once, as a read the medium
# refused, with the medium left exactly where it is. The stick is plugged in
# ONCE after the pull — that single arrival is spent on the attempt that
# fails — so every line after it is the machine asking again by itself.
#
# ‼ SINCE 1.12 THERE IS A RE-MOUNT ONLY WHEN THE BOOK IS GIVEN UP. A stick that
# comes back as this kernel left it is taken up with its book open, and nothing
# is read again (tagfs/mount/away.h) — there would be no re-mount here to fail.
# So the stick comes back CHANGED: its Ledger is moved on from the host while
# it is out, as another BoxOS kernel settling a cycle on it would move it
# (ledger_moved_on_host). That book is given up by design, the volume is let
# go of, and the mount that follows is the one RETURNFAIL fails.
#
# ‼ AND IT IS ASKED AGAIN WHEN THE SEAT SAYS IT ANSWERS AGAIN, NOT ON A CLOCK.
# The refusal is a read the medium refused, so the machine owes the mount until
# `seat:ready` names its seat (tagfs.c, the note over g_mount_owed). A seat
# rings that on the first command it answers after a refusal — and while the
# mount is owed nobody sends it one. So the machine resets the seat ONCE, by
# its driver's own recovery (tagfs.c, the note over g_reset_seating): here the
# stick's transport reset, whose bell the mount is asked again on. A key that
# said seat:ready for the stick two seconds after the refusal stood here until
# the machine could make the event itself; it is gone. A machine that asked
# again on its own, on a clock or straight away, asks BEFORE the reset; one
# that resets in a loop resets more than once; the order and the counts below
# say so.
#
# ‼ THE MUTATION IT EXISTS FOR, measured on P9:
#
#   take the reset out of mount_attempt (tagfs.c: attendance_reset) — the
#      refusal is owed and nothing ever rings: no asking, no mount, the machine
#      volumeless with its stick in. MEASURED: 9 passed, 8 failed.
run_returnfail() {
    echo "== returnfail: a mount that did not finish is asked again =="

    latearrival_on
    make RETURNFAIL=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (RETURNFAIL=on) — tail:"; tail -25 "$SCRATCH/build.log"
        latearrival_off; bad "RETURNFAIL=on build"; return
    fi
    make run-stop >/dev/null 2>&1
    make run-bg RETURNFAIL=on USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latearrival_off; return; }
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    # First arrival — the mount that succeeds, and the one the stamp counts.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    local FIRST_LINES AT
    FIRST_LINES=$(wc -l < build/serial.log)

    # Pulled out.
    mon_raw "device_del usbstick"
    wait_for_line build/serial.log "the medium the volume lives on has left" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 2

    # Changed while it is out, and pushed back in ONCE. That single arrival is
    # spent on the re-mount this build is told to fail; nothing is plugged in
    # after it.
    local MOVED=1
    ledger_moved_on_host "$SCRATCH/stick.img" && MOVED=0
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"

    # Waited on the machine's own words, not a clock: the mount it asks for.
    wait_for_line build/serial.log "a medium arrived carrying a volume, and this machine had none" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 3

    # And the volume it asked for takes a write and anchors it.
    AT=$(wc -l < build/serial.log)
    type_cmd "anchor_test"
    sleep 1; key_press ret
    wait_for_line build/serial.log "\[ANCHOR\] WROTE+ANCHORED\|\[ANCHOR\] create rc\|\[ANCHOR\] fwrite rc\|\[ANCHOR\] anchor rc" "$WAIT_QUIET_S" $((AT + 1))
    sleep 2

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.returnfail.log"
    latearrival_off
    L="$SCRATCH/serial.returnfail.log"
    tail -n +$((FIRST_LINES + 1)) "$L" > "$SCRATCH/serial.returnfail.second.log"
    local S="$SCRATCH/serial.returnfail.second.log"

    # ── the situation was reached ───────────────────────────────────────────
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the stick was mounted on its first arrival"
    grep -q "the medium the volume lives on has left — its book is held open" "$S"
    chk $? "and the medium then left, its book held open"
    [ "$MOVED" = 0 ]
    chk $? "the host moved the stick's Ledger on while it was out"
    grep -q "came back in seat 1 with a Ledger or a Deed this kernel did not leave on it" "$S"
    chk $? "the changed stick was not taken for the book this kernel holds, and the book was given up"
    grep -q "this volume is NOT mounted: RETURNFAIL=on asked this re-mount to fail" "$S"
    chk $? "the re-mount failed where it was told to, and said so out loud"
    grep -q "the medium in seat 1 refused a read while its volume was being read — the mount is asked again when the seat says it answers again (seat:ready)" "$S"
    chk $? "the machine took it for a read the medium refused, and owes the mount until seat:ready"
    grep -q "the volume came back and would not mount" "$S"
    chk $? "and the return road carried the refusal up"

    # ‼ ONE arrival, and it was spent. Everything after is the machine itself.
    local arrivals
    arrivals=$(grep -c "took a seat while the machine was" "$S")
    [ "$arrivals" = 1 ]
    chk $? "the stick was plugged in exactly once after the pull ($arrivals)"

    # ── ‼ THE MACHINE ASKED AGAIN BY ITSELF, WHEN THE SEAT SAID SO ─────────
    grep -q "seat 1 refused a read and no bell rang after the attempt that met it — nobody else asks it anything, so its medium is reset once" "$S"
    chk $? "nothing rang the refused seat, and the machine reset it once, by its driver's own recovery"
    grep -qE "\[USB disk [0-9]+\] resetting the transport" "$S"
    chk $? "the stick's transport was reset"
    local resets
    resets=$(grep -c "its medium is reset once" "$S")
    [ "$resets" = 1 ]
    chk $? "once, never again ($resets)"
    grep -q "seat 1 answers again — asking for the volume nobody has mounted" "$S"
    chk $? "the machine asked again for the volume nobody had mounted"
    grep -q "a medium arrived carrying a volume, and this machine had none — mounted from seat 1" "$S"
    chk $? "and the volume came up in the seat it sat in"

    # In that order: given up, refused, the seat reset, the asking, the mount.
    # An asking that comes before the reset is a machine asking on a clock, or
    # at once, which is what this replaced.
    local GIVEN_AT OWED_AT RESET_AT ASK_AT UP_AT
    GIVEN_AT=$(grep -n "did not leave on it" "$S" | head -1 | cut -d: -f1)
    OWED_AT=$(grep -n "refused a read while its volume was being read" "$S" | head -1 | cut -d: -f1)
    RESET_AT=$(grep -n "its medium is reset once" "$S" | head -1 | cut -d: -f1)
    ASK_AT=$(grep -n "asking for the volume nobody has mounted" "$S" | head -1 | cut -d: -f1)
    UP_AT=$(grep -n "\[TagFS\] the volume on seat 1 is MOUNTED" "$S" | tail -1 | cut -d: -f1)
    [ -n "$GIVEN_AT" ] && [ -n "$OWED_AT" ] && [ -n "$RESET_AT" ] && [ -n "$ASK_AT" ] && \
        [ -n "$UP_AT" ] && [ "$GIVEN_AT" -lt "$OWED_AT" ] && [ "$OWED_AT" -lt "$RESET_AT" ] && \
        [ "$RESET_AT" -lt "$ASK_AT" ] && [ "$ASK_AT" -lt "$UP_AT" ]
    chk $? "given up, refused, the seat reset, asked, mounted — in that order (${GIVEN_AT:-?}, ${OWED_AT:-?}, ${RESET_AT:-?}, ${ASK_AT:-?}, ${UP_AT:-?})"

    # One reset, one asking: the mount owed is asked for once.
    local asked
    asked=$(grep -c "asking for the volume nobody has mounted" "$S")
    [ "$asked" = 1 ]
    chk $? "and it was asked for once, on the one event ($asked)"

    # ── and the machine is usable afterwards, which is the point ───────────
    grep -q "\[ANCHOR\] WROTE+ANCHORED" "$S"
    chk $? "the volume it asked for takes a write and anchors it"
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still has a shell"
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
    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latearrival_off; return; }
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
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
    mon_raw "block_set_io_throttle stick 0 512 0 0 0 0"
    sleep 1
    type_line "today"

    # ‼ WAITED ON THE PROGRAM'S OWN ANSWER, AND NOTHING IS TYPED UNTIL IT COMES.
    #
    # Fixed pauses here cost two whole runs. The shell is one thing doing one
    # thing at a time: a command typed while the last one is still reading is a
    # command that lands in a buffer, and the scenario then reports a machine
    # that was never given a chance as a machine that failed. `today` prints a
    # date, and the date arriving is the fact that it was read.
    wait_for_line -E build/serial.log "^20[0-9][0-9]-[0-9][0-9]-[0-9][0-9] " "$WAIT_QUIET_SLOW_S" $((MARK + 1))
    GAVE_UP_AFTER=$(grep -cE "giving up on the command|no answer in .* at stage" build/serial.log)
    mon_raw "block_set_io_throttle stick 0 0 0 0 0 0"
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
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    wait_for_prompt

    # 1536 bytes a second puts ONE 4 KiB filesystem block at not quite three
    # seconds — past the two this driver used to allow a whole command, and
    # nowhere near the thirty the class gives one. Everything below must simply
    # work, slowly.
    #
    # The volume is already mounted, so this is aimed at what a filesystem does
    # afterwards: `files` is a tag query, and a tag query is metadata reads,
    # and metadata reads are the synchronous path.
    mon_raw "block_set_io_throttle disk0 0 1536 0 0 0 0"
    sleep 1
    type_line "files"

    wait_for_line build/serial.log "^shell.bin " "$WAIT_QUIET_SLOW_S"

    mon_raw "block_set_io_throttle disk0 0 0 0 0 0 0"
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

# ---------------------------------------------------------------------------
# haltslow: the power button while a slow disk keeps work in flight (R17)
#
# A Settle cycle on the committer settles nothing until its last step, and on a
# slow medium it outlasts any core's ceiling: the halt's drain judged it by its
# measure alone, gave up on it mid-cycle, and stopped the machine without a
# cycle of its own — the volume's records never written. And a write goes a
# window at a time, each window run at a service point as a held span: a core
# the halt's IPI found inside one owed its stop, nothing after the window paid
# it, and the roll call waited for that core for ever.
#
# Both are made on the plain BIOS machine with four cores — two K-Cores, so the
# committer is not the core the power button's halt runs on — by holding the
# disk's writes to a crawl (QEMU's block_set_io_throttle). The button goes once
# the writes the case waits for have landed, counted off QEMU's own block
# statistics, never off a clock: the anchor's data and the first write of the
# cycle that settles it; a write's first windows.
#
# ‼ THE WINDOW CASE IS NOT ASKED SINCE H1 (check #1), measured: the halt's IPI
# met a core inside a window only because the drain gave up on a write that was
# moving — its windows' answers counted for nothing (haltcycle's root). Now
# they move the work, and no window begins after the halt shuts the volume
# (TagFSShutForHalt), so an orderly halt sends its IPI with no window in
# flight, and the leg says "the window case was not asked". Only a drain that
# gives up on a window that stopped answering on another core meets one now;
# at 512 bytes a second the drain gave up on the committer's cycle step
# instead, and the case was not asked either. Asking it again is open.
# ---------------------------------------------------------------------------

HALTSLOW_CYCLE_BPS=2048    # two seconds a block: a cycle of a few blocks outlasts the core's five
HALTSLOW_WINDOW_BPS=8192   # half a second a window: a window is what the halt's IPI meets

haltslow_writes() {   # the boot disk's writes so far, as QEMU counts them
    ./tools/qemu-input.sh raw "info blockstats" 2>/dev/null | tr -d '\r' |
        grep -aoE '^ide0-hd0: .* wr_operations=[0-9]+' | sed 's/.*wr_operations=//'
}

# haltslow_boot TAG BPS CMD WRITES — a copy of the image booted, the disk's
# writes held to BPS bytes a second, CMD typed, and the power button pressed
# once WRITES more writes have landed; the log kept as serial.haltslow.TAG.log.
# 0 when the machine switched itself off, 1 when it did not, 2 when the case
# could not be put to it.
haltslow_boot() {
    local tag=$1 bps=$2 cmd=$3 want=$4 img="$SCRATCH/haltslow.$1.img" base pid off=1
    make run-stop >/dev/null 2>&1
    cp build/boxos.img "$img"
    if ! make run-bg BOOTDISK="$img" CORES=4 MEM=4G >"$SCRATCH/runbg.haltslow.$tag.log" 2>&1; then
        harness "haltslow: no machine was started for $tag — $(tail -1 "$SCRATCH/runbg.haltslow.$tag.log")"
        rm -f "$img"
        return 2
    fi
    pid=$(cat build/qemu.pid)
    if wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
       base=$(haltslow_writes) && [ -n "$base" ] &&
       mon_raw "block_set_io_throttle ide0-hd0 0 0 $bps 0 0 0" && type_line "$cmd" &&
       wait_until "$WAIT_QUIET_SLOW_S" "$want writes of '$cmd' to land" \
           '[ "$(haltslow_writes)" -ge $((base + want)) ]' &&
       mon_raw system_powerdown; then
        wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
        off=$?
    else
        harness "haltslow: the $tag case could not be put to the machine (writes before it: ${base:-unread})"
        off=2
    fi
    cp build/serial.log "$SCRATCH/serial.haltslow.$tag.log"
    make run-stop >/dev/null 2>&1
    rm -f "$img"
    return $off
}

run_haltslow() {
    echo "== haltslow: the power button while a slow disk has a cycle and a write's windows in flight =="
    local L rc
    build

    haltslow_boot cycle "$HALTSLOW_CYCLE_BPS" anchor_test 2; rc=$?
    L="$SCRATCH/serial.haltslow.cycle.log"
    if [ "$rc" != 2 ]; then
        if grep -aq '^\[HALT\] a Settle cycle runs on another core' "$L"; then
            [ "$rc" = 0 ]
            chk $? "a cycle on the committer when the button went: the machine switched itself off"
            ! grep -aq '^\[HALT\] giving up' "$L"
            chk $? "the drain waited for the cycle while it took steps, and gave up on nothing ($(grep -aE '^\[HALT\] (work owed|giving up)' "$L" | head -1 | cut -c1-100))"
            grep -aq '^\[HALT\] Storage sync complete' "$L"
            chk $? "and the halt's own cycle wrote the volume's records ($(grep -aE "$STAND_HALT_ENDS|DID NOT WRITE" "$L" | head -1 | cut -c1-100))"
        else
            harness "haltslow: no Settle cycle was running on another core when the button went, so the cycle case was not asked"
        fi
    fi

    haltslow_boot window "$HALTSLOW_WINDOW_BPS" "quilltest coldwrite" 6; rc=$?
    L="$SCRATCH/serial.haltslow.window.log"
    if [ "$rc" != 2 ]; then
        [ "$rc" = 0 ] && grep -aq '^\[HALT\] All AP cores stopped (roll call complete)' "$L"
        rc=$?
        chk $rc "a write's windows at the service points when the button went: every core answered the roll call and the machine switched itself off ($(grep -aE '^\[HALT\] (Stopping all|All AP|core [0-9]+ )' "$L" | tail -1 | cut -c1-100))"
        if [ "$rc" = 0 ] &&
           ! grep -aq '^\[HALT\] core [0-9]* stops after the Helm work it ran at a service point' "$L"; then
            harness "haltslow: the halt found no core inside a window of the write, so the window case was not asked"
        fi
    fi
}

# ---------------------------------------------------------------------------
# haltcycle: the power button while the committer settles a writer's anchor
# (RT-12), on the machines a person's writes go through the port
#
# A writer keeps going — `quilltest keeper` rewrites its record and anchors it,
# again and again, each anchor said — and the power button lands while a Settle
# cycle runs on the committer, an AP (settle.h: the first K-Core that is not
# the BSP), and the halt's drain runs on the drain core. The drain must wait
# for that cycle as long as it takes steps, then run the volume's last cycle
# itself before any core is stopped: the machine switches itself off, says
# "[HALT] Storage sync complete", never "giving up", and the record the
# writer's last anchor named before the roll call is the record the volume
# holds when it comes back — read by `quilltest verify` on the next boot, the
# volume found consistent by volume-read in between. UEFI on q35, AHCI and the
# WriteJob, with four cores (one committer beside the drain core) and with
# sixteen (four K-Cores).
#
# The cycle is made to be in flight when the button goes, never hoped to be:
# the disk's writes are held to HALTCYCLE_BPS once the writer runs, so a cycle
# lasts seconds; the button goes on the first cache flush QEMU counts after an
# anchor was said — a flush no data write makes (write_job.c, "no flush of its
# own"), so it is the next cycle's FLUSH#1, and that cycle's pool and Ledger
# are still to be written. And the halt says it found the cycle ("a Settle
# cycle runs on another core"); a leg where it did not asked nothing. That
# happens: on sixteen cores, measured once in three runs, the button met no
# cycle on another core — the drain found a command in the port and nobody
# else holding the Settle, "work owed stopped falling at 2 (0 pass(es), 1 AHCI
# command(s) still issued)", and halted clean all the same. So such a leg is
# said and booted again on a fresh copy, up to HALTCYCLE_TRIES boots, and only
# when none of them asked the case is it the harness's failure — never a
# verdict, and never a leg judged on a case it did not ask.
#
# ‼ THE MUTATION IT EXISTS FOR, MEASURED when it was written (P4): take the
# halt's own last cycle out of the drain (halt_drain_async_writes: `refused =
# TagFSSettleForHalt() != OK;` made `refused = false;`) — the committer still
# settles the anchors it is asked for, the drain still empties, and the halt
# never writes the volume's last records: "[HALT] the medium on seat 0 took
# what it was given, and the volume's own records were not part of it", 13
# passed, 2 failed, one a leg. The halt's cycle begun while the committer's
# still runs (`owed.Committer == 0` taken out) did NOT turn it: it waits for
# that cycle at the Settle's Scribe all the same (ScribeWaitTakingOver), 15
# passed.
#
# ‼ AND WHAT IT CAUGHT (H1, check #1): on the tree it was written for, the drain
# gave up 3 boots in 4 on four cores and 1 in 4 on sixteen, and the anchored
# record read back as rot ("a read of 16 answered -19", ERR_CHECKSUM) on a
# volume volume-read found consistent. Measured on four cores, two roots. The
# glass turned as the committer's cycle ended ran on through the halt's own two
# cycles — their steps counted only when another core took them — and ran out
# while a writer's window was taking the medium's answers, which counted not
# at all: both move the work now (HaltDrainMoved; stand_halt_drain_counts_
# what_moves). And that window was a write in place the keeper began while the
# halt's cycles ran: it landed after the last of them sealed the volume clean,
# the digest of the bytes before it on the medium, and a clean seal is the
# next mount's word that every digest stands — now the halt shuts the volume
# to changes not begun before its drain, and seals clean only with none in
# flight (TagFSShutForHalt; stand_settle_halt_seals_what_stands). The keeper's
# write after the shut is answered -203 (ERR_DEVICE_NOT_READY), said by it.
# ---------------------------------------------------------------------------

HALTCYCLE_BPS=2048       # two seconds a block, as haltslow's: every step well inside the core's five
HALTCYCLE_NAME=_qkeep
HALTCYCLE_ANCHORED="keeper: $HALTCYCLE_NAME fid=[0-9]+ anchored seq=[0-9]+"
HALTCYCLE_TRIES=3

haltcycle_flushes() {   # the boot disk's cache flushes so far, as QEMU counts them
    ./tools/qemu-input.sh raw "info blockstats" 2>/dev/null | tr -d '\r' |
        grep -aoE '^ide0-hd0: .* flush_operations=[0-9]+' | sed 's/.*flush_operations=//'
}

haltcycle_anchors() {   # how many anchors the keeper has said
    tr -d '\r' < build/serial.log 2>/dev/null | grep -acE "$HALTCYCLE_ANCHORED"
}

# haltcycle_press — the writer's disk held to HALTCYCLE_BPS, an anchor said,
# and the button pressed on the first flush after it. 0 once pressed.
haltcycle_press() {
    local n0 f0
    mon_raw "block_set_io_throttle ide0-hd0 0 0 $HALTCYCLE_BPS 0 0 0" || return 1
    n0=$(haltcycle_anchors)
    wait_until "$WAIT_QUIET_S" "an anchor said once the disk was slowed" \
        '[ "$(haltcycle_anchors)" -gt "$n0" ]' || return 1
    f0=$(haltcycle_flushes)
    [ -n "$f0" ] || return 1
    wait_until "$WAIT_QUIET_S" "the next cycle's first flush" \
        '[ "$(haltcycle_flushes)" -gt "$f0" ]' || return 1
    mon_raw system_powerdown
}

# haltcycle_leg TAG KEYS... — a copy of the image booted, the keeper started,
# the button pressed (haltcycle_press), the machine left to switch itself off;
# the copy judged by volume-read, booted again and the keeper's file verified.
# Leaves serial.haltcycle.TAG.log and .again.log, haltcycle.TAG.check and
# HALTCYCLE_OFF (0 when the machine switched itself off). 1 when there is no
# run to judge, said as the harness's failure.
haltcycle_leg() {
    local tag=$1 img="$SCRATCH/haltcycle.$1.img" pid mark; shift
    HALTCYCLE_OFF=1
    rm -f "$SCRATCH/serial.haltcycle.$tag.log" "$SCRATCH/serial.haltcycle.$tag.again.log" \
          "$SCRATCH/haltcycle.$tag.check"
    make run-stop >/dev/null 2>&1
    cp -c build/boxos.img "$img" 2>/dev/null || cp build/boxos.img "$img"
    if ! make run-bg BOOTDISK="$img" "$@" >"$SCRATCH/runbg.haltcycle.$tag.log" 2>&1; then
        harness "haltcycle: no machine was started for $tag — $(tail -1 "$SCRATCH/runbg.haltcycle.$tag.log")"
        rm -f "$img"
        return 1
    fi
    pid=$(cat build/qemu.pid)
    if wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_quiet "$BOOT_BURST_QUIET_S" &&
       type_line "quilltest keeper $HALTCYCLE_NAME" &&
       wait_for_line -E build/serial.log "$HALTCYCLE_ANCHORED" "$WAIT_QUIET_S" && haltcycle_press; then
        wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
        HALTCYCLE_OFF=$?
    else
        cp build/serial.log "$SCRATCH/serial.haltcycle.$tag.log"
        make run-stop >/dev/null 2>&1
        harness "haltcycle: the $tag case could not be put to the machine (last: $(tr -d '\r' < "$SCRATCH/serial.haltcycle.$tag.log" | grep -a '^\[QUILL\]' | tail -1))"
        rm -f "$img"
        return 1
    fi
    cp build/serial.log "$SCRATCH/serial.haltcycle.$tag.log"
    make run-stop >/dev/null 2>&1
    python3 tools/volume-read.py "$img" check >"$SCRATCH/haltcycle.$tag.check" 2>&1
    echo "rc=$?" >>"$SCRATCH/haltcycle.$tag.check"
    if make run-bg BOOTDISK="$img" "$@" >"$SCRATCH/runbg.haltcycle.$tag.again.log" 2>&1 &&
       wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_quiet "$BOOT_BURST_QUIET_S"; then
        mark=$(wc -l < build/serial.log)
        type_line "quilltest verify $HALTCYCLE_NAME" &&
            wait_for_line -E build/serial.log '\[QUILL\] verify: (PASS|FAIL)' "$WAIT_QUIET_S" $((mark + 1))
    fi
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.haltcycle.$tag.again.log"
    rm -f "$img"
    return 0
}

# haltcycle_legs TAG WHAT KEYS... — the leg run until the button meets a cycle
# on another core, at most HALTCYCLE_TRIES times, each boot that did not said;
# then judged. Nothing when no leg could be run at all (said by the leg).
haltcycle_legs() {
    local tag=$1 what=$2 try=1; shift 2
    while haltcycle_leg "$tag" "$@"; do
        if grep -aq '^\[HALT\] a Settle cycle runs on another core' "$SCRATCH/serial.haltcycle.$tag.log" ||
           [ "$try" -ge "$HALTCYCLE_TRIES" ]; then
            haltcycle_judge "$tag" "$what"
            return
        fi
        echo "  (haltcycle: $what — boot $try of $HALTCYCLE_TRIES: the button met no cycle on another core ($(grep -aE '^\[HALT\] (work owed|Async writes|giving up)' "$SCRATCH/serial.haltcycle.$tag.log" | tr -d '\r' | head -1 | cut -c1-100)); booted again)"
        try=$((try + 1))
    done
}

# haltcycle_judge TAG WHAT — the leg just run.
haltcycle_judge() {
    local tag=$1 what=$2 L="$SCRATCH/serial.haltcycle.$1.log" A="$SCRATCH/serial.haltcycle.$1.again.log"
    local fault floor seq check
    if ! grep -aq '^\[HALT\] a Settle cycle runs on another core' "$L"; then
        harness "haltcycle: $what — the halt found no Settle cycle on another core when the button went, so the case was not asked ($(grep -aE '^\[HALT\] (work owed|Async writes|giving up)' "$L" | head -1 | cut -c1-100))"
        return
    fi
    ok "haltcycle: $what — the button went while a Settle cycle ran on the committer, and the drain found it"
    [ "$HALTCYCLE_OFF" = 0 ] && grep -aq '^\[HALT\] Powering off' "$L"
    chk $? "haltcycle: $what — the machine switched itself off ($(grep -a '^\[HALT\]' "$L" | tr -d '\r' | tail -1 | cut -c1-100))"
    ! grep -aq '^\[HALT\] giving up' "$L"
    chk $? "haltcycle: $what — the drain waited for the cycle while it took steps, and gave up on nothing ($(grep -aE '^\[HALT\] (work owed|giving up)' "$L" | tr -d '\r' | tail -1 | cut -c1-110))"
    grep -aq '^\[HALT\] Storage sync complete' "$L"
    chk $? "haltcycle: $what — the halt's own cycle wrote the volume's records and the medium kept them ($(grep -aE "$STAND_HALT_ENDS" "$L" | tr -d '\r' | head -1 | cut -c1-100))"
    fault=$(grep -aE 'KERNEL PANIC|\[EXCEPTION\]|^\[NIGHTWATCH\] VERDICT: .* a defect' "$L" "$A" | tr -d '\r' | head -1 | cut -c1-160)
    [ -z "$fault" ]
    chk $? "haltcycle: $what — no panic, no exception, nothing owed proved unserved${fault:+: $fault}"
    check=$(grep -a '^check: ' "$SCRATCH/haltcycle.$tag.check" | tail -1)
    grep -qx 'rc=0' "$SCRATCH/haltcycle.$tag.check"
    chk $? "haltcycle: $what — volume-read finds the volume the halt left consistent (${check:-$(tail -2 "$SCRATCH/haltcycle.$tag.check" | head -1)})"
    floor=$(tr -d '\r' < "$L" | sed '/^\[HALT\] Terminating all processes/q' | grep -aoE "$HALTCYCLE_ANCHORED" |
            tail -1 | sed 's/.*seq=//')
    seq=$(tr -d '\r' < "$A" | sed -nE "s/^\[QUILL\] verify: PASS — $HALTCYCLE_NAME fid=[0-9]+ seq=([0-9]+) bytes=16: .*/\1/p" | tail -1)
    [ -n "$floor" ] && [ -n "$seq" ] && [ "$seq" -ge "$floor" ]
    chk $? "haltcycle: $what — after the reboot the record is whole at seq ${seq:-none}, the last anchor said before the roll call was seq ${floor:-none} ($(tr -d '\r' < "$A" | grep -aoE '\[QUILL\] verify: (PASS|FAIL) — [^:]*' | tail -1))"
}

run_haltcycle() {
    echo "== haltcycle: the power button while the committer, an AP, settles a writer's anchor, on UEFI 4c and 16c =="
    build STRICT=on
    haltcycle_legs uefi4 "UEFI 4c, one committer beside the drain core" UEFI=on STRICT=on CORES=4 MEM=4G
    haltcycle_legs uefi16 "UEFI 16c, four K-Cores" UEFI=on STRICT=on CORES=16 MEM=8G
    lockrank_judge "$SCRATCH/serial.haltcycle.uefi4.log $SCRATCH/serial.haltcycle.uefi4.again.log $SCRATCH/serial.haltcycle.uefi16.log $SCRATCH/serial.haltcycle.uefi16.again.log" "haltcycle"
    build   # leave the tree built plain, as every scenario here does
}

# ---------------------------------------------------------------------------
# fullvolume: a volume with no block left gives a write the ground a delete
# frees, and answers every delete (RT-06/07)
#
# Three legs, each on its own copy of the image, UEFI on q35, four cores:
#
#   roomback   the copy's partition stretched by FULLVOLUME_STRETCH sectors, a
#              GiB, and `quilltest roomback FULLVOLUME_MIB`: _qra.bin of a GiB
#              and one MiB laid down, the rest of the volume filled until it
#              answers no-space, anchored; then _qra.bin deleted and at once a
#              GiB written to _qrc.bin. The ground the delete frees rests in
#              Fallow until a cycle has its Quitclaim in a closed block and the
#              cycles after it settle (fallow.h): the write must wait for that,
#              never be told there is no room, and land whole — read back by
#              the guest, then off the medium by volume-read, every block of it
#              beginning with its own stream, _qra.bin gone, the volume
#              consistent once the machine halted through bye.
#   emptyfull  the image as built, `quilltest emptyfull FULLVOLUME_EMPTIES`:
#              that many empty files, the volume filled to its no-space,
#              anchored, and every one of them deleted, then the file that
#              filled the volume — each delete answered, and answered OK (the
#              design: on a full volume the delete past the tail's room waits
#              for a Spare, it is not refused; one that frees no ground waits
#              for Glean's sweep, glean.h); the machine halts through bye and
#              volume-read finds the volume consistent with neither on it.
#   emptyback  the same case on one copy booted twice, the machine halted
#              through bye between its halves: `quilltest emptyfill
#              FULLVOLUME_EMPTIES` makes the files and fills the volume, and
#              after the reboot `quilltest emptyback FULLVOLUME_EMPTIES` finds
#              every one of them and deletes them, then the file that filled
#              the volume — the volume a machine left full of empty files is
#              mounted again, its reserve made again from the ground the halt
#              gave back (meta_pool.h), and not one delete is refused.
#
# The guest says a line each time a file passes 128 MiB and each 10000 files
# (quilltest_room.c), so a machine at work talks and one that hangs goes quiet:
# a leg whose verdict never came is a FAIL — the write or the deletes hung —
# not the harness's.
#
# ‼ THE MUTATION IT EXISTS FOR, MEASURED: the room cycle's close kept off the
# deletes' Spare — settle.c SettleTake `bool dig = why == SETTLE_FOR_HALT ||
# (room && FallowRestsOnTail(PoolClosedGenMax()));` cut back to `why ==
# SETTLE_FOR_HALT`, as the tree stood before this scenario. The fill spends
# the Glean's Spare and a full volume cannot make it again ("could make the
# pool only 1 of the 2 Spare(s)"), the delete's Quitclaim stays in the open
# tail, Fallow never lets its GiB go, and the write waits through cycle after
# cycle — each a Ledger write and a flush, thousands a second — and never
# answers: roomback goes quiet after "the volume is full", 1 failed — "the
# machine went quiet without roomback's verdict: the write of a GiB never
# answered (last: [MetaPool] a cycle owes a waiter a closed block and the pool
# holds 1 Spare(s), the last 1 of them the deletes' …)". Left alone, that
# machine wrote 8.3 million 512-byte Ledger sectors, each with its flush, in
# twenty-five minutes, and the write never came back. With the line the write
# lands in ten seconds. (The line is now `bool dig = why == SETTLE_FOR_HALT ||
# GleanMovedGen() > PoolClosedGenMax() || (room && FallowRestsOnTail(...))`:
# the mutation is the last term cut, as it was then.)
#
# ‼ AND THE ONE EMPTYFULL EXISTS FOR (E1), MEASURED. The deletes of empty
# files free no ground, so once the tail and the deletes' share of the reserve
# hold their Quitclaims, the room for the next is Glean's to make (glean.h):
# on the tree before the fix every delete past the 194th was refused
# ERR_TAGFS_NO_SPACE (852), the delete of the file that filled the volume
# among them — "100000 of 100000 deletes answered in 1268 ms, 99806 refused
# (the first, index 194, with -852), and _qrf.bin's after them answered -852".
# The sweep a delete waits on taken out — ink.c InkQuitclaim's
# `if (rc == ERR_TAGFS_NO_SPACE && GleanSweepAsk())` made `&& false` — and
# emptyfull is red again: "100000 of 100000 deletes answered in 1253 ms, 99903
# refused (the first, index 97, with -852), and _qrf.bin's after them
# answered 0" — the delete that frees ground still goes, past the close its
# ground needs. (stand_room_empty_deletes_on_a_full_volume turns red on the
# same mutation; stand_ink_full_volume_still_deletes on a Quitclaim of an
# empty file let into Glean's share.)
# ---------------------------------------------------------------------------

FULLVOLUME_MIB=1024
FULLVOLUME_STRETCH=2097152   # sectors: a GiB of ground more than the image's
FULLVOLUME_EMPTIES=100000
FULLVOLUME_KEYS="UEFI=on STRICT=on CORES=4 MEM=4G"

# fullvolume_type COMMAND — typed once, and its echo waited for. Not
# type_line: a kernel line said right after the prompt carries the prompt away
# from the echo ("~ [KLIB] …" and then the bare command on a line of its own),
# and a command that runs for minutes in silence leaves type_line no echo it
# knows within its window — it types the command a second time, and the shell
# runs it again after the first. qemu-input.sh has already seen every
# character echoed; what is waited for here is the line.
fullvolume_type() {
    local from
    from=$(wc -l < build/serial.log)
    type_cmd "$1" && sleep 1 && key_press ret &&
        wait_for_line -E build/serial.log "^(~ )?$1" "$TYPE_ECHO_QUIET_S" $((from + 1))
}

# fullvolume_leg TAG STRETCH COMMAND MODE [ON] — a copy of the image, its
# ground stretched by STRETCH sectors when not 0, booted — or, with ON, the
# copy leg ON left, booted again as the halt left it; COMMAND typed and
# MODE's verdict waited for, then bye and the machine left to switch itself
# off; the copy judged by volume-read. Leaves serial.fullvolume.TAG.log,
# fullvolume.TAG.check.json, fullvolume.TAG.list, FULLVOLUME_OFF (0 when the
# machine switched itself off) and FULLVOLUME_SILENT (what never came: the
# verdict, or nothing). 1 when there is no run to judge, said as the
# harness's failure.
fullvolume_leg() {
    local tag=$1 stretch=$2 cmd=$3 mode=$4 on=${5:-} L="$SCRATCH/serial.fullvolume.$1.log" img pid from
    img="$SCRATCH/fullvolume.${on:-$tag}.img"
    FULLVOLUME_OFF=1
    FULLVOLUME_SILENT=""
    rm -f "$L" "$SCRATCH/fullvolume.$tag.check.json" "$SCRATCH/fullvolume.$tag.list"
    make run-stop >/dev/null 2>&1
    if [ -n "$on" ] && [ ! -f "$img" ]; then
        harness "fullvolume: $tag has no copy left by $on to boot again"
        return 1
    fi
    [ -n "$on" ] || cp -c build/boxos.img "$img" 2>/dev/null || cp build/boxos.img "$img"
    if [ "$stretch" != 0 ] && ! grow_stretch "$img" "$stretch" >/dev/null; then
        harness "fullvolume: the $tag copy has no BoxOS partition to stretch"
        rm -f "$img"
        return 1
    fi
    # shellcheck disable=SC2086
    if ! make run-bg BOOTDISK="$img" $FULLVOLUME_KEYS >"$SCRATCH/runbg.fullvolume.$tag.log" 2>&1; then
        harness "fullvolume: no machine was started for $tag — $(tail -1 "$SCRATCH/runbg.fullvolume.$tag.log")"
        rm -f "$img"
        return 1
    fi
    pid=$(cat build/qemu.pid)
    if ! { wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" &&
           wait_for_quiet "$BOOT_BURST_QUIET_S" && fullvolume_type "$cmd"; }; then
        cp build/serial.log "$L"
        make run-stop >/dev/null 2>&1
        harness "fullvolume: $tag could not be put to the machine (last: $(tr -d '\r' < "$L" | grep -aE '^\[(QUILL|TagFS)\]' | tail -1 | cut -c1-120))"
        rm -f "$img"
        return 1
    fi
    if wait_for_line -E build/serial.log "\[QUILL\] $mode: (PASS|FAIL) — " "$WAIT_QUIET_S"; then
        from=$(wc -l < build/serial.log)
        wait_for_quiet "$BOOT_BURST_QUIET_S"
        fullvolume_type bye && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" $((from + 1))
        wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
        FULLVOLUME_OFF=$?
    else
        FULLVOLUME_SILENT="$mode's verdict"
    fi
    cp build/serial.log "$L"
    make run-stop >/dev/null 2>&1
    python3 tools/volume-read.py "$img" check --json >"$SCRATCH/fullvolume.$tag.check.json" 2>/dev/null
    python3 tools/volume-read.py "$img" list >"$SCRATCH/fullvolume.$tag.list" 2>&1
    return 0
}

# fullvolume_census TAG — volume-read's roll call of the copy the leg left:
# consistent, every finding a note, or a FAIL with its words.
fullvolume_census() {
    local said
    said=$(python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except (OSError, ValueError) as e:
    print("no roll call: %s" % e); sys.exit(1)
bad = [f for f in d.get("findings", []) if f.get("severity") != "note"]
if bad:
    print("; ".join("%s %s: %s" % (f.get("severity"), f.get("code"), f.get("text")) for f in bad[:3])); sys.exit(1)
print("%s, %d note(s)" % (d.get("verdict"), len(d.get("findings", []))))
' "$SCRATCH/fullvolume.$1.check.json")
    chk $? "fullvolume: $1 — volume-read finds the volume the halt left consistent ($(printf '%s' "$said" | cut -c1-300))"
}

# fullvolume_halted TAG [OFF] — the machine halted through bye, its sync wrote
# the volume, and nothing went wrong on the way; OFF is the leg's
# FULLVOLUME_OFF, the last leg's when not given.
fullvolume_halted() {
    local L="$SCRATCH/serial.fullvolume.$1.log" off=${2:-$FULLVOLUME_OFF} fault
    [ "$off" = 0 ] && grep -aq '^\[HALT\] Storage sync complete' "$L"
    chk $? "fullvolume: $1 — the machine halted through bye and its sync wrote the volume ($(grep -aE "$STAND_HALT_ENDS" "$L" | tr -d '\r' | tail -1 | cut -c1-100))"
    fault=$(grep -aE 'KERNEL PANIC|\[EXCEPTION\]|^\[NIGHTWATCH\] VERDICT: .* a defect' "$L" | tr -d '\r' | head -1 | cut -c1-160)
    [ -z "$fault" ]
    chk $? "fullvolume: $1 — no panic, no exception, nothing owed proved unserved${fault:+: $fault}"
}

# fullvolume_roomback_medium — off the copy: no _qra.bin, and _qrc.bin of
# FULLVOLUME_MIB MiB whose every block begins with its own stream.
fullvolume_roomback_medium() {
    local img="$SCRATCH/fullvolume.room.img" out="$SCRATCH/fullvolume.qrc.bin" rc
    ! grep -qE '^ +_qra\.bin ' "$SCRATCH/fullvolume.room.list"
    chk $? "fullvolume: roomback — volume-read finds no _qra.bin on the medium"
    rm -f "$out"
    python3 tools/volume-read.py "$img" get _qrc.bin -o "$out" >/dev/null 2>&1
    python3 - "$out" "$((FULLVOLUME_MIB << 20))" >"$out.said" 2>&1 <<'FULLVOLUME_EOF'
import struct, sys
path, want = sys.argv[1], int(sys.argv[2])
M = (1 << 64) - 1
def word(seed, off):     # quilltest.c QuillWord, the stream QuillFill lays
    x = (off ^ ((seed << 56) & M)) & M
    x = (x * 0x9E3779B97F4A7C15) & M
    return x ^ (x >> 29)
try:
    f = open(path, "rb")
except OSError:
    sys.exit("volume-read found no _qrc.bin on the medium")
at = 0
while True:
    b = f.read(4096)
    if not b:
        break
    if len(b) < 8 or struct.unpack_from("<Q", b, 0)[0] != word(0xC0, at):
        sys.exit("its block at byte %d is not its own stream" % at)
    at += len(b)
if at != want:
    sys.exit("it holds %d bytes, not %d" % (at, want))
print("%d bytes, every one of its %d blocks beginning with its own stream" % (at, at // 4096))
FULLVOLUME_EOF
    rc=$?
    chk $rc "fullvolume: roomback — _qrc.bin read off the medium by volume-read: $(tail -1 "$out.said")"
    rm -f "$out" "$out.said"
}

fullvolume_judge_roomback() {
    local L="$SCRATCH/serial.fullvolume.room.log" full verdict last
    full=$(tr -d '\r' < "$L" | grep -aoE '\[QUILL\] roomback: the volume is full — .*' | tail -1)
    verdict=$(tr -d '\r' < "$L" | grep -aoE '\[QUILL\] roomback: (PASS|FAIL) — .*' | tail -1)
    if [ -z "$full" ]; then
        harness "fullvolume: roomback never filled the volume, so the case was not asked (${verdict:-no verdict at all})"
        return
    fi
    ok "fullvolume: roomback filled the volume to its no-space — ${full#*— }"
    if [ -n "$FULLVOLUME_SILENT" ]; then
        last=$(tr -d '\r' < "$L" | grep -aE '^\[(QUILL|TagFS|Settle|MetaPool|Glean)\]' | tail -1 | cut -c1-160)
        bad "fullvolume: roomback — _qra.bin deleted, and the machine went quiet without $FULLVOLUME_SILENT: the write of a GiB never answered (last: $last)"
        return
    fi
    grep -aq '^\[QUILL\] roomback: PASS' "$L"
    chk $? "fullvolume: roomback — a GiB written the moment a GiB was deleted got its room, every byte, and read back whole (${verdict#*— })"
    fullvolume_halted room
    fullvolume_census room
    fullvolume_roomback_medium
}

fullvolume_judge_emptyfull() {
    local L="$SCRATCH/serial.fullvolume.empty.log" full verdict last left
    full=$(tr -d '\r' < "$L" | grep -aoE '\[QUILL\] emptyfull: the volume is full — .*' | tail -1)
    verdict=$(tr -d '\r' < "$L" | grep -aoE '\[QUILL\] emptyfull: (PASS|FAIL) — .*' | tail -1)
    if [ -z "$full" ]; then
        harness "fullvolume: emptyfull never made its files and filled the volume, so the case was not asked (${verdict:-no verdict at all})"
        return
    fi
    ok "fullvolume: emptyfull made its empty files and filled the volume to its no-space — ${full#*— }"
    if [ -n "$FULLVOLUME_SILENT" ]; then
        last=$(tr -d '\r' < "$L" | grep -aE '^\[(QUILL|TagFS|Settle|MetaPool|Glean)\]' | tail -1 | cut -c1-160)
        bad "fullvolume: emptyfull — the machine went quiet without $FULLVOLUME_SILENT: a delete never answered (last: $last)"
        return
    fi
    grep -aq '^\[QUILL\] emptyfull: PASS' "$L"
    chk $? "fullvolume: emptyfull — all $FULLVOLUME_EMPTIES deletes of empty files on the full volume answered OK, and the delete of the file that filled it after them (${verdict#*— })"
    fullvolume_halted empty
    fullvolume_census empty
    left=$(grep -cE '^ +_qre\.bin ' "$SCRATCH/fullvolume.empty.list")
    [ "$left" = 0 ] && ! grep -qE '^ +_qrf\.bin ' "$SCRATCH/fullvolume.empty.list"
    chk $? "fullvolume: emptyfull — volume-read finds $left _qre.bin on the medium, and $(grep -cE '^ +_qrf\.bin ' "$SCRATCH/fullvolume.empty.list") _qrf.bin"
}

# fullvolume_judge_emptyback — the fill's boot left the volume full and
# halted cleanly; the reboot mounted it with every file there and gave all of
# them up, each delete answered OK; neither is on the medium after.
fullvolume_judge_emptyback() {
    local F="$SCRATCH/serial.fullvolume.fill.log" L="$SCRATCH/serial.fullvolume.back.log" full found verdict last left
    full=$(tr -d '\r' < "$F" | grep -aoE '\[QUILL\] emptyfill: the volume is full — .*' | tail -1)
    if [ -z "$full" ] || ! grep -aq '^\[QUILL\] emptyfill: PASS' "$F"; then
        harness "fullvolume: emptyfill never made its files and filled the volume, so emptyback was not asked ($(tr -d '\r' < "$F" | grep -aoE '\[QUILL\] emptyfill: (PASS|FAIL) — .*' | tail -1 | cut -c1-160))"
        return
    fi
    ok "fullvolume: emptyfill made its empty files and filled the volume to its no-space — ${full#*— }"
    fullvolume_halted fill "$FULLVOLUME_FILL_OFF"
    found=$(tr -d '\r' < "$L" | grep -aoE '\[QUILL\] emptyback: the volume mounted again holds .*' | tail -1)
    verdict=$(tr -d '\r' < "$L" | grep -aoE '\[QUILL\] emptyback: (PASS|FAIL) — .*' | tail -1)
    [ -n "$found" ]
    chk $? "fullvolume: emptyback — the volume the halt left full is mounted again with every file emptyfill made (${found:-${verdict:-no word from emptyback}})"
    if [ -n "$FULLVOLUME_SILENT" ]; then
        last=$(tr -d '\r' < "$L" | grep -aE '^\[(QUILL|TagFS|Settle|MetaPool|Glean)\]' | tail -1 | cut -c1-160)
        bad "fullvolume: emptyback — the machine went quiet without $FULLVOLUME_SILENT: a delete never answered (last: $last)"
        return
    fi
    grep -aq '^\[QUILL\] emptyback: PASS' "$L"
    chk $? "fullvolume: emptyback — after the reboot all $FULLVOLUME_EMPTIES deletes of empty files on the full volume answered OK, and the delete of the file that filled it after them (${verdict#*— })"
    fullvolume_halted back
    fullvolume_census back
    left=$(grep -cE '^ +_qre\.bin ' "$SCRATCH/fullvolume.back.list")
    [ "$left" = 0 ] && ! grep -qE '^ +_qrf\.bin ' "$SCRATCH/fullvolume.back.list"
    chk $? "fullvolume: emptyback — volume-read finds $left _qre.bin on the medium, and $(grep -cE '^ +_qrf\.bin ' "$SCRATCH/fullvolume.back.list") _qrf.bin"
}

run_fullvolume() {
    echo "== fullvolume: a full volume gives a GiB deleted to the GiB written after it, and answers every delete of an empty file, before a reboot and after one =="
    build STRICT=on
    fullvolume_leg room "$FULLVOLUME_STRETCH" "quilltest roomback $FULLVOLUME_MIB" roomback &&
        fullvolume_judge_roomback
    fullvolume_leg empty 0 "quilltest emptyfull $FULLVOLUME_EMPTIES" emptyfull &&
        fullvolume_judge_emptyfull
    FULLVOLUME_FILL_OFF=1
    if fullvolume_leg fill 0 "quilltest emptyfill $FULLVOLUME_EMPTIES" emptyfill; then
        FULLVOLUME_FILL_OFF=$FULLVOLUME_OFF
        if grep -aq '^\[QUILL\] emptyfill: PASS' "$SCRATCH/serial.fullvolume.fill.log"; then
            fullvolume_leg back 0 "quilltest emptyback $FULLVOLUME_EMPTIES" emptyback fill
        else
            : >"$SCRATCH/serial.fullvolume.back.log"
        fi
        fullvolume_judge_emptyback
    fi
    lockrank_judge "$SCRATCH/serial.fullvolume.room.log $SCRATCH/serial.fullvolume.empty.log $SCRATCH/serial.fullvolume.fill.log $SCRATCH/serial.fullvolume.back.log" "fullvolume"
    rm -f "$SCRATCH/fullvolume.room.img" "$SCRATCH/fullvolume.empty.img" "$SCRATCH/fullvolume.fill.img"
    build   # leave the tree built plain, as every scenario here does
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
        wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

        local MARK
        MARK=$(wc -l < build/serial.log)
        mon_raw "drive_add 0 if=none,id=gptdisk,file=$img,format=raw"
        sleep 1
        mon_raw "device_add usb-storage,drive=gptdisk,id=gptstick"

        # Waited on the survey having spoken, not on a clock.
        wait_for_line build/serial.log "$wait_for" "$WAIT_QUIET_S" $((MARK + 1))
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

    # The number is ASKED OF THE DISK, not typed. It used to be 122325 here —
    # the last sector of the image as it was the day this was written — and the
    # day the build began sizing the volume from its content, this check failed
    # for a machine that had done exactly the right thing.
    LAST=$(( $(stat -f%z "$SCRATCH/gpt-bad.img" 2>/dev/null || stat -c%s "$SCRATCH/gpt-bad.img") / 512 - 1 ))
    grep -q "reading the copy at the far end, sector $LAST" "$BAD_LOG"
    chk $? "and the far copy is looked for in the last sector of the medium"

    grep -q "the copy at the far end is good" "$BAD_LOG"
    chk $? "and it is read"

    grep -q "from GPT entry" "$BAD_LOG"
    chk $? "and the BoxOS ground on that disk is found after all"

    # And the machine that was handed a damaged table finished the pass it was
    # in rather than stopping inside it — the room says so on its way out.
    grep -q "took a seat while the machine was" "$BAD_LOG"
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
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    local MARK
    MARK=$(wc -l < build/serial.log)

    # Five arrivals and five departures on the second controller. A fresh
    # drive_add each time: HMP marks the drive auto-del, so device_del takes it
    # with it and the next device_add silently does nothing — one cycle
    # pretending to be ten.
    for n in 1 2 3 4 5; do
        mon_raw "drive_add 0 if=none,id=st$n,file=$SCRATCH/stick.img,format=raw"
        sleep 1
        mon_raw "device_add usb-storage,bus=xhci2.0,drive=st$n,id=us"
        sleep 4
        mon_raw "device_del us"
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

    # ── each controller's records are ITS records ──────────────────────────
    #
    # Everything above this in the scenario is the damage the machine-wide
    # table did: one array of sixty-four for every controller, one lock, and a
    # special case in the retirement pass for "somebody else's device" that a
    # live board proved was not enough — fifteen chairs from one flash drive.
    # The records now live on the controller that hands out the slots.
    local ctl rec
    ctl=$(grep -c "controller ready:" build/serial.log)
    rec=$(grep -c "carries .* device record(s) of its own" build/serial.log)
    [ "$ctl" -ge 2 ] && [ "$ctl" = "$rec" ]
    chk $? "every controller carries records of its own ($rec of $ctl)"

    # And as many as it said it can address. ‼ This cannot fail here: QEMU's
    # xHCI has no way to report anything but sixty-four slots, so a kernel that
    # went back to one machine-wide 64 would satisfy it too. It bites on a
    # board whose controller says something else — which is the only place the
    # question was ever open.
    local mismatched
    mismatched=$(awk '
        /controller ready:/ { match($0, /([0-9]+) slot\(s\)/, m); want = m[1] }
        /carries [0-9]+ device record/ { match($0, /carries ([0-9]+) device/, g);
            if (want != "" && g[1] != want) n++ }
        END { print n+0 }' build/serial.log 2>/dev/null || echo 0)
    [ "$mismatched" = 0 ]
    chk $? "and as many of them as it said it can address ($mismatched wrong)"
}

# ── a pass that did not survive the journey ────────────────────────────────
#
# The seal exists for one case that cannot be staged any other way: the block
# still carries a valid magic, states lengths that fit, and walks cleanly —
# and is not what the loader wrote. Firmware handing the page to something
# else between the write and ExitBootServices produces exactly that, and so
# does a loader that died halfway.
#
# So the mutation corrupts the pass AFTER it has been sealed. One byte, inside
# the span the seal covers, in the stamp the kernel leans on hardest.
seal_on() {
    mutate src/boot/stage2/stage2.asm
    python3 - <<'EOF'
p = "src/boot/stage2/stage2.asm"
s = open(p).read()
anchor = "    mov [BOARDING_PASS_ADDR+68], eax\n    ret\n"
assert anchor in s, "seal anchor missing"
s = s.replace(anchor,
              "    mov [BOARDING_PASS_ADDR+68], eax\n"
              "    mov byte [BOARDING_PASS_ADDR+41], 0x7F   ; logcheck mutation: the block rots after it was sealed\n"
              "    ret\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/boot/stage2/stage2.asm "logcheck mutation"
}
seal_off() {
    restore src/boot/stage2/stage2.asm
}

# ── a pass from a loader newer than this kernel ────────────────────────────
#
# The other half of the same question: what may be believed off the block.
# The version used to throw the WHOLE pass away on any number the kernel had
# not been compiled against — volume identity and all — which is the version
# ladder boarding_pass.h promises not to build. The stamps are self-describing
# and the header is frozen, so a later loader is readable.
passver_on() {
    mutate src/boot/stage2/stage2.asm
    python3 - <<'EOF'
p = "src/boot/stage2/stage2.asm"
s = open(p).read()
anchor = "    mov word  [BOARDING_PASS_ADDR+4],   BOARDING_PASS_VERSION   ; +4  version\n"
assert anchor in s, "pass-version anchor missing"
s = s.replace(anchor,
              "    mov word  [BOARDING_PASS_ADDR+4],   2   ; logcheck mutation: a loader newer than this kernel\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/boot/stage2/stage2.asm "logcheck mutation"
}
passver_off() {
    restore src/boot/stage2/stage2.asm
}

run_seal() {
    echo "== seal: the pass was written, then something walked over it =="
    seal_on; build
    boot seal.broken
    seal_off
    local L="$SCRATCH/serial.seal.broken.log"

    # The kernel CAUGHT it, and said both sums rather than "bad pass".
    grep -qE "the seal on the pass is broken: [0-9]+ bytes sum to 0x[0-9a-f]+ and the loader wrote 0x[0-9a-f]+" "$L"
    chk $? "the broken seal was caught, and both sums named"

    # And having caught it, refused the block ENTIRELY. A pass that does not
    # add up must not have some of its stamps believed.
    grep -q "the loader left no pass" "$L"
    chk $? "and the whole pass was refused, not part of it"

    ! grep -q "read out of the volume" "$L"
    chk $? "no volume identity was taken off a block that did not add up"

    # And the room does not go on crediting a loader whose word it threw
    # away. A healthy boot prints "the loader said so on its boarding pass"
    # over the seat it mounts; after a refused pass that sentence must be
    # gone, because nothing on the block is usable — not the volume id, and
    # not the confidence. This is the check that would catch a kernel which
    # rejects the pass and then quietly uses a value it took off it anyway.
    ! grep -q "the loader said so on its boarding pass" "$L"
    chk $? "the room does not credit a pass it refused"

    # It still found the volume — by its own means, which is the arrangement
    # the pass was only ever an improvement on.
    grep -qE '^\[TagFS\] volume on seat [0-9]+' "$L"
    chk $? "and found the volume anyway, without being told"

    # ‼ AND THE MACHINE STILL BOOTS. A corrupted pass is not a reason to
    # refuse a machine: the pass is an optimisation over a rule that works.
    grep -q "BoxOS Shell" "$L";  chk $? "the machine still reaches a shell"
    typed_ok "$L";               chk $? "and still answers a keystroke"

    # ── and now a pass this kernel is too old to have heard of ─────────────
    passver_on; build
    boot seal.newer
    passver_off
    local N="$SCRATCH/serial.seal.newer.log"

    grep -q "the pass is version 2 and this kernel was built for 1" "$N"
    chk $? "a newer pass is noticed and named"

    # THE point. The stamps are self-describing and the header is frozen, so
    # everything this kernel knows how to read is still there to be read. The
    # old rule discarded the block whole and sent the room back to guessing.
    grep -q "read out of the volume" "$N"
    chk $? "and its volume identity is still read off it"
    grep -q "the loader said so on its boarding pass" "$N"
    chk $? "and the room is told which seat, by a loader it does not know"
    grep -q "sealed, and the seal agrees" "$N"
    chk $? "and the seal on it still checks out"
}

# ── the one control transfer nobody can answer ─────────────────────────────
#
# xhci_ep_wait gives up when the device has gone, when the pipe has broken, or
# — last — when the time ran out, and on every one of those it has to take the
# transfer back OFF the endpoint. Under emulation none of the three happens on
# the control pipe: the emulated device answers everything immediately. That
# was measured six ways before this scenario was written (a zero budget, yank,
# stillthere, usbrecover, a hot-removed hub, an ordinary boot) and the path was
# reached by none of them.
#
# So the kernel runs it deliberately, once, in a build made with CTRLGIVEUP=on:
# a control transfer posted with the doorbell withheld, which is a faithful
# "no answer came" rather than a simulated one. The proof is the transfer
# AFTER it — see xhci_ctrl_giveup_proof.
run_enumsilence() {
    echo "== enumsilence: a pipe that never spoke is answered by the port, once =="

    # ‼ WHY THIS SCENARIO HAS TO EXIST.
    #
    # The eight seconds the owner's two machines spent on every boot happened on
    # a road emulation never takes: under QEMU every control request is answered
    # immediately, so a device going silent at its first descriptor request
    # cannot occur. Eight scenarios were green while that road was broken, and
    # the only evidence for it was a photograph of a screen. ENUMSILENCE=on
    # withholds the doorbell for ONE descriptor request, once — the exact state
    # both boards reported: endpoint Running, dequeue pointer on the first TRB,
    # no event ever raised.
    build_with_key ENUMSILENCE=on
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi

    # Four cores, for the reason latearrival and replug give: the deferred work
    # of this kernel runs from the K-Core guide loop and from cpu_idle, and on
    # one core neither runs. USB=on gives the machine a keyboard, which is the
    # device the injection will land on — deliberately, because the owner's
    # desktop has exactly one input device and the whole risk of this change is
    # whether a silenced pipe costs it.
    make run-stop >/dev/null 2>&1
    make run-bg ENUMSILENCE=on USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    type_cmd "help"
    sleep 1
    key_press ret
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.enumsilence.log"
    buildkey_restore
    build
    L="$SCRATCH/serial.enumsilence.log"

    grep -q "ENUMSILENCE — the doorbell for the first descriptor request" "$L"
    chk $? "the silence was actually made (the scenario is not vacuous)"

    # THE POINT. A pipe that never spoke gets ONE give-up and then the remedy
    # that works. Before this change it got the give-up four times over — 2000 ms
    # each — and only then the port reset.
    local ladder
    ladder=$(grep -c "taking the transfer back and asking again" "$L")
    [ "$ladder" = 0 ]
    chk $? "the silence ladder was not entered at all (entered $ladder time(s))"

    local gaveup
    gaveup=$(grep -c "of silence" "$L")
    [ "$gaveup" = 1 ]
    chk $? "one give-up, not a ladder of them (counted $gaveup)"

    grep -q "trying again on a freshly reset port (attempt 1," "$L"
    chk $? "and the port was reset instead, on the first attempt"

    # ‼ AND THE WAIT ENDED ON THE CONTROLLER'S OWN FACTS, NOT ON A CLOCK.
    # GateClock: the endpoint Running, the dequeue pointer where it stood at the
    # doorbell, and nothing ever answered. Under the injection all three are
    # true from the first microframe, which is exactly what the boards showed.
    grep -q "the controller never began it" "$L"
    chk $? "the give-up named the controller, not the clock"

    # And it was fast. The old road spent XHCI_ENUM_TIMEOUT_MS = 2000 ms here;
    # GateClock is 64 microframes = 8 ms, seen by a 250 Hz tick, so anything
    # under half a second proves the clock is no longer what decides.
    local gms
    gms=$(sed -n 's/.*gave up after \([0-9]*\) ms of silence.*/\1/p' "$L" | head -1)
    [ -n "$gms" ] && [ "$gms" -lt 500 ]
    chk $? "and it was decided in ${gms:-?} ms, not in the two-second budget"

    # The report tells the two counts apart now: everything that happened on the
    # pipe, and how much of it came from the device. A silenced pipe must read 0.
    grep -q "transfer event(s) ever raised on it 0 came from the device" "$L"
    chk $? "the report says the device itself answered nothing"

    grep -q "BoxOS Shell" "$L"
    chk $? "the machine still reaches a shell"

    typed_ok "$L"
    chk $? "and the device that was silenced came back — the keystroke landed"
}

run_ctrlgiveup() {
    echo "== ctrlgiveup: a control transfer is given up on, and taken back =="

    make CTRLGIVEUP=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi
    cp build/boxos.img "$SCRATCH/stick.img"

    # Four cores, for the reason latearrival and replug give: the deferred work
    # of this kernel runs from the K-Core guide loop and from cpu_idle, and on
    # one core neither runs.
    make run-stop >/dev/null 2>&1
    make run-bg CTRLGIVEUP=on USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2

    # USB=on gives this machine a KEYBOARD, not a disk. The proof runs on the
    # first mass-storage device the kernel configures, so one is plugged in —
    # measured: without this the proof never runs and the scenario is vacuous.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "xHCI PROOF. the next control transfer" "$WAIT_QUIET_S"
    sleep 2

    type_cmd "help"
    sleep 1; key_press ret; sleep 3

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.ctrlgiveup.log"
    local L="$SCRATCH/serial.ctrlgiveup.log"

    # It ran at all. The first version of this proof could decline in silence,
    # which reads exactly like a proof that passed.
    grep -q "a control transfer is posted and not rung for" "$L"
    chk $? "the unanswerable transfer was posted"
    ! grep -q "the give-up proof did NOT run" "$L"
    chk $? "and the proof did not decline"

    # The wait ended on the clock — the last question it asks, and the only one
    # that can be reached here — and it NAMED that.
    grep -q "it has not answered in the time it was given — taking the transfer back" "$L"
    chk $? "the wait gave up and said which of its questions ended it"

    # THE repair: the transfer comes off the endpoint at the controller, not
    # just out of the driver's mind.
    grep -qE "slot [0-9]+ endpoint 1: the transfer on it is no longer wanted — stopping it" "$L"
    chk $? "and the controller was told to stop it"

    grep -q "the wait ended with -1" "$L"
    chk $? "the caller was told the transfer failed"

    # ‼ AND THIS IS THE PROOF. The next question down the same pipe must get
    # ITS OWN answer. Left where it was, the abandoned Setup runs first when
    # the doorbell next rings, and its event is accepted for the new transfer
    # because a control transfer sets xfer_trb_phys to zero and that turns the
    # TRB match off — so the buffer holds a DEVICE descriptor (type 1) where
    # the caller asked for a CONFIGURATION one (type 2), and cannot tell.
    grep -q "the next control transfer asked something the device answers and got its own answer" "$L"
    chk $? "the next control transfer got its own answer, not the abandoned one"

    # And the device carried on being a disk afterwards.
    grep -qE "\[USB disk [0-9]+\] .* sectors, .* MiB" "$L"
    chk $? "the disk still enumerated after its control pipe was given up on"
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still has a shell"
    typed_ok "$L";              chk $? "and still answers a keystroke"
}

# ── a device this kernel has no use for, and says so ───────────────────────
#
# enum_pick_visit answers bulk and interrupt and lets everything else fall
# through its default, so a headset or a camera used to be addressed,
# configured and settled exactly like a device with nothing on it. "The machine
# saw my microphone and did nothing" is a different fault from "the machine did
# not see my microphone", and only one of them was readable off a screen.
run_isoch() {
    echo "== isoch: a device whose useful half this kernel does not configure =="
    build
    make run-stop >/dev/null 2>&1
    make run-bg USB=on ISOCH=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    type_cmd "help"
    sleep 1; key_press ret; sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.isoch.log"
    local L="$SCRATCH/serial.isoch.log"

    # The device got as far as being configured at all.
    grep -qE "port [0-9]+: .*device 46f4:0002 class 01/01/00 configured, no driver claims it" "$L"
    chk $? "the audio device was addressed, configured and settled"

    # ‼ AND THE PART THIS KERNEL DROPS IS NAMED, with a count that came from
    # the descriptor rather than from a guess.
    grep -qE "port [0-9]+: it has [1-9][0-9]* isochronous endpoint\(s\), and this kernel configures none" "$L"
    chk $? "and the isochronous endpoints it has are named, not dropped in silence"

    # A device nobody drives must not cost the machine anything.
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still reaches a shell"
    typed_ok "$L";              chk $? "and still answers a keystroke"
    grep -q "endpoint-context self-test 15/15" "$L"
    chk $? "and the endpoint-context fields still agree with the specification"
}

# ===========================================================================
# sleeps — the box with nothing to do costs nothing, and still hears
# ===========================================================================
#
# Two questions, and they are opposite halves of the same change. Sleeping is
# easy on its own: stop looking, and a machine goes quiet for ever. Hearing is
# easy on its own: never stop looking, which is what this box did, at a hundred
# percent of a core with an empty screen. Only together are they worth
# anything, and only together can a mutation to either one be seen.
#
#   IT SLEEPS   — at an idle prompt, the emulator's own CPU time barely moves.
#                 Measured on the HOST, deliberately: the guest cannot be asked
#                 whether it is really idle, because the loop that would answer
#                 is the loop that would be burning the core.
#
#   IT HEARS    — quietprint writes one line into its console lane and then
#                 says nothing at all for three seconds. Nothing else happens
#                 in that window: no key, no message, no death. The line must
#                 be on screen BEFORE the program has finished, and the only
#                 thing that can put it there is the bell — the sleeping
#                 daemon's pid, hung on the lane, taken and rung by the writer.
#                 Without it the line still arrives, but only when the process
#                 DIES, because a death is a Touch and a Touch wakes the daemon.
#                 So the check is on WHEN, never on whether.
#
# Single core on purpose. It is the configuration where a spinning strand is
# unmistakable — there is one core, and either it is idle or it is not.

QUIET_SETTLE_S=6      # long enough for boot chatter to stop
QUIET_SAMPLE_S=8      # window the host CPU is measured over

# Host CPU time (in centiseconds) that the QEMU process has used so far.
qemu_cpu_cs() {
    local pid t
    pid=$(cat build/qemu.pid 2>/dev/null) || return 1
    [ -n "$pid" ] || return 1
    t=$(ps -o time= -p "$pid" 2>/dev/null | tr -d ' ') || return 1
    [ -n "$t" ] || return 1
    # ps prints [[hh:]mm:]ss[.cc]
    printf '%s\n' "$t" | awk -F: '
        { n=NF; sec=$n; m=(n>1)?$(n-1):0; h=(n>2)?$(n-2):0;
          printf "%d\n", (h*3600 + m*60 + sec) * 100 }'
}

run_sleeps() {
    echo "== sleeps: an idle box costs nothing, and a sleeping console still hears =="
    build STRICT=on

    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null || { bad "sleeps: never reached a shell"; make run-stop >/dev/null 2>&1; return; }

    # ── IT SLEEPS ───────────────────────────────────────────────────────────
    sleep $QUIET_SETTLE_S
    local A B USED
    A=$(qemu_cpu_cs); sleep $QUIET_SAMPLE_S; B=$(qemu_cpu_cs)
    USED=$(( (B - A) * 100 / (QUIET_SAMPLE_S * 100) ))   # percent of one core
    echo "     idle: ${USED}% of one core over ${QUIET_SAMPLE_S}s (was ~100% before Turn In)"

    # 25% is not a target, it is a LINE. A box that turns in measures single
    # digits and a box that spins measures a hundred; nothing lands in between,
    # so the threshold only has to be far from both to be immune to a loaded
    # build host.
    if [ "$USED" -lt 25 ]; then ok "sleeps: idle box costs ${USED}% of a core"
    else                        bad "sleeps: idle box costs ${USED}% of a core — it never turned in"; fi

    # ── IT HEARS ────────────────────────────────────────────────────────────
    local MARK EARLY LATE
    MARK=$(wc -l < build/serial.log)
    type_cmd "quietprint"
    sleep 1
    key_press ret

    # quietprint: line 1, three seconds asleep in brook_pop, line 2, three more
    # in a touch park, done. Measure INSIDE the first silence — that window has
    # a strand blocked in brook_pop, a sibling parked, and a shell and a daemon
    # turned in, so a box that is not idle here has a waiter that spins.
    sleep 1
    local WA WB WUSED
    WA=$(qemu_cpu_cs); sleep 2; WB=$(qemu_cpu_cs)
    WUSED=$(( (WB - WA) * 100 / 200 ))
    echo "     with a strand blocked in brook_pop: ${WUSED}% of one core"
    if [ "$WUSED" -lt 25 ]; then ok "sleeps: a blocked brook_pop costs ${WUSED}% of a core"
    else                        bad "sleeps: a blocked brook_pop costs ${WUSED}% of a core — the reader is spinning"; fi

    # Look at three seconds — still inside the FIRST silence. Line one must
    # already be there and the program must still be running.
    EARLY=$(tail -n +$((MARK + 1)) build/serial.log)
    printf '%s\n' "$EARLY" > "$SCRATCH/serial.sleeps.early.log"

    if printf '%s' "$EARLY" | grep -q "\[QP\] line 1"; then
        ok "sleeps: the line reached the screen while the writer was still quiet"
    else
        bad "sleeps: line 1 had not been rendered two seconds in — the bell never rang"
    fi
    if printf '%s' "$EARLY" | grep -q "\[QP\] done"; then
        bad "sleeps: quietprint had already finished — the window proves nothing, fix the timing"
    else
        ok "sleeps: the window was real — the writer had not finished yet"
    fi

    # And it must all still arrive, in order, and the silences must be the
    # length they were asked for — read off the guest's OWN timestamps below,
    # never off this script's clock, which is why waiting for the program to
    # say it is done costs the measurement nothing.
    wait_for_line build/serial.log "\[QP\] done" "$WAIT_QUIET_S" $((MARK + 1))
    LATE=$(tail -n +$((MARK + 1)) build/serial.log)
    printf '%s\n' "$LATE" > "$SCRATCH/serial.sleeps.log"
    make run-stop >/dev/null 2>&1

    printf '%s' "$LATE" | grep -q "\[QP\] done" \
        && ok "sleeps: quietprint ran to the end" \
        || bad "sleeps: quietprint never finished"

    printf '%s' "$LATE" | grep -q "\[QP\] line 2 at .* (brook)" \
        && ok "sleeps: the first silence really was a blocked brook_pop" \
        || bad "sleeps: quietprint fell back to a touch park — the brook sleep was not exercised"

    local T2
    T2=$(printf '%s' "$LATE" | sed -n 's/.*\[QP\] line 2 at \([0-9]*\) ms.*/\1/p' | head -1)
    if [ -n "$T2" ] && [ "$T2" -ge 2900 ] && [ "$T2" -le 4200 ]; then
        ok "sleeps: three seconds of silence took ${T2} ms"
    else
        bad "sleeps: three seconds of silence took ${T2:-?} ms — a wait is counting its deadline twice"
    fi
}

# The mutation, and it has to be the RING rather than the bell itself: a bell
# nobody hangs out is also a bell nobody rings, and Nightwatch would then have
# nothing to describe. Leaving the sign up and refusing to pull the cord is the
# exact defect the check exists for, and it is the one that leaves evidence.
# BOTH ringers, and both on purpose. Silencing only the peer's leaves the
# kernel's departure ring to rescue the sleeper the moment the other end lets
# go — measured: the staged wedge healed itself in six seconds and there was
# nothing left to observe. A bell that nobody rings, from anywhere, is the
# defect this is about.
bell_off() {
    mutate src/userspace/boxlib/src/brook.c
    mutate src/kernel/core/brook/brook.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/brook.c"
s = open(p).read()
anchor = "    if (!brook_cas_u32(bell, who, who | BROOK_BELL_RUNG)) return;"
assert anchor in s, "bell mutation anchor missing"
s = s.replace(anchor, anchor + "\n    return;   /* logcheck mutation: the bell is never rung */", 1)
open(p, "w").write(s)

p = "src/kernel/core/brook/brook.c"
s = open(p).read()
anchor = "    if (who == 0) return;\n    process_t *target = process_find_ref(who);"
assert anchor in s, "kernel bell mutation anchor missing"
s = s.replace(anchor, "    return;   /* logcheck mutation: the kernel never rings either */\n" + anchor, 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/brook.c "logcheck mutation"
    mut_done src/kernel/core/brook/brook.c "logcheck mutation"
}

bell_restore() {
    restore src/userspace/boxlib/src/brook.c
    restore src/kernel/core/brook/brook.c
}

# ── kcoreclaim ───────────────────────────────────────────────────────────────
#
# A K-Core's queue is taken in two steps: the producer CLAIMS a position (the
# CAS on `tail`) and PUBLISHES it afterwards (the slot store). The consumer
# that stood in kcore_queue_pop read the tail as well as the slot, and when the
# tail said "claimed" while the slot still said "not yet" it waited a million
# spins and then moved head past the claim — taking the position from under a
# producer that was still walking up to it. The pocket was published behind
# the head, where nobody would look; its owner kept kcore_pending set, so no
# notify of its own could ring the bell again; and it waited for an answer to a
# question nobody would open. The warning was debug_printf, which is nothing.
#
# On the board the gap between claim and publish holds an SMI; on this stand a
# descheduled vCPU. Neither is rare enough to wait for, so the gap is WIDENED
# from both sides: every submit by an APPLICATION (pid >= 3) stands claimed and
# unpublished for two milliseconds — the display daemon and the shell are left
# alone, so the machine stays typeable and the harness can drive it — and the
# K-Cores never sleep, so the consumer is always awake to meet the gap. Without
# the second half the meeting is a coin toss: a K-Core that has already parked
# in HLT is woken by the IPI that follows the publish and never sees the claim
# in flight, and the loss struck the first bench in one run and the third in
# the next. Under the fix — a consumer that judges only the slot and never
# moves head past a claim — bench completes three runs; under the impatient
# consumer (put back by kcoreclaimmut) the first gap it meets loses a submit,
# the machine stops, and Nightwatch names it: POCKET UNSERVED. The completion
# row (the storage benchmark, the last one bench prints) is what is counted:
# the PING row comes early in a run, and a machine that stops after it has
# still stopped.
kcoreclaim_window_on() {
    mutate src/kernel/arch/x86-64/amp/kcore.c
    python3 - <<'EOF'
p = "src/kernel/arch/x86-64/amp/kcore.c"
s = open(p).read()
anchor = """    atomic_fetch_add_u32(&q->count, 1);
    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);"""
assert anchor in s, "kcoreclaim window anchor missing"
widened = """    atomic_fetch_add_u32(&q->count, 1);
    if (proc->pid >= 3) delay(2);   /* logcheck mutation: an application's claim stands unpublished for 2 ms */
    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);"""
s = s.replace(anchor, widened, 1)
gate = """            __asm__ volatile("sti; hlt");   /* atomic arm-and-sleep */"""
assert gate in s, "kcoreclaim sleep-gate anchor missing"
awake = """            __asm__ volatile("sti");        /* logcheck mutation: the K-Core never sleeps, so every claim is met awake */"""
s = s.replace(gate, awake, 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/arch/x86-64/amp/kcore.c "logcheck mutation" 2
}

# Restores the whole file, so it takes the impatient consumer out as well.
kcoreclaim_window_off() {
    restore src/kernel/arch/x86-64/amp/kcore.c
}

# The consumer that stood here before: it reads the tail, waits on a claimed
# slot, and when its patience runs out moves head past the claim. Its patience
# is a thousand spins instead of the million that stood in the tree, so that
# the two-millisecond gap always outlasts it on this stand — the shape is the
# defect, not the number: any patience loses to a producer delayed longer, and
# an SMI is not consulted about how long it may take.
# ‼ THERE IS NO `mutate` HERE, AND THAT IS THE SHAPE, NOT AN OMISSION.
#
# This is the SECOND edit to kcore.c: run_kcoreclaimmut installs the claim
# window first and then stacks the impatient consumer on top of it, so the file
# is already under the manifest and `mutate` would — rightly — refuse it. The
# pristine copy taken by the FIRST mutation is the pristine copy of the file,
# full stop, and restoring it undoes both edits at once. The alternative would
# be holding two "pristine" copies of one file, the second of which is a
# mutated file saved as its own original: precisely the bug this mechanism
# exists to make impossible. `mut_done` is still called, because the second
# edit has to be proved to have landed exactly like the first.
impatient_on() {
    python3 - <<'EOF'
p = "src/kernel/arch/x86-64/amp/kcore.c"
s = open(p).read()
anchor = """    uint32_t h = q->head;
    struct process_t* proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
    if (!proc) return NULL;"""
assert anchor in s, "impatient anchor missing"
impatient = """    uint32_t h = q->head;
    /* logcheck mutation: the impatient consumer — waits on a claim, then moves head past it */
    struct process_t* proc;
    uint32_t patience = 0;
    for (;;) {
        proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
        if (proc != NULL) break;
        if (h == atomic_load_u32(&q->tail)) return NULL;
        cpu_pause();
        if (++patience > 1000u) {
            q->slots[h] = NULL;
            q->head = (h + 1) & KCORE_QUEUE_MASK;
            atomic_fetch_sub_u32(&q->count, 1);
            return NULL;
        }
    }"""
open(p, "w").write(s.replace(anchor, impatient, 1))
EOF
    mut_done src/kernel/arch/x86-64/amp/kcore.c "logcheck mutation: the impatient consumer"
}

# Sixteen cores, bench three times over. Each run is waited out to the row
# AFTER the one at risk (the storage row is the last one bench prints), or to
# Nightwatch speaking — whichever comes first. A verdict ends the whole thing:
# the shell is parked on a bench that will never return, so nothing typed after
# it would run.
claim_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local run=0
    while [ $run -lt 3 ]; do
        local MARK
        MARK=$(wc -l < build/serial.log)
        # The typer's verdict is read, not thrown away: a line the guest did not
        # take must stop the run and say so — pressing Enter on it would run
        # something else and blame the OS (the stress matrix learned this once).
        if ! type_cmd "bench"; then break; fi
        sleep 1
        key_press ret
        wait_until "$WAIT_QUIET_S" "a bench row or a verdict" \
            'tail -n +$((MARK + 1)) build/serial.log | grep -q "create+write64+delete" ||
              grep -qE "POCKET UNSERVED|ANSWER OWED" build/serial.log'
        grep -qE "POCKET UNSERVED|ANSWER OWED" build/serial.log && break
        run=$((run+1))
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

run_kcoreclaim() {
    echo "== kcoreclaim: a claim stands unpublished for 2 ms and the K-Core must wait for it =="
    kcoreclaim_window_on; build STRICT=on
    claim_boot kcoreclaim
    kcoreclaim_window_off
    L="$SCRATCH/serial.kcoreclaim.log"

    local rows
    rows=$(grep -c "create+write64+delete" "$L")
    [ "$rows" -ge 3 ]
    chk $? "kcoreclaim: bench completed three runs with every application claim standing unpublished for 2 ms before an awake K-Core (rows=$rows)"
    ! grep -q "POCKET UNSERVED" "$L"; chk $? "kcoreclaim: and no pocket went unserved"
    ! grep -q "ANSWER OWED" "$L";     chk $? "kcoreclaim: and no answer was owed"
}

# The oracle measured against itself: with the impatient consumer back, the
# same run must STOP, and Nightwatch must be the one to say why.
run_kcoreclaimmut() {
    echo "== kcoreclaimmut: put the impatient consumer back and require the machine to stop =="
    kcoreclaim_window_on; impatient_on; build STRICT=on
    claim_boot kcoreclaimmut
    kcoreclaim_window_off
    L="$SCRATCH/serial.kcoreclaimmut.log"

    grep -q "POCKET UNSERVED" "$L"
    chk $? "kcoreclaimmut: with the impatient consumer back, Nightwatch named the lost submit — POCKET UNSERVED"
    local rows
    rows=$(grep -c "create+write64+delete" "$L")
    [ "$rows" -lt 3 ]
    chk $? "kcoreclaimmut: and bench did not complete three runs (rows=$rows)"
}

# ── runnable ─────────────────────────────────────────────────────────────────
#
# RUNNABLE UNSCHEDULED is Nightwatch's proof for a strand that is WORKING and
# yet on no CPU and never dispatched again: the runqueue lost it. For as long
# as it existed it could not fire. It asked `!e->on_cpu`, and a strand off the
# CPU carries -1 there, so the test was true only of a strand RUNNING on core
# 0 — which on a multi-core machine is a K-Core and runs none. And the loss it
# was written for was real: a steal could take a strand its core was still
# running, the thief's dispatch claim then lost to the owner, and schedule()
# answered a lost claim by going idle over a strand it had already taken out of
# every runqueue (both closed 2026-09-26, scheduler.c).
#
# runnable    — bench three times at sixteen cores: the proof stays silent and
#               every run completes.
# runnablemut — schedule() drops one application strand right after taking it
#               out of its runqueue, which is what the lost-claim branch used
#               to do. The proof must name it, "in no runqueue", on a machine
#               whose bench then never finishes.
runnable_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local run=0
    while [ $run -lt 3 ]; do
        local MARK
        MARK=$(wc -l < build/serial.log)
        if ! type_cmd "bench"; then break; fi
        sleep 1
        key_press ret
        wait_until "$WAIT_QUIET_S" "a bench row or a verdict" \
            'tail -n +$((MARK + 1)) build/serial.log | grep -q "create+write64+delete" ||
              grep -q "RUNNABLE UNSCHEDULED" build/serial.log'
        grep -q "RUNNABLE UNSCHEDULED" build/serial.log && break
        run=$((run+1))
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

runnable_drop_on() {
    mutate src/kernel/core/scheduler/scheduler.c
    python3 - <<'EOF'
p = "src/kernel/core/scheduler/scheduler.c"
s = open(p).read()
anchor = """    // Select next
    process_t *next = scheduler_select_next();
"""
assert anchor in s, "runnable drop anchor missing"
dropped = """    // Select next
    process_t *next = scheduler_select_next();
    { static uint32_t lcm_seen; if (next && !process_is_idle(next) && next->pid >= 3 && ++lcm_seen == 2000) next = idle_process_get(); } /* logcheck mutation: one application strand is taken out of its runqueue and never dispatched */
"""
open(p, "w").write(s.replace(anchor, dropped, 1))
EOF
    mut_done src/kernel/core/scheduler/scheduler.c "logcheck mutation"
}

runnable_drop_off() {
    restore src/kernel/core/scheduler/scheduler.c
}

run_runnable() {
    echo "== runnable: bench three times at sixteen cores, and RUNNABLE UNSCHEDULED stays silent =="
    build STRICT=on
    runnable_boot runnable
    L="$SCRATCH/serial.runnable.log"
    local rows
    rows=$(grep -c "create+write64+delete" "$L")
    [ "$rows" -ge 3 ]
    chk $? "runnable: bench completed three runs at sixteen cores (rows=$rows)"
    ! grep -q "RUNNABLE UNSCHEDULED" "$L"
    chk $? "runnable: and no strand was accused of being runnable and unscheduled"
}

run_runnablemut() {
    echo "== runnablemut: schedule() drops one strand after dequeuing it, and Nightwatch must name it =="
    runnable_drop_on; build STRICT=on
    runnable_boot runnablemut
    runnable_drop_off
    L="$SCRATCH/serial.runnablemut.log"
    grep -q "RUNNABLE UNSCHEDULED: on no CPU and in no runqueue" "$L"
    chk $? "runnablemut: Nightwatch named the dropped strand — RUNNABLE UNSCHEDULED, in no runqueue"
    local rows
    rows=$(grep -c "create+write64+delete" "$L")
    [ "$rows" -lt 3 ]
    chk $? "runnablemut: and bench did not complete three runs (rows=$rows)"
}

# ── lostwake ─────────────────────────────────────────────────────────────────
#
# Nightwatch's LOST WAKE used to be judged at a glance: a strand parked on a
# word, no deadline armed, and the word already changed. Two things make a
# glance no proof at all. A waker stores first and asks for the wake in a
# SEPARATE call, so "changed and still asleep" is what every delivery looks
# like for the length of one syscall; and the watch's own glance is two steps —
# the park's expectation is copied under the process lock, the word is read
# later, after the walk and its printing — while the machine goes on. Measured
# on BIOS 16c (2026-09-06, the day matrix): eleven workers of the
# std::execution::par brigade accused at once, the printed words climbing along
# the walk (0xc1, 0xc3, 0xc7 … 0xd3 against one and the same expectation), and
# cxxtest passing the phase a moment later. The matrix was announced 6/6 over
# that verdict, because nobody read verdicts.
#
# The proof is now persistence, the same as the other six proofs: the same park
# (pid, generation, park seq), the word still changed, a full look later. A
# delivery in flight cannot last ten seconds; a lost wake lasts for ever.
#
# The glance is WIDENED: the watch waits two seconds between copying the
# parks and reading the words, and says so — but only on a look that found an
# application alive, so the witness never lands inside the shell's echo of a
# typed command (the shell echoes keystroke by keystroke, and a kernel line
# between two of them breaks the echo the typer verifies) and every witness
# counted is a look that fell inside the workload. The look's CADENCE is left
# alone on purpose: a look every second was tried, and it changed what the
# other proofs mean — ANSWER OWED needs "no answer anywhere for a whole look",
# which at one second is any sequential stretch of a test (seven brigade
# workers convicted thirteen seconds into the boot) — and it let two walks
# overlap and print over each other. So the width comes from the walk and
# from repetition instead: eight rounds of cxxtest's parallel-algorithm phases
# (201-202: eleven workers parked and woken thousands of times a second), so
# that several looks land with the brigade alive. Under the one-glance judge
# every such look convicts — measured: after three green rounds, all eleven
# workers named on the first look that found the brigade alive, and two more
# verdicts before the harness stopped the machine; under the fix none may,
# and the phases must pass every time.
slowwalk_on() {
    mutate src/kernel/core/nightwatch/nightwatch.c
    python3 - <<'EOF'
p = "src/kernel/core/nightwatch/nightwatch.c"
s = open(p).read()
anchor = """    process_list_unlock();

    /* What each strand was PROMISED is read with the snapshot above (ChitPeek):"""
assert anchor in s, "slowwalk anchor missing"
slow = """    process_list_unlock();
    if (n > 2)   /* logcheck mutation: the watch walks slowly — 2 s between the parks and the words */
        kprintf("[MUTATION] the watch walks slowly: 2 s between the parks and the words, %u processes alive\\n", n);
    delay(2000);

    /* What each strand was PROMISED is read with the snapshot above (ChitPeek):"""
open(p, "w").write(s.replace(anchor, slow, 1))
EOF
    mut_done src/kernel/core/nightwatch/nightwatch.c "logcheck mutation" 1
}
slowwalk_off() {
    restore src/kernel/core/nightwatch/nightwatch.c
}

# The wake that was never rung. The first application wake (pid >= 3 — the
# console daemon and the shell are left alone, so the harness can still drive
# the machine) that would have reached a sleeper is dropped: the bucket is
# walked, a parked strand is found on exactly that word, and the call returns
# OK having claimed nobody. The word has already moved — the caller stored
# before it asked — so this is precisely a lost wake, once, and it says so on
# serial. The brigade's region never completes: the worker sleeps on a ticket
# that has moved, the caller sleeps on a done-count that never will, and
# Nightwatch has to be the one to say which strand and why.
wakedrop_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = """    spin_lock(&bucket->lock);
    AddrWaitEntry *e = bucket->head;
"""
assert s.count(anchor) == 1, "wakedrop anchor missing"
dropped = """    {   /* logcheck mutation: the wake that was never rung */
        static uint32_t s_rung_short;
        if (ctx->proc->pid >= 3 && __atomic_load_n(&s_rung_short, __ATOMIC_ACQUIRE) == 0) {
            bool sleeper = false;
            spin_lock(&bucket->lock);
            for (AddrWaitEntry *w = bucket->head; w; w = w->next)
                if (!w->done && w->space == space && w->user_va == user_va) { sleeper = true; break; }
            spin_unlock(&bucket->lock);
            if (sleeper && __atomic_exchange_n(&s_rung_short, 1u, __ATOMIC_ACQ_REL) == 0) {
                kprintf("[MUTATION] wake dropped: pid %u asked for 0x%lx and the sleeper was not told\\n",
                        ctx->proc->pid, (unsigned long)user_va);
                return OK;
            }
        }
    }
    spin_lock(&bucket->lock);
    AddrWaitEntry *e = bucket->head;
"""
open(p, "w").write(s.replace(anchor, dropped, 1))
EOF
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation: the wake that was never rung"
}
wakedrop_off() {
    restore src/kernel/core/decks/system/sync_ops.c
}

# Sixteen cores. cxxtest's two parallel-algorithm phases are run `rounds`
# times, each waited out to its SUBSET PASS (or an announced failure), or to
# Nightwatch naming a lost wake — whichever comes first. A verdict ends the
# whole thing: the shell is parked on a cxxtest that will never return, so
# nothing typed after it would run.
#
# Between rounds the shell must be back at its prompt, and that is read from
# the LINES, not from the last two bytes of the file: the watch's witness line
# lands after a prompt that has no newline yet, so the file ends in the
# witness and wait_for_prompt would wait its whole ceiling for a shell that
# was ready all along (measured: the first run of this scenario). A second
# line beginning "~ " since the round was typed is the shell's next prompt,
# whatever the kernel appended to it.
lostwake_prompt_back() {
    local mark=$1
    wait_until "$WAIT_QUIET_S" "the shell's next prompt" \
        '[ "$(tail -n +$((mark + 1)) build/serial.log | grep -c "^~ ")" -ge 2 ]' \
        || { echo "  (the shell never came back to its prompt)"; return 1; }
    sleep 2
    return 0
}
# rounds = how many brigades must pass; want_looks = how many of the watch's
# looks must have landed while one ran. A look comes every ten seconds and a
# round lasts about as long, so the two lock phase: on 2026-09-26 a kernel that
# printed through the console faster moved every look into the gaps between
# rounds, and eight clean rounds proved nothing (looks=2). So the rounds go on
# until the watch has looked often enough — a pause that grows with the round
# number walks the phase — and stop at three times the rounds asked for.
lostwake_boot() {
    local rounds=$2 want_looks=${3:-0}
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local run=0
    while [ $run -lt "$rounds" ] ||
          { [ $run -lt $((rounds * 3)) ] &&
            [ "$(grep -c "the watch walks slowly" build/serial.log)" -lt "$want_looks" ]; }; do
        local MARK
        MARK=$(wc -l < build/serial.log)
        if ! type_cmd "cxxtest 201-202"; then break; fi
        sleep 1
        key_press ret
        wait_until "$WAIT_QUIET_S" "a cxxtest verdict or a LOST WAKE" \
            'tail -n +$((MARK + 1)) build/serial.log | grep -qE "\[CXX\] (SUBSET PASS|TOTAL FAILURES)" ||
              grep -q "LOST WAKE" build/serial.log'
        grep -q "LOST WAKE" build/serial.log && break
        lostwake_prompt_back "$MARK" || break
        [ "$want_looks" -gt 0 ] && sleep $((run % 4))
        run=$((run+1))
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

run_lostwake() {
    echo "== lostwake: the watch walks slowly through a brigade, and must not cry wolf =="
    slowwalk_on; build STRICT=on
    lostwake_boot lostwake 8 3
    slowwalk_off
    L="$SCRATCH/serial.lostwake.log"

    local passes looks
    passes=$(grep -c "\[CXX\] SUBSET PASS" "$L")
    [ "$passes" -ge 8 ]
    chk $? "lostwake: cxxtest 201-202 passed eight times over with the watch reading every parked word 2 s late (passes=$passes)"
    # The witness is printed only on a look that found an application alive,
    # so its count is the count of looks that fell inside the workload. Fewer
    # than three and the run proved nothing — the watch hardly looked while
    # the brigade was running.
    looks=$(grep -c "the watch walks slowly" "$L")
    [ "$looks" -ge 3 ]
    chk $? "lostwake: and the watch looked at least three times while a program was alive (looks=$looks)"
    ! grep -q "LOST WAKE" "$L";                 chk $? "lostwake: and no wake was called lost"
    ! grep -q "a defect, not a slow test" "$L"; chk $? "lostwake: and no verdict was spoken"

    build   # leave the tree built from clean sources
}

# The oracle measured against itself: with one wake dropped for real, the same
# run must STOP, and Nightwatch must be the one to say on whom.
run_lostwakemut() {
    echo "== lostwakemut: one wake is never rung, and the watch must name the sleeper =="
    wakedrop_on; build STRICT=on
    lostwake_boot lostwakemut 1
    wakedrop_off
    L="$SCRATCH/serial.lostwakemut.log"

    grep -q "\[MUTATION\] wake dropped" "$L"
    chk $? "lostwakemut: a wake to a sleeping strand was dropped (the mutation bit)"
    grep -q "LOST WAKE" "$L"
    chk $? "lostwakemut: Nightwatch named it — LOST WAKE"
    # The accused must be a worker of the brigade: a strand of cxxtest, pid 4
    # or above. The line two above the verdict names it.
    local accused
    accused=$(grep -B2 "LOST WAKE" "$L" | grep -oE "pid [0-9]+ gen [0-9]+ waiting" | head -1 | awk '{print $2}')
    [ -n "$accused" ] && [ "$accused" -ge 4 ]
    chk $? "lostwakemut: and the accused is a brigade worker (pid ${accused:-?})"
    ! grep -q "\[CXX\] SUBSET PASS" "$L"
    chk $? "lostwakemut: and cxxtest 201-202 did not pass"

    build   # leave the tree built from clean sources
}

# ── chit ─────────────────────────────────────────────────────────────────────
#
# ANSWER OWED used to rest on two witnesses about the MACHINE — every K-Core
# asleep, and no answer published anywhere since the last look — rather than
# on facts about the strand it accused. A brigade of workers parked on their
# leader while the leader merely computes satisfies both (measured 2026-09-06:
# seven workers convicted while the phase passed). The kernel now keeps its own
# half of the cloakroom token, the chit (chit.h): the handler that puts an
# answer off leaves one naming itself, the event that determines the answer
# marks it due, the push that delivers it keeps it. ANSWER OWED convicts only
# a token nobody left a chit for, or one whose chit fell due and was never
# kept — the same a full look later — and the guide says at once, out of band,
# when a deferred answer to a waited-for token left no chit at all.
#
# Three scenarios, on sixteen cores. The healthy one runs every kind of
# deferral this machine has (a brigade parked and woken on addr_park, strands
# parked and joined, files written and read back, children waited out by the
# shell) and then stands at the prompt: nothing may be convicted and neither
# guard may speak. The two mutations each break one promise the way a real bug
# would — the death of a child determined and never delivered; a handler that
# raised the async flag without leaving its chit — and the watch must name the
# strand and the holder, and the guide must name the omission at once.
chit_boot_and_prompt() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
}
# Type one command and wait for either its marker or a Nightwatch verdict.
# Returns 0 only when the MARKER came. The ceiling this used to carry is gone:
# the wait now ends on the machine going quiet, so a slower host no longer
# turns a command that was merely long into a command that never answered.
chit_command() {
    local cmd=$1 marker=$2
    CHIT_MARK=$(wc -l < build/serial.log)
    type_cmd "$cmd" || return 1
    sleep 1
    key_press ret
    # Either the command answered, or the watch convicted somebody. The second
    # is an answer too — just not the one that was asked for — so both end the
    # wait and the two are told apart afterwards.
    wait_until "$WAIT_QUIET_S" "'$marker' or a verdict" \
        'tail -n +$((CHIT_MARK + 1)) build/serial.log | grep -qE -- "$marker" ||
          grep -q "ANSWER OWED" build/serial.log' || return 1
    tail -n +$((CHIT_MARK + 1)) build/serial.log | grep -qE -- "$marker"
}

run_chit() {
    echo "== chit: every deferral leaves its chit, and a healthy machine is convicted of nothing =="
    build STRICT=on
    chit_boot_and_prompt
    local rounds=0 r
    for r in 1 2; do
        chit_command "cxxtest 201-202" "\[CXX\] (SUBSET PASS|TOTAL FAILURES)" || break
        lostwake_prompt_back "$CHIT_MARK" || break
        rounds=$((rounds+1))
    done
    local strands=0 benches=0
    if chit_command "strandtest" "STRAND|strandtest"; then
        lostwake_prompt_back "$CHIT_MARK" && strands=1
    fi
    if chit_command "bench" "create\+write64\+delete"; then
        lostwake_prompt_back "$CHIT_MARK" && benches=1
    fi
    sleep 30   # at the prompt: three looks with nothing to accuse
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.chit.log"
    L="$SCRATCH/serial.chit.log"

    [ "$rounds" -eq 2 ];                        chk $? "chit: cxxtest 201-202 (the brigade) passed twice (rounds=$rounds)"
    [ "$strands" -eq 1 ];                       chk $? "chit: strandtest ran and the shell came back"
    [ "$benches" -eq 1 ];                       chk $? "chit: bench wrote and read the volume and the shell came back"
    ! grep -q "ANSWER OWED" "$L";               chk $? "chit: no answer was called owed"
    ! grep -q "a defect, not a slow test" "$L"; chk $? "chit: and no verdict was spoken"
    ! grep -q "\[GUIDE\] DEFECT" "$L";          chk $? "chit: and every deferred answer to a waited-for token left a chit (the guide stayed silent)"
    ! grep -q "\[CHIT\] DEFECT" "$L";           chk $? "chit: and no strand moved on from an answer the kernel still owed"
    ! grep -qE "PANIC|\[EXCEPTION\]" "$L";      chk $? "chit: and no panic"
}

# The death is determined and the answer never delivered. The first delivery
# of an application's death (pid >= 3) to a waiter marks the chit due — exactly
# as the real path does — and then throws the Result away instead of pushing
# it. The shell waits for a child that has already died, for ever; nothing is
# queued, nobody is serving it, and the only trace is a chit DUE that stays DUE.
chitwithhold_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = """        ChitDue(list->waiter, list->submit_cookie);
        KResultPush(list->waiter, &r);
"""
assert anchor in s, "chitwithhold anchor missing"
held = """        ChitDue(list->waiter, list->submit_cookie);
        {   /* logcheck mutation: the answer is determined, and never delivered */
            static uint32_t s_withheld;
            if (proc->pid >= 3 && list->waiter->pid >= 2 &&
                __atomic_exchange_n(&s_withheld, 1u, __ATOMIC_ACQ_REL) == 0) {
                kprintf("[MUTATION] answer withheld: pid %u owed token 0x%06x for the death of pid %u\\n",
                        list->waiter->pid, list->submit_cookie, proc->pid);
                process_ref_dec(list->waiter);
                process_ref_dec(proc);
                kfree(list);
                list = next;
                continue;
            }
        }
        KResultPush(list->waiter, &r);
"""
open(p, "w").write(s.replace(anchor, held, 1))
EOF
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation: the answer is determined"
}
chit_mutation_off() {
    restore src/kernel/core/decks/system/sync_ops.c
}

# The handler that forgot. SysProcessGone raises the async flag the old way,
# by hand, and leaves no chit — the shape of every future async handler that
# does not go through ChitGive. The guide must say so the moment the shell's
# wait for its child is deferred, and Nightwatch must convict the shell two
# looks later: waiting, nothing queued, nobody serving, no chit.
chitforgot_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = """    ChitGive(ctx, "system.process.gone", ((uint64_t)want_gen << 32) | pid);
"""
assert anchor in s, "chitforgot anchor missing"
forgot = """    if (ctx->async_owns_crates) *ctx->async_owns_crates = true;   /* logcheck mutation: the handler forgot its chit */
"""
open(p, "w").write(s.replace(anchor, forgot, 1))
EOF
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation: the handler forgot its chit"
}

# Wait for Nightwatch to speak: a verdict takes two looks (>= 20 s) after the
# stall begins; sixty seconds is three looks and a margin, not a deadline on
# the machine — a healthy one would simply never speak.
#
# ‼ THIS ONE KEEPS ITS DEADLINE, AND IT IS THE RIGHT INSTRUMENT HERE.
#
# Everything else in this file gave its ceiling up for a silence watchdog,
# because silence is evidence of being wedged. Here it is the opposite: the
# machine under test IS stalled — that is the whole scenario — and what is
# being waited for is the WATCH noticing. A silence watchdog would give up the
# instant the stall it is measuring took hold. The number is not a guess about
# speed either; it is counted in Nightwatch's own looks, which are a machine-
# side interval. If the watch ever starts printing per-look, this becomes a
# wait on the look count and the number goes with it.
chit_wait_for_verdict() {
    local t=0
    while [ $t -lt 60 ]; do
        grep -q "ANSWER OWED" build/serial.log && return 0
        sleep 2; t=$((t+1))
    done
    return 1
}

run_chitmut() {
    echo "== chitmut: a child's death is determined and its answer withheld — the watch must name the shell and the holder =="
    chitwithhold_on; build STRICT=on
    chit_boot_and_prompt
    chit_command "say hi" "^hi"
    chit_wait_for_verdict
    make run-stop >/dev/null 2>&1
    chit_mutation_off
    cp build/serial.log "$SCRATCH/serial.chitmut.log"
    L="$SCRATCH/serial.chitmut.log"

    grep -q "\[MUTATION\] answer withheld" "$L"
    chk $? "chitmut: a determined answer was thrown away (the mutation bit)"
    grep -q "ANSWER OWED: system.process.gone took the event" "$L"
    chk $? "chitmut: Nightwatch named it — ANSWER OWED, held by system.process.gone"
    local accused
    accused=$(grep "ANSWER OWED" "$L" | grep -oE "pid [0-9]+ gen" | head -1 | awk '{print $2}')
    [ "${accused:-0}" = 2 ]
    chk $? "chitmut: and the accused is the shell (pid ${accused:-?})"
    ! grep -q "\[GUIDE\] DEFECT" "$L"
    chk $? "chitmut: and the guide had nothing to say — the chit WAS left, it was the delivery that failed"

    build   # leave the tree built from clean sources
}

run_chitmissmut() {
    echo "== chitmissmut: a handler raises the async flag and leaves no chit — the guide must say so at once, the watch two looks later =="
    chitforgot_on; build STRICT=on
    chit_boot_and_prompt
    chit_command "cxxtest" "\[GUIDE\] DEFECT"     # the child runs for minutes; the omission is said as it is deferred
    chit_wait_for_verdict
    make run-stop >/dev/null 2>&1
    chit_mutation_off
    cp build/serial.log "$SCRATCH/serial.chitmissmut.log"
    L="$SCRATCH/serial.chitmissmut.log"

    grep -q "\[GUIDE\] DEFECT: pid 2 deferred the answer to token" "$L"
    chk $? "chitmissmut: the guide named the omission the moment the shell's wait was deferred"
    grep -q "ANSWER OWED: waiting on token .* no handler left a chit" "$L"
    chk $? "chitmissmut: Nightwatch named it — ANSWER OWED, nobody promised the answer"
    local accused
    accused=$(grep "ANSWER OWED" "$L" | grep -oE "pid [0-9]+ gen" | head -1 | awk '{print $2}')
    [ "${accused:-0}" = 2 ]
    chk $? "chitmissmut: and the accused is the shell (pid ${accused:-?})"

    build   # leave the tree built from clean sources
}

# ── turnin ───────────────────────────────────────────────────────────────────
#
# A producer moves a reply ring's `tail` when it claims a slot and releases the
# slot's seq later. A strand that takes its Turn In mark after the claim and
# looks before the release finds nothing to pop, asks to sleep with a mark that
# already covers the arrival, and is put to bed by a kernel that compared only
# cursors. The release lands a moment later; nobody releases a slot twice; a
# wake that then re-checks only the mark re-arms the sleep on top of a message
# already delivered. Measured on BIOS 16c (2026-09-06): the console daemon
# parked at result 36 with pos 35 (a lane request) released and unread, and the
# child that asked for the lane waited for the rest of the run.
#
# The window is microseconds wide on silicon, so it is WIDENED: every IPC push
# into the console daemon's ring waits, after its claim and before its release,
# until the daemon has actually gone to sleep (bounded, so a daemon that never
# sleeps costs the push a few hundred milliseconds and nothing else). That
# forces exactly the interleaving above on every lane request. Under the fix —
# a sleep judged on the slot at the head as well as on the cursor, on both
# sides — phase 58 of cxxtest, which spawns a printing child per step, runs
# three times over; under the cursor-only sleep (put back by turninmut) the
# first lane request that meets the window stops the machine.
turnin_window_on() {
    mutate src/kernel/core/ipc/kring.c
    python3 - <<'EOF'
p = "src/kernel/core/ipc/kring.c"
s = open(p).read()
# Anchored on the payload store alone: the stretch between it and the publish
# has grown (the stray-token check, e1e39272, 2026-09-09) and an anchor that
# spanned it stopped matching — this scenario could not run from that day
# until it was noticed on 2026-09-26.
anchor = """    slot->r = *r;
"""
assert s.count(anchor) == 1, "turnin window anchor missing"
widened = """    slot->r = *r;
    /* logcheck mutation: an IPC push into pid 1 holds its released-later slot
     * until the daemon has gone to sleep (or 300 ms), so the claim lands
     * before the daemon's mark and the release after its look — every time. */
    if (r->sender_pid != 0 && target->pid == 1) {
        for (uint32_t w = 0; w < 300 && process_get_state(target) != PROC_WAITING; w++)
            delay(1);
    }
"""
open(p, "w").write(s.replace(anchor, widened, 1))
EOF
    mut_done src/kernel/core/ipc/kring.c "logcheck mutation"
}

turnin_window_off() {
    restore src/kernel/core/ipc/kring.c
}

# The sleep that stood here before: cursors only, on both sides — exactly the
# old SysTurnIn re-check and the old box_turn_in exit. The writer waiting for
# its lane grant used to re-ask on a clock, and that re-ask had to be blinded
# here too or it ended the wedge by itself; since 3141287b (2026-09-08) the
# writer asks once and asks again only when the daemon dies, so there is
# nothing left to blind in print.c — and the anchor that blinded it had
# stopped matching, which kept this mutant from building until 2026-09-26.
cursoronly_on() {
    mutate src/kernel/core/decks/system/turnin_ops.c
    mutate src/userspace/boxlib/src/turnin.c
    python3 - <<'EOF'
p = "src/kernel/core/decks/system/turnin_ops.c"
s = open(p).read()
anchor = """    return turnin_touch_tail(proc)  != touch_seen  ||
           turnin_result_tail(proc) != result_seen ||
           KResultRingHasUnreadAtHead(proc)        ||
           KTouchRingHasUnreadAtHead(proc);"""
assert anchor in s, "cursoronly kernel anchor missing"
blind = """    /* logcheck mutation: cursors only, the sleep that stood here before */
    return turnin_touch_tail(proc)  != touch_seen  ||
           turnin_result_tail(proc) != result_seen;"""
open(p, "w").write(s.replace(anchor, blind, 1))
p = "src/userspace/boxlib/src/turnin.c"
s = open(p).read()
anchor = """    return box_mark_moved(seen) ||
           result_published_at_head() ||
           touch_ring_published_at_head();"""
assert anchor in s, "cursoronly boxlib anchor missing"
blind = """    return box_mark_moved(seen);   /* logcheck mutation: the mark alone */"""
open(p, "w").write(s.replace(anchor, blind, 1))
EOF
    mut_done src/kernel/core/decks/system/turnin_ops.c "logcheck mutation"
    mut_done src/userspace/boxlib/src/turnin.c "logcheck mutation: the mark alone"
}

cursoronly_off() {
    restore src/kernel/core/decks/system/turnin_ops.c
    restore src/userspace/boxlib/src/turnin.c
}

# Sixteen cores, `cxxtest 58` three times over. Each run is waited out to its
# SUBSET PASS or to a frozen serial — a child waiting for a lane it will never
# be granted prints nothing, and the shell is parked on a cxxtest that will
# never return, so nothing typed after it would run.
turnin_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local run=0
    while [ $run -lt 3 ]; do
        local before
        before=$(grep -c "SUBSET PASS" build/serial.log)
        if ! type_cmd "cxxtest 58"; then break; fi
        sleep 1
        key_press ret
        wait_until "$WAIT_QUIET_S" "cxxtest 58 to pass its subset" \
            '[ "$(grep -c "SUBSET PASS" build/serial.log)" -gt "$before" ]'
        [ "$(grep -c "SUBSET PASS" build/serial.log)" -gt "$before" ] || break
        run=$((run+1))
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

run_turnin() {
    echo "== turnin: a slot claimed before the sleeper's mark and released after its look =="
    turnin_window_on; build STRICT=on
    turnin_boot turnin
    turnin_window_off
    L="$SCRATCH/serial.turnin.log"

    local passes
    passes=$(grep -c "SUBSET PASS" "$L")
    [ "$passes" -ge 3 ]
    chk $? "turnin: cxxtest 58 passed three runs running with every lane request released only after the daemon slept (passes=$passes)"
}

# The oracle measured against itself: with the cursor-only sleep back, the
# same run must STOP on its first lane request.
run_turninmut() {
    echo "== turninmut: put the cursor-only sleep back and require the machine to stop =="
    turnin_window_on; cursoronly_on; build STRICT=on
    turnin_boot turninmut
    cursoronly_off; turnin_window_off
    L="$SCRATCH/serial.turninmut.log"

    local passes
    passes=$(grep -c "SUBSET PASS" "$L")
    [ "$passes" -lt 3 ]
    chk $? "turninmut: with the cursor-only sleep back, cxxtest 58 did not get through three runs (passes=$passes)"
}

# And the other half: a box that never turns in. Removing the submit leaves the
# loop exactly as it was before this work — look, find nothing, look again.
turnin_off() {
    mutate src/userspace/boxlib/src/turnin.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/turnin.c"
s = open(p).read()
anchor = "        if (pocket_ring_is_empty(pocket_ring())) turn_in_submit(seen);"
assert anchor in s, "turnin mutation anchor missing"
s = s.replace(anchor, "        /* logcheck mutation: never ask to be put down */", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/turnin.c "logcheck mutation"
}

turnin_restore() {
    restore src/userspace/boxlib/src/turnin.c
}

# And the reader's own half: a brook_pop that spins instead of sleeping. This
# is the narrowest of the three — turn-in stays, the bell stays, only the wait
# inside Brook goes back to watching a cacheline — so it is the one that says
# whether the window measurement is about brook_pop or merely about the box.
brookpop_off() {
    mutate src/userspace/boxlib/src/brook.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/brook.c"
s = open(p).read()
anchor = "        brook_sleep_until(b, brook_pop_ready);"
assert anchor in s, "brookpop mutation anchor missing"
s = s.replace(anchor, "        brook_wait_cycle(&h->tail, &spin, 0);   /* logcheck mutation: spin, do not sleep */", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/brook.c "logcheck mutation"
}

brookpop_restore() {
    restore src/userspace/boxlib/src/brook.c
}

# sleepsmut — the oracle measured against itself.
#
# A check nobody has ever seen go red is a check nobody should believe. This
# breaks each half in turn, on purpose, and requires the corresponding half of
# `sleeps` to notice. Neither mutation is subtle and neither is a crash: both
# leave a machine that boots, runs, and prints everything — which is precisely
# why an oracle is needed to tell them apart from a healthy one.
run_sleepsmut() {
    echo "== sleepsmut: break the bell, then the sleep, and require the oracle to say so =="

    local before_fail
    before_fail=$FAIL

    bell_off; build STRICT=on
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local MARK EARLY
    MARK=$(wc -l < build/serial.log)
    type_cmd "quietprint"
    sleep 1
    key_press ret
    sleep 2
    EARLY=$(tail -n +$((MARK + 1)) build/serial.log)
    # Long enough for TWO Nightwatch looks. The watch convicts on a delivery
    # that is still unread a whole look later, never on one instant — a
    # delivery in flight looks exactly like a lost one for the microseconds
    # between the record and the wake, and sleeping strands go through that gap
    # thousands of times a second under a print storm.
    sleep 28
    cp build/serial.log "$SCRATCH/serial.sleepsmut.bell.log"
    make run-stop >/dev/null 2>&1
    bell_restore

    if printf '%s' "$EARLY" | grep -q "\[QP\] line 1"; then
        bad "sleepsmut: bell disabled and the line STILL arrived on time — the check proves nothing"
    else
        ok "sleepsmut: bell disabled — the line was not on screen while the writer lived"
    fi

    # lane_push used to announce its own jam after sixty seconds. It does not
    # any more, because it sleeps instead of spinning and a sleeper cannot
    # narrate. This is what took that job: the watch names the jam from facts,
    # with no clock in it. If it ever goes quiet, the jam goes silent with it.
    if grep -q "BELL UNRUNG" "$SCRATCH/serial.sleepsmut.bell.log"; then
        ok "sleepsmut: Nightwatch named the jam — BELL UNRUNG, on facts, with no deadline"
    else
        bad "sleepsmut: nobody rang and NOBODY SAID SO — a stream can now jam in silence"
    fi

    turnin_off; build STRICT=on
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep $QUIET_SETTLE_S
    local A B USED
    A=$(qemu_cpu_cs); sleep $QUIET_SAMPLE_S; B=$(qemu_cpu_cs)
    USED=$(( (B - A) * 100 / (QUIET_SAMPLE_S * 100) ))
    make run-stop >/dev/null 2>&1
    turnin_restore

    echo "     idle without Turn In: ${USED}% of one core"
    if [ "$USED" -lt 25 ]; then
        bad "sleepsmut: Turn In removed and the box was STILL idle at ${USED}% — the check proves nothing"
    else
        ok "sleepsmut: Turn In removed — the box went back to ${USED}% of a core"
    fi

    brookpop_off; build STRICT=on
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "quietprint"
    sleep 1
    key_press ret
    sleep 1
    local WA WB WUSED
    WA=$(qemu_cpu_cs); sleep 2; WB=$(qemu_cpu_cs)
    WUSED=$(( (WB - WA) * 100 / 200 ))
    wait_for_line build/serial.log "\[QP\] done" "$WAIT_QUIET_S"
    cp build/serial.log "$SCRATCH/serial.sleepsmut.brookpop.log"
    make run-stop >/dev/null 2>&1
    brookpop_restore

    echo "     blocked brook_pop without the sleep: ${WUSED}% of one core"
    if [ "$WUSED" -lt 25 ]; then
        bad "sleepsmut: brook_pop set back to spinning and the box was STILL idle at ${WUSED}% — the window measurement proves nothing"
    else
        ok "sleepsmut: brook_pop set back to spinning — the window went to ${WUSED}% of a core"
    fi

    build   # leave the tree built from clean sources either way
    [ $FAIL -eq $before_fail ] || echo "     (a red line above means the oracle cannot see its own defect)"
}

# ===========================================================================
# lines — one strand's line is never cut in half by another's
# ===========================================================================
#
# The console is a fan-in: every printing strand has its own lane, and the
# daemon merges them in global push order. The unit it interleaves is a FRAME,
# so where a frame ends decides whether concurrent output is readable. It used
# to end wherever the 108-byte run filled up, which lands mid-line, and two
# strands printing at once then spliced into each other:
#
#     [PS-11] 00000[PS-04] 00000000
#     [PS-04787
#
# Nothing was ever lost — the character count came out exact on every run, on
# both sides of the fix — so no test could see it by counting. What it cost was
# grep: every marker in this project is one, and a torn "[STRAND] PASS" reads
# as a machine that hung. That is what made sixteen-core runs a lottery.
#
# print_stress is the instrument: sixteen strands, two thousand numbered lines
# each, all of one exact shape. Anything that does not match that shape is a
# line something else got into. MEASURED: ~300-700 spliced lines per run before
# the fix, 0 of 32000 after.
run_lines() {
    echo "== lines: sixteen strands printing at once, and not one line spliced =="
    build STRICT=on

    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null || { bad "lines: never reached a shell"; make run-stop >/dev/null 2>&1; return; }

    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "print_stress"
    sleep 1
    key_press ret
    wait_for_line build/serial.log "PS SUMMARY" "$WAIT_QUIET_S" $((MARK + 1))
    # The serial line discipline is CRLF, and a trailing \r defeats a `$`
    # anchor on BSD grep — strip it once here rather than have every check
    # below carry the same footnote.
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.lines.log"
    make run-stop >/dev/null 2>&1

    grep -q "PS SUMMARY.*PASS" "$SCRATCH/serial.lines.log" \
        && ok "lines: print_stress ran to the end" \
        || bad "lines: print_stress never finished"

    local WHOLE DIGITS
    WHOLE=$(grep -cE '^\[PS-[0-9]{2}\] [0-9]{8}$' "$SCRATCH/serial.lines.log" | tr -d ' ')
    DIGITS=$(tr -cd '0-9' < "$SCRATCH/serial.lines.log" | wc -c | tr -d ' ')
    echo "     $WHOLE of 32000 lines whole; $DIGITS digits on the wire"

    # Exactly 32000, not "nearly". A single splice is a marker somebody's grep
    # will miss one run in ten, which is the whole failure this closes.
    if [ "$WHOLE" -eq 32000 ]; then ok "lines: all 32000 lines came out whole"
    else                            bad "lines: $WHOLE of 32000 whole — $((32000 - WHOLE)) line(s) were spliced"; fi

    # And the older, weaker fact, kept because it is the one that says the
    # difference between a splice and a LOSS: the characters are all there.
    if [ "$DIGITS" -ge 256000 ]; then ok "lines: every character reached the log ($DIGITS digits)"
    else                              bad "lines: only $DIGITS digits reached the log — characters were LOST, not merely spliced"; fi
}

# The mutation: cut a frame wherever the buffer happens to fill, as it did
# before. Nothing breaks, nothing is lost, and the log becomes unmatchable —
# which is exactly why this needs an oracle rather than an eye.
linecut_off() {
    mutate src/userspace/boxlib/src/print.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/print.c"
s = open(p).read()
anchor = "                lane_flush_lines(ps);"
assert anchor in s, "linecut mutation anchor missing"
s = s.replace(anchor, "                lane_flush_run(ps);   /* logcheck mutation: cut anywhere */", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/print.c "logcheck mutation"
}

linecut_restore() {
    restore src/userspace/boxlib/src/print.c
}

run_linesmut() {
    echo "== linesmut: cut frames anywhere again, and require the oracle to notice =="
    linecut_off; build STRICT=on

    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "print_stress"
    sleep 1
    key_press ret
    wait_for_line build/serial.log "PS SUMMARY" "$WAIT_QUIET_S" $((MARK + 1))
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.linesmut.log"
    make run-stop >/dev/null 2>&1
    linecut_restore

    local WHOLE
    WHOLE=$(grep -cE '^\[PS-[0-9]{2}\] [0-9]{8}$' "$SCRATCH/serial.linesmut.log" | tr -d ' ')
    echo "     cutting anywhere: $WHOLE of 32000 lines whole"
    if [ "$WHOLE" -eq 32000 ]; then
        bad "linesmut: frames cut anywhere and nothing spliced — the check proves nothing"
    else
        ok "linesmut: frames cut anywhere — $((32000 - WHOLE)) line(s) spliced, and the oracle sees them"
    fi

    build   # leave the tree built from clean sources
}

# ===========================================================================
# rollcall — every line said answers with its number, whole, on the wire
# ===========================================================================
#
# Two paths carry a line into the serial account, and each had a way of
# losing or tearing it that no count of characters could see.
#
# printf stages a strand's text in the strand's own frame and pushes it when
# the frame fills, the colour changes or somebody flushes — so the line a
# strand said just before returning died with it (MEASURED on BIOS 16c: the
# last of 400 numbered lines, every printf strand, every run; print_stress
# hid it with an io_flush of its own). And the daemon drew a line's text in
# one VGA operation and its newline in the next, with the console lock held
# per operation, so a kprintf from another core landed between the two:
#
#     [ROLLCALL] s=1 n=0 via=printf[6] [ROLLCALL] s=2 n=2 via=kdbg
#
# (MEASURED: 184 and 71 such lines of 1600 in two runs.) `lines` can see
# neither: print_stress flushed by hand, and nothing prints from the kernel
# while it runs.
#
# rollcall is the instrument: four strands, four hundred numbered lines each,
# two through printf and two through kdbg_print (the kernel's kprintf), all
# at once. Every number must answer exactly once, and no line may carry
# another's.
rollcall_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "rollcall"
    sleep 1
    key_press ret
    wait_for_line build/serial.log "\[ROLLCALL\] done" "$WAIT_QUIET_S" $((MARK + 1))
    sleep 2
    # CRLF on the wire, and a kdbg line wears the kernel's "[pid] " prefix:
    # strip both once so the shape below is the line's own.
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' \
        | sed -E 's/^\[[0-9]+\] //' > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

# Prints "<answered> <carrying>": numbers that answered as a whole line of
# their own, and lines that carry more than one record.
rollcall_count() {
    local answered=0 s
    for s in 0 1 2 3; do
        answered=$(( answered + $(grep -E "^\[ROLLCALL\] s=$s n=[0-9]+ via=(printf|kdbg)$" "$1" | sort -u | wc -l | tr -d ' ') ))
    done
    local carrying
    carrying=$(grep -c "ROLLCALL.*ROLLCALL" "$1" | tr -d ' ')
    echo "$answered $carrying"
}

run_rollcall() {
    echo "== rollcall: 1600 numbered lines from four strands, every one answering, none carrying another =="
    build STRICT=on
    if ! rollcall_boot rollcall; then bad "rollcall: never reached a shell"; return; fi
    L="$SCRATCH/serial.rollcall.log"

    grep -q "\[ROLLCALL\] done: 4 strands x 400 lines" "$L" \
        && ok "rollcall: all four strands spoke and the utility finished" \
        || bad "rollcall: the utility did not finish with four strands"

    set -- $(rollcall_count "$L")
    echo "     $1 of 1600 numbers answered whole; $2 line(s) carrying another record"
    if [ "$1" -eq 1600 ]; then ok "rollcall: every number answered, whole"
    else                       bad "rollcall: $((1600 - $1)) number(s) never answered whole"; fi
    if [ "$2" -eq 0 ]; then ok "rollcall: no line carried another's"
    else                    bad "rollcall: $2 line(s) carried another record"; fi
}

# The oracle measured against itself: put both defects back — the strand
# leaves without handing in its frame, the daemon draws text and newline as
# two operations — and each symptom above must come back.
rollcall_defects_on() {
    mutate src/userspace/boxlib/src/strand.c
    mutate src/userspace/display/display.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/strand.c"
s = open(p).read()
anchor = "    io_flush();\n"
assert s.count(anchor) == 1, "rollcall strand anchor missing"
s = s.replace(anchor, "    /* logcheck mutation: the strand leaves without handing in its frame */\n", 1)
open(p, "w").write(s)
p = "src/userspace/display/display.c"
s = open(p).read()
anchor = """    for (uint32_t i = 0; i < len; i++) {
        char c = f->text[i];
        if (c == '\\n' || c == '\\b' || (unsigned char)c >= 0x20) seg[pos++] = c;
    }
    if (pos > 0) {
        seg[pos] = '\\0';
        vga_puts(seg);
    }
"""
assert s.count(anchor) == 1, "rollcall display anchor missing"
mutated = """    for (uint32_t i = 0; i <= len; i++) {   /* logcheck mutation: text and newline as two operations */
        int at_end = (i == len);
        char c     = at_end ? '\\0' : f->text[i];
        if (!at_end && c != '\\n' && (c == '\\b' || (unsigned char)c >= 0x20)) { seg[pos++] = c; continue; }
        if (pos > 0) { seg[pos] = '\\0'; vga_puts(seg); pos = 0; }
        if (!at_end && c == '\\n') vga_newline();
    }
"""
open(p, "w").write(s.replace(anchor, mutated, 1))
EOF
    mut_done src/userspace/boxlib/src/strand.c "logcheck mutation"
    mut_done src/userspace/display/display.c "logcheck mutation"
}

rollcall_defects_off() {
    restore src/userspace/boxlib/src/strand.c
    restore src/userspace/display/display.c
}

run_rollcallmut() {
    echo "== rollcallmut: both defects put back, and the roll call must see each =="
    rollcall_defects_on; build STRICT=on
    rollcall_boot rollcallmut; local booted=$?
    rollcall_defects_off
    if [ $booted -ne 0 ]; then bad "rollcallmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.rollcallmut.log"

    set -- $(rollcall_count "$L")
    echo "     $1 of 1600 numbers answered whole; $2 line(s) carrying another record"
    if [ "$1" -lt 1600 ]; then ok "rollcallmut: a strand that leaves without handing in loses lines — $((1600 - $1)) missing, and the oracle sees them"
    else                       bad "rollcallmut: the flush removed and every number STILL answered — the oracle cannot see that defect"; fi
    if [ "$2" -gt 0 ]; then ok "rollcallmut: text and newline as two operations let $2 line(s) carry another — and the oracle sees them"
    else                    bad "rollcallmut: two operations per line and NO line carried another — the oracle cannot see that defect"; fi

    build   # leave the tree built from clean sources
}


# ===========================================================================
# handset — who holds the handset hears the keys
# ===========================================================================
#
# The console's ear: the display daemon consumes keyboard Touches only while
# some lane listens, and republishes each key as a Touch on the tag of the
# lane at the top of the listening stack. A program that asks for input takes
# the top; a lane that closes — its owner died or left — is dropped, and the
# one beneath hears again. Nothing is asked of a clock, and nothing typed is
# lost: keys pressed before anybody listens stay banked in the daemon's ring
# until somebody does.
#
# Three facts, one boot:
#   1. a child (handset) hears the line typed AFTER it asked;
#   2. the shell hears again once the child is gone (it runs `hw`);
#   3. a child hears the line typed BEFORE it asked (type-ahead banked), the
#      line typed while it hears the keyboard itself and holds no ear.
handset_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3

    # 1 + 2: ask, then the line; then `hw` for the shell.
    type_cmd "handset"; sleep 1
    key_press ret
    wait_for_line build/serial.log "\[HANDSET\] ask" "$WAIT_QUIET_S"
    sleep 1
    type_cmd "after the ask"; sleep 1
    key_press ret
    wait_for_line build/serial.log "\[HANDSET\] got: after the ask" "$WAIT_QUIET_S"
    sleep 2
    type_cmd "hw"; sleep 1
    key_press ret
    wait_for_line build/serial.log "TSC freq" "$WAIT_QUIET_S"
    sleep 2

    # 3: the line typed before the ask. `handset banked` hears the keyboard
    # itself and says "[HANDSET] not listening"; the words are typed only then,
    # and it asks for the ear only once it has heard their Enter — so they were
    # typed while nobody listened, as a fact. Typed straight after the spawn
    # they raced it, and they landed after the ask: the leg tested nothing, and
    # its mutant could not turn it. The words go as RAW keystrokes: `type`
    # waits for each character's echo and retypes what was not echoed, and a
    # key that nobody echoes because nobody listens is exactly the key this
    # case is about — a witness would type it again once the child listens and
    # hide the loss.
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "handset banked"; sleep 1
    key_press ret
    wait_for_line build/serial.log "\[HANDSET\] not listening" "$WAIT_QUIET_S" $((MARK + 1))
    local k
    for k in b e f o r e spc t h e spc a s k ret; do
        mon_raw "sendkey $k 30"
        sleep 0.03
    done
    wait_for_line build/serial.log "\[HANDSET\] got:" "$REACT_QUIET_S" $((MARK + 1))
    sleep 2

    tr -d '\r' < build/serial.log > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_handset() {
    echo "== handset: the child hears, the shell hears again, nothing typed early is lost =="
    build STRICT=on
    if ! handset_boot handset; then bad "handset: never reached a shell"; return; fi
    L="$SCRATCH/serial.handset.log"

    grep -q "\[HANDSET\] got: after the ask" "$L" \
        && ok "handset: the child heard the line typed after it asked" \
        || bad "handset: the child never got the line typed after it asked"

    # The shell heard again: `hw` ran after the first handset was gone.
    sed -n '/\[HANDSET\] got: after the ask/,$p' "$L" | grep -q "TSC freq" \
        && ok "handset: the shell heard again once the child was gone (hw ran)" \
        || bad "handset: after the child died the shell never heard hw"

    grep -q "\[HANDSET\] got: before the ask" "$L" \
        && ok "handset: the line typed before the ask was banked and heard" \
        || bad "handset: the line typed before the ask was lost"
}

# The oracle measured against itself: the daemon consumes keys while nobody
# listens — the very thing "keys wait at the daemon for the next reader"
# forbids — so the line typed before the child asked is gone.
handset_deaf_on() {
    mutate src/userspace/display/display.c
    python3 - <<'EOF'
p = "src/userspace/display/display.c"
s = open(p).read()
anchor = "    while (g_ear) {\n        if (!g_key_in_hand) {\n"
assert s.count(anchor) == 1, "handset mutation anchor missing"
s = s.replace(anchor, "    for (;;) {   /* logcheck mutation: consumed while nobody listens */\n"
                      "        if (!g_ear) { if (!touch_try_pop_tag(g_kb, &g_key)) break; did = true; continue; }\n"
                      "        if (!g_key_in_hand) {\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/display/display.c "logcheck mutation"
}

handset_deaf_off() {
    restore src/userspace/display/display.c
}

run_handsetmut() {
    echo "== handsetmut: keys consumed while nobody listens, and the early line must be lost =="
    handset_deaf_on; build STRICT=on
    handset_boot handsetmut; local booted=$?
    handset_deaf_off
    if [ $booted -ne 0 ]; then bad "handsetmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.handsetmut.log"

    if grep -q "\[HANDSET\] got: before the ask" "$L"; then
        bad "handsetmut: keys consumed while nobody listened and the early line STILL arrived — the oracle cannot see that defect"
    else
        ok "handsetmut: keys consumed while nobody listened lost the early line — and the oracle sees it"
    fi

    build   # leave the tree built from clean sources
}


# ===========================================================================
# brigade — one wake for more sleepers than any tray
# ===========================================================================
#
# SysAddrWake used to claim its waiters into a stack array of 256 and stop
# when it was full: a notify_all with more sleepers than that woke 256 and
# left the rest asleep for ever, and nothing said so. The claimed entries now
# ride to their delivery on their own link, so there is nothing to size.
#
# strandpark part 3 is the instrument: 300 strands park on one word with no
# deadline, main learns of every park from the kernel's own strand:parked
# Touch, then changes the word and wakes all parked on it — once. Every
# sleeper must come back.
# ===========================================================================
# draft — what was typed is what the volume holds, and a shorter save leaves
#         no tail of the longer one behind it
# ===========================================================================
#
# TagFS writes only ever GREW a file: rewriting one shorter left the old tail
# readable behind the new content, and every shorter rewrite in the system was
# silently wrong until a file could be cut (file_span). An editor is where that
# shows first and worst — a line deleted and saved would come back on the next
# read.
#
# draft is the instrument, and the whole path is under test: a key becomes a
# Touch, the Touch becomes a byte in the book, ^S writes the book, spans the
# file to its length and anchors it, and `show` reads the volume back. Ten bytes
# are typed and saved; five are taken back and saved again; the volume must
# hold five.
draft_keys() {
    # draft_keys KEY...  — straight to the monitor, no echo to wait for: a
    # painted frame never enters the log ring, so qemu-input's `type` (which
    # verifies the guest echo) has nothing to see and would give up.
    local k
    for k in "$@"; do
        mon_raw "sendkey $k 30"
        sleep 0.06
    done
}

draft_session() {
    # draft_session NAME "RUN-BG ARGS" — boots, drives one editing session,
    # then reads the file back with `show`. Leaves the transcript in
    # $SCRATCH/serial.NAME.log and the machine stopped.
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)

    type_cmd "draft oracle.txt"
    sleep 1
    key_press ret
    sleep 3

    draft_keys a b c d e f g h i j
    draft_keys ctrl-s
    sleep 2
    draft_keys backspace backspace backspace backspace backspace
    draft_keys ctrl-s
    sleep 2
    draft_keys ctrl-q
    sleep 3

    type_cmd "show oracle.txt"
    sleep 1
    key_press ret
    sleep 3

    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_draft() {
    echo "== draft: ten bytes typed and saved, five taken back and saved again =="
    build STRICT=on
    if ! draft_session draft "STRICT=on CORES=4 MEM=4G"; then
        bad "draft: never reached a shell"; return
    fi
    L="$SCRATCH/serial.draft.log"
    grep -q "\[DRAFT\] oracle.txt - 1 lines, 5 bytes, saved 2 times" "$L" \
        && ok "draft: the editor says five bytes over two saves" \
        || bad "draft: $(grep -m1 '\[DRAFT\]' "$L" || echo 'the editor said nothing at all')"
    if grep -q "^abcde$" "$L"; then
        ok "draft: the volume holds exactly what was left on the screen"
    elif grep -q "abcdefghij" "$L"; then
        bad "draft: the old tail is still on the volume - the shorter save did not cut it"
    else
        bad "draft: the volume did not answer with the draft at all"
    fi
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "draft: the kernel spoke of a stall or fault" \
        || ok "draft: no verdict, no panic"
}

draft_notail_on() {
    mutate src/userspace/utils/draft_file.c
    python3 - <<'EOF2'
p = "src/userspace/utils/draft_file.c"
s = open(p).read()
anchor = "    int rc = file_span(g_fid, len, 0);"
assert s.count(anchor) == 1, "draft mutation anchor missing"
s = s.replace(anchor, "    int rc = 0;   /* logcheck mutation: the tail is left where it was */\n    (void)len;", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/utils/draft_file.c "logcheck mutation"
}

draft_notail_off() {
    restore src/userspace/utils/draft_file.c
}

run_draftmut() {
    echo "== draftmut: the span taken out, and the old tail must come back =="
    draft_notail_on; build STRICT=on
    draft_session draftmut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    draft_notail_off
    if [ $booted -ne 0 ]; then bad "draftmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.draftmut.log"
    if grep -q "abcdefghij" "$L"; then
        ok "draftmut: without the cut the old tail is read back - and the oracle sees it"
    else
        bad "draftmut: the span was taken out and the file was STILL five bytes - the oracle cannot see that defect ($(grep -m1 'abcde' "$L" || echo 'no content at all'))"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# paint — a whole frame of colour reaches the glass as ONE op, and says
#         nothing into the log ring on the way
# ===========================================================================
#
# Before HW_VGA_PAINT the only way to colour a cell was HW_VGA_PUTCHAR, which
# is one Manifest op per cell AND one LogRingPut per cell — a global spinlock
# and a wire kick for every one of sixteen thousand cells, twenty-five times a
# second, burying everything the machine actually says under a screenful of
# spaces. PAINT lays a rectangle down in one op, one Canvas commit, one blit,
# and deliberately does not mirror: a picture is not speech.
#
# playtime is the instrument: it paints every cell of the screen every frame
# and stops on a key. The screendump must show a painted screen (a shell
# prompt is black almost everywhere), and the run must have said nothing but
# its own one line.
paint_shot_verdict() {
    # paint_shot_verdict PPM — prints "painted N" or "bare N", N = the
    # percentage of sampled pixels that are not the console's black.
    python3 - "$1" <<'EOF2'
import sys
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
lit = total = 0
for y in range(0, h, 4):
    for x in range(0, w, 4):
        i = (y * w + x) * 3
        total += 1
        if px[i:i+3] != b'\x00\x00\x00':
            lit += 1
pct = (100 * lit) // max(total, 1)
print(("painted " if pct >= 80 else "bare ") + str(pct))
EOF2
}

paint_boot() {
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "playtime"
    sleep 1
    key_press ret
    # ‼ THERE IS NOTHING IN THE LOG TO WAIT FOR HERE, AND THAT IS THE SCENARIO.
    # A painted frame says nothing into the ring — proving that is half of what
    # this measures — so the fact has to be read off the GLASS instead of the
    # log: take the screendump, and if it is not a painted screen yet, take
    # another. `playtime` keeps pouring until a key stops it, so a shot can
    # only ever be too EARLY, which is the one way a flat six seconds could
    # fail and did not announce itself.
    local shot=0
    while [ $shot -lt "$PAINT_SHOT_TRIES" ]; do
        mon_shot "$SCRATCH/$name.ppm"
        case "$(paint_shot_verdict "$SCRATCH/$name.ppm" 2>/dev/null)" in
            painted*) break ;;
        esac
        sleep 1; shot=$((shot+1))
    done
    sleep 1
    key_press spc
    sleep 3
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_paint() {
    echo "== paint: one op lays a whole screen of colour, and the log stays quiet =="
    build STRICT=on
    if ! paint_boot paint "UEFI=on STRICT=on CORES=4 MEM=4G"; then
        bad "paint: never reached a shell"; return
    fi
    L="$SCRATCH/serial.paint.log"
    V=$(paint_shot_verdict "$SCRATCH/paint.ppm")
    case "$V" in
        painted*) ok "paint: the screendump is a painted screen ($V per cent lit)" ;;
        *)        bad "paint: the screen was not painted ($V per cent lit)" ;;
    esac
    FR=$(grep -oE "\[PLAYTIME\] [0-9]+ frames" "$L" | grep -oE "[0-9]+" | head -1)
    [ -n "$FR" ] && [ "$FR" -gt 10 ] \
        && ok "paint: $FR frames were laid down and a key stopped them" \
        || bad "paint: the pour said '$(grep -m1 '\[PLAYTIME\]' "$L" || echo nothing)'"
    # A painted cell must not enter the log ring. The whole session is allowed
    # its own lines; a paint that mirrored would put thousands of spaces here.
    SP=$(tr -d '\n' < "$L" | tr -dc ' ' | wc -c | tr -d ' ')
    [ "$SP" -lt 4000 ] \
        && ok "paint: the frames said nothing into the log ring ($SP spaces in the whole session)" \
        || bad "paint: the log ring carries $SP spaces - the frames are being mirrored into it"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "paint: the kernel spoke of a stall or fault" \
        || ok "paint: no verdict, no panic"
}

paint_blind_on() {
    mutate src/kernel/core/decks/hardware/hardware_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/hardware/hardware_ops.c"
s = open(p).read()
anchor = "    console_lock_acquire();\n    bool fit = VideoPaintCells(row, col, height, width, cells);\n    console_lock_release();"
assert s.count(anchor) == 1, "paint mutation anchor missing"
s = s.replace(anchor, "    bool fit = true;   /* logcheck mutation: the op takes the frame and drops it */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/hardware/hardware_ops.c "logcheck mutation"
}

paint_blind_off() {
    restore src/kernel/core/decks/hardware/hardware_ops.c
}

run_paintmut() {
    echo "== paintmut: the op answers OK and paints nothing, and the screen must stay bare =="
    paint_blind_on; build STRICT=on
    paint_boot paintmut "UEFI=on STRICT=on CORES=4 MEM=4G"; local booted=$?
    paint_blind_off
    if [ $booted -ne 0 ]; then bad "paintmut: never reached a shell"; build; return; fi
    V=$(paint_shot_verdict "$SCRATCH/paintmut.ppm")
    case "$V" in
        bare*) ok "paintmut: nothing was painted and the oracle sees it ($V per cent lit)" ;;
        *)     bad "paintmut: the op dropped every frame and the screen was STILL painted ($V) - the oracle cannot see that defect" ;;
    esac
    build   # leave the tree built from clean sources
}

# ===========================================================================
# finish — the crew of a tag is named, the machine's own cabins are held back,
#          and the word `anyway` really ends one
# ===========================================================================
#
# `broadcast` has always walked the carriers of a tag to speak to them; until
# system.proc.crew there was no way to ASK who they are, and killing meant
# knowing a pid — a seat, which is re-let. finish ends the crew of a tag by
# (pid, generation) and refuses, without the word `anyway`, to end a cabin
# wearing the bare `system` tag: the shell, the display daemon and itself.
#
# The display daemon is the instrument, because ending it is survivable — the
# lane dies, boxlib says so and falls back to writing the glass directly, and
# the shell keeps answering. So all three answers are provable on one boot.
finish_boot() {
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    local cmd
    for cmd in "finish nobody" "finish display" "finish display anyway" "me"; do
        type_cmd "$cmd"
        sleep 1
        key_press ret
        sleep 4
    done
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_finish() {
    echo "== finish: nobody wears it, the daemon is held back, and anyway ends it =="
    build STRICT=on
    if ! finish_boot finish "STRICT=on CORES=4 MEM=4G"; then
        bad "finish: never reached a shell"; return
    fi
    L="$SCRATCH/serial.finish.log"
    grep -q "nobody: nothing wears that tag" "$L" \
        && ok "finish: a tag nobody wears is said plainly" \
        || bad "finish: an unworn tag was not reported"
    grep -q "wears system - this machine runs on it" "$L" \
        && ok "finish: the daemon is named and held back" \
        || bad "finish: the guard over a system cabin did not speak"
    grep -q "if that is what you want: finish display anyway" "$L" \
        && ok "finish: the way past the guard is spelled out" \
        || bad "finish: the guard did not say how to override it"
    grep -q "^nothing ended$" "$L" \
        && ok "finish: nothing was ended while the guard stood" \
        || bad "finish: something was ended although the guard spoke"
    grep -q "Processes: 2 live" "$L" \
        && ok "finish: after the word anyway the daemon is gone and the shell still answers" \
        || bad "finish: the daemon survived the word anyway, or the shell stopped answering ($(grep -m1 'Processes:' "$L" || echo 'no answer at all'))"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "finish: the kernel spoke of a stall or fault" \
        || ok "finish: no verdict, no panic"
}

finish_unguarded_on() {
    mutate src/userspace/utils/finish.c
    python3 - <<'EOF2'
p = "src/userspace/utils/finish.c"
s = open(p).read()
anchor = "        if (!anyway && wears(crew->mates[i].tags, SYSTEM_TAG)) {"
assert s.count(anchor) == 1, "finish mutation anchor missing"
s = s.replace(anchor, "        if (false && wears(crew->mates[i].tags, SYSTEM_TAG)) {   /* logcheck mutation: the guard is gone */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/utils/finish.c "logcheck mutation"
}

finish_unguarded_off() {
    restore src/userspace/utils/finish.c
}

run_finishmut() {
    echo "== finishmut: the guard taken out, and the daemon must fall to a bare finish =="
    finish_unguarded_on; build STRICT=on
    finish_boot finishmut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    finish_unguarded_off
    if [ $booted -ne 0 ]; then bad "finishmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.finishmut.log"
    if grep -q "wears system - this machine runs on it" "$L"; then
        bad "finishmut: the guard was taken out and STILL spoke - the oracle cannot see that defect"
    else
        ok "finishmut: without the guard nothing is held back - and the oracle sees the silence"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# overtake — a finished program's last words are not overtaken by the prompt
# ===========================================================================
#
# The console daemon merges the lanes by the stamp each frame carries, and the
# stamp is cpu_rdtsc read on whatever core the writer was running on. Cores
# agree about that counter only to within what the boot-time sync left them
# (per_core.c tolerates four microseconds), and a program that lists a volume
# pushes its whole output in less time than that. So the stamps of one burst
# and the stamp of a line printed afterwards ON ANOTHER CORE are not reliably
# ordered — and the shell's next prompt landed INSIDE the listing of the
# command that had just finished.
#
# MEASURED on the board: four listings out of twelve cut open at exactly the
# same byte, 891 of 4235. The same byte every time is what a FIXED offset
# between two cores looks like; a race would have scattered. Never once in an
# emulator, where the cores agree exactly — which is why this scenario has to
# MAKE them disagree.
#
# The injection is one writer's clock running ahead from its eleventh word on.
# The prompt, stamped normally, is then older than the rest of that writer's
# burst and the merge puts it in the middle — unless the order comes from the
# death instead of from the clock, which is the fix under test.

# ‼ TWO CONDITIONS ARE NEEDED TO SEE THIS, AND THE BOARD HAS BOTH: the cores
# disagree about the counter, AND the daemon is behind the writer. An emulator
# has neither — the first attempt at this scenario injected only the skew and
# the listings came out whole, because the daemon had drained every frame long
# before the shell woke up. So the second condition is injected too, by making
# one render cost what a render costs on a real framebuffer.
overtake_slow_on() {
    mutate src/userspace/display/display.c
    python3 - <<'EOF2'
p = "src/userspace/display/display.c"
s = open(p).read()
a = "    if (f->kind != CONSOLE_RUN_TEXT) return;"
assert s.count(a) == 1, "overtake slow anchor missing"
inject = "\n    /* logcheck injection: a render costs what one costs on glass. */"
inject += "\n    for (volatile uint32_t z = 0; z < 400000u; z++) { }"
s = s.replace(a, a + inject, 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/display/display.c "logcheck injection"
}

overtake_slow_off() {
    restore src/userspace/display/display.c
}

overtake_clockonly_on() {
    mutate src/userspace/boxlib/src/print.c
    python3 - <<'EOF2'
p = "src/userspace/boxlib/src/print.c"
s = open(p).read()
a = "    return __atomic_add_fetch(ps->order, 1u, __ATOMIC_SEQ_CST);"
assert s.count(a) == 1, "overtake mutation anchor missing"
m  = "    /* logcheck mutation: the clock decides again, and two cabins' clocks"
m += "\n     * differ by a fixed amount — which is exactly what an imperfect TSC"
m += "\n     * sync leaves behind. The shell is pid 2; what it spawns has a higher"
m += "\n     * one, so this is two processes whose counters disagree. */"
m += "\n    { static uint32_t n;"
m += "\n      uint64_t t = cpu_rdtsc();"
m += "\n      if (cabin_info()->pid > 2u && ++n > 10u) t += 4000000000ull;"
m += "\n      return t; }"
s = s.replace(a, m, 1)
if "box/core/cabin.h" not in s:
    s = s.replace('#include "box/bay.h"', '#include "box/bay.h"\n#include "box/core/cabin.h"', 1)
if "box/cpu.h" not in s:
    s = s.replace('#include "box/bay.h"', '#include "box/cpu.h"\n#include "box/bay.h"', 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/boxlib/src/print.c "logcheck mutation"
}

overtake_clockonly_off() {
    restore src/userspace/boxlib/src/print.c
}

overtake_listing() {
    # overtake_listing NAME — boot, list the volume three times, and report the
    # shortest run of listing that reached the log before a prompt did.
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1; return 1
    fi
    sleep 3
    # Three listings, each driven off two facts and no clock. A listing has
    # STARTED when the utility prints its own heading; it is OVER when the log
    # stops growing. The gap between "the prompt is back" and "the burst has
    # drained" is the whole subject of this scenario, so the second fact has to
    # be the drain and not the prompt.
    local n MARK
    for n in 1 2 3; do
        MARK=$(wc -l < build/serial.log)
        type_cmd "files"
        key_press ret
        wait_for_line build/serial.log "Files:" "$WAIT_QUIET_S" $((MARK + 1))
        wait_for_quiet "$OVERTAKE_DRAIN_QUIET_S"
    done
    cp build/serial.log "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

# ‼ 4000 WAS A NUMBER TYPED IN ON A DAY THIS VOLUME HELD THE FILES IT HELD.
#
# The old verdict was "the shortest listing is at least 4000 bytes long", which
# is a statement about how many programs happen to be on the medium, not about
# whether a prompt cut one open. Add a file and the whole listing grows; remove
# two and a WHOLE listing drops under the line and the oracle reddens against a
# kernel with nothing wrong with it.
#
# The exact fact is in the run itself and needs no constant at all. A listing
# the prompt did NOT cut has that prompt at its end: nothing of the listing
# follows it. A listing the prompt landed inside has the rest of its ROWS
# stranded after it — which is precisely what was measured on the board, 891
# bytes of 4235 and the other 3344 arriving after the prompt.
#
# So: one line per listing, "<bytes before the prompt> <listing rows after it>".
# A second number of 0 is a whole listing, whatever the volume holds today; a
# positive one is a cut and says how much was stranded; -1 means no prompt ever
# followed the listing at all, which is a third thing and not a pass.
overtake_cuts() {
    python3 - "$1" <<'EOF2'
import re, sys
t = open(sys.argv[1], "rb").read().replace(b"\r", b"")
row = re.compile(rb"(?m)^[!-~]+ +\[")
starts = [m.end() for m in re.finditer(rb"Files:\n", t)]
for k, s in enumerate(starts):
    end = starts[k + 1] if k + 1 < len(starts) else len(t)
    body = t[s:end]
    n = body.find(b"\n~ ")
    if n < 0:
        print("%d -1" % len(body))
        continue
    # The prompt is two bytes with no newline after it, so whatever the merge
    # put next lands on the prompt's OWN line. Drop the "~ " and the rest of
    # that line is read like any other, or a listing stranded by exactly one
    # row would be counted as a listing that was never cut.
    after = body[n + 1:]
    if after.startswith(b"~ "):
        after = after[2:]
    print("%d %d" % (n, len(row.findall(after))))
EOF2
}

# The three numbers every caller of overtake_cuts wants, read once.
overtake_tally() {
    OT_ROWS=$(overtake_cuts "$1")
    OT_COUNT=$(printf '%s\n' "$OT_ROWS" | grep -c '[0-9]')
    OT_CUT=$(printf '%s\n' "$OT_ROWS"   | awk '$2 > 0 { n++ } END { print n+0 }')
    OT_NOPROMPT=$(printf '%s\n' "$OT_ROWS" | awk '$2 < 0 { n++ } END { print n+0 }')
    OT_SHORTEST=$(printf '%s\n' "$OT_ROWS" | awk 'NF { if (m == "" || $1 < m) m = $1 } END { print (m == "" ? 0 : m) }')
}

run_overtake() {
    echo "== overtake: a finished program's last words come before the next prompt =="
    overtake_slow_on; build STRICT=on
    local booted=1
    overtake_listing overtake && booted=0
    overtake_slow_off
    if [ $booted -ne 0 ]; then bad "overtake: never reached a shell"; build; return; fi

    overtake_tally "$SCRATCH/serial.overtake.log"
    if [ "$OT_COUNT" -lt 3 ]; then
        bad "overtake: only $OT_COUNT of 3 listings reached the log — the machine was not asked what this scenario means to ask"
    elif [ "$OT_NOPROMPT" -gt 0 ]; then
        bad "overtake: $OT_NOPROMPT listing(s) were never followed by a prompt at all"
    elif [ "$OT_CUT" -gt 0 ]; then
        bad "overtake: a prompt cut $OT_CUT of $OT_COUNT listings open (the shortest ran $OT_SHORTEST bytes before it) — the clock is still deciding the order"
    else
        ok "overtake: all $OT_COUNT listings reached the log whole ($OT_SHORTEST bytes) before their prompt, with the daemon left far behind the writer"
    fi
    build
}

run_overtakemut() {
    echo "== overtakemut: with the clock deciding again, the prompt must cut in =="
    overtake_clockonly_on; overtake_slow_on; build STRICT=on
    local booted=1
    overtake_listing overtakemut && booted=0
    overtake_clockonly_off; overtake_slow_off
    if [ $booted -ne 0 ]; then bad "overtakemut: never reached a shell"; build; return; fi

    overtake_tally "$SCRATCH/serial.overtakemut.log"
    if [ "$OT_COUNT" -lt 3 ]; then
        bad "overtakemut: only $OT_COUNT of 3 listings reached the log — the scenario proved nothing either way"
    elif [ "$OT_CUT" -gt 0 ]; then
        ok "overtakemut: a prompt cut $OT_CUT of $OT_COUNT listings open after $OT_SHORTEST bytes, stranding rows behind it, and the oracle sees it"
    else
        bad "overtakemut: the death was taken out of the order and every listing was STILL whole - the oracle cannot see that defect"
    fi
    build
}

# ===========================================================================
# orderlife — a Bay window outlives the strand that opened it
# ===========================================================================
#
# The console's order is a counter in a Bay, and every printing strand maps it.
# The question this asks is not about the console at all: it is whether a Bay
# window survives the STRAND that opened it, because the window lives in the
# CABIN's address space and every sibling runs in that same space.
#
# It used to not survive. A Bay claim belonged to the strand, and the cleanup
# on strand death unmapped it — out from under siblings still reading through
# it. The console found it: one shared order pointer, opened by whichever
# strand printed first, vanished for all of them when that strand finished.
#
# ‼ AND THE ORDINARY SHAPE HIDES IT, which is why this scenario exists rather
# than trusting `lines`. print_stress prints its banner from the MAIN strand
# before it spawns anybody, so the main strand opens the order and outlives
# every worker, and nothing ever faults. Remove that one line and the first
# strand to print is a worker — one that runs its two thousand lines and then
# exits while fifteen siblings are still printing. That is the shape, and it
# is one line of difference from a program that has passed for months.
#
# The scenario keeps that line removed for BOTH halves, so the two runs differ
# only by the defect under test and not by what the program does.
orderlife_shape_on() {
    mutate src/userspace/apps/print_stress.c
    python3 - <<'EOF2'
p = "src/userspace/apps/print_stress.c"
s = open(p).read()
a = """    printf("[PS] print_stress starting: %u strands x %u iters each\\n",
           STRAND_COUNT, ITERS);
    io_flush();
"""
assert s.count(a) == 1, "orderlife shape anchor missing"
s = s.replace(a, """    /* logcheck mutation: the banner is gone, so the first strand to print is
     * a spawned worker and not main. */
""", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/apps/print_stress.c "logcheck mutation"
}

orderlife_shape_off() { restore src/userspace/apps/print_stress.c; }

# The defect, put back in one line: let ANY strand's death take the whole
# cabin's Bay windows, which is what strand-scoped claims did to a sibling's
# window and is the class this is guarding. The real rule is that only the
# cabin's own teardown may do this (cabin.c, cabin_destroy).
orderlife_strandkill_on() {
    mutate src/kernel/core/process/process.c
    python3 - <<'EOF2'
p = "src/kernel/core/process/process.c"
s = open(p).read()
a = "    BrookCleanupStrand(proc);"
assert s.count(a) == 1, "orderlife strandkill anchor missing"
m  = "    /* logcheck mutation: a strand's death takes the cabin's Bay windows,"
m += "\n     * which is what strand-scoped claims did to a sibling's. */"
m += "\n    if (proc->cabin) BayCleanupCabin(proc->cabin);"
m += "\n" + a
s = s.replace(a, m, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/process/process.c "logcheck mutation"
}

orderlife_strandkill_off() { restore src/kernel/core/process/process.c; }

# orderlife_run NAME — boot sixteen cores, run print_stress, and report what
# reached the log. Returns 1 if the machine never reached a shell at all.
orderlife_run() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1; return 1
    fi
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "print_stress"
    key_press ret
    wait_for_line build/serial.log "PS SUMMARY" "$WAIT_QUIET_S" $((MARK + 1))
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_orderlife() {
    echo "== orderlife: the strand that opened the console's order finishes first, and the rest keep printing =="
    orderlife_shape_on; build STRICT=on
    local booted=1
    orderlife_run orderlife && booted=0
    orderlife_shape_off
    if [ $booted -ne 0 ]; then bad "orderlife: never reached a shell"; build; return; fi

    local WHOLE
    WHOLE=$(grep -cE '^\[PS-[0-9]{2}\] [0-9]{8}$' "$SCRATCH/serial.orderlife.log" | tr -d ' ')
    echo "     $WHOLE of 32000 lines whole"
    if grep -q "PS SUMMARY.*PASS" "$SCRATCH/serial.orderlife.log"; then
        ok "orderlife: every strand printed to the end though the first one to print had long finished"
    else
        bad "orderlife: print_stress never finished — a worker printed first and the survivors lost something"
    fi
    if grep -qE "PAGE FAULT|#PF|PANIC" "$SCRATCH/serial.orderlife.log"; then
        bad "orderlife: the run faulted"
    else
        ok "orderlife: and nothing faulted"
    fi
    build
}

run_orderlifemut() {
    echo "== orderlifemut: let a strand's death take the cabin's windows, and require the oracle to notice =="
    orderlife_shape_on; orderlife_strandkill_on; build STRICT=on
    local booted=1
    orderlife_run orderlifemut && booted=0
    orderlife_shape_off; orderlife_strandkill_off
    if [ $booted -ne 0 ]; then
        ok "orderlifemut: the machine could not even reach a shell with windows pulled from under it, and the oracle sees it"
        build; return
    fi

    if grep -q "PS SUMMARY.*PASS" "$SCRATCH/serial.orderlifemut.log" \
       && ! grep -qE "PAGE FAULT|#PF|PANIC" "$SCRATCH/serial.orderlifemut.log"; then
        bad "orderlifemut: a strand's death took the cabin's Bay windows and print_stress finished anyway - the oracle cannot see that defect"
    else
        ok "orderlifemut: the survivors lost the window under them, and the oracle sees it"
    fi
    build
}

# ===========================================================================
# batchfail — one refused op costs its own cell, and nothing behind it
# ===========================================================================
#
# A Manifest stops at its first refusal: manifest_exec.c breaks out of the
# dispatch loop on an op that fails and is not marked OPTIONAL, so every op
# BEHIND it never runs. That is right for a Manifest that is one transaction
# and wrong for the display daemon's, which packs a whole burst of output into
# one batch — a single cell the kernel would not draw took the entire rest of
# the burst with it, silently, up to and including the prompt printed at the
# end. Reported from the board: after `files` there was no prompt, after `me`
# there was, over and over.
#
# The instrument has to MAKE an op refuse, because on a desk nothing does. One
# PUTSTRING in the middle of the run is made to fail, and the question is what
# reaches the log ring afterwards — the ring is written by the op as it prints,
# so it is a faithful witness of what was EXECUTED, not of what was asked.
#
#   with the ops OPTIONAL : one line of output is missing and everything after
#                           it is there, prompt included
#   without               : the tail of the burst is gone

batchfail_inject_on() {
    mutate src/kernel/core/decks/hardware/hardware_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/hardware/hardware_ops.c"
s = open(p).read()
a = """    uint32_t fg    = hw_color_param(&op->params[0]);
    uint32_t bg    = hw_color_param(&op->params[4]);
    uint8_t  flags = op->params[8];"""
assert s.count(a) == 1, "batchfail inject anchor missing"
s = s.replace(a, """    /* logcheck injection: one run in sixteen refuses.
     * ‼ A REFUSAL COUNTED FROM BOOT IS NOT A REFUSAL INSIDE THE BURST. The
     * first try failed exactly once, at the tenth run of the session — which
     * is somewhere in the boot banner, hundreds of runs before the listing
     * this scenario measures. The mutation caught it: the tail arrived with
     * the ops made non-optional, because nothing had refused inside that
     * batch at all. It has to keep refusing, so that a burst of seventy runs
     * is certain to contain one. */
    { static uint32_t n; if ((++n % 16u) == 0) return ERR_INVALID_ARGUMENT; }
""" + a, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/hardware/hardware_ops.c "logcheck injection"
}

batchfail_inject_off() {
    restore src/kernel/core/decks/hardware/hardware_ops.c
}

batchfail_optional_off() {
    mutate src/userspace/boxlib/src/vga.c
    python3 - <<'EOF2'
p = "src/userspace/boxlib/src/vga.c"
s = open(p).read()
a = "    return ManifestBuilderAddOp(&s_mb, DECK_HARDWARE, opcode, OP_FLAG_OPTIONAL,"
assert s.count(a) == 1, "batchfail optional anchor missing"
s = s.replace(a, "    return ManifestBuilderAddOp(&s_mb, DECK_HARDWARE, opcode, 0,   /* logcheck mutation */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/boxlib/src/vga.c "logcheck mutation"
}

batchfail_optional_on() {
    restore src/userspace/boxlib/src/vga.c
}

batchfail_run() {
    # batchfail_run NAME — boot, list the volume, keep the window of the log.
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1; return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "files"
    key_press ret
    # Started when the utility prints its heading, over when the log stops
    # growing. A flat six seconds was a guess that would cut the very tail this
    # scenario is about, on any host slower than the one it was written on.
    wait_for_line build/serial.log "Files:" "$WAIT_QUIET_S" $((MARK + 1))
    wait_for_quiet "$OVERTAKE_DRAIN_QUIET_S"
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$1.log"
    # The mount line is in the BOOT, before the window — and it is the only
    # place the guest states how many files the volume holds, which is what the
    # listing below is measured against.
    tr -d '\r' < build/serial.log > "$SCRATCH/serial.$1.full.log"
    make run-stop >/dev/null 2>&1
    return 0
}

# How many files the guest itself says the volume holds, and how many listing
# rows arrived. A row is a filename, spaces, then the bracket that opens its
# tags — the shape files.c prints and nothing else in the log has.
batchfail_tally() {
    BF_TOTAL=$(grep -oE "MOUNTED — [0-9]+ file" "$2" | tail -1 | grep -oE "[0-9]+")
    BF_ROWS=$(grep -cE "^[^ ]+ +\[" "$1")
}

run_batchfail() {
    echo "== batchfail: a refused op costs its own cell and nothing behind it =="
    batchfail_inject_on; build STRICT=on
    local booted=1
    batchfail_run batchfail && booted=0
    batchfail_inject_off
    if [ $booted -ne 0 ]; then bad "batchfail: never reached a shell"; build; return; fi

    local L="$SCRATCH/serial.batchfail.log"
    batchfail_tally "$L" "$SCRATCH/serial.batchfail.full.log"

    # ‼ 50 WAS A NUMBER TYPED IN AGAINST THE FILES THIS VOLUME HELD THAT DAY.
    #
    # The quantity is now taken from the run: the guest states how many files
    # it mounted, and the listing is measured against that. The line between
    # the two behaviours is not narrow and does not need to be guessed — with
    # the ops OPTIONAL a refusal costs its own cell and the burst carries on,
    # so nearly every row arrives; without, each batch dies at its first
    # refusal and roughly half the listing is gone. Measured on this listing:
    # 60 rows against 30. Half the volume's own count sits between them and
    # moves with the volume, which a typed-in 50 could not.
    #
    # Naming a PARTICULAR row instead would be too strict: with one run in
    # sixteen refused the casualty can be any row, the last one included.
    if [ -z "$BF_TOTAL" ]; then
        bad "batchfail: the mount never said how many files the volume holds — the harness has nothing to measure the listing against"
    else
        [ "$BF_ROWS" -gt $((BF_TOTAL / 2)) ]
        chk $? "batchfail: $BF_ROWS of the volume's $BF_TOTAL files reached the log with one run in sixteen refused"
    fi

    grep -qE "^~" "$L"
    chk $? "batchfail: and the prompt printed after the burst"

    build   # leave the tree built from clean sources
}

run_batchfailmut() {
    echo "== batchfailmut: with the ops not optional, the tail must disappear =="
    batchfail_inject_on; batchfail_optional_off; build STRICT=on
    local booted=1
    batchfail_run batchfailmut && booted=0
    batchfail_inject_off; batchfail_optional_on
    if [ $booted -ne 0 ]; then bad "batchfailmut: never reached a shell"; build; return; fi

    local L="$SCRATCH/serial.batchfailmut.log"
    batchfail_tally "$L" "$SCRATCH/serial.batchfailmut.full.log"
    if [ -z "$BF_TOTAL" ]; then
        bad "batchfailmut: the mount never said how many files the volume holds — the harness has nothing to measure the listing against"
    elif [ "$BF_ROWS" -gt $((BF_TOTAL / 2)) ]; then
        bad "batchfailmut: the ops were made non-optional and $BF_ROWS of $BF_TOTAL rows STILL arrived - the oracle cannot see that defect"
    else
        ok "batchfailmut: the refusal took the rest of the burst with it — $BF_ROWS of the volume's $BF_TOTAL files reached the log, and the oracle sees it"
    fi
    build
}

# ===========================================================================
# caret — there is one cursor on the screen, and it is where the program put it
# ===========================================================================
#
# The caret is the part of a console a person actually watches, and until this
# was measured nobody could say where it was: the serial mirror carries the
# TEXT a program said and says nothing at all about the cursor. So this reads
# the glass itself. A screendump is 8x16 cells of pixels; the GOP backend draws
# the caret as two solid rows at the bottom of a cell, and the 8x16 font is
# right here in the tree, so a cell can be told back into the character it
# carries AND whether a caret is standing on it.
#
# Two things are checked, and they are different defects:
#   ONE caret — a caret is pixels inside the surface, so a scroll carries it up
#               with the text; erasing it at the row it was PUT in leaves a
#               ghost on the row it moved to, one per scroll, climbing the
#               screen. MEASURED 2026-09-10: three presses of Enter left four.
#   WHERE it is — a cursor move is one op, a single-op Manifest is not wrapped
#               in a Canvas batch, and CanvasSetCursor used to change the
#               cursor without committing. Left, Right, Home, End and Backspace
#               moved the insertion point and left the caret behind.

caret_read() {
    # caret_read PPM — prints "carets N at r,c r,c ..." and the last screen row
    # that carries anything, so a verdict can name what it saw.
    python3 - "$1" <<'EOF2'
import re, sys, collections
ROOT = "."
src = open(ROOT + "/src/kernel/drivers/video/font/vga_font.h").read()
body = src[src.index("vga_font_8x16[256][16] = {"):]
rows = re.findall(r"\{([^{}]*)\}", body)
FONT = []
for r in rows[:256]:
    v = [int(x, 0) for x in r.split(",") if x.strip()]
    if len(v) == 16: FONT.append(tuple(v))
BY = {}
TOP = {}
for code in range(255, -1, -1):
    # A cell of nothing is a SPACE, not a NUL: the glyphs are identical and the
    # lower code would win, and then a blank row could not be told from a row
    # of text by trimming it.
    if code == 0: continue
    BY[FONT[code]] = code
    TOP.setdefault(FONT[code][:14], code)

d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
def pix(x, y):
    i = (y * w + x) * 3
    return (px[i], px[i+1], px[i+2])

carets = []
lines = []
for r in range(h // 16):
    line = []
    for c in range(w // 8):
        box = [[pix(c*8 + x, r*16 + y) for x in range(8)] for y in range(16)]
        flat = [p for row in box for p in row]
        cnt = collections.Counter(flat)
        bg = cnt.most_common(1)[0][0]
        bits = tuple(sum(1 << (7-x) for x in range(8) if box[y][x] != bg) for y in range(16))
        ch = BY.get(bits)
        caret = False
        if ch is None:
            if bits[14] == 0xFF and bits[15] == 0xFF:
                ch = TOP.get(bits[:14])
                if ch is not None: caret = True
        elif bits[14] == 0xFF and bits[15] == 0xFF and not any(bits[:14]):
            caret = True; ch = 32
        if ch is None: ch = ord('?')
        if caret: carets.append((r, c))
        line.append(chr(ch) if 32 <= ch < 127 else '.')
    lines.append("".join(line).rstrip())

last = ""
for i, l in enumerate(lines):
    if l: last = "%d|%s" % (i, l)
print("carets %d at %s" % (len(carets), " ".join("%d,%d" % x for x in carets)))
print("last " + last)
EOF2
}

caret_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null
}

caret_run() {
    # caret_run NAME — one scripted editing session; leaves two readings.
    local n=$1
    type_cmd "abcdef"
    sleep 1
    key_press left
    key_press left
    sleep 2
    mon_shot "$SCRATCH/$n.edit.ppm"
    caret_read "$SCRATCH/$n.edit.ppm" > "$SCRATCH/$n.edit.txt" 2>&1

    # then a screenful of output and three scrolls, which is what left ghosts
    key_press home
    key_press delete || true
    key_press ret
    sleep 2
    type_cmd "files"
    key_press ret
    sleep 4
    local i
    for i in 1 2 3; do key_press ret; sleep 1; done
    sleep 2
    mon_shot "$SCRATCH/$n.scroll.ppm"
    caret_read "$SCRATCH/$n.scroll.ppm" > "$SCRATCH/$n.scroll.txt" 2>&1
}

run_caret() {
    echo "== caret: one cursor on the glass, standing where the editor put it =="
    build STRICT=on
    if ! caret_boot; then bad "caret: never reached a shell"; make run-stop >/dev/null 2>&1; return; fi
    caret_run caret
    make run-stop >/dev/null 2>&1

    local E S EN SN
    E=$(head -1 "$SCRATCH/caret.edit.txt")
    S=$(head -1 "$SCRATCH/caret.scroll.txt")
    EN=$(echo "$E" | awk '{print $2}')
    SN=$(echo "$S" | awk '{print $2}')

    [ "$EN" = 1 ]
    chk $? "caret: one cursor while a line is being edited ($E)"

    [ "$SN" = 1 ]
    chk $? "caret: still one after a screenful of output and three scrolls ($S)"

    # Where it stands: `abcdef` typed, then Left twice, so the caret must be on
    # the cell holding 'e' — two back from the end of what was typed.
    local LINE COL WANT
    LINE=$(tail -1 "$SCRATCH/caret.edit.txt")
    COL=$(echo "$E" | awk '{print $4}' | cut -d, -f2)
    WANT=$(echo "$LINE" | sed 's/^last [0-9]*|//' | awk '{ i=index($0,"abcdef"); print (i>0 ? i-1+4 : -1) }')
    if [ -n "$COL" ] && [ "$COL" = "$WANT" ]; then
        ok "caret: it sits on the cell the editor is editing (column $COL of \"$(echo "$LINE" | sed 's/^last [0-9]*|//')\")"
    else
        bad "caret: the editor is two cells back from the end of \"abcdef\" and the caret is at column $COL, not $WANT"
    fi
}

caret_blind_on() {
    mutate src/kernel/drivers/video/canvas.c
    python3 - <<'EOF2'
p = "src/kernel/drivers/video/canvas.c"
s = open(p).read()
a = """    bool opened = implicit_open_locked(c);
    c->col = (uint32_t)x < c->cols ? (uint32_t)x : c->cols - 1u;
    c->row = (uint32_t)y < c->rows ? (uint32_t)y : c->rows - 1u;
    implicit_close_locked(c, opened);"""
assert s.count(a) == 1, "caret mutation anchor missing"
s = s.replace(a, """    /* logcheck mutation: the cursor moves and the glass is not told */
    c->col = (uint32_t)x < c->cols ? (uint32_t)x : c->cols - 1u;
    c->row = (uint32_t)y < c->rows ? (uint32_t)y : c->rows - 1u;""", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/drivers/video/canvas.c "logcheck mutation"
}

caret_blind_off() {
    restore src/kernel/drivers/video/canvas.c
}

run_caretmut() {
    echo "== caretmut: with the commit taken out, the caret must stop following the cursor =="
    caret_blind_on; build STRICT=on
    local booted=1
    caret_boot && booted=0
    if [ $booted -eq 0 ]; then caret_run caretmut; fi
    make run-stop >/dev/null 2>&1
    caret_blind_off
    if [ $booted -ne 0 ]; then bad "caretmut: never reached a shell"; build; return; fi

    local E COL LINE WANT
    E=$(head -1 "$SCRATCH/caretmut.edit.txt")
    LINE=$(tail -1 "$SCRATCH/caretmut.edit.txt")
    COL=$(echo "$E" | awk '{print $4}' | cut -d, -f2)
    WANT=$(echo "$LINE" | sed 's/^last [0-9]*|//' | awk '{ i=index($0,"abcdef"); print (i>0 ? i-1+4 : -1) }')
    if [ -n "$COL" ] && [ "$COL" = "$WANT" ]; then
        bad "caretmut: the commit was removed and the caret STILL followed the arrows — the oracle cannot see that defect"
    else
        ok "caretmut: the caret stayed at column $COL instead of $WANT, and the caret oracle would have caught it"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# grow — the volume takes the ground it is given
# ===========================================================================
#
# An image is written to a medium with dd, and the medium is nearly always
# bigger than the image. Until the Deed could be amended in place, the volume
# simply did not use the rest: a 90 MiB image on a 64 GB stick left 63 GB of it
# dark for ever, and nothing said so.
#
# The instrument is the image itself. The MBR entry that claims the volume's
# ground is stretched and the file behind it grown — exactly what writing the
# image to a bigger medium and enlarging the partition does — and then the
# machine is booted on it. Nothing inside the volume is touched: whether it
# takes the ground is the machine's business, which is the whole point.

grow_stretch() {
    # grow_stretch IMG SECTORS — give the BoxOS partition more ground.
    python3 - "$1" "$2" <<'EOF2'
import struct, sys, os
img, more = sys.argv[1], int(sys.argv[2])
f = open(img, "r+b")
f.seek(446); table = f.read(64)
for i in range(4):
    e = table[i*16:(i+1)*16]
    if e[4] == 0x7f:
        start, count = struct.unpack_from("<II", e, 8)
        ent = bytearray(e)
        struct.pack_into("<I", ent, 12, count + more)
        f.seek(446 + i*16); f.write(bytes(ent))
        end = (start + count + more) * 512
        f.seek(0, os.SEEK_END)
        if f.tell() < end: f.truncate(end)
        f.close()
        print("%d %d" % (start, count + more))
        sys.exit(0)
f.close(); sys.exit(1)
EOF2
}

# grow_stood_on LOG START COUNT — did the machine that wrote LOG stand on the
# stretched ground, sectors START..START+COUNT-1? A `make run-bg` that repacked
# build/boxos.img boots the ground the image was made with, and a scenario that
# measures growth on such a boot measures nothing: growmut passed exactly so,
# its log showing the unstretched ground. Says what it found when it is not.
grow_stood_on() {
    local want found
    want="sectors $2..$(($2 + $3 - 1))"
    found=$(tr -d '\r' < "$1" 2>/dev/null | grep -oE "stands on [A-Za-z]+ ground: sectors [0-9]+\.\.[0-9]+" \
            | tail -1 | sed 's/.*: //')
    [ "$found" = "$want" ] && return 0
    echo "${found:-no ground line at all}, not $want"
    return 1
}

grow_boot() {
    # grow_boot NAME — boot the image as it stands and keep the log window.
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    cp build/serial.log "$SCRATCH/serial.$1.log"
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null
}

grow_rollback_head() {
    # Put the PRE-GROWTH head deed back: the state a power cut between the two
    # writes of a growth leaves behind — a new far copy and an old head.
    python3 - "$1" "$2" "$3" <<'EOF2'
import sys
pristine, img, start = sys.argv[1], sys.argv[2], int(sys.argv[3])
off = start * 512
o = open(pristine, "rb"); o.seek(off); head = o.read(4096); o.close()
f = open(img, "r+b"); f.seek(off); f.write(head); f.close()
EOF2
}

run_grow() {
    echo "== grow: a volume on a bigger medium takes the ground behind it =="
    rm -f build/boxos.img   # a scenario that measures growth starts from a volume that has not grown
    build STRICT=on
    make run-stop >/dev/null 2>&1

    # The stretched image goes under the manifest as the build product it is,
    # so a run that dies anywhere below leaves no stretched image behind for
    # the next scenario to measure.
    mutate_product build/boxos.img
    local START
    START=$(grow_stretch build/boxos.img 409600 | cut -d' ' -f1)
    if [ -z "$START" ]; then
        bad "grow: the image has no BoxOS partition to stretch"
        restore build/boxos.img; build; return
    fi

    if ! grow_boot grow; then
        bad "grow: never reached a shell"
        make run-stop >/dev/null 2>&1; restore build/boxos.img; build; return
    fi
    local L="$SCRATCH/serial.grow.log"

    grep -q "takes the rest" "$L"
    chk $? "grow: the volume sees the ground behind it and takes it"

    grep -qE "the volume now runs [0-9]+ sectors; its data run is [0-9]+ blocks, [0-9]+ more" "$L"
    chk $? "grow: $(grep -oE 'its data run is [0-9]+ blocks, [0-9]+ more than it had' "$L" | tail -1)"

    grep -q "far copy has moved to sector" "$L"
    chk $? "grow: the far copy moved to the new end"

    grep -q "is inside the data run now and has been erased" "$L"
    chk $? "grow: the deed that used to sit at the old end was erased"

    grep -q "its far copy agrees — the whole volume is present" "$L"
    chk $? "grow: both copies agree about the volume it has become"

    # The free count and the data run have to be a possible pair of numbers.
    local DB FB
    DB=$(grep -oE "^\[TagFS\] [0-9]+ data blocks" "$L" | tail -1 | grep -oE "[0-9]+" || true)
    FB=$(grep -oE "[0-9]+ free," "$L" | tail -1 | grep -oE "[0-9]+" || true)
    if [ -n "$DB" ] && [ -n "$FB" ] && [ "$FB" -le "$DB" ]; then
        ok "grow: $FB blocks free out of $DB — a volume cannot have more free than it has"
    else
        bad "grow: the volume claims $FB free out of $DB data blocks"
    fi

    grep -qE "MOUNTED — [0-9]+ file" "$L"
    chk $? "grow: $(grep -oE 'MOUNTED — [0-9]+ file\(s\)' "$L" | tail -1)"

    # And a file laid down before the growth still reads.
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "show hello.txt"
    key_press ret
    sleep 3
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' | grep -q "Hello from BoxOS"
    chk $? "grow: a file written before the growth still reads afterwards"

    # Boot it again: the volume now fills its ground and says so.
    if grow_boot grow2; then
        grep -q "fills the ground it was given" "$SCRATCH/serial.grow2.log"
        chk $? "grow: the next mount finds nothing left to take, and says so"
    else
        bad "grow: the grown volume would not boot a second time"
    fi

    make run-stop >/dev/null 2>&1
    restore build/boxos.img   # the stretched image is discarded
    build                     # and an unstretched one is made again
}

run_growcut() {
    echo "== growcut: a growth cut between its two writes still mounts, and finishes =="
    #
    # The order is the crash safety: the far copy is written first, the head
    # second, and the head is the commit point. This puts the machine in the
    # exact state a power cut between the two leaves — a new far copy and the
    # old head — by rolling the head deed back on the medium afterwards.
    rm -f build/boxos.img   # a scenario that measures growth starts from a volume that has not grown
    build STRICT=on
    make run-stop >/dev/null 2>&1
    # ‼ THIS COPY IS EVIDENCE, NOT A BACKUP. The scenario needs the bytes of the
    # head deed as they stood BEFORE the growth, because writing them back is
    # how the power cut between the two writes is reproduced. Putting the image
    # back afterwards is the manifest's job, below, and it does it by deleting.
    cp build/boxos.img "$SCRATCH/growcut.pristine"

    mutate_product build/boxos.img
    local START
    START=$(grow_stretch build/boxos.img 409600 | cut -d' ' -f1)
    if [ -z "$START" ]; then
        bad "growcut: the image has no BoxOS partition to stretch"
        restore build/boxos.img; build; return
    fi

    if ! grow_boot growcut1; then
        bad "growcut: never reached a shell"
        make run-stop >/dev/null 2>&1; restore build/boxos.img; build; return
    fi
    grep -q "the volume now runs" "$SCRATCH/serial.growcut1.log"
    chk $? "growcut: the volume grew once, so there is a growth to cut"

    make run-stop >/dev/null 2>&1
    sleep 1
    grow_rollback_head "$SCRATCH/growcut.pristine" build/boxos.img "$START"

    if ! grow_boot growcut2; then
        bad "growcut: a half-written growth would not boot"
        make run-stop >/dev/null 2>&1; restore build/boxos.img; build; return
    fi
    local L="$SCRATCH/serial.growcut2.log"

    grep -qE "MOUNTED — [0-9]+ file" "$L"
    chk $? "growcut: the volume mounts whole — $(grep -oE 'MOUNTED — [0-9]+ file\(s\)' "$L" | tail -1)"

    grep -q "takes the rest" "$L"
    chk $? "growcut: and finishes the growth from the head's own numbers"

    local DB FB
    DB=$(grep -oE "^\[TagFS\] [0-9]+ data blocks" "$L" | tail -1 | grep -oE "[0-9]+" || true)
    FB=$(grep -oE "[0-9]+ free," "$L" | tail -1 | grep -oE "[0-9]+" || true)
    if [ -n "$DB" ] && [ -n "$FB" ] && [ "$FB" -le "$DB" ]; then
        ok "growcut: $FB free out of $DB — the ground taken twice was not counted twice"
    else
        bad "growcut: the volume claims $FB free out of $DB data blocks"
    fi

    grep -qE "PANIC|\[EXCEPTION\]" "$L" \
        && bad "growcut: the kernel faulted on a half-written growth" \
        || ok "growcut: no panic, no fault"

    make run-stop >/dev/null 2>&1
    restore build/boxos.img
    build
}

grow_blind_on() {
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF2'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "        volume_take_more_ground(seat, ground_run, &head);"
assert s.count(anchor) == 1, "grow mutation anchor missing"
s = s.replace(anchor, "        (void)ground_run; (void)volume_take_more_ground;   /* logcheck mutation: the ground is left unclaimed */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation"
}

grow_blind_off() {
    restore src/kernel/tagfs/tagfs.c
}

run_growmut() {
    echo "== growmut: with the taking removed, the volume must leave the ground unclaimed =="
    rm -f build/boxos.img
    grow_blind_on; build STRICT=on
    make run-stop >/dev/null 2>&1
    mutate_product build/boxos.img
    local grown booted=1 stood
    grown=$(grow_stretch build/boxos.img 409600)
    grow_boot growmut && booted=0
    grow_blind_off
    restore build/boxos.img
    if [ $booted -ne 0 ]; then bad "growmut: never reached a shell"; build; return; fi

    local L="$SCRATCH/serial.growmut.log"
    if ! stood=$(grow_stood_on "$L" ${grown:-0 0}); then
        harness "growmut: the boot stood on $stood — the stretched image was not the one booted, so nothing was judged"
    elif grep -q "takes the rest" "$L"; then
        bad "growmut: the taking was removed and the volume STILL grew — the oracle cannot see that defect"
    else
        ok "growmut: on the stretched ground ($(tr -d '\r' < "$L" | grep -oE 'sectors [0-9]+\.\.[0-9]+' | tail -1)) nothing was taken, and the grow oracle would have caught it"
    fi
    make run-stop >/dev/null 2>&1
    build   # leave the tree built from clean sources
}


util_boot() {
    # util_boot NAME "RUN-BG ARGS" COMMAND DONE_REGEX [QUIET_S]
    #
    # The fifth argument used to be a number of LOOKS — a ceiling on how long
    # the command was allowed to take, which is a guess about a machine. It is
    # now the quiet window: how long the machine may say nothing before this
    # gives up on it. Left out, it is the ordinary one.
    local name=$1 args=$2 cmd=$3 done_re=$4 quiet=${5:-$WAIT_QUIET_S}
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "$cmd"
    sleep 1
    key_press ret
    wait_for_line -E build/serial.log "$done_re" "$quiet" $((MARK + 1))
    sleep 2
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_brigade() {
    echo "== brigade: 300 strands parked on one word, one wake, all 300 back =="
    build STRICT=on
    if ! util_boot brigade "STRICT=on CORES=16 MEM=8G" strandpark "\[SPK\] (PASS|FAIL): brigade"; then
        bad "brigade: never reached a shell"; return
    fi
    L="$SCRATCH/serial.brigade.log"
    grep -q "\[SPK\] PASS: brigade of 300 parked, one wake, 300 back" "$L" \
        && ok "brigade: one wake reached all 300 sleepers" \
        || bad "brigade: $(grep -m1 '\[SPK\] FAIL' "$L" || echo 'no verdict from strandpark at all')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "brigade: the kernel spoke of a stall or fault" \
        || ok "brigade: no verdict, no panic"
}

# The oracle measured against itself: put the tray of 256 back, and 44
# sleepers must be left behind — and the oracle must see them.
brigade_tray_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = "    while (e && (count == 0 || wake_count < count))\n"
assert s.count(anchor) == 1, "brigade mutation anchor missing"
s = s.replace(anchor, "    while (e && (count == 0 || wake_count < count) && wake_count < 256u)   /* logcheck mutation: the tray of 256 */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation"
}

brigade_tray_off() {
    restore src/kernel/core/decks/system/sync_ops.c
}

run_brigademut() {
    echo "== brigademut: the tray of 256 put back, and 44 sleepers must be left behind =="
    brigade_tray_on; build STRICT=on
    util_boot brigademut "STRICT=on CORES=16 MEM=8G" strandpark "\[SPK\] (PASS|FAIL): brigade"; local booted=$?
    brigade_tray_off
    if [ $booted -ne 0 ]; then bad "brigademut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.brigademut.log"
    if grep -q "\[SPK\] FAIL: brigade: one wake reached 256 of 300 sleepers" "$L"; then
        ok "brigademut: the tray left 44 asleep — and the oracle sees them"
    else
        bad "brigademut: the tray put back and the brigade STILL all woke — the oracle cannot see that defect ($(grep -m1 '\[SPK\] .*brigade' "$L" || echo 'no verdict'))"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# headcount — every carrier of a tag is told once, and every one answers
# ===========================================================================
#
# system.broadcast used to take the carriers of a tag into a stack tray of
# 256 and stop when it was full: the 257th carrier was never told, and
# nothing said so. The snapshot is now made for the count the walk found,
# and made again if the list grew in between.
#
# headcount is the instrument: 300 children carry the crew tag from boarding,
# the parent says one word to the tag, and every child answers with a Touch.
run_headcount() {
    echo "== headcount: one word to 300 carriers of a tag, 300 answers =="
    build STRICT=on
    if ! util_boot headcount "STRICT=on CORES=16 MEM=8G" headcount "\[HEADCOUNT\] (PASS|FAIL)"; then
        bad "headcount: never reached a shell"; return
    fi
    L="$SCRATCH/serial.headcount.log"
    grep -q "\[HEADCOUNT\] PASS: one word to 300, 300 answered" "$L" \
        && ok "headcount: every carrier of the tag was told" \
        || bad "headcount: $(grep -m1 '\[HEADCOUNT\] FAIL' "$L" || echo 'no verdict from headcount at all')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "headcount: the kernel spoke of a stall or fault" \
        || ok "headcount: no verdict, no panic"
}

headcount_tray_on() {
    mutate src/kernel/core/decks/system/system_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/system/system_ops.c"
s = open(p).read()
anchor = "            if (pid_count < pid_cap) pid_list[pid_count] = iter->pid;\n"
assert s.count(anchor) == 1, "headcount mutation anchor missing"
s = s.replace(anchor, "            if (pid_count >= 256u) break;   /* logcheck mutation: the tray of 256 */\n" + anchor, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/system/system_ops.c "logcheck mutation"
}

headcount_tray_off() {
    restore src/kernel/core/decks/system/system_ops.c
}

run_headcountmut() {
    echo "== headcountmut: the tray of 256 put back, and 44 carriers must go untold =="
    headcount_tray_on; build STRICT=on
    util_boot headcountmut "STRICT=on CORES=16 MEM=8G" headcount "\[HEADCOUNT\] (PASS|FAIL)"; local booted=$?
    headcount_tray_off
    if [ $booted -ne 0 ]; then bad "headcountmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.headcountmut.log"
    if grep -q "\[HEADCOUNT\] FAIL: one word to 300, 256 answered" "$L"; then
        ok "headcountmut: the tray left 44 untold — and the oracle sees them"
    else
        bad "headcountmut: the tray put back and all 300 STILL answered — the oracle cannot see that defect ($(grep -m1 '\[HEADCOUNT\]' "$L" || echo 'no verdict'))"
    fi
    build   # leave the tree built from clean sources
}


# ===========================================================================
# byname — a file is found by its name through the index, and the name's tag
# follows a rename
# ===========================================================================
#
# The volume stamps every file with the stem of its name as a tag, and that is
# how by-name lookup (find_file_by_name, boxcxx fopen) and proc_exec find a
# file — the index, not a walk of the first 255 files. rename used to change
# the string and leave the tag, so a renamed file answered to its old name and
# not its new one. The scenario creates a file, renames it, and asks the volume
# by tag and by name; then trashes it and erases the trash, which asks for the
# trashed files by their tag (a plain listing leaves them out on purpose).
#
# Then a second file is trashed and erased BY NAME. erase looked the name up
# the way a listing does — and a listing leaves the trash out — then refused
# any file that was not trashed, so `erase NAME` could delete nothing at all.
# A third file, never trashed, must still be refused, and a name nobody has
# must be not found. The machine is halted through `bye` and the host lists
# the volume it left: the refused file on it, the erased one not.
byname_session() {
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    local cmd
    for cmd in "create alpha.txt" "name alpha.txt beta.txt" "files beta" "files alpha" \
               "info beta.txt" "info alpha.txt" "trash beta.txt" "erase trashed" "files beta" \
               "create gamma.txt" "trash gamma.txt" "erase gamma.txt" "files trashed" \
               "create delta.txt" "erase delta.txt" "erase nosuch.txt"; do
        type_line "$cmd" || break
        wait_for_prompt || break
    done
    type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" $((MARK + 1))
    sleep 1
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    python3 tools/volume-read.py build/boxos.img list >"$SCRATCH/$name.list" 2>&1
    return 0
}

byname_judge() {
    local name=$1 L="$SCRATCH/serial.$1.log"
    grep -q "Created file: alpha.txt" "$L" \
        && ok "$name: the file was created" \
        || bad "$name: create did not answer"
    grep -q "File renamed" "$L" \
        && ok "$name: the file was renamed" \
        || bad "$name: rename did not answer"
    # `files beta` asks by tag: the renamed file must carry its new name's tag
    sed -n '/~ files beta/,/~ files alpha/p' "$L" | grep -q "^beta.txt" \
        && ok "$name: asked by its new name's tag, the volume answers with the file" \
        || bad "$name: the tag did not follow the name — 'files beta' does not list beta.txt"
    sed -n '/~ files alpha/,/~ info beta.txt/p' "$L" | grep -q "No files found" \
        && ok "$name: the old name's tag is gone" \
        || bad "$name: the file still answers to its old name's tag"
    sed -n '/~ info beta.txt/,/~ info alpha.txt/p' "$L" | grep -q "beta.txt" \
        && ok "$name: found by its new name" \
        || bad "$name: 'info beta.txt' did not find the file"
    sed -n '/~ info alpha.txt/,/~ trash beta.txt/p' "$L" | grep -q "File not found" \
        && ok "$name: not found by its old name" \
        || bad "$name: 'info alpha.txt' still finds the renamed file"
    grep -q "Deleted 1 trashed files" "$L" \
        && ok "$name: erase found the trashed file by its tag" \
        || bad "$name: 'erase trashed' did not delete the one trashed file ($(grep -m1 'Deleted' "$L" || echo 'no answer'))"
    sed -n '/~ trash gamma.txt/,/~ erase gamma.txt/p' "$L" | grep -q "^File moved to trash"
    chk $? "$name: gamma.txt went to the trash ($(sed -n '/~ trash gamma.txt/,/~ erase gamma.txt/p' "$L" | grep -m1 -E '^(File moved|Error)' || echo 'no answer'))"
    local erased said
    said=$(sed -n '/~ erase gamma.txt/,/~ files trashed/p' "$L" | grep -m1 -E "^(File deleted|Error)")
    erased=$(echo "$said" | grep -oE "^File deleted: gamma\.txt \(file [0-9]+\)")
    [ -n "$erased" ]
    chk $? "$name: erase by name found the trashed file in the trash (${said:-no answer})"
    sed -n '/~ files trashed/,/~ create delta.txt/p' "$L" | grep -q "No files found"
    chk $? "$name: and the trash is empty after it ($(sed -n '/~ files trashed/,/~ create delta.txt/p' "$L" | grep -m1 -E '^(No files found|gamma\.txt)' || echo 'no answer'))"
    sed -n '/~ erase delta.txt/,/~ erase nosuch.txt/p' "$L" | grep -q "^Error: File must be trashed first"
    chk $? "$name: a file out of the trash is refused ($(sed -n '/~ erase delta.txt/,/~ erase nosuch.txt/p' "$L" | grep -m1 -E '^(File deleted|Error)' || echo 'no answer'))"
    sed -n '/~ erase nosuch.txt/,/~ bye/p' "$L" | grep -q "^Error: File not found"
    chk $? "$name: a name nobody has is not found ($(sed -n '/~ erase nosuch.txt/,/~ bye/p' "$L" | grep -m1 -E '^(File deleted|Error)' || echo 'no answer'))"
    local halt rows kept refused seen
    halt=$(grep -m1 -E "$STAND_HALT_ENDS" "$L" | cut -c1-120)
    grep -q '^\[HALT\] Storage sync complete' "$L"
    chk $? "$name: the halt through bye said its sync wrote the volume (${halt:-no halt line})"
    rows=$(grep -cE ' bytes +[0-9]+ extent\(s\), [0-9]+ block\(s\)$' "$SCRATCH/$name.list")
    kept=$(grep -m1 -E '^ +gamma\.txt +[0-9]+ bytes ' "$SCRATCH/$name.list" | sed 's/^ *//; s/  */ /g')
    refused=$(grep -m1 -E '^ +delta\.txt +[0-9]+ bytes ' "$SCRATCH/$name.list" | sed 's/^ *//; s/  */ /g')
    if [ -n "$refused" ]; then seen="delta.txt, which erase refused, is on it ($refused)"
    else seen="delta.txt, which erase refused, is NOT on it"; fi
    if [ -n "$kept" ]; then seen="$seen, and gamma.txt IS ($kept)"
    else seen="$seen, and gamma.txt is not"; fi
    grep -qE '^ground sector [0-9]+ ' "$SCRATCH/$name.list" && [ -n "$refused" ] && [ -z "$kept" ]
    chk $? "$name: the host lists $rows file(s) on the volume bye left — $seen"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "$name: the kernel spoke of a stall or fault" \
        || ok "$name: no verdict, no panic"
}

run_byname() {
    echo "== byname: a file is found by its name through the index, and the tag follows a rename =="
    rm -f build/boxos.img   # a fresh volume: the host listing judges this session's files alone
    build STRICT=on
    if ! byname_session byname "STRICT=on CORES=4 MEM=4G"; then
        bad "byname: never reached a shell"; return
    fi
    byname_judge byname
}

# The oracle measured against itself: rename leaves the tag where it was, and
# the renamed file must stop answering to its new name.
byname_mut_on() {
    mutate src/kernel/tagfs/ink/ink.c
    python3 - <<'EOF2'
p = "src/kernel/tagfs/ink/ink.c"
s = open(p).read()
anchor = "    uint32_t old_name = name ? InkNameTagLookup(cur->name) : TAGFS_INVALID_TAG_ID;\n"
assert s.count(anchor) == 1, "byname mutation anchor missing"
s = s.replace(anchor, "    uint32_t old_name = TAGFS_INVALID_TAG_ID;   /* logcheck mutation: the tag stays behind */\n    if (name)\n        add = TAGFS_INVALID_TAG_ID;\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/tagfs/ink/ink.c "logcheck mutation"
}

byname_mut_off() {
    restore src/kernel/tagfs/ink/ink.c
}

run_bynamemut() {
    echo "== bynamemut: rename leaves the tag behind, and the oracle must see the file answer to the wrong name =="
    byname_mut_on; build STRICT=on
    byname_session bynamemut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    byname_mut_off
    if [ $booted -ne 0 ]; then bad "bynamemut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.bynamemut.log"
    if sed -n '/~ files beta/,/~ files alpha/p' "$L" | grep -q "^beta.txt"; then
        bad "bynamemut: the tag was left behind and 'files beta' STILL lists beta.txt — the oracle cannot see that defect"
    else
        ok "bynamemut: the tag left behind, and the oracle sees the file gone from its new name"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# stash — what a strand is handed that is not for the caller at hand is KEPT
# ===========================================================================
#
# A strand waiting for one Touch tag keeps the events of other tags; a strand
# waiting for a kernel reply keeps the IPC messages ahead of it. Both stashes
# used to hold 256 entries and lose the rest in silence — the Touch one shed
# the newest, the IPC one dropped the oldest. They grow by the chunk now
# (box/core/stash.h). TT 23 publishes 300 events of another tag past a wait,
# TT 24 sends 300 messages past a kernel reply; both must all be there.
run_stash() {
    echo "== stash: 300 events of another tag and 300 messages ahead of a reply, all kept =="
    build STRICT=on
    if ! util_boot stash "STRICT=on CORES=4 MEM=4G" touch_test "\[TT SUMMARY\]"; then
        bad "stash: never reached a shell"; return
    fi
    L="$SCRATCH/serial.stash.log"
    grep -q "\[TT 23\] PASS" "$L" \
        && ok "stash: 300 events of the other tag were kept across the wait" \
        || bad "stash: $(grep -m1 '\[TT 23\]' "$L" || echo 'TT 23 never reported')"
    grep -q "\[TT 24\] PASS" "$L" \
        && ok "stash: 300 messages ahead of a kernel reply were kept" \
        || bad "stash: $(grep -m1 '\[TT 24\]' "$L" || echo 'TT 24 never reported')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]|DEFECT" "$L" \
        && bad "stash: the kernel or boxlib spoke of a stall, fault or defect" \
        || ok "stash: no verdict, no panic, no defect"
}

# The oracle measured against itself: the stash of 256 put back — with the
# new rule that an entry with no place stays in the ring, said aloud.
stash_cap_on() {
    mutate src/userspace/boxlib/src/core/stash.c
    python3 - <<'EOF2'
p = "src/userspace/boxlib/src/core/stash.c"
s = open(p).read()
anchor = "    if (s->tail && s->tail->count < s->chunk_cap) return true;\n"
assert s.count(anchor) == 1, "stash mutation anchor missing"
s = s.replace(anchor, "    if (s->count >= 256u) return false;   /* logcheck mutation: the stash of 256 */\n" + anchor, 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/boxlib/src/core/stash.c "logcheck mutation"
}

stash_cap_off() {
    restore src/userspace/boxlib/src/core/stash.c
}

run_stashmut() {
    echo "== stashmut: the stash of 256 put back — TT 23 and TT 24 must go red, and the strand must say why =="
    stash_cap_on; build STRICT=on
    util_boot stashmut "STRICT=on CORES=4 MEM=4G" touch_test "\[TT SUMMARY\]"; local booted=$?
    stash_cap_off
    if [ $booted -ne 0 ]; then bad "stashmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.stashmut.log"
    if grep -q "\[TT 23\] PASS" "$L"; then
        bad "stashmut: the stash capped at 256 and TT 23 STILL passed — the oracle cannot see that loss"
    else
        ok "stashmut: TT 23 sees the events that had no place"
    fi
    if grep -q "\[TT 24\] PASS" "$L"; then
        bad "stashmut: the stash capped at 256 and TT 24 STILL passed — the oracle cannot see that loss"
    else
        ok "stashmut: TT 24 sees the messages that had no place"
    fi
    grep -q "DEFECT: no memory to keep" "$L" \
        && ok "stashmut: the strand said aloud that it had no place for what it was handed" \
        || bad "stashmut: nothing said about the entries that had no place"
    build   # leave the tree built from clean sources
}

# ===========================================================================
# wire — the serial line reads the log ring by its own interrupt, and the
# last words are drained before a halt
# ===========================================================================
#
# Every byte the kernel says goes into the log ring; the Wire is the line's
# reader, advanced by the UART's transmitter-empty interrupt (IRQ4). No core
# waits on the UART — what stood here spun four million times per byte and
# then dropped it. At panic and halt the line cannot drive itself (interrupts
# are off), so the last words are drained on the transmitter's own word.
# The scenario: a boot must reach the shell on the wire, a command must be
# heard, and `reboot` must leave "[HALT] Rebooting..." on the wire — the
# drain at halt, seen.
wire_session() {
    local name=$1
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    local reached=0
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null && reached=1
    sleep 3
    # Typed through the monitor's keyboard, not the serial line, so the wire's
    # own state cannot stop the command from landing.
    type_cmd "hw"
    sleep 1; key_press ret; sleep 4
    type_cmd "reboot"
    sleep 1; key_press ret
    wait_for_line build/serial.log "Rebooting" "$WAIT_QUIET_S"
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    return $((1 - reached))
}

run_wire() {
    echo "== wire: the line reads the ring by interrupt; the last words are drained at halt =="
    build STRICT=on
    wire_session wire; local reached=$?
    L="$SCRATCH/serial.wire.log"
    [ $reached -eq 0 ] \
        && ok "wire: the boot reached the shell on the wire" \
        || bad "wire: the shell never appeared on the wire"
    grep -q "TSC freq" "$L" \
        && ok "wire: a command's answer came down the wire" \
        || bad "wire: hw's answer never came down the wire"
    grep -q "\[HALT\] Rebooting" "$L" \
        && ok "wire: the last words before the reboot were drained onto the wire" \
        || bad "wire: '[HALT] Rebooting' never reached the wire"
    grep -q "\[WIRE\]" "$L" \
        && bad "wire: the line reports falling behind on a plain boot" \
        || ok "wire: the line never fell behind the ring"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "wire: the kernel spoke of a stall or fault" \
        || ok "wire: no verdict, no panic"
}

# The oracle measured against itself: the ring grows and nobody tells the
# line (WireKick does nothing once the line drives itself), so after the
# first THRE has drained what was pending the line goes quiet for good — the
# shell never appears on the wire — and only the drain at halt brings the
# rest out.
#
# ‼ The other half of the mechanism — IRQ4 continuing a BUSY line on THRE —
# cannot be made red on this desk: QEMU's transmitter is empty again by the
# time the next byte is put, so every byte is pumped by its own kick and the
# interrupt is never needed (measured: with the THRE branch cut, the whole
# boot still came down the wire). That half is for the board, where a byte
# takes 87 us and a kick finds the transmitter busy.
wire_deaf_on() {
    mutate src/kernel/drivers/serial/serial.c
    python3 - <<'EOF2'
p = "src/kernel/drivers/serial/serial.c"
s = open(p).read()
anchor = "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n    if (!__atomic_load_n(&g_wire_idle, __ATOMIC_SEQ_CST)) return;\n"
assert s.count(anchor) == 1, "wire mutation anchor missing"
s = s.replace(anchor, "    return;   /* logcheck mutation: the ring grew and nobody told the line */\n" + anchor, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/drivers/serial/serial.c "logcheck mutation"
}

wire_deaf_off() {
    restore src/kernel/drivers/serial/serial.c
}

run_wiremut() {
    echo "== wiremut: nobody tells the line the ring grew — the shell must NOT reach the wire, and the drain at halt must bring it out =="
    wire_deaf_on; build STRICT=on
    wire_session wiremut; local reached=$?
    wire_deaf_off
    L="$SCRATCH/serial.wiremut.log"
    [ $reached -ne 0 ] \
        && ok "wiremut: with nobody telling the line, the shell never reached the wire — the oracle sees the stall" \
        || bad "wiremut: the shell reached the wire with nobody telling the line — the oracle cannot see a dead line"
    grep -q "BoxOS Shell" "$L" && grep -q "\[HALT\] Rebooting" "$L" \
        && ok "wiremut: the drain at halt brought the whole ring out, shell banner and last words alike" \
        || bad "wiremut: the drain at halt did not bring the ring out"
    build   # leave the tree built from clean sources
}

# ===========================================================================
# knock — an interrupt's event rides the ring and ONE knock; nothing drops
# ===========================================================================
#
# An interrupt cannot publish where it stands, so it leaves the event in the
# Touch IRQ ring and knocks; the drain core comes once per knock and reads
# everything up to the producers' cursor. What stood between the ring and the
# K-Core was a note per event in a ring of notes that could be full — and a
# full ring of notes lost the event in silence, three ways (before its init,
# when its chunk had no successor, after four tries in a race). The knock is
# a baton embedded in the ring itself: nothing to allocate, nowhere to
# overflow, and a burst of interrupts is one pass.
#
# The scenario: a boot on sixteen cores and on one (the idle/tick drain) must
# hear the keyboard — a builtin AND an external program — with the ring never
# saying it lost anything, and the baton self-test must have proven the knock
# at boot (a thousand knocks, one pass; a knock after the open, one more).
run_knock() {
    echo "== knock: a keystroke rides the ring and one knock, on 16 cores and on one =="
    build STRICT=on
    if ! util_boot knock16 "STRICT=on CORES=16 MEM=8G" help "Show available commands" "$REACT_QUIET_S"; then
        bad "knock: never reached a shell (16c)"; return
    fi
    L="$SCRATCH/serial.knock16.log"
    typed_ok "$L" \
        && ok "knock (16c): a builtin answered the keyboard" \
        || bad "knock (16c): 'help' was typed and nothing answered"
    grep -q "\[BATON-TEST\] PASS (never-drop + exactly-once + FIFO + stub-re-anchor + knock)" build/serial.log \
        && ok "knock (16c): the baton self-test proved the knock at boot" \
        || bad "knock (16c): no baton self-test PASS with the knock at boot"
    grep -q "\[TOUCH\] ERROR" "$L" \
        && bad "knock (16c): the ring reports a loss on plain typing" \
        || ok "knock (16c): the ring lost nothing"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knock (16c): the kernel spoke of a stall or fault" \
        || ok "knock (16c): no verdict, no panic"

    if ! util_boot knock1 "STRICT=on CORES=1 MEM=2G" hw "TSC freq" "$REACT_QUIET_S"; then
        bad "knock: never reached a shell (1c)"; return
    fi
    L="$SCRATCH/serial.knock1.log"
    grep -q "TSC freq" "$L" \
        && ok "knock (1c): an external program was started from the keyboard through the idle/tick drain" \
        || bad "knock (1c): 'hw' was typed and never ran"
    grep -q "\[TOUCH\] ERROR" "$L" \
        && bad "knock (1c): the ring reports a loss on plain typing" \
        || ok "knock (1c): the ring lost nothing"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knock (1c): the kernel spoke of a stall or fault" \
        || ok "knock (1c): no verdict, no panic"
}

# The oracle measured against itself: nobody knocks. The event is in the ring
# and no visit ever comes for it — the machine boots and is deaf.
knock_deaf_on() {
    mutate src/kernel/core/touch/touch.c
    python3 - <<'EOF2'
p = "src/kernel/core/touch/touch.c"
s = open(p).read()
anchor = "    KnockOn(&g_touch_irq_knock);\n}\n"
assert s.count(anchor) == 1, "knock mutation anchor missing"
s = s.replace(anchor, "    /* logcheck mutation: nobody knocks */\n}\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/touch/touch.c "logcheck mutation"
}

knock_deaf_off() {
    restore src/kernel/core/touch/touch.c
}

run_knockmut() {
    echo "== knockmut: nobody knocks — the machine must boot and be deaf, and the oracle must see it =="
    knock_deaf_on; build STRICT=on
    util_boot knockmut "STRICT=on CORES=4 MEM=4G" help "Show available commands" "$REACT_QUIET_S"; local booted=$?
    knock_deaf_off
    if [ $booted -ne 0 ]; then bad "knockmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.knockmut.log"
    if typed_ok "$L"; then
        bad "knockmut: nobody knocked and the keyboard STILL answered — the oracle cannot see a dead ring"
    else
        ok "knockmut: with nobody knocking the keyboard is deaf — and the oracle sees it"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# knockstorm — more events than the ring, and every one of them delivered;
# then the reader held back, the ring outrun, and the loss counted exactly
# ===========================================================================
#
# One key held for five seconds through the monitor. The kernel's own
# typematic repeat (PIT IRQ0, 33 ms) publishes some hundred and forty events
# — more than two rings — and each one must reach the shell. The shell's echo
# is the listener's account; the kernel's is said at the halt: published,
# delivered, lost, still in the ring. Every event the keyboard published in
# the session is an 'a' the shell echoed, plus the eight keys that ended the
# line and typed `reboot` (Enter, six letters, Enter — the driver publishes
# presses and repeats, never releases), so delivered = echoed + 8 exactly, and
# published = delivered + lost + still. Under the mutation the reader stalls
# three seconds on every visit: the ring is outrun, and the kernel must name
# the loss with a number that the same two equations confirm.
storm_session() {
    # storm_session NAME "RUN-BG ARGS"
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    # 5000 is the monitor's own hold time in milliseconds; the seven seconds
    # are that hold plus its delivery, and they are a property of QEMU rather
    # than a guess about the guest.
    mon_raw "sendkey a 5000"
    sleep 7
    # Enter: the line of a's becomes a command nobody knows, and the prompt
    # comes back. Then `reboot`, key by key through the monitor with no echo
    # check — under the mutation the echo lags the keys by design.
    key_press ret
    wait_for_line build/serial.log "Unknown command" "$WAIT_QUIET_S" $((MARK + 1))
    sleep 1
    local k
    for k in r e b o o t ret; do
        key_press "$k"
    done
    wait_for_line build/serial.log "\[HALT\] Rebooting" "$WAIT_QUIET_S"
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    echo "$MARK" > "$SCRATCH/serial.$name.mark"
    return 0
}

# Reads the two accounts out of a storm session's log and prints them as
# shell assignments: E (a's the shell echoed on the storm line), P D L S (the
# kernel's account at the halt), K (of those, delivered under the keyboard's
# own name), X (the loss the kernel announced, -1 if it announced none).
# Kernel lines that landed inside the echo are removed before counting.
storm_numbers() {
    python3 - "$1" "$2" <<'EOF2'
import re, sys
log = open(sys.argv[1]).read().replace("\r", "")
mark = int(open(sys.argv[2]).read().strip())
tail = "\n".join(log.split("\n")[mark:])
clean = re.sub(r"\[[A-Z0-9 -]+\][^\n]*\n", "", tail)
i = clean.find("~ a")
E = clean[i:].split("\n")[0].count("a") if i >= 0 else 0
acc = re.search(r"interrupt ring since boot: published (\d+), delivered (\d+), lost (\d+), still in the ring (\d+)", log)
P, D, L, S = (int(x) for x in acc.groups()) if acc else (-1, -1, -1, -1)
err = re.search(r"\[TOUCH\] ERROR: (\d+) interrupt event\(s\) were overwritten", log)
X = int(err.group(1)) if err else -1
kb = re.search(r"delivered by name: keyboard (\d+)", log)
K = int(kb.group(1)) if kb else -1
names = " ".join(m.group(1) for m in re.finditer(r"delivered by name: (\S+ \d+)", log))
print(f"E={E} P={P} D={D} L={L} S={S} X={X} K={K} NAMES='{names}'")
EOF2
}

run_knockstorm() {
    echo "== knockstorm: a key held five seconds — more events than two rings, every one delivered =="
    build STRICT=on
    if ! storm_session knockstorm "STRICT=on CORES=4 MEM=4G"; then
        bad "knockstorm: never reached a shell"; return
    fi
    L="$SCRATCH/serial.knockstorm.log"
    eval "$(storm_numbers "$L" "$SCRATCH/serial.knockstorm.mark")"
    echo "   shell echoed $E a's; kernel: published $P delivered $D lost $L still $S; by name: $NAMES"
    [ "$E" -ge 65 ] \
        && ok "knockstorm: the shell received more events than the ring holds ($E a's)" \
        || bad "knockstorm: only $E a's reached the shell — the storm did not outsize the ring"
    grep -q "\[TOUCH\] ERROR" "$L" \
        && bad "knockstorm: the ring reports a loss under a burst it should carry" \
        || ok "knockstorm: the ring lost nothing"
    [ "$P" -ge 0 ] \
        && ok "knockstorm: the kernel gave its account at the halt" \
        || bad "knockstorm: no ring account at the halt"
    [ "$K" -eq $((E + 8)) ] \
        && ok "knockstorm: keyboard events delivered ($K) = echoed ($E) + the 8 keys after the storm — the kernel's count is the shell's" \
        || bad "knockstorm: keyboard events delivered $K but the shell echoed $E (+8 expected) — the kernel's account and the shell's disagree"
    [ "$P" -eq $((D + L + S)) ] && [ "$L" -eq 0 ] && [ "$S" -eq 0 ] \
        && ok "knockstorm: published = delivered, nothing lost, nothing left in the ring" \
        || bad "knockstorm: published $P, delivered $D, lost $L, still $S — the account does not add up"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knockstorm: the kernel spoke of a stall or fault" \
        || ok "knockstorm: no verdict, no panic"
}

# The oracle measured against itself: the reader stalls three seconds on
# every visit. During the first stall the held key outruns the ring; the
# oldest events are overwritten, and the kernel must say how many — a number
# the shell's echo and the halt account confirm.
storm_stall_on() {
    mutate src/kernel/core/touch/touch.c
    python3 - <<'EOF2'
p = "src/kernel/core/touch/touch.c"
s = open(p).read()
anchor = "    KnockOpen(&g_touch_irq_knock);\n\n    for (;;) {\n        uint32_t claimed = __atomic_load_n(&g_touch_irq_claimed, __ATOMIC_ACQUIRE);\n"
assert s.count(anchor) == 1, "storm mutation anchor missing"
stall = ("    KnockOpen(&g_touch_irq_knock);\n"
         "    {   /* logcheck mutation: the reader stalls three seconds on every visit */\n"
         "        uint64_t until = clockboard_uptime_ms() + 3000u;\n"
         "        while (clockboard_uptime_ms() < until) __asm__ volatile(\"pause\");\n"
         "    }\n\n    for (;;) {\n        uint32_t claimed = __atomic_load_n(&g_touch_irq_claimed, __ATOMIC_ACQUIRE);\n")
s = s.replace(anchor, stall, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/touch/touch.c "logcheck mutation"
}

storm_stall_off() {
    restore src/kernel/core/touch/touch.c
}

run_knockstormmut() {
    echo "== knockstormmut: the reader stalls, the ring is outrun, and the loss must be named exactly =="
    storm_stall_on; build STRICT=on
    storm_session knockstormmut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    storm_stall_off
    if [ $booted -ne 0 ]; then bad "knockstormmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.knockstormmut.log"
    eval "$(storm_numbers "$L" "$SCRATCH/serial.knockstormmut.mark")"
    echo "   shell echoed $E a's; kernel: published $P delivered $D lost $L still $S; announced $X; by name: $NAMES"
    [ "$X" -ge 1 ] \
        && ok "knockstormmut: the kernel said the ring was outrun and named the loss ($X)" \
        || bad "knockstormmut: the ring was outrun and nothing was said"
    [ "$L" -ge 1 ] && [ "$P" -eq $((D + L + S)) ] && [ "$S" -eq 0 ] \
        && ok "knockstormmut: the halt account carries the loss and adds up (published $P = delivered $D + lost $L)" \
        || bad "knockstormmut: the halt account does not add up (published $P, delivered $D, lost $L, still $S)"
    [ "$K" -eq $((E + 8)) ] \
        && ok "knockstormmut: keyboard events delivered ($K) = echoed ($E) + 8 — what the kernel counts as delivered is exactly what the shell received" \
        || bad "knockstormmut: keyboard events delivered $K but the shell echoed $E (+8 expected) — the count of the loss is not true"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knockstormmut: the kernel spoke of a stall or fault" \
        || ok "knockstormmut: no verdict, no panic"
    build   # leave the tree built from clean sources
}

# ===========================================================================
# deadline — a timed park's ERR_TIMEOUT rides the process's own baton
# ===========================================================================
#
# The PIT tick used to post the deadline's delivery through irq_defer, which
# drops when its chunk has no successor, and boxlib held a +100 ms clock of its
# own over that hop. The delivery now rides the process's embedded baton — a
# pass that cannot fail for want of a slot — and boxlib waits for the answer
# with no clock at all. strandpark is the instrument: 600 timed parks from
# four strands at once (T4), then the brigade. On one core the drain is the
# idle loop and the tick from ring 3, so that path is boot-tested too.
run_deadline() {
    echo "== deadline: 600 timed parks answered at their deadline, on 16 cores and on one =="
    build STRICT=on
    if ! util_boot deadline16 "STRICT=on CORES=16 MEM=8G" strandpark "\[SPK\] (PASS|FAIL): brigade"; then
        bad "deadline: never reached a shell (16c)"; return
    fi
    L="$SCRATCH/serial.deadline16.log"
    grep -q "\[SPK\] PASS: all 3 workers + main completed 150 concurrent parks" "$L" \
        && ok "deadline (16c): every timed park came back at its deadline" \
        || bad "deadline (16c): $(grep -m1 '\[SPK\] FAIL' "$L" || echo 'strandpark T4 never finished')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "deadline (16c): the kernel spoke of a stall or fault" \
        || ok "deadline (16c): no verdict, no panic"

    if ! util_boot deadline1 "STRICT=on CORES=1 MEM=2G" strandpark "\[SPK\] (PASS|FAIL): brigade"; then
        bad "deadline: never reached a shell (1c)"; return
    fi
    L="$SCRATCH/serial.deadline1.log"
    # The self-test speaks at boot, before the shell — that is before the mark
    # util_boot keeps from, so it is read off the whole serial log.
    grep -q "\[BATON-TEST\] PASS" build/serial.log \
        && ok "deadline (1c): the baton self-test passed at boot" \
        || bad "deadline (1c): no baton self-test PASS at boot"
    grep -q "\[SPK\] PASS: all 3 workers + main completed 150 concurrent parks" "$L" \
        && ok "deadline (1c): every timed park came back at its deadline through the idle/tick drain" \
        || bad "deadline (1c): $(grep -m1 '\[SPK\] FAIL' "$L" || echo 'strandpark T4 never finished')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "deadline (1c): the kernel spoke of a stall or fault" \
        || ok "deadline (1c): no verdict, no panic"
}

# The oracle measured against itself: the tick drops the pass, so no deadline
# is ever delivered. With no clock left in boxlib the timed park never returns,
# and Nightwatch must name it: the chit fell DUE at the tick and was never kept.
deadline_drop_on() {
    mutate src/kernel/core/touch/touch_queue.c
    python3 - <<'EOF2'
p = "src/kernel/core/touch/touch_queue.c"
s = open(p).read()
anchor = "            BatonPass(&target->deadline_baton);\n"
assert s.count(anchor) == 1, "deadline mutation anchor missing"
s = s.replace(anchor, "            process_ref_dec(target);   /* logcheck mutation: the pass is dropped */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/touch/touch_queue.c "logcheck mutation"
}

deadline_drop_off() {
    restore src/kernel/core/touch/touch_queue.c
}

run_deadlinemut() {
    echo "== deadlinemut: the pass dropped, and the kernel must name the answer it owes =="
    deadline_drop_on; build STRICT=on
    util_boot deadlinemut "STRICT=on CORES=16 MEM=8G" strandpark "ANSWER OWED|\[SPK\] (PASS|FAIL): brigade"; local booted=$?
    deadline_drop_off
    if [ $booted -ne 0 ]; then bad "deadlinemut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.deadlinemut.log"
    if grep -q "ANSWER OWED: system.addr.park took the event" "$L"; then
        ok "deadlinemut: the dropped deadline is named — system.addr.park owes an answer it never delivered"
    else
        bad "deadlinemut: the pass dropped and nobody said so ($(grep -m1 -E 'ANSWER OWED|\[SPK\] (PASS|FAIL)' "$L" || echo 'no verdict, no strandpark line'))"
    fi
    if grep -q "\[SPK\] PASS: all 3 workers + main completed 150 concurrent parks" "$L"; then
        bad "deadlinemut: no deadline delivered and strandpark T4 STILL passed — the oracle cannot see that defect"
    else
        ok "deadlinemut: without the pass no timed park came back — and the oracle sees it"
    fi
    build   # leave the tree built from clean sources
}


# ===========================================================================
# handsetdeaf — a reader that vanished with the handset in its hand
# ===========================================================================
#
# The daemon said each key to the lane on top and never learned whether anyone
# heard: a reader that died in the middle of its reading kept the ear until the
# daemon met its death by another road, and the keys typed meanwhile went to a
# tag nobody wore. touch_send now returns how many heard; nobody drops that ear
# and the key is said again to the next listener.
#
# handset deaf is the instrument: a strand takes the ear and gives up its claim
# (alive, lane open), main reads a moment later. The line typed in between must
# reach main.
handsetdeaf_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "handset deaf"; sleep 1
    key_press ret
    wait_for_line build/serial.log "\[HANDSET\] deaf ear on top" "$WAIT_QUIET_S"
    # RAW keys, no witness: a key nobody echoes is exactly the key this case is
    # about, and the typer would retype it once main listens and hide the loss.
    local k
    for k in a b c ret; do
        mon_raw "sendkey $k 30"
        sleep 0.05
    done
    wait_for_line build/serial.log "\[HANDSET\] got:" "$REACT_QUIET_S" $((MARK + 1))
    sleep 2
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_handsetdeaf() {
    echo "== handsetdeaf: the reader on top hears nothing, and the line still reaches the next =="
    build STRICT=on
    if ! handsetdeaf_boot handsetdeaf; then bad "handsetdeaf: never reached a shell"; return; fi
    L="$SCRATCH/serial.handsetdeaf.log"
    grep -q "\[HANDSET\] got: abc" "$L" \
        && ok "handsetdeaf: the line said to a deaf ear reached the reader beneath" \
        || bad "handsetdeaf: $(grep -m1 '\[HANDSET\]' "$L" | tail -1; echo '— the line typed at the deaf ear was lost')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "handsetdeaf: the kernel spoke of a stall or fault" \
        || ok "handsetdeaf: no verdict, no panic"
}

# The oracle measured against itself: the daemon takes a key nobody heard for
# heard, so the deaf ear keeps the handset and the line is lost.
handsetdeaf_blind_on() {
    mutate src/userspace/display/display.c
    python3 - <<'EOF2'
p = "src/userspace/display/display.c"
s = open(p).read()
anchor = "        if (heard > 0) {\n"
assert s.count(anchor) == 1, "handsetdeaf mutation anchor missing"
s = s.replace(anchor, "        if (heard >= 0) {   /* logcheck mutation: a key nobody heard counts as heard */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/display/display.c "logcheck mutation"
}

handsetdeaf_blind_off() {
    restore src/userspace/display/display.c
}

run_handsetdeafmut() {
    echo "== handsetdeafmut: the count ignored, and the line at the deaf ear must be lost =="
    handsetdeaf_blind_on; build STRICT=on
    handsetdeaf_boot handsetdeafmut; local booted=$?
    handsetdeaf_blind_off
    if [ $booted -ne 0 ]; then bad "handsetdeafmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.handsetdeafmut.log"
    if grep -q "\[HANDSET\] got: abc" "$L"; then
        bad "handsetdeafmut: the count ignored and the line STILL arrived — the oracle cannot see that defect"
    else
        ok "handsetdeafmut: with the count ignored the deaf ear kept the line — and the oracle sees it"
    fi
    build   # leave the tree built from clean sources
}


# ── unattended: a read nobody stands over, proved at boot ─────────────────────
#
# The proof used to be called [USB ASYNC TEST], and on this stand it never
# touched USB: the volume sits on AHCI under q35 (UEFI, and BIOS with
# STRICT=on) and on the legacy channel under a plain BIOS run. On AHCI it was
# red on every boot — the completion is delivered by an interrupt the BSP has
# not opened yet, and the proof turned only the USB handle — and on the legacy
# channel it said nothing at all. Measured with a
# probe: PxCI clear, PxIS.DHRS set, the MSI pending in the LAPIC's IRR, and the
# callback landing 259 ms after the proof had given up and returned, at the
# address of a stack frame that no longer existed.
#
# Now it turns the seat's own handle, has no clock of its own, and speaks on
# every boot — saying where it was not asked, and why.
unattended_boot() {   # $1 = tag, the rest = run-bg arguments
    local tag=$1; shift
    make run-stop >/dev/null 2>&1
    make run-bg "$@" >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "hw"
    sleep 1
    key_press ret
    sleep 4
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$tag.log"
}

run_unattended() {
    echo "== unattended: the volume's seat answers a read nobody stands over, at boot =="
    build STRICT=on
    unattended_boot unattended UEFI=on STRICT=on CORES=4 MEM=4G
    L="$SCRATCH/serial.unattended.log"
    grep -q "BoxOS Shell" "$L"; chk $? "unattended: UEFI 4c reaches the shell"
    grep -qE "\[UNATTENDED READ\] seat [0-9]+ \(AHCI port [0-9]+\): PASSED — 4096 bytes read" "$L"
    chk $? "unattended: the AHCI seat under the volume answered, and the bytes agree"
    ! grep -q "\[UNATTENDED READ\].*FAILED" "$L"; chk $? "unattended: and nothing about it failed"
    # Judged during boot, before the machine was running — not by a late answer.
    local judged finished
    judged=$(grep -n "\[UNATTENDED READ\]" "$L" | head -1 | cut -d: -f1)
    finished=$(grep -n "Kernel initialization complete" "$L" | head -1 | cut -d: -f1)
    [ -n "$judged" ] && [ -n "$finished" ] && [ "$judged" -lt "$finished" ]
    chk $? "unattended: judged before the end of boot (line ${judged:-?} < ${finished:-?})"
    ! grep -q "went past its deadline" "$L"; chk $? "unattended: no port was called overdue"
    external_ok "$L"; chk $? "unattended: and the shell runs a program afterwards"

    # Where the machine will never read this way it says so, and asks nothing.
    unattended_boot unattended1c UEFI=on STRICT=on CORES=1 MEM=2G
    L="$SCRATCH/serial.unattended1c.log"
    grep -q "\[UNATTENDED READ\] not asked: with one core every read is attended" "$L"
    chk $? "unattended: one core — not asked, and said"
    # BIOS with STRICT=on is q35 too, so its volume is on AHCI and the proof is
    # asked. Until 2026-09-27 run-bg left it on i440fx, and this leg read "not
    # asked" off the legacy channel while `make run STRICT=on` wrote through
    # AHCI — the two stands disagreed and this scenario confirmed the wrong one.
    unattended_boot unattendedq35 STRICT=on CORES=4 MEM=4G
    L="$SCRATCH/serial.unattendedq35.log"
    grep -qE "\[UNATTENDED READ\] seat [0-9]+ \(AHCI port [0-9]+\): PASSED — 4096 bytes read" "$L"
    chk $? "unattended: BIOS on q35 — the AHCI seat answered, and the bytes agree"
    ! grep -q "\[UNATTENDED READ\].*FAILED" "$L"; chk $? "unattended: BIOS on q35 — nothing about it failed"
    # A plain BIOS run is i440fx: the legacy channel, the one machine left that
    # has it, whose seat answers only a read somebody stands over.
    build
    unattended_boot unattendedbios CORES=4 MEM=4G
    L="$SCRATCH/serial.unattendedbios.log"
    grep -qE "\[UNATTENDED READ\] not asked: seat [0-9]+ \(ATA [a-z ]+\) answers only a read somebody stands over" "$L"
    chk $? "unattended: the legacy channel — not asked, and said"
    lockrank_judge "$SCRATCH/serial.unattended.log $SCRATCH/serial.unattended1c.log $SCRATCH/serial.unattendedq35.log $SCRATCH/serial.unattendedbios.log" "unattended"
}

# The disk's answer withheld: Tier 1 of the AHCI watch — the level the proof
# brings a completion in by — is switched off, so nothing can bring it in
# before the machine is running. The proof must then be ended by the disk's
# own patience (Tier 2, CONFIG_AHCI_IO_TIMEOUT_MS): the port is called overdue,
# the slot is failed and given back, the link is reset, the proof says the
# medium refused — and the machine boots. No clock of the proof's own is
# involved, which is what the deadline line measures.
unattended_silence_on() {
    mutate src/kernel/drivers/disk/ahci.c
    python3 - <<'EOF'
p = "src/kernel/drivers/disk/ahci.c"
s = open(p).read()
anchor = """        if (!port_error) {
            uint32_t outstanding = state->ncq"""
assert anchor in s, "unattended silence anchor missing"
mut = """        if (!port_error && 0) {   /* logcheck mutation: the disk's answer is never brought in by its level */
            uint32_t outstanding = state->ncq"""
open(p, "w").write(s.replace(anchor, mut, 1))
EOF
    mut_done src/kernel/drivers/disk/ahci.c "logcheck mutation: the disk's answer"
}
unattended_silence_off() {
    restore src/kernel/drivers/disk/ahci.c
}

# The wrong block: the unattended read asks for the volume's first sector
# instead of the one the attended read fetched. The bytes must disagree, and
# the proof must say at which byte.
unattended_wrongblock_on() {
    mutate src/kernel/core/boardroom/boardroom.c
    python3 - <<'EOF'
p = "src/kernel/core/boardroom/boardroom.c"
s = open(p).read()
anchor = """    error_t sub = BoardroomReadAsync(seat, 0, sectors, dma,"""
assert anchor in s, "unattended wrong-block anchor missing"
mut = """    error_t sub = BoardroomReadAsync(seat, 2048, sectors, dma,   /* logcheck mutation: a different block than the one compared against */"""
open(p, "w").write(s.replace(anchor, mut, 1))
EOF
    mut_done src/kernel/core/boardroom/boardroom.c "logcheck mutation: a different block"
}
unattended_wrongblock_off() {
    restore src/kernel/core/boardroom/boardroom.c
}

run_unattendedmut() {
    echo "== unattendedmut: withhold the disk's answer, then ask for the wrong block =="
    unattended_silence_on; build STRICT=on
    # The wait is the disk's own patience, thirty seconds; the boot waits it out.
    unattended_boot unattendedmut UEFI=on STRICT=on CORES=4 MEM=4G
    unattended_silence_off
    L="$SCRATCH/serial.unattendedmut.log"
    grep -qE "\[AHCI\] port [0-9]+ went past its deadline — the command\(s\) on slots 0x[0-9a-f]+ are failed and the link is reset" "$L"
    chk $? "unattendedmut: the disk's own watch called the port overdue"
    grep -qE "\[AHCI\] port [0-9]+: back after commands past their deadline, by a link reset — 1 command\(s\) failed, 0 sent again" "$L"
    chk $? "unattendedmut: and its recovery failed that one read and gave the slot back"
    grep -qE "\[UNATTENDED READ\] seat [0-9]+ \(AHCI port [0-9]+\): FAILED — the medium refused the read \(error -?[0-9]+\)" "$L"
    chk $? "unattendedmut: and the proof said the medium refused"
    ! grep -q "\[UNATTENDED READ\].*PASSED" "$L"; chk $? "unattendedmut: no PASSED was said"
    grep -q "BoxOS Shell" "$L"; chk $? "unattendedmut: the machine still boots"
    external_ok "$L"; chk $? "unattendedmut: and runs a program afterwards"
    ! grep -qE "PANIC|^\[EXCEPTION\]" "$L"; chk $? "unattendedmut: no panic, no exception"

    unattended_wrongblock_on; build STRICT=on
    unattended_boot unattendedwrong UEFI=on STRICT=on CORES=4 MEM=4G
    unattended_wrongblock_off
    L="$SCRATCH/serial.unattendedwrong.log"
    grep -qE "\[UNATTENDED READ\] seat [0-9]+ \(AHCI port [0-9]+\): FAILED — the two reads disagree, first at byte [0-9]+" "$L"
    chk $? "unattendedwrong: the bytes disagreed and the proof said where"
    grep -q "BoxOS Shell" "$L"; chk $? "unattendedwrong: the machine still boots"
}

# ---------------------------------------------------------------------------
# footing: the kernel's own map goes under the firmware, and a fall inside the
# firmware does not take the reboot ladder with it.
#
# Every runtime-services call now enters the firmware through Footing (the
# kernel CR3 loaded for the duration of the call) and under a Lifeline (a fault
# in foreign code throws control back instead of panicking). A UEFI boot that
# is told to reboot is the one path that exercises both, and nothing on this
# desk exercised either before this scenario existed.
#
# ‼ WHAT THIS DESK CANNOT DECIDE, said here so nobody reads more into a green
# run than it carries: on QEMU the reboot op already runs on the kernel map, so
# Footing reports "already on the kernel map" and changes nothing. Whether a
# real board enters the firmware in a process cabin is a question only that
# board answers. What this gate proves is that the measurement SPEAKS, that the
# two mechanisms cost the machine nothing when they are not needed, and that
# the machine still reboots.
# ---------------------------------------------------------------------------
run_footing() {
    echo "== footing: the map under the firmware, and the ladder that survives it =="
    build STRICT=on
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    local reached=0
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null && reached=1
    sleep 3
    type_cmd "reboot"
    sleep 1; key_press ret
    wait_for_line build/serial.log "Rebooting" "$WAIT_QUIET_S"
    sleep 5
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.footing.log"
    L="$SCRATCH/serial.footing.log"

    [ $reached -eq 1 ]; chk $? "footing: the uefi boot reached the shell"

    # The measurement must SPEAK, and it must say which of the two it was.
    #
    # ‼ Both alternatives are the kernel's OWN words, copied from the ternary
    # in EfiResetSystem. This read "NOT the kernel map" first — a string no
    # build emits — so on this bench, where the first branch always prints,
    # the alternation only ever tested one arm and the other could have been
    # renamed away without a gate noticing.
    grep -qE "\[EFI\] ResetSystem\(type=1.*(already on the kernel map|the firmware was about to be entered in a process cabin)" "$L"
    chk $? "footing: the reset says which map the firmware was entered on"

    grep -q "\[EFI\] ResetSystem(type=1" "$L"; chk $? "footing: and names g_rt, the pointer the call dereferences"

    # The map itself, dumped once so any faulting address can be placed in it.
    local descs
    descs=$(grep -c "^\[EFI\]   \[" "$L")
    [ "${descs:-0}" -gt 0 ]; chk $? "footing: every EFI descriptor is named on the boot ($descs)"
    grep -q "RUNTIME" "$L"; chk $? "footing: and the runtime ones are marked as such"
    grep -qE "^\[EFI\]   \[ *[0-9]+\] RuntimeServices(Code|Data) .*virt=0xffffff" "$L"
    chk $? "footing: a runtime region carries the virtual address this kernel gave it"

    # Lifeline must stay silent when the firmware behaves. A throwback here
    # would mean OVMF faulted, which it does not — so a line is a regression,
    # not a pass.
    grep -q "\[LIFELINE\]" "$L" \
        && bad "footing: a lifeline was thrown on a bench where the firmware does not fall" \
        || ok "footing: the lifeline stayed out of the way"

    # And the machine actually came back.
    local boots
    boots=$(grep -c "BoxOS Shell" "$L")
    [ "${boots:-0}" -ge 2 ]; chk $? "footing: the machine rebooted and came back up ($boots boot(s))"

    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "footing: the kernel spoke of a stall or a fault" \
        || ok "footing: no verdict, no panic"
}

# ===========================================================================
# census — the host's roll call and the kernel's name the same blocks
# ===========================================================================
#
# `tools/volume-read.py check` holds two roll calls of one image: the CENSUS,
# every owner of every block found the way the volume's writers laid it down,
# and the KERNELVIEW, a simulation of what the mount in this tree decides —
# which runs its fsck calls orphan and missing, which files it loses, what
# free count it prints. The simulation is only worth anything if it IS the
# mount, and the only way to know that is to boot the image and hold the
# kernel's own lines against it.
#
# So an image is planted with damage the mount must see (self-test's
# `reconcile`: two orphan blocks, a registry chained past data block 0, a
# block of a live file cleared in the bitmap, a file whose only record was
# retired), the tool predicts every line, and the machine boots it. The kernel
# names its runs (MapTallySay, block_map_mount.c) — the first 32 of each kind one by
# one, ascending, and the rest in one line that counts them — and every named
# run, every count and the free figure must be the ones predicted, and every
# orphan the kernel names one the census cannot find an owner for. A kernel
# that calls an owned block an orphan is one whose roll call would free a live
# block the day the repair starts giving orphans back. A second image, C17,
# carries 40 orphan runs, so the line that counts the rest is held to its
# numbers too.
#
# The rest hold the seal's decisions to the same prediction: S1, a newest seal
# the medium did not keep, fallen back from; S2, an open block torn under both
# seals, rolled back, every file whose only record settled there said lost;
# S7, the newest seal alone, its open block torn — nothing rolled back; S8,
# the other seal's open block a closed block of the newest, whose open block
# alone rolls back (the owner's B3); GL1, a chain whose first block Glean
# moved, the head's old blocks the pool's still; R2, a file's newest record
# rotten in a sealed span, the file standing on the record before it and that
# record's block another file's; R3, a file's only record rotten in a sealed
# span, a file the seal had settled — lost, said, never let go as a cycle that
# did not settle. And what the mount reads before it trusts:
# C21b, a Quitclaim whose row was left, which goes with it; C24, a record's
# file id rotten, which names no id; C25, an extent past the data run, which
# is not followed; C26, a record's length rotten, the rest of its block said
# unread. Each boot is held to the line the kernel says of the seal it stood
# on, of the rows it let go or set, the records it passed over or did not
# follow, the bytes of a block it did not read, the blocks before the chain's
# first, a registry rolled back, the next file id and the blocks named twice —
# every one the census's prediction.
#
# Host only, beside them: (A) a real blklogwrites log written by qemu-io, read
# back by `cut` and replayed onto its base, byte for byte; and volume-read's
# self-test in its plain form on a fresh image, the one gate that answers for
# the rules no boot plants — TUSE, Spare, BOOT, the grounds, the constants
# held against the headers.

census_qemu_io() {
    local d="$SCRATCH/census-io"
    if ! command -v qemu-io >/dev/null 2>&1; then
        harness "census: qemu-io is not on this desk, so no real blklogwrites log can be made"
        return
    fi
    rm -rf "$d"; mkdir -p "$d"
    dd if=/dev/zero of="$d/base.img" bs=1048576 count=2 2>/dev/null
    cp "$d/base.img" "$d/run.img"
    : > "$d/io.log"
    qemu-io --image-opts "driver=blklogwrites,file.driver=file,file.filename=$d/run.img,log.driver=file,log.filename=$d/io.log,log-sector-size=512" \
        -c 'write -P 0xa1 0 4k' -c 'flush' -c 'write -P 0xb2 8k 1k' \
        -c 'write -f -P 0xc3 16k 512' -c 'write -z 32k 4k' >"$d/qemu-io.out" 2>&1 \
        || { harness "census: qemu-io would not write through blklogwrites: $(tail -1 "$d/qemu-io.out")"; return; }

    local said
    said=$(python3 tools/volume-read.py "$d/base.img" cut "$d/io.log" --against "$d/run.img" 2>&1)
    chk $? "census: $said"

    local epochs
    epochs=$(python3 tools/volume-read.py "$d/base.img" cut "$d/io.log" --count --json 2>&1 \
             | python3 -c 'import json,sys; print(json.load(sys.stdin)["epochs"])' 2>&1)
    case "$epochs" in
        "[1, 2, 1"*) ok "census: qemu-io's log splits into epochs $epochs — write, flush, write + FUA write, zeroes" ;;
        *)           bad "census: qemu-io's log splits into epochs $epochs, not [1, 2, 1, ...]" ;;
    esac
}

# census_selftest — volume-read's self-test, every case, on a fresh image.
census_selftest() {
    local rc said counts
    rm -f build/boxos.img
    build
    python3 tools/volume-read.py build/boxos.img self-test >"$SCRATCH/census.selftest.out" 2>&1
    rc=$?
    said=$(tail -1 "$SCRATCH/census.selftest.out")
    counts=$(printf '%s\n' "$said" | sed -nE 's/^self-test: ([0-9]+) of ([0-9]+) cases answered as expected$/\1 \2/p')
    [ "$rc" = 0 ] && [ -n "$counts" ] && [ "${counts% *}" = "${counts#* }" ]
    chk $? "census: volume-read's self-test on a fresh image — ${said:-no answer} (exit $rc)"
    grep -v -e 'as expected$' -e '^self-test: aiming at' -e 'cases answered as expected$' \
        "$SCRATCH/census.selftest.out" | sed 's/^/  /'
}

# census_boot NAME CASE — a fresh image, planted with self-test's CASE,
# predicted, and booted as the disk it was written to: make builds no disk it
# was handed, so what boots is what was predicted. It says why itself when it
# answers 1 — a plant that would not take, the boot a harness fault, or a
# machine that never reached its shell — and its caller counts nothing more.
census_boot() {
    local img="$SCRATCH/census.$1.img"
    rm -f build/boxos.img        # a volume that booted before is not the one predicted
    build
    make run-stop >/dev/null 2>&1
    if ! python3 tools/volume-read.py build/boxos.img self-test --emit "$2" --out "$img" \
            >"$SCRATCH/census.$1.emit" 2>&1; then
        bad "census: $2 could not be planted in a fresh image — $(tail -1 "$SCRATCH/census.$1.emit")"
        return 1
    fi
    python3 tools/volume-read.py "$img" check --json >"$SCRATCH/census.$1.json"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/census.$1.run-bg" 2>&1; then
        harness "census: run-bg would not start the volume planted with $2: $(tail -1 "$SCRATCH/census.$1.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    local rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
    [ $rc = 0 ] || bad "census: the volume planted with $2 never reached a shell"
    return $rc
}

# census_judge NAME — the prediction against the kernel's own lines, one
# "ok|bad <sentence>" line per assertion into $SCRATCH/census.NAME.verdict.
census_judge() {
    python3 - "$SCRATCH/census.$1.json" "$SCRATCH/serial.$1.log" \
        >"$SCRATCH/census.$1.verdict" 2>&1 <<'CENSUS_EOF'
import json, re, sys
pred = json.load(open(sys.argv[1]))
log = open(sys.argv[2], errors="replace").read()
k = pred["kernel"]

NAMED = 32    # MapTallySay names this many runs of each kind, and counts the rest in one line

def say(held, text):
    print(("ok " if held else "bad ") + text)

def said(kind):
    """What the mount says of one kind: the runs it names, in the order it
    names them, and its line for the rest as [(runs, blocks)] — empty when it
    says none."""
    named = [(int(a), int(b)) for a, b in re.findall(
        r"\[TagFS FSCK\] %s run at data block (\d+), (\d+) block\(s\)" % kind, log)]
    rest = [(int(a), int(b)) for a, b in re.findall(
        r"\[TagFS FSCK\] and (\d+) more %s run\(s\), (\d+) block\(s\) — tools/volume-read\.py names "
        r"every one" % kind, log)]
    return named, rest

def due(runs):
    """What the mount must say of these runs: the first NAMED ascending, and
    the count and blocks of the rest when there are any."""
    runs = sorted(tuple(r) for r in runs)
    rest = runs[NAMED:]
    return runs[:NAMED], [(len(rest), sum(n for _s, n in rest))] if rest else []

def text(rs):
    return ", ".join("%u+%u" % r for r in rs) or "none"

def more(rest):
    return "%u more run(s), %u block(s)" % rest[0] if rest else "no more"

if not k["mounts"]:
    say(False, "the census predicts no mount at all: %s" % k["refusal"])
    sys.exit(0)
fsck = k["fsck"]
m = re.search(r"is MOUNTED — (\d+) file", log)
say(m is not None, "the kernel mounted the planted volume — %s file(s)" % (m.group(1) if m else "no MOUNTED line"))
# The mount says the line only when it has something to repair
# (block_map_mount.c): no line is 0 orphan, 0 missing.
m = re.search(r"\[TagFS FSCK\] Bitmap mismatch: (\d+) orphan, (\d+) missing", log)
have = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
say(have == (fsck["orphan"], fsck["missing"]),
    "the kernel's mismatch line says %s; the census predicted %u orphan, %u missing"
    % ("%u orphan, %u missing" % have if m else "nothing", fsck["orphan"], fsck["missing"]))
for kind in ("orphan", "missing"):
    (got, got_rest), (want, want_rest) = said(kind), due(fsck[kind + "_runs"])
    say(got == want, "the kernel names %s runs %s; the census predicted %s" % (kind, text(got), text(want)))
    say(got_rest == want_rest, "past the %u %s runs it names the kernel counts %s; the census predicted %s"
        % (NAMED, kind, more(got_rest) if len(got_rest) < 2 else "%u lines of them" % len(got_rest),
           more(want_rest)))
m = re.search(r"\[TagFS FSCK\] (\d+) file table entry\(s\) corrected from this volume's "
              r"metadata pool, (\d+) file\(s\) lost", log)
have = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
say(have == (fsck["corrected"], fsck["lost"]),
    "the kernel corrected %u table entry(s) and lost %u file(s); the census predicted %u and %u"
    % (have + (fsck["corrected"], fsck["lost"])))
m = re.search(r"\[TagFS\] (\d+) data blocks, (\d+) free, (\d+) files", log)
have = tuple(int(x) for x in m.groups()) if m else None
want = (k["says"]["data_blocks"], k["says"]["free"], k["says"]["files"])
say(have == want, "the kernel says %s; the census predicted \"%u data blocks, %u free, %u files\""
    % ('"%u data blocks, %u free, %u files"' % have if have else "nothing", *want))
owned = []
named, rest = said("orphan")
for a, n in named:
    for c in pred["census"]["claims"]:
        for s, cn in c["runs"]:
            lo, hi = max(a, s), min(a + n, s + cn)
            if lo < hi:
                owned.append("%s at data block %u+%u" % (c["label"], lo, hi - lo))
census_named, census_rest = due(pred["census"]["orphan_runs"])
say(not owned and (named, rest) == (census_named, census_rest),
    "every orphan the kernel names (%s; %s) is one the census finds no owner for (%s; %s)%s"
    % (text(named), more(rest), text(census_named), more(census_rest),
       " — the kernel calls OWNED blocks orphans: " + "; ".join(owned) if owned else ""))

# What the mount stood on, and what it rolled back, in the kernel's own words
# (mount.c MountSealsRead, meta_pool_mount.c SayFellBack / WalkChoose).
seal = k["seal"]
m_none = re.search(r"\[TagFS\] this volume's newest Ledger carries no seal", log)
m_fell = re.search(r"\[TagFS\] fell back: the newest seal \(cycle \d+\)[^\n]*so the volume stands on "
                   r"cycle (\d+)", log)
m_torn = re.search(r"\[TagFS\] the pool's open block (\d+) does not hold what [^\n]*ROLLED BACK", log)
m_alone = re.search(r"\[TagFS\] the newest seal \(cycle \d+\) is the only one the Ledger holds, and "
                    r"the pool's open block (\d+) does not hold what it says", log)
m_unborne = re.search(r"\[TagFS\] no copy's seal is borne out by the pool's closed blocks", log)
have = ("no seal on the newest copy" if m_none else
        "the seal of cycle %s, fallen back to" % m_fell.group(1) if m_fell else
        "its open block %s rolled back" % m_torn.group(1) if m_torn else
        "the newest seal alone, not borne out" if m_alone else
        "no seal borne out" if m_unborne else "the newest seal")
want = ("no seal on the newest copy" if not seal["newest_sealed"] else
        "the seal of cycle %s, fallen back to" % seal["settle"] if seal["fell_back"] else
        "its open block %s rolled back" % seal["to_block"] if seal["stood"] == "torn" else
        "the newest seal alone, not borne out" if seal["alone"] else
        "no seal borne out" if seal["stood"] == "none" else "the newest seal")
say(have == want, "the kernel's pool stands on %s; the census predicted %s" % (have, want))

def count(pattern, groups=1):
    m = re.search(pattern, log)
    return tuple(int(x) for x in m.groups()) if m else (0,) * groups

have = count(r"\[TagFS\] the file table is derived from the sealed pool: (\d+) row\(s\) named files "
             r"of a cycle that did not settle and are let go, (\d+) row\(s\) the table lacked are set "
             r"from the pool", 2)
say(have == (fsck["unsettled"], fsck["derived"]),
    "the kernel let %u row(s) go unsettled and set %u from the pool; the census predicted %u and %u"
    % (have + (fsck["unsettled"], fsck["derived"])))
have = count(r"\[TagFS FSCK\] (\d+) ACTIVE record\(s\) of the pool no table row names")
say(have == (fsck["unnamed"],), "the kernel took up no record of %u unnamed one(s); the census "
    "predicted %u" % (have + (fsck["unnamed"],)))
have = count(r"\[MetaPool\] (\d+) record\(s\) of this volume's pool do not check out")
say(have == (fsck["unreadable"],), "the kernel passed %u record(s) over that do not check out; the "
    "census predicted %u" % (have + (fsck["unreadable"],)))
have = count(r"\[TagFS\] (\d+) table row\(s\) named files whose newest record is a Quitclaim")
say(have == (fsck["quitclaimed"],), "the kernel let %u row(s) go with the Quitclaim of their file; "
    "the census predicted %u" % (have + (fsck["quitclaimed"],)))
have = count(r"\[MetaPool\] (\d+) block\(s\) of this volume's pool end in (\d+) byte\(s\) that do "
             r"not read as records [^\n]*the first at block (\d+), byte (\d+)", 4)
want = tuple(fsck["skipped"]) + tuple(fsck["skipped_first"] or (0, 0))
say(have == want, "the kernel left %u block(s) unread past a record that is not one, %u byte(s), the "
    "first at block %u byte %u; the census predicted %u, %u, %u, %u" % (have + want))
have = count(r"\[MetaPool\] (\d+) record\(s\) of this volume's pool name a block outside its data "
             r"run of \d+ blocks[^\n]*the first is file (\d+)'s", 2)
want = (fsck["dropped"], fsck["dropped_first"] or 0)
say(have == want, "the kernel followed none of the extents of %u record(s), the first file %u's; the "
    "census predicted %u, file %u's" % (have + want))
have = count(r"\[Glean\] (\d+) block\(s\) the pool's head still names lie before the chain's first")
say(have == (len(seal["prefix"]),), "the kernel rests %u block(s) the pool's head names before the "
    "chain; the census predicted %u (%s)" % (have + (len(seal["prefix"]), seal["prefix"])))
m = re.search(r"\[TagRegistry\] block (\d+), the registry's last, does not hold what the volume's "
              r"seal says", log)
have = int(m.group(1)) if m else None
say(have == seal["registry_rolled"], "the kernel rolled back registry block %s; the census predicted %s"
    % (have, seal["registry_rolled"]))
stood = next(c for c in pred["ledger"]["copies"] if c["copy"] == pred["ledger"]["standing_on"])
m = re.search(r"\[TagFS\] the Ledger's next file id is \d+ and the medium names file id \d+ — "
              r"[^\n]*so the next file is given (\d+)", log)
have = int(m.group(1)) if m else stood["next_file_id"]
say(have == k["next_file_id"], "the kernel gives the next file id %u; the census predicted %u"
    % (have, k["next_file_id"]))
have = sorted(int(b) for b in re.findall(r"\[TagFS FSCK\] data block (\d+) is named by [^\n]* at once", log))
want = sorted(s for s, _n in pred["census"]["double"])
say(have == want, "the kernel names data block(s) %s twice owned; the census predicted %s"
    % (have or "none", want or "none"))
CENSUS_EOF
    local rc=$?
    [ $rc = 0 ] && [ -s "$SCRATCH/census.$1.verdict" ] && return 0
    echo "bad the judge could not read $SCRATCH/census.$1.json against $SCRATCH/serial.$1.log: $(tail -1 "$SCRATCH/census.$1.verdict")" \
        > "$SCRATCH/census.$1.verdict"
}

run_census() {
    echo "== census: the host's roll call and the kernel's name the same blocks =="
    local plant line
    census_qemu_io
    census_selftest
    for plant in reconcile C17 S1 S2 S7 S8 GL1 R2 R3 C21b C24 C25 C26; do
        census_boot "$plant" "$plant" || continue
        census_judge "$plant"
        while IFS= read -r line; do
            case "$line" in
                "ok "*)  ok  "census $plant: ${line#ok }" ;;
                "bad "*) bad "census $plant: ${line#bad }" ;;
            esac
        done < "$SCRATCH/census.$plant.verdict"
        lockrank_judge "$SCRATCH/serial.$plant.log" "census $plant"
    done
}

# The two halves of the reconcile must each be able to go red. The kernel's:
# a census the pool does not claim its chain in calls pool[1] an orphan. The
# host's: a census that calls every retired record damage — they fail their
# sum by construction — is a census that cries wolf on every volume that ever
# deleted a file.
census_poolblind_on() {
    mutate src/kernel/tagfs/mount/mount.c
    python3 - <<'EOF2'
p = "src/kernel/tagfs/mount/mount.c"
s = open(p).read()
anchor = "    PoolClaimGround(MountDataBlock(fs->layout.metadata_pool_block));\n"
assert s.count(anchor) == 1, "census pool-chain anchor missing"
s = s.replace(anchor, "    /* logcheck mutation: the pool chain is not claimed */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/tagfs/mount/mount.c "logcheck mutation"
}

census_exempt_on() {
    mutate tools/volumeread/census.py
    python3 - <<'EOF2'
p = "tools/volumeread/census.py"
s = open(p).read()
anchor = "            if r.fid != 0 and not r.unpack_ok:\n"
assert s.count(anchor) == 1, "census exemption anchor missing"
s = s.replace(anchor, "            if not r.unpack_ok:  # logcheck mutation: retired records are called bad\n", 1)
open(p, "w").write(s)
EOF2
    mut_done tools/volumeread/census.py "logcheck mutation"
}

run_censusmut() {
    echo "== censusmut: with either half broken, the reconcile must go red =="
    census_poolblind_on
    if census_boot censusmut reconcile; then
        census_judge censusmut
        local reds
        reds=$(grep -c '^bad ' "$SCRATCH/census.censusmut.verdict")
        if [ "${reds:-0}" -gt 0 ]; then
            ok "censusmut: the kernel left the pool chain out and $reds assertion(s) went red — $(grep -m1 '^bad ' "$SCRATCH/census.censusmut.verdict" | cut -c5-)"
        else
            bad "censusmut: the kernel left the pool chain out of its roll call and the reconcile stayed green"
        fi
    fi
    restore src/kernel/tagfs/mount/mount.c
    build

    census_exempt_on
    python3 tools/volume-read.py build/boxos.img self-test >"$SCRATCH/census.exempt.out" 2>&1
    local rc=$?
    restore tools/volumeread/census.py
    local c14
    c14=$(grep -m1 "C14 retired is not bad" "$SCRATCH/census.exempt.out")
    if [ $rc -ne 0 ] && echo "$c14" | grep -q "unexpected RECORD_CRC"; then
        ok "censusmut: a census that calls retired records bad fails its self-test (exit $rc) — ${c14#self-test: }"
    else
        bad "censusmut: a census that calls retired records bad passed its self-test (exit $rc): ${c14:-no C14 line}"
    fi
    build   # every put-back discards the image; leave one built from clean sources
}

# ---------------------------------------------------------------------------
# stand: the startup tests that write — every suite a VOLUME_TESTS=on kernel
# runs at boot — as a gate that stays.
#
# Those tests could only ever be read by hand: nothing here built them, and a
# run where one of them failed, or a caveat stopped holding, or a fault
# mechanism quietly stopped refusing anything, looked from the outside exactly
# like one where nothing happened. So they are booted on the two roads the
# tests take to the medium — plain BIOS, the legacy channel (ATA), and UEFI on
# q35, AHCI — each on a volume made fresh by the build, and the boot is held
# to what it must say: nothing failed, exactly the caveats this table names
# are held, each with the number it measured, the faults fired where they
# were aimed, the road the writes took, and a halt whose sync wrote the
# volume — what the tests gave back reaches the medium only there.
#
# Then the volume that boot left is booted a second time: a test that leaked a
# block, or gave one back twice, leaves a volume whose next roll call says
# "Bitmap mismatch". That boot is judged only on the volume the first one
# really left: the first must have said "Storage sync complete", the image
# must still carry the Deed the build laid down (a repack lays down another),
# and the second boot stands on a copy of it named by BOOTDISK, which run-bg
# boots as it stands, so no source touched in between can repack a fresh
# volume under it. Every boot starts on an empty serial log, and one that
# started no machine is the harness's failure — never a verdict read off the
# log the last boot left.
#
# The held evidence is the same on the second boot as on the first, and on
# both roads — measured — so it is held to on every boot. A number that moves
# is a change to what a test saw, and this says so; the step that lifts a
# caveat deletes its row here in the same commit.
# ---------------------------------------------------------------------------

# NAME|STEP|HELD|LIFTED|ROADS — every caveat a VOLUME_TESTS boot holds: the
# evidence it gives while its defect is there, the evidence it gives once
# standmut has put its step's fix in, and the roads it is held on — both (or
# empty), bios or uefi. A row written for one road only is a test that cannot
# be asked on the other and stands down there; held on a road it is not
# written for is a failure. How many caveats a boot holds is counted off these
# rows, per road. A sweep's LIFTED count is how many allocations the operation
# takes on a fresh volume of this tree.
STAND_HELD="
"

# How many startup tests a VOLUME_TESTS boot runs: every TestCase of every
# suite in tests.h TEST_SUITES — the plain build's 22 (healthy says how) and
# the 199 of the stand's own twenty-seven suites: stand 28, stand_lock 4,
# stand_chain 7, stand_spelling 6, stand_roadstead 4, stand_map 9,
# stand_plot 10, stand_index 6, stand_ink 6, stand_helm 7, stand_port 2,
# stand_use 3, stand_use_chain 4, stand_seal 8, stand_settle 14, stand_glean 9,
# stand_snapshot 1, stand_read 8, stand_run 4, stand_medium 19,
# stand_warrant 8, stand_room 7, stand_integrity 2, frame 2, width 7,
# parcel 13, tract 1. Counted off the code, not a
# run (K9): a suite that drops out of the walk is a test that stopped covering
# anything, and the count is where that shows. How many of them pass and how
# many stand aside is the road's — said, not pinned.
STAND_RUN=221

# What a VOLUME_TESTS kernel says of a record memory holds that is not the
# file a format-v1 Ink packed (plot_conveyance.c, PackV1Agrees and
# PlotConveyanceAgrees), of a live file the map holds a row and no record of,
# or the other way round (file_map.c, FileMapRows), of a record that names a
# run of no blocks (file_record.c, FileRecordBuild), and of a Tract about to
# stand taller than its fill allows (tract.c, TractTooTall): a STRICT kernel
# stops at each, and on a road that is not STRICT the line itself is the
# failure.
STAND_MIRROR_DEFECT='\[TagFS\] DEFECT: (the record memory holds of file|[0-9]+ file\(s\) of the map of live files|file [0-9]+.s record names a run of no blocks|a Tract of [0-9]+ levels was about to be built)'

# The last thing a halt says about the volume, whichever way its sync went
# (system_halt.c, halt_sync_storage). Not a bare ‼: that also opens "DID NOT
# WRITE ITS RECORDS", which comes BEFORE the flush, and a machine stopped
# there is stopped in the middle of it.
STAND_HALT_ENDS='^\[HALT\] (Storage sync complete|the medium on seat [0-9]+ took|‼ THE VOLUME ON SEAT [0-9]+ .* WAS NOT MADE DURABLE)'

# stand_boot TAG KEYS... — boot the image that is on the disk (or the one
# BOOTDISK names) under VOLUME_TESTS=on and KEYS (the machine), wait for the
# startup tests' summary — or a panic, which ends them too — and halt through
# `bye`, so the halt's sync carries what the tests gave back to the medium.
# The key goes on run-bg as well: it owns a rebuild marker, and a run-bg
# without it rebuilds the kernel without the tests.
#
# Answers 0 when the machine halted and said its sync wrote the volume, 1 when
# a machine ran — its log is kept — and did not, and 2 when no machine was
# started at all, which is said as the harness's failure: there is no log of
# this boot to judge, only the last one's.
stand_boot() {
    local tag=$1 halted=1; shift
    make run-stop >/dev/null 2>&1
    rm -f build/serial.log
    if ! make run-bg VOLUME_TESTS=on "$@" >"$SCRATCH/runbg.$tag.log" 2>&1 || [ ! -S build/qemu.mon ]; then
        harness "stand: no machine was started for $tag — $(tail -1 "$SCRATCH/runbg.$tag.log" | tr -d '\r')"
        make run-stop >/dev/null 2>&1
        return 2
    fi
    if wait_for_line -E build/serial.log 'TagFS: [0-9]+ run, |KERNEL PANIC' "$WAIT_QUIET_S" &&
       grep -qE 'TagFS: [0-9]+ run, ' build/serial.log && wait_for_prompt && type_line "bye" &&
       wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" &&
       grep -aq '^\[HALT\] Storage sync complete' build/serial.log; then
        halted=0
    fi
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$tag.log"
    return $halted
}

# stand_summary LOG — the startup tests' summary line, without its tag.
stand_summary() {
    grep -aE '^\[TESTS\] TagFS: [0-9]+ run, ' "$1" | tr -d '\r' | tail -1 | sed 's/^\[TESTS\] //'
}

# stand_row_on ROADS ROAD — whether a row written for ROADS (both, bios or
# uefi; empty is both) is held on ROAD.
stand_row_on() {
    case "${1:-both}" in
        both|"$2") return 0 ;;
        *)         return 1 ;;
    esac
}

# stand_held_count ROAD — how many caveats a boot on ROAD holds.
stand_held_count() {
    local road=$1 name step held lifted roads n=0
    while IFS='|' read -r name step held lifted roads; do
        [ -n "$name" ] && stand_row_on "$roads" "$road" && n=$((n + 1))
    done <<STAND_HELD_EOF
$STAND_HELD
STAND_HELD_EOF
    echo "$n"
}

# stand_held LOG WHAT ROAD — every row of ROAD held, each with its number, and
# no row of the other road held at all.
stand_held() {
    local L=$1 what=$2 road=$3 name step held lifted roads
    while IFS='|' read -r name step held lifted roads; do
        [ -n "$name" ] || continue
        if stand_row_on "$roads" "$road"; then
            grep -aF "[TESTS] caveat held: $name — " "$L" | grep -qF " — discharged by step $step ($held)"
            chk $? "stand: $what — $name held under step $step ($held)"
        else
            ! grep -aqF "[TESTS] caveat held: $name — " "$L"
            chk $? "stand: $what — $name is not held here: its row is written for the $roads road only"
        fi
    done <<STAND_HELD_EOF
$STAND_HELD
STAND_HELD_EOF
}

# The two orders LockRank names on every stand boot while their steps stand:
# the chain growth tests (stand_chain_tests.c) grow the tag registry and the
# file table on every boot, each under its own lock, and the growth takes the
# volume's state lock under it — the two ABBA of MP-D4, seen without a race.
STAND_ORDERS="registry@tag_registry_flush/state@tagfs_alloc_blocks_inside
table@file_table_flush/state@tagfs_alloc_blocks_inside"

# stand_orders LOG WHAT — each of those places named, while its row carries a
# step. Once the step lifts it, the place is lockrank_judge's to fail if it is
# ever named again.
stand_orders() {
    local L=$1 what=$2 key step
    for key in $STAND_ORDERS; do
        step=$(caveat_step "lockrank:$key")
        [ -n "$step" ] || continue
        lockrank_rows "$L" | grep -qF "|$key|"
        chk $? "stand: $what — LockRank names the order $key, held under step $step: the chain growth tests take it on every boot"
    done
}

# stand_judge TAG WHAT ROAD SEAT BLOCKFAILS — one first boot, held to all of
# it. ROAD is bios or uefi; SEAT is the volume seat's name as an ERE;
# BLOCKFAILS is how many commands the faults refuse on that machine, counted
# off the tests' code, test by test. A refusal is said as "seat N: …"; a
# protection a test puts the seat in (stand_settle_write_protected) is said
# as "seat N protects …" and is no refusal. A plan that only counts
# (BLOCK_FAIL_NEVER: stand_run_megabyte_*, stand_helm_guarded_file_refused)
# refuses nothing, and neither does one armed for a command that never comes
# (stand_flush_ticket's second plan).
#
#   test                                              BIOS 4c  UEFI 4c  USB 4c
#   stand_blockfail_reads_the_nth                      1        1        1
#   stand_blockfail_write_never_lands                  1        1        1
#   stand_blockfail_flush_refused                      1        1        1
#   stand_blockfail_async_read                         0        1        1   no asynchronous read on the legacy channel
#   stand_flush_ticket                                 1        1        1
#   stand_rmw_read_refused                             1        1        1
#   stand_sync_owns_a_refused_write                    1        1        1
#   stand_sync_owns_a_refused_flush                    1        1        1
#   stand_registry_growth_links_what_landed            1        1        1
#   stand_registry_flush_keeps_its_block               1        1        1
#   stand_table_grows_onto_a_stamp                     1        1        1
#   stand_helm_write_refused_part_way                  1        1        1
#   stand_port_write_refused_part_way                  0        1        1   no port on the legacy channel
#   stand_seal_cut_keeps_the_seal                      2        2        2   its open block, then its Ledger
#   stand_seal_torn_table_stands                       2        2        2   its torn block refused, then a bad sector in it
#   stand_seal_bad_open_block_rolls_back               2        2        2   a bad sector: the judging walk, the roll back
#   stand_seal_bad_closed_block_is_rot                 2        2        2   a bad sector: the judging walk, the unsealed walk
#   stand_glean_replaced_waits_for_the_close           2        2        2   a bad sector: the judging walk, the roll back
#   stand_run_refused_inside_a_run                     0        1        1   one block a command: a run has no inside
#   stand_run_refused_inside_a_run_through_the_port    0        1        1   and no port either
#   stand_medium_no_medium_flush_refused               0        0        1   a USB disk's flush, answered NO MEDIUM
#   stand_medium_sync_cache_remembered                 0        0        3   ILLEGAL REQUEST, then NO MEDIUM twice
#   stand_medium_bell_rings_once                       1        1        1
#   stand_medium_mount_rings_in_its_own_breath         1        1        1   the mount's first read of the data
#   stand_medium_mount_bad_sector_waits                4        4        4   both Ledger copies, in each of its two attempts
#   stand_medium_mount_refused_read_is_no_verdict      1        1        1   the pool block a file's record lies in
#   stand_medium_return_refused_once                   1        1        1   the newest Ledger copy, on the return
#   stand_medium_mount_resets_the_seat                 1        1        1   the mount's first read of the data
#   stand_roadstead_refusal_rings_once                 0        1        0   an AHCI port's alone
#   stand_settle_refusal_waits_for_the_bell            1        1        1   its Ledger; then a plan that only counts
#   stand_settle_ring_during_the_cycle                 1        1        1   its Ledger
#   stand_settle_table_refusal_is_the_mediums          2        2        2   the block written alone, then by the cycle
#   total                                             34       39       42
stand_judge() {
    local L="$SCRATCH/serial.$1.log" what=$2 road=$3 seat=$4 want=$5 said got count
    count=$(stand_held_count "$road")
    said=$(stand_summary "$L")
    echo "$said" | grep -qE "^TagFS: $STAND_RUN run, [0-9]+ passed, 0 failed, [0-9]+ skipped, $count held under a caveat"
    chk $? "stand: $what — the $STAND_RUN tests this tree registers ran, nothing failed and exactly $count caveats are held (${said:-no summary})"
    grep -aq '^\[TESTS\] All tests PASSED' "$L"
    chk $? "stand: $what — and the runner answers PASSED"
    ! grep -aq 'caveat lifted early' "$L"
    chk $? "stand: $what — no caveat was lifted early"
    ! grep -aq 'KERNEL PANIC' "$L"
    chk $? "stand: $what — no panic"
    ! grep -aqE "$STAND_MIRROR_DEFECT" "$L"
    chk $? "stand: $what — every record memory held was the file its Ink packed, and every live file had its row"
    grep -aqE "^\[TESTS\] the tests that write go through seat [0-9]+ \($seat\), on the synchronous path" "$L"
    chk $? "stand: $what — the writes' road is said: $(grep -aoE 'go through seat [0-9]+ \([^)]*\)' "$L" | head -1)"
    stand_held "$L" "$what" "$road"
    stand_orders "$L" "$what"
    got=$(grep -ac '^\[BLOCKFAIL\] seat [0-9]*: ' "$L")
    [ "$got" = "$want" ]
    chk $? "stand: $what — BLOCKFAIL refused $got command(s), as many as the tests that arm it refuse on this road ($want)"
    got=$(grep -ac '^\[TagFS\] seat [0-9]* refused a read and no bell rang after the attempt that met it' "$L")
    [ "$got" = 5 ]
    chk $? "stand: $what — a refused seat no bell answered after its attempt was reset once by each of the five tests that leave one so — a bell rung inside the attempt answers nothing — never again (stand_medium_mount_rings_in_its_own_breath, stand_medium_mount_bad_sector_waits, stand_medium_mount_refused_read_is_no_verdict, stand_medium_return_refused_once, stand_medium_mount_resets_the_seat; reset $got time(s))"
    ! grep -aq '^\[TagFS\] seat [0-9]* did not come back from its reset' "$L"
    chk $? "stand: $what — and every seat reset came back from it"
    grep -aqE '^\[BLOCKFAIL\] seat [0-9]+: the read of sectors [0-9]+\.\.[0-9]+ is refused as a medium error — occurrence 2 since arming' "$L"
    chk $? "stand: $what — the read it refused was the second one at its block, not the first"
    grep -aqE '^\[BLOCKFAIL\] seat [0-9]+: the write of sectors [0-9]+\.\.[0-9]+ is refused as a medium error — occurrence 1 since arming, and nothing of it was issued' "$L"
    chk $? "stand: $what — a write refused before it reached the medium"
    grep -aqE '^\[BLOCKFAIL\] seat [0-9]+: the cache flush is refused as a medium error — occurrence 2 since arming' "$L"
    chk $? "stand: $what — the second of three flushes refused"
    got=$(grep -ac '^\[Nightwatch\] SETTLE PARKED: 1 waiter(s) of the volume wait for a cycle, and the cycle refused [0-9]* ms ago waits for the volume.s seat to say seat:ready' "$L")
    [ "$got" = 1 ]
    chk $? "stand: $what — Nightwatch said the refused cycle its waiter waits on, parked a window long, once (stand_settle_refusal_waits_for_the_bell; said $got time(s))"
    grep -aqE '^\[HEAPFAIL\] core [0-9]+: allocation 3 since arming \(32 bytes\) is answered NULL' "$L"
    chk $? "stand: $what — HEAPFAIL refused the third allocation of its plan, a slab one"
    grep -aqE '^\[HEAPFAIL\] core [0-9]+: allocation 4 since arming \(3000 bytes\) is answered NULL' "$L"
    chk $? "stand: $what — and the fourth of the next, one the pool would have served"
    grep -aqE '^\[HEAPFAIL\] core [0-9]+: allocation 1 since arming \(16 bytes\) is answered NULL' "$L"
    chk $? "stand: $what — and the tag window's array doubling to eight members, which the index then refused to write past"
    grep -aq '^\[HALT\] Storage sync complete' "$L"
    chk $? "stand: $what — the halt through bye said its sync wrote the volume ($(grep -aE "$STAND_HALT_ENDS" "$L" | tr -d '\r' | head -1 | cut -c1-120))"
}

# stand_again TAG WHAT ROAD — the second boot of the volume the first one
# left: its roll call finds nothing to repair, and the suite holds what it
# held, number for number.
stand_again() {
    local L="$SCRATCH/serial.$1.log" what=$2 road=$3 said mounted count
    count=$(stand_held_count "$road")
    said=$(stand_summary "$L")
    mounted=$(grep -aoE '\[TagFS\] [0-9]+ data blocks, [0-9]+ free, [0-9]+ files' "$L" | head -1)
    [ -n "$mounted" ] && ! grep -aq 'Bitmap mismatch' "$L"
    chk $? "stand: $what, second boot — the roll call finds no Bitmap mismatch (${mounted:-no mount line})"
    echo "$said" | grep -qE "^TagFS: $STAND_RUN run, [0-9]+ passed, 0 failed, [0-9]+ skipped, $count held under a caveat"
    chk $? "stand: $what, second boot — the $STAND_RUN ran again, nothing failed and the same $count caveats are held (${said:-no summary})"
    ! grep -aq 'KERNEL PANIC' "$L"
    chk $? "stand: $what, second boot — no panic"
    ! grep -aqE "$STAND_MIRROR_DEFECT" "$L"
    chk $? "stand: $what, second boot — every record memory held was the file its Ink packed, and every live file had its row"
    stand_held "$L" "$what, second boot" "$road"
    stand_orders "$L" "$what, second boot"
}

# stand_twice TAG WHAT ROAD SEAT BLOCKFAILS KEYS... — one road: a fresh volume
# built and booted, judged, and the volume it left booted again and judged.
stand_twice() {
    local tag=$1 what=$2 road=$3 seat=$4 want=$5 img built kept rc; shift 5
    img="$SCRATCH/stand.$tag.img"
    rm -f build/boxos.img "$img"
    if ! build_with_key VOLUME_TESTS=on "$(strict_key "$@")"; then
        echo "BUILD FAILED (VOLUME_TESTS=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "stand: $what — VOLUME_TESTS=on does not build"
        return
    fi
    built=$(deed_uuid build/boxos.img) || built=""
    stand_boot "$tag" "$@"; rc=$?
    [ "$rc" = 2 ] && return
    stand_judge "$tag" "$what" "$road" "$seat" "$want"
    if [ "$rc" != 0 ]; then
        bad "stand: $what, second boot — not judged: the first boot never said its halt's sync wrote the volume, so a Bitmap mismatch on the next would be the halt's, not the tests'"
        return
    fi
    cp -c build/boxos.img "$img" 2>/dev/null || cp build/boxos.img "$img"
    kept=$(deed_uuid "$img") || kept=""
    if [ -z "$built" ] || [ "$kept" != "$built" ]; then
        bad "stand: $what, second boot — not judged: the image the first boot left carries Deed ${kept:-none} and the build laid down ${built:-none}, so it was repacked under that boot"
        rm -f "$img"
        return
    fi
    stand_boot "${tag}_again" BOOTDISK="$img" "$@"; rc=$?
    [ "$rc" = 2 ] || stand_again "${tag}_again" "$what" "$road"
    rm -f "$img"
}

run_stand() {
    echo "== stand: the startup tests that write, on both roads to the medium, twice each =="
    local f logs=""
    rm -f "$SCRATCH"/serial.stand_bios.log "$SCRATCH"/serial.stand_bios_again.log \
          "$SCRATCH"/serial.stand_uefi.log "$SCRATCH"/serial.stand_uefi_again.log
    stand_twice stand_bios "BIOS 4c" bios "ATA [a-z ]+" 34 CORES=4 MEM=4G
    stand_twice stand_uefi "UEFI 4c" uefi "AHCI port [0-9]+" 39 UEFI=on STRICT=on CORES=4 MEM=4G
    for f in "$SCRATCH"/serial.stand_bios.log "$SCRATCH"/serial.stand_bios_again.log \
             "$SCRATCH"/serial.stand_uefi.log "$SCRATCH"/serial.stand_uefi_again.log; do
        [ -f "$f" ] && logs="$logs $f"
    done
    lockrank_judge "$logs" "stand, both roads"
    buildkey_restore
}

# ---------------------------------------------------------------------------
# standusb: the startup tests that write, on a USB volume (step 1.5c)
#
# stand runs the suite on the two disks a machine boots from. A USB stick is
# the third road, and the one a person's machine has: a disk that runs one
# command at a time, whose every step is a baton on the drain core, whose
# flush answers with sense, and whose port's writes go on at a service point
# (write_job.c, "A USB DISK'S ANSWER IS NOT AN INTERRUPT'S"). So the suite is
# booted with the volume on a stick: BIOS, four cores, the image the loader
# reads on the internal disk, the stick — a copy of that same image, the same
# sixteen bytes — on the xHCI from power-on, and the internal disk declared
# not this machine's (latearrival_on), so the mount takes the stick and the
# tests write through it. Judged as stand judges its roads (stand_judge,
# stand_again): every test ran, nothing failed, the writes' road named as the
# stick's seat, BLOCKFAIL refusing as many commands as the tests that arm it
# refuse on this road (STANDUSB_BLOCKFAILS, the USB column of stand_judge's
# table), the halt's sync wrote the stick, and the stick that boot left booted
# again with no Bitmap mismatch.
#
# ‼ THE MUTATION IT EXISTS FOR, MEASURED: two tests of the suite run on a USB
# disk alone, and stand steps them aside on both its roads, so only this sees
# the line they guard — xhci_msd.c msd_flush_judge, a flush answered NO
# MEDIUM taken as kept, as Linux takes it (`return MSD_FLUSH_REFUSED;` under
# ASC_NO_MEDIUM made `return MSD_FLUSH_KEPT;`): stand_medium_no_medium_
# flush_refused and stand_medium_sync_cache_remembered fail on both boots,
# "190 run, 184 passed, 2 failed" — 17 passed, 3 failed. And a boot that
# mounted the internal disk instead names another seat for the writes and is
# red on that line.
# ---------------------------------------------------------------------------

STANDUSB_BLOCKFAILS=42   # stand_judge's table, its USB column
STANDUSB_SEAT="usb[0-9]+ [^)]*"

run_standusb() {
    echo "== standusb: the startup tests that write, on a USB volume, twice =="
    local tag=stand_usb what="USB, BIOS 4c" boot="$SCRATCH/standusb.boot.img"
    local stick="$SCRATCH/standusb.stick.img" built kept rc extra logs="" f
    rm -f "$SCRATCH"/serial.stand_usb.log "$SCRATCH"/serial.stand_usb_again.log build/boxos.img \
          "$boot" "$stick"
    latearrival_on
    if ! build_with_key VOLUME_TESTS=on STRICT=off; then
        echo "BUILD FAILED (VOLUME_TESTS=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "standusb: VOLUME_TESTS=on does not build"
        latearrival_off; buildkey_restore
        return
    fi
    # Both copies off the image just built, before any boot: run-bg boots a
    # BOOTDISK as it stands and lays nothing down, so the stick and the disk
    # the loader reads carry the one volume the boarding pass will name.
    built=$(deed_uuid build/boxos.img) || built=""
    if ! cp build/boxos.img "$boot" || ! cp build/boxos.img "$stick" || [ -z "$built" ]; then
        harness "standusb: the image could not be copied to the boot disk and the stick (Deed ${built:-unreadable})"
        latearrival_off; buildkey_restore; rm -f "$boot" "$stick"
        return
    fi
    extra="-drive if=none,id=stick,file=$stick,format=raw -device usb-storage,bus=xhci1.0,drive=stick,id=usbstick"
    stand_boot "$tag" BOOTDISK="$boot" USB=on CORES=4 MEM=4G QEMUEXTRA="$extra"; rc=$?
    if [ "$rc" != 2 ]; then
        stand_judge "$tag" "$what" usb "$STANDUSB_SEAT" "$STANDUSB_BLOCKFAILS"
        kept=$(deed_uuid "$stick") || kept=""
        if [ "$rc" != 0 ]; then
            bad "standusb: $what, second boot — not judged: the first boot never said its halt's sync wrote the stick, so a Bitmap mismatch on the next would be the halt's, not the tests'"
        elif [ "$kept" != "$built" ]; then
            bad "standusb: $what, second boot — not judged: the stick the first boot left carries Deed ${kept:-none} and the build laid down $built"
        else
            stand_boot "${tag}_again" BOOTDISK="$boot" USB=on CORES=4 MEM=4G QEMUEXTRA="$extra"; rc=$?
            [ "$rc" = 2 ] || stand_again "${tag}_again" "$what" usb
        fi
    fi
    for f in "$SCRATCH"/serial.stand_usb.log "$SCRATCH"/serial.stand_usb_again.log; do
        [ -f "$f" ] && logs="$logs $f"
    done
    lockrank_judge "$logs" "standusb"
    latearrival_off
    rm -f "$boot" "$stick"
    buildkey_restore
}

# ---------------------------------------------------------------------------
# standmut: every test in the stand suite has to be able to go red.
#
# A caveated test is only worth holding if it sees its defect because the
# defect is there: so each defect's real fix is put in — the fix its step will
# make, in the least code that makes it — and each test must then say
# "caveat lifted early", with the number the fix makes it see, and fail. And a
# test that proves a fault mechanism is only worth passing if the mechanism,
# broken, fails it: no range check, no kind check, not single-shot, a disarm
# that does not disarm, a refused write that lands anyway, a refusal inside
# a command that lets nothing before its sector through, a flush never
# asked, an asynchronous refusal returned instead of called back, HEAPFAIL
# asked after the slab, counting without refusing, blind to the core, the tag
# row's PF-1 fix taken out, and a test that forgets to take its fault down
# (the runner must take it down and fail it). LockRank, which the lock tests
# count with, the same: its witness keyed on a bare IF instead of a TagFS lock
# held, and its order never checked. The flush tickets and the Scribe the
# same: a refused flush that moves the mark, a count of kept flushes, a need
# that is the last flush begun, a pass that publishes every ask, a Scribe
# this core holds waited for like any other, a drain core that runs no baton
# while it waits, and a Give that never lets the scheduler go. And a caveat a
# step has lifted stays worth its test only while the test would see the
# defect come back: the legacy-IDE DMA wait's `sti`, the Ledger
# written, and the integrity map paged in, under their own locks again, the
# file table's growth as step 1.1 found it — a new block linked with no
# stamp and no flush behind the link, the memory for the link asked only
# after the block was taken — and the tag registry linking before it stamps,
# writing under its lock, sharing its mark with a registry beside it, loading
# what it cannot hold, handing out its own array, and forgetting the block a
# refused pass took. And room on its way, waited for and never answered (R6):
# the border every answer passes letting ERR_WOULD_BLOCK through, the Settle's
# own holder refused its own cycle, a job that runs the cycles in its caller's
# call, a caller in the kernel answered the word, an op waited for answered
# without being asked again — and, on a STRICT kernel through the port, a
# W_SPARE that answers the allocator's word, which the border stops the
# machine on. And the mirror's check after every Ink of format v1 — memory's
# record held to the one v1 packed — broken three ways, one per kind of Ink:
# a Plot's last splice not conveyed, a rename's record saying another time, a
# new file's carrying a flag the packed one does not. Mutations whose tests do
# not meet each other's share a boot; each is judged by its own test's own
# line, number and all.
#
# And the runner is only worth believing if it cannot be made to pass what it
# should fail. A failed assertion must say where, and what it saw — shown by
# one forced to fail — and with that print put back on debug_printf, the
# failure shrinks to a count nobody can place. A judge that holds whatever a
# caveated test saw lets every fix in go by as held, with no "lifted early"
# at all. And a test that takes a block and never gives it back — what CR-12
# was, a copy of the literal block 1 its test took for a snapshot — must be
# caught twice: by the runner's count as it runs, and by the next boot's roll
# call as an orphan.
# ---------------------------------------------------------------------------

# stand_mut_boot TAG KEYS... — build what the tree now is under VOLUME_TESTS,
# on a fresh volume, and boot it (first boot only; the volume is discarded).
# Answers 1 only when there is nothing to judge: no build, or no machine.
stand_mut_boot() {
    local tag=$1; shift
    rm -f build/boxos.img
    if ! build_with_key VOLUME_TESTS=on "$(strict_key "$@")"; then
        echo "BUILD FAILED ($tag) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "standmut: $tag does not build"
        return 1
    fi
    stand_boot "$tag" "$@"
    [ $? != 2 ]
}

# stand_fix_on — the real fix of every caveated test, emulated: one edit per
# row of STAND_HELD, each taken out by the step that lifts its row. Since 1.10
# no row is held, so there is no fix to emulate.
stand_fix_on() {
    :
}
stand_fix_off() {
    :
}

# stand_mech_on GROUP — the fault mechanisms broken, a few at a time: each
# group's mutations are caught by different tests, and none of them changes
# what another group member's test sees. Two groups break the runner instead:
# `range` also forces one assertion to fail with its print intact, and
# `blind` forces the same one with its print muted and a judge that holds
# whatever a caveated test saw.
stand_mech_on() {
    local group=$1 marks mark
    # FILE:COUNT — every file the group edits, and how many edits land in it.
    case "$group" in
        range)  marks="src/kernel/core/boardroom/block_fail.c:1 src/lib/kernel/klib_mem.c:1 src/kernel/tagfs/tests/stand_tests.c:1 src/kernel/tagfs/tests/core_tests.c:1" ;;
        kind)   marks="src/kernel/core/boardroom/block_fail.c:1 src/lib/kernel/klib_mem.c:1" ;;
        shots)  marks="src/kernel/core/boardroom/block_fail.c:1 src/lib/kernel/klib_mem.c:1" ;;
        disarm) marks="src/kernel/core/boardroom/block_fail.c:1 src/kernel/core/boardroom/boardroom.c:1" ;;
        inside) marks="src/kernel/core/boardroom/block_fail.c:1" ;;
        lands)  marks="src/kernel/core/boardroom/boardroom.c:2 src/kernel/tagfs/tag_bitmap/tag_bitmap.c:1" ;;
        blind)  marks="src/kernel/tagfs/tests/tests.c:2 src/kernel/tagfs/tests/core_tests.c:1" ;;
        lockrank) marks="src/kernel/tagfs/lock_rank/lock_rank.c:2" ;;
        underlock) marks="src/kernel/tagfs/ledger/ledger.c:1 src/kernel/tagfs/integrity/integrity.c:1" ;;
        scribe)    marks="src/kernel/tagfs/scribe/scribe.c:4" ;;
        flushsum)  marks="src/kernel/tagfs/scribe/scribe.c:3" ;;
        flushneed) marks="src/kernel/tagfs/scribe/scribe.c:1" ;;
        dmawait)   marks="src/kernel/drivers/disk/ata_async.c:1" ;;
        table)     marks="src/kernel/tagfs/file_table/file_table.c:4" ;;
        registry)     marks="src/kernel/tagfs/tag_registry/tag_registry.c:4 src/kernel/tagfs/tag_registry/tag_registry_load.c:2" ;;
        registrykeep) marks="src/kernel/tagfs/tag_registry/tag_registry.c:1" ;;
        room)     marks="src/kernel/core/ipc/chit.c:1 src/kernel/tagfs/settle/settle.c:1 src/kernel/core/decks/storage/async/write_job.c:1 src/kernel/core/decks/storage/settle_ops.c:2" ;;
        roomport) marks="src/kernel/core/decks/storage/async/write_job_port.c:1" ;;
        mirror)   marks="src/kernel/tagfs/plot/plot_conveyance.c:1 src/kernel/tagfs/file_map/file_record.c:1 src/kernel/tagfs/ink/ink.c:1" ;;
    esac
    for mark in $marks; do mutate "${mark%%:*}"; done
    python3 - "$group" <<'EOF'
import sys
group = sys.argv[1]
M = "logcheck mutation"
forced = ("src/kernel/tagfs/tests/core_tests.c",
          "    TEST_ASSERT(fs->ledger.free_blocks > 0, \"Free blocks should be > 0\");\n",
          "    TEST_ASSERT(fs->ledger.free_blocks == 0,   /* " + M + ": an assertion forced to fail */\n"
          "                \"Free blocks should be > 0\");\n")
edits = {
 "range": [
  ("src/kernel/core/boardroom/block_fail.c",
   "    if (kind != BLOCK_FAIL_FLUSH && plan.end != 0 &&\n        (lba + count <= plan.first || lba >= plan.end))\n        return false;\n",
   "    /* " + M + ": no range check — every sector of the seat is the plan's */\n"),
  ("src/lib/kernel/klib_mem.c",
   "    if (HeapFailStrikes(size))\n        return NULL;\n#endif\n\n    KHeapScope who = KHeapScopeHere();\n",
   "#endif\n\n    KHeapScope who = KHeapScopeHere();\n"),
  ("src/lib/kernel/klib_mem.c",
   "        /* Fall through if slab exhausted — the pool handles it. */\n    }\n",
   "        /* Fall through if slab exhausted — the pool handles it. */\n    }\n#ifdef CONFIG_VOLUME_TESTS\n    if (HeapFailStrikes(size))   /* " + M + ": asked only after the slab has served */\n        return NULL;\n#endif\n"),
  ("src/kernel/tagfs/tests/stand_tests.c",
   "    TEST_ASSERT_EQ(BlockFailSeen(), 3, \"and all three met the plan\");\nout:\n    if (armed)\n        BlockFailDisarm();\n    return verdict;\n",
   "    TEST_ASSERT_EQ(BlockFailSeen(), 3, \"and all three met the plan\");\nout:\n    (void)armed;   /* " + M + ": the test forgets to take its plan down */\n    return verdict;\n"),
  forced,
 ],
 "kind": [
  ("src/kernel/core/boardroom/block_fail.c",
   "    if (plan.seat != seat || plan.kind != (uint8_t)kind)\n        return false;\n",
   "    if (plan.seat != seat)   /* " + M + ": no kind check — a write meets a read's plan */\n        return false;\n"),
  ("src/lib/kernel/klib_mem.c",
   "            \"from the heap\\n\", me, n, size);\n    return true;\n}\n",
   "            \"from the heap\\n\", me, n, size);\n    return false;   /* " + M + ": counts and says, and serves anyway */\n}\n"),
 ],
 "shots": [
  ("src/kernel/core/boardroom/block_fail.c",
   "        fire = plan.bad ? n >= plan.nth : n == plan.nth && !(s & 1u);\n",
   "        fire = n >= plan.nth;   /* " + M + ": not single-shot — every command from the nth on is refused */\n"),
  ("src/lib/kernel/klib_mem.c",
   "    if (me != plan.core)\n        return false;               /* another core is not the code under test */\n",
   "    /* " + M + ": every core is the code under test */\n"),
 ],
 "disarm": [
  ("src/kernel/core/boardroom/block_fail.c",
   "        if (__atomic_compare_exchange_n(&g_block_fail_state, &s, s + BLOCK_FAIL_GEN_ONE, false,\n                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))\n            return true;\n",
   "        return true;   /* " + M + ": disarm says it took the plan down and leaves it standing */\n"),
  ("src/kernel/core/boardroom/boardroom.c",
   "    if (BlockFailFlushStrikes(seat, sense)) {\n        if (s->kind == BOARD_USB && (sense[0] | sense[1] | sense[2]))\n            return xhci_msd_flush_answered(s->index, sense[0], sense[1], sense[2]);\n        return BoardMediumRefusal(s->kind, BLOCK_FAIL_FLUSH);\n    }\n",
   "    (void)sense;   /* " + M + ": the flush is never asked */\n"),
 ],
 "inside": [
  ("src/kernel/core/boardroom/block_fail.c",
   "    uint32_t before = (keep && kind != BLOCK_FAIL_FLUSH && plan.first > lba)\n                          ? (uint32_t)(plan.first - lba) : 0;\n",
   "    uint32_t before = 0;   /* " + M + ": nothing before the refused sector reaches the medium */\n"),
 ],
 "lands": [
  ("src/kernel/core/boardroom/boardroom.c",
   "            rc = keep ? BoardSeatWrite(s, lba, keep, in) : 0;\n",
   "            rc = BoardSeatWrite(s, lba, run, in);   /* " + M + ": the refused write reaches the medium first */\n"),
  ("src/kernel/core/boardroom/boardroom.c",
   "        cb(s->index, 0, ERR_IO, ctx);\n        return OK;\n",
   "        (void)cb;\n        return ERR_IO;   /* " + M + ": refused by the submit's answer, never through the callback */\n"),
  ("src/kernel/tagfs/tag_bitmap/tag_bitmap.c",
   "        if (!piece.piece) {\n            IndexSayShort(idx, tag_id, file_id, &want);\n            return -1;\n        }\n",
   "        if (!piece.piece) {   /* " + M + ": PF-1 taken out — a window that could not grow is written past */\n"
   "            TagFSLockTake(&idx->lock);\n            IndexAdd(idx, tag_id, file_id);\n"
   "            TagFSLockDrop(&idx->lock);\n            return 0;\n        }\n"),
 ],
 "lockrank": [
  ("src/kernel/tagfs/lock_rank/lock_rank.c",
   "    if (core->Depth == 0 && core->Unwatched == 0) {\n        LockRankIrqRestore(flags);\n        return;\n    }\n    core->Witnessed++;\n",
   "    if (core->Depth == 0 && core->Unwatched == 0 && (flags & 0x200)) {   /* " + M + ": the witness keyed on a bare IF */\n        LockRankIrqRestore(flags);\n        return;\n    }\n    core->Witnessed++;\n"),
  ("src/kernel/tagfs/lock_rank/lock_rank.c",
   "        if (lock->Rank == LOCK_RANK_NONE || h->Rank < lock->Rank)\n            continue;\n",
   "        continue;   /* " + M + ": the order is never checked */\n"),
 ],
 "underlock": [
  ("src/kernel/tagfs/ledger/ledger.c",
   "    int wrote = tagfs_volume_write(ledger_vlba(s_image.slot), 1, s_image.sector);\n",
   "    TagFSLockTake(&g_ledger_lock);   /* " + M + ": the Ledger's write is back under its lock */\n"
   "    int wrote = tagfs_volume_write(ledger_vlba(s_image.slot), 1, s_image.sector);\n"
   "    TagFSLockDrop(&g_ledger_lock);\n"),
  ("src/kernel/tagfs/integrity/integrity.c",
   "    TagFSLockDropAt(&g_map_lock, site);\n\n    error_t got = TagFSMetaBlockRead(at, g_map_page[s]);\n\n    TagFSLockTakeAt(&g_map_lock, site);\n",
   "    (void)site;   /* " + M + ": the map's page comes in under its lock again */\n"
   "    error_t got = TagFSMetaBlockRead(at, g_map_page[s]);\n\n"),
 ],
 "scribe": [
  ("src/kernel/tagfs/scribe/scribe.c",
   "    if (!kept)\n        return;\n    uint64_t cur = __atomic_load_n(&g_flush_kept, __ATOMIC_RELAXED);\n",
   "    (void)kept;   /* " + M + ": a refused flush moves the mark too */\n    uint64_t cur = __atomic_load_n(&g_flush_kept, __ATOMIC_RELAXED);\n"),
  ("src/kernel/tagfs/scribe/scribe.c",
   "        __atomic_store_n(&s->written, cover, __ATOMIC_RELEASE);\n",
   "        __atomic_store_n(&s->written, __atomic_load_n(&s->asked, __ATOMIC_SEQ_CST),   /* " + M + ": a pass publishes every ask, not what it covered */\n"
   "                         __ATOMIC_RELEASE);\n"),
  ("src/kernel/tagfs/scribe/scribe.c",
   "        return seen == me ? SCRIBE_SELF : SCRIBE_HELD;\n",
   "        return SCRIBE_HELD;   /* " + M + ": a Scribe this core holds is waited for like any other */\n"),
  ("src/kernel/tagfs/scribe/scribe.c",
   "    if (core == g_amp.bsp_index)\n        BatonPump(core);\n    cpu_pause();\n",
   "    (void)core;   /* " + M + ": the drain core runs no baton while it waits */\n    cpu_pause();\n"),
 ],
 "flushsum": [
  ("src/kernel/tagfs/scribe/scribe.c",
   "    if (!kept)\n        return;\n    uint64_t cur = __atomic_load_n(&g_flush_kept, __ATOMIC_RELAXED);\n"
   "    while (cur < ticket &&\n           !__atomic_compare_exchange_n(&g_flush_kept, &cur, ticket, false,\n"
   "                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))\n        ;\n",
   "    (void)ticket;   /* " + M + ": a count of the flushes kept, raised as each answers */\n"
   "    if (kept)\n        __atomic_add_fetch(&g_flush_kept, 1u, __ATOMIC_RELEASE);\n"),
  ("src/kernel/tagfs/scribe/scribe.c",
   "    return __atomic_load_n(&g_flush_asked, __ATOMIC_SEQ_CST) + 1u;\n",
   "    return __atomic_load_n(&g_flush_kept, __ATOMIC_SEQ_CST) + 1u;   /* " + M + ": the next flush kept is the one that covers */\n"),
  ("src/kernel/tagfs/scribe/scribe.c",
   "    g_scribe_held[core][s->rank]--;\n    SchedulerLetGoHere();\n",
   "    g_scribe_held[core][s->rank]--;   /* " + M + ": the scheduler is never let go */\n"),
 ],
 "flushneed": [
  ("src/kernel/tagfs/scribe/scribe.c",
   "    return __atomic_load_n(&g_flush_asked, __ATOMIC_SEQ_CST) + 1u;\n",
   "    return __atomic_load_n(&g_flush_asked, __ATOMIC_SEQ_CST);   /* " + M + ": the need is the last flush begun */\n"),
 ],
 "dmawait": [
  ("src/kernel/drivers/disk/ata_async.c",
   "    AtaAsyncCh *aa = &g_ata_async[cmd.channel];\n    while (!__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {\n",
   "    AtaAsyncCh *aa = &g_ata_async[cmd.channel];\n"
   "    asm volatile(\"sti\" ::: \"memory\");   /* " + M + ": the DMA wait turns interrupts on under its caller again */\n"
   "    while (!__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {\n"),
 ],
 "table": [
  ("src/kernel/tagfs/file_table/file_table.c",
   "        error_t  got     = SpareStamp(TAGFS_FILETBL_MAGIC, &g_image, &block, &stamped);\n",
   "        uint64_t taken   = 0;   /* " + M + ": the new block is linked with no stamp */\n"
   "        error_t  got     = tagfs_alloc_blocks(1, &taken) == 0 ? OK : ERR_TAGFS_NO_SPACE;\n"
   "        block = (uint32_t)taken;   /* format v1: the table's link is a u32 block */\n"),
  ("src/kernel/tagfs/file_table/file_table.c",
   "    error_t ready = VolumeFlushFor(need);\n",
   "    error_t ready = OK;   /* " + M + ": and no flush stands between it and its link */\n    (void)need;\n"),
  ("src/kernel/tagfs/file_table/file_table.c",
   "    if (LinksReserve(wanted) != OK) {\n"
   "        FileTableShortSay(wanted, have, ERR_NO_MEMORY);\n"
   "        return OK;\n"
   "    }\n",
   "    /* " + M + ": the memory for a link is asked only after its block is taken */\n"),
  ("src/kernel/tagfs/file_table/file_table.c",
   "        TagFSLockTake(table_lock());\n        g_links[have + in_hand] = (FileTableLink){ .block = block };\n",
   "        if (LinksReserve(wanted) != OK)   /* " + M + ": and a refusal forgets the block */\n"
   "            return -1;\n"
   "        TagFSLockTake(table_lock());\n        g_links[have + in_hand] = (FileTableLink){ .block = block };\n"),
 ],
 "registry": [
  ("src/kernel/tagfs/tag_registry/tag_registry.c",
   "        rc = SpareStamp(TAGFS_REGISTRY_MAGIC, reg->spare_image, &reg->spare_block, &reg->spare_need);\n",
   "        uint64_t taken = 0;   /* " + M + ": the new block is taken and never stamped */\n"
   "        rc = tagfs_alloc_blocks(1, &taken) == 0 ? OK : ERR_TAGFS_NO_SPACE;\n"
   "        reg->spare_block = (uint32_t)taken;   /* format v1: the registry's link is a u32 block */\n"),
  ("src/kernel/tagfs/tag_registry/tag_registry.c",
   "    rc = TagFSMetaBlockWrite(reg->spare_block, reg->spare_image);\n",
   "    reg->tail_image->next_block = reg->spare_block;   /* " + M + ": the link goes down before the block it names */\n"
   "    TagFSMetaBlockWrite(reg->tail_block, reg->tail_image);\n"
   "    rc = TagFSMetaBlockWrite(reg->spare_block, reg->spare_image);\n"),
  ("src/kernel/tagfs/tag_registry/tag_registry.c",
   "        error_t wrote = TagFSMetaBlockWrite(reg->tail_block, tail);\n",
   "        TagFSLockTake(&reg->lock);   /* " + M + ": the tail is written holding the registry's lock */\n"
   "        error_t wrote = TagFSMetaBlockWrite(reg->tail_block, tail);\n"
   "        TagFSLockDrop(&reg->lock);\n"),
  ("src/kernel/tagfs/tag_registry/tag_registry_load.c",
   "    /* The two blocks every flush of this registry is written through, and the\n",
   "    if (fs->registry && fs->registry != reg)   /* " + M + ": a registry beside the volume's shares its mark of what is owed */\n"
   "        fs->registry->written_next = 0;\n"
   "    /* The two blocks every flush of this registry is written through, and the\n"),
  ("src/kernel/tagfs/tag_registry/tag_registry_load.c",
   "        end += size;\n        walked++;\n",
   "        if (held == ERR_NO_MEMORY)   /* " + M + ": a tag the load cannot hold is dropped, and the load goes on */\n"
   "            held = OK;\n"
   "        end += size;\n        walked++;\n"),
  ("src/kernel/tagfs/tag_registry/tag_registry.c",
   "        out->ids = kmalloc(sizeof(uint32_t) * group->count);\n"
   "        if (out->ids) {\n"
   "            memcpy(out->ids, group->tag_ids, sizeof(uint32_t) * group->count);\n",
   "        out->ids = group->tag_ids;   /* " + M + ": the registry's own array is handed out */\n"
   "        if (out->ids) {\n"),
 ],
 "registrykeep": [
  ("src/kernel/tagfs/tag_registry/tag_registry.c",
   "    tail->used_bytes  = was_used;\n",
   "    reg->spare_block  = 0;   /* " + M + ": a refused pass forgets the block it took */\n"
   "    tail->used_bytes  = was_used;\n"),
 ],
 "room": [
  ("src/kernel/core/ipc/chit.c",
   "    if (rc != ERR_WOULD_BLOCK)\n        return rc;\n",
   "    return rc;   /* " + M + ": the border lets ERR_WOULD_BLOCK through */\n"),
  ("src/kernel/tagfs/settle/settle.c",
   "    if (LockRankAnyHeldHere() || BatonRunningHere() || (ScribeAnyHeldHere() && !own)) {\n",
   "    (void)own;   /* " + M + ": the Settle's own holder may not wait for its own cycle */\n"
   "    if (LockRankAnyHeldHere() || BatonRunningHere() || ScribeAnyHeldHere()) {\n"),
  ("src/kernel/core/decks/storage/async/write_job.c",
   "        if (rc == ERR_WOULD_BLOCK && wjob_answered_later(j)) {\n",
   "        if (rc == ERR_WOULD_BLOCK && wjob_answered_later(j) && !j) {   /* " + M + ": the job runs the cycles in its caller's call */\n"),
  ("src/kernel/core/decks/storage/settle_ops.c",
   "    for (;;) {\n        if (!tagfs_enter())\n            return ERR_TAGFS_NOT_INITIALIZED;\n        error_t room = SettleForRoom();\n",
   "    for (;;) {\n        if (op)   /* " + M + ": a caller in the kernel is answered the word */\n            return ERR_WOULD_BLOCK;\n        if (!tagfs_enter())\n            return ERR_TAGFS_NOT_INITIALIZED;\n        error_t room = SettleForRoom();\n"),
  ("src/kernel/core/decks/storage/settle_ops.c",
   "    if (rc == OK) {\n        bool      owns = false;\n",
   "    if (rc != OK) {   /* " + M + ": the op waited for is answered, never asked again */\n        bool      owns = false;\n"),
 ],
 "roomport": [
  ("src/kernel/core/decks/storage/async/write_job_port.c",
   "    error_t rc = PoolSpareBegin(&j->spare_block, &j->spare_image);\n    if (rc == ERR_WOULD_BLOCK) {\n",
   "    error_t rc = PoolSpareBegin(&j->spare_block, &j->spare_image);\n    if (rc == ERR_WOULD_BLOCK && !j) {   /* " + M + ": W_SPARE answers the allocator's word */\n"),
 ],
 "mirror": [
  ("src/kernel/tagfs/plot/plot_conveyance.c",
   "    ConveyanceSplices(cur, p->runs, p->run_count, *out);\n    return OK;\n",
   "    ConveyanceSplices(cur, p->runs, p->run_count, *out);\n"
   "    (*count)--;   /* " + M + ": the last splice a Plot made is not conveyed */\n    return OK;\n"),
  ("src/kernel/tagfs/file_map/file_record.c",
   "    rec->modified_time = old->modified_time;\n    TractHold(old->tract);\n",
   "    rec->modified_time = old->modified_time + 1u;   /* " + M + ": a rename's record says another time */\n"
   "    TractHold(old->tract);\n"),
  ("src/kernel/tagfs/ink/ink.c",
   "    rc = InkRecordMake(&meta, &r);\n    if (rc == OK && !(r.rec = FileRecordBuild(&meta))) {\n",
   "    rc = InkRecordMake(&meta, &r);\n"
   "    meta.flags |= 0x80000000u;   /* " + M + ": a new file's record carries a flag the packed one does not */\n"
   "    if (rc == OK && !(r.rec = FileRecordBuild(&meta))) {\n"),
 ],
 "blind": [
  ("src/kernel/tagfs/tests/tests.c",
   "    kprintf(\"[TESTS] %s failed at %s:%d: %s (%lld, %lld)\\n\",\n",
   "    /* " + M + ": the assertion's print back on debug_printf */\n    debug_printf(\"[TESTS] %s failed at %s:%d: %s (%lld, %lld)\\n\",\n"),
  ("src/kernel/tagfs/tests/tests.c",
   "    if (caveated && seen == TEST_DEFECT) {\n",
   "    if (caveated) {   /* " + M + ": a judge that holds whatever a caveated test saw */\n"),
  forced,
 ],
}[group]
for path, a, m in edits:
    s = open(path).read()
    assert s.count(a) == 1, "stand mechanism anchor missing in %s: %s" % (path, a.splitlines()[0])
    open(path, "w").write(s.replace(a, m, 1))
EOF
    for mark in $marks; do mut_done "${mark%%:*}" "logcheck mutation" "${mark##*:}"; done
}
stand_mech_off() {
    local f
    for f in src/kernel/core/boardroom/block_fail.c src/lib/kernel/klib_mem.c \
             src/kernel/tagfs/tests/stand_tests.c src/kernel/core/boardroom/boardroom.c \
             src/kernel/tagfs/tag_bitmap/tag_bitmap.c src/kernel/tagfs/tests/tests.c \
             src/kernel/tagfs/tests/core_tests.c src/kernel/tagfs/lock_rank/lock_rank.c \
             src/kernel/tagfs/scribe/scribe.c src/kernel/drivers/disk/ata_async.c \
             src/kernel/tagfs/ledger/ledger.c src/kernel/tagfs/integrity/integrity.c \
             src/kernel/tagfs/file_table/file_table.c src/kernel/tagfs/tag_registry/tag_registry.c \
             src/kernel/tagfs/tag_registry/tag_registry_load.c src/kernel/core/ipc/chit.c \
             src/kernel/tagfs/settle/settle.c src/kernel/core/decks/storage/async/write_job.c \
             src/kernel/core/decks/storage/settle_ops.c \
             src/kernel/core/decks/storage/async/write_job_port.c \
             src/kernel/tagfs/plot/plot_conveyance.c src/kernel/tagfs/file_map/file_record.c \
             src/kernel/tagfs/ink/ink.c; do
        restore "$f"
    done
}

# stand_leak_on — a test that takes a block and never gives it back, the way
# CR-12's test left the copy it took for a snapshot: the cow suite's test,
# with one block taken at its start and nothing done with it.
stand_leak_on() {
    mutate src/kernel/tagfs/tests/snapshot_tests.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/tests/snapshot_tests.c"
s = open(p).read()
a = "    uint8_t*      now       = g_test_buffer + COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE;\n\n    TEST_ASSERT_EQ(tagfs_create_file(\"cow-test\", NULL, 0, &file_id), 0, \"a file is created\");\n"
assert s.count(a) == 1, "stand leak anchor missing"
open(p, "w").write(s.replace(a,
    "    uint8_t*      now       = g_test_buffer + COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE;\n"
    "    uint64_t      leaked;   /* logcheck mutation: a block taken and never given back */\n"
    "\n"
    "    TEST_ASSERT_EQ(tagfs_alloc_blocks(1, &leaked), 0, \"a block is taken\");\n"
    "    TEST_ASSERT_EQ(tagfs_create_file(\"cow-test\", NULL, 0, &file_id), 0, \"a file is created\");\n", 1))
EOF
    mut_done src/kernel/tagfs/tests/snapshot_tests.c "logcheck mutation" 1
}
stand_leak_off() {
    restore src/kernel/tagfs/tests/snapshot_tests.c
}

# stand_red TAG WHAT ERE — a line the mutated boot must carry.
stand_red() {
    local L="$SCRATCH/serial.$1.log"
    grep -aqE "$3" "$L"
    chk $? "standmut: $2 — $(grep -aE "$3" "$L" | tr -d '\r' | head -1 | cut -c1-200)"
}

run_standmut() {
    echo "== standmut: every stand test, and every fault it stands on, can go red =="
    local L name step held lifted roads count said n before after img

    # ── the real fixes: every caveat must be lifted early, and fail ────────
    # On the legacy channel, so the rows written for the bios road are asked.
    stand_fix_on
    if stand_mut_boot standmut_fix CORES=4 MEM=4G; then
        L="$SCRATCH/serial.standmut_fix.log"; count=0
        while IFS='|' read -r name step held lifted roads; do
            [ -n "$name" ] && stand_row_on "$roads" bios || continue
            grep -aF "[TESTS] caveat lifted early: $name — " "$L" | grep -qF " — lift the caveat ($lifted)"
            chk $? "standmut: with its step's fix in, $name says its caveat was lifted early ($(grep -aF "caveat lifted early: $name — " "$L" | tr -d '\r' | grep -oE '\([^()]*\)$'))"
            count=$((count + 1))
        done <<STAND_HELD_EOF
$STAND_HELD
STAND_HELD_EOF
        said=$(stand_summary "$L")
        echo "$said" | grep -qE ", $count failed, [0-9]+ skipped, 0 held under a caveat"
        chk $? "standmut: and exactly those $count fail, none held (${said:-no summary})"
    fi

    # ── the same fixes, judged by a runner that cannot see them ────────────
    stand_mech_on blind
    if stand_mut_boot standmut_blind CORES=4 MEM=4G; then
        L="$SCRATCH/serial.standmut_blind.log"
        said=$(stand_summary "$L")
        n=$(grep -ac 'caveat lifted early' "$L")
        [ "$n" = 0 ] && echo "$said" | grep -qE ", 1 failed, [0-9]+ skipped, $(stand_held_count bios) held under a caveat"
        chk $? "standmut: a judge that holds whatever a caveated test saw lets every fix in go by as held — $n 'lifted early' line(s) (${said:-no summary})"
        n=$(grep -acE '^\[TESTS\] [a-z_0-9]+ failed at ' "$L")
        [ "$n" = 0 ] && echo "$said" | grep -qE ", 1 failed, "
        chk $? "standmut: with the assertion's print back on debug_printf, the forced failure is a count nobody can place — $n 'failed at' line(s) (${said:-no summary})"
    fi
    stand_mech_off
    stand_fix_off

    # ── the mechanisms, broken ──────────────────────────────────────────────
    stand_mech_on range
    if stand_mut_boot standmut_range CORES=4 MEM=4G; then
        stand_red standmut_range "BLOCKFAIL with no range check refuses a block the plan never named" \
            'stand_blockfail_reads_the_nth failed at [^:]+:[0-9]+: and meets no plan — the plan names another block \(1, 0\)'
        stand_red standmut_range "HEAPFAIL asked only after the slab lets a small allocation through" \
            'stand_heapfail_nth_on_this_core failed at [^:]+:[0-9]+: the third is answered NULL \(0, 1\)'
        stand_red standmut_range "a test that leaves its fault armed is taken down by the runner, and failed" \
            'stand_blockfail_flush_refused failed: it left BLOCKFAIL armed — taken down by the runner before anything else could meet it'
        stand_red standmut_range "a failed assertion says where it failed and what it saw" \
            'tagfs_deed failed at src/kernel/tagfs/tests/core_tests\.c:[0-9]+: Free blocks should be > 0 \(0, 1\)'
    fi
    stand_mech_off

    stand_mech_on kind
    if stand_mut_boot standmut_kind CORES=4 MEM=4G; then
        stand_red standmut_kind "BLOCKFAIL with no kind check counts a write against a read's plan" \
            'stand_blockfail_reads_the_nth failed at [^:]+:[0-9]+: a write is not a read, and is not counted \(3, 2\)'
        stand_red standmut_kind "HEAPFAIL that counts without refusing refuses nothing" \
            'stand_heapfail_nth_on_this_core failed at [^:]+:[0-9]+: the third is answered NULL \(0, 1\)'
    fi
    stand_mech_off

    stand_mech_on shots
    if stand_mut_boot standmut_shots CORES=4 MEM=4G; then
        stand_red standmut_shots "BLOCKFAIL that is not single-shot refuses the read after its own" \
            'stand_blockfail_reads_the_nth failed at [^:]+:[0-9]+: the third read is served — the plan refuses once \([0-9]+, 0\)'
        stand_red standmut_shots "HEAPFAIL blind to the core refuses a core no plan named" \
            'stand_heapfail_other_core_untouched failed at [^:]+:[0-9]+: an allocation on this core is served \(0, 1\)'
    fi
    stand_mech_off

    stand_mech_on disarm
    if stand_mut_boot standmut_disarm CORES=4 MEM=4G; then
        stand_red standmut_disarm "a BLOCKFAIL disarm that leaves the plan standing goes on counting" \
            'stand_blockfail_reads_the_nth failed at [^:]+:[0-9]+: and meets no plan \(4, 3\)'
        stand_red standmut_disarm "a flush the Boardroom never asks about is never refused" \
            'stand_blockfail_flush_refused failed at [^:]+:[0-9]+: the second is refused \(0, 1\)'
    fi
    stand_mech_off

    # On q35: the asynchronous read has no road on the legacy channel.
    stand_mech_on lands
    if stand_mut_boot standmut_lands UEFI=on STRICT=on CORES=4 MEM=4G; then
        stand_red standmut_lands "a refused write that reaches the medium first is found on it" \
            'stand_blockfail_write_never_lands failed at [^:]+:[0-9]+: holding what it held — nothing of the refused write landed \(0, 4096\)'
        stand_red standmut_lands "an asynchronous read refused by the submit's answer is not the medium's refusal" \
            'stand_blockfail_async_read failed at [^:]+:[0-9]+: the seat takes the read, as it takes one the medium then refuses \(200, 0\)'
        stand_red standmut_lands "with PF-1 taken out, the tag row is written past its end" \
            'stand_tag_row_refuses_what_it_cannot_widen failed at [^:]+:[0-9]+: a row that could not widen refuses the set instead of writing past its end \(0, -1\)'
    fi
    stand_mech_off

    # A medium that names the sector it failed at keeps what came before it,
    # and BLOCKFAIL refuses the way one does (step 1.11): taken out, a run
    # refused at its k-th block answers none of the k-1 before it, on both
    # roads — an AHCI port takes 32 blocks a command, so k is 17 and the
    # sixteen before it are 65536 bytes. On q35: the legacy channel takes one
    # block a command, and a run there has no inside to refuse.
    stand_mech_on inside
    if stand_mut_boot standmut_inside UEFI=on STRICT=on CORES=4 MEM=4G; then
        stand_red standmut_inside "a BLOCKFAIL that holds nothing before the sector answers a run refused inside as refused whole" \
            'stand_run_refused_inside_a_run failed at [^:]+:[0-9]+: exactly the k-1 blocks of the run before the refused one \(0, 65536\)'
        stand_red standmut_inside "and through the port the same" \
            'stand_run_refused_inside_a_run_through_the_port failed at [^:]+:[0-9]+: exactly the k-1 blocks of the run before the refused one \(0, 65536\)'
    fi
    stand_mech_off

    # On one core, where every system call runs with interrupts off: a read
    # that holds no TagFS lock is exactly what a bare-IF witness would name.
    stand_mech_on lockrank
    if stand_mut_boot standmut_lockrank CORES=1 MEM=2G; then
        stand_red standmut_lockrank "a witness keyed on a bare IF counts a read that holds no TagFS lock" \
            'stand_lockrank_names_the_medium failed at [^:]+:[0-9]+: a read that holds no TagFS lock is not witnessed, interrupts off or not \(1, 0\)'
        stand_red standmut_lockrank "an order never checked takes a state-rank lock under a leaf and says nothing" \
            'stand_lockrank_names_the_order failed at [^:]+:[0-9]+: a state-rank lock taken under a leaf, twice, is out of order twice \(0, 2\)'
    fi
    stand_mech_off

    # Step 1.1 lifted the caveats of the Ledger's write and of the map's
    # paging: each put back under its own lock, the test says the defect it
    # stands for is back, with the number it saw.
    stand_mech_on underlock
    if stand_mut_boot standmut_underlock CORES=4 MEM=4G; then
        stand_red standmut_underlock "the Ledger written under its lock again is the defect back" \
            'stand_ledger_writes_outside_its_lock failed: the defect it stands for is back — .* \(Ledger writes of one that crossed the medium holding the Ledger.s lock 1\)'
        stand_red standmut_underlock "the map paged in under its lock again is the defect back" \
            'stand_integrity_pages_outside_its_lock failed: the defect it stands for is back — .* \(17-page runs of block writes whose map paging crossed the medium under a lock 1\)'
    fi
    stand_mech_off

    # The flush tickets and the Scribe, broken one way per test and boot: the
    # three wrong accounts of a flush go red in the same test, so each has a
    # boot of its own.
    stand_mech_on scribe
    if stand_mut_boot standmut_scribe CORES=4 MEM=4G; then
        stand_red standmut_scribe "a refused flush that moves the mark claims a need it never covered" \
            'stand_flush_ticket failed at [^:]+:[0-9]+: keeps nothing \(0, 1\)'
        stand_red standmut_scribe "a pass that publishes every ask answers one made during it as written" \
            'stand_scribe_covers_only_what_was_asked failed at [^:]+:[0-9]+: the second is not, and takes a pass of its own \(1, 0\)'
        stand_red standmut_scribe "a Scribe this core holds, waited for like any other, is never answered SELF" \
            'stand_scribe_never_waits_on_itself failed at [^:]+:[0-9]+: a Scribe this core holds beneath the caller is answered SELF \(3, 4\)'
        stand_red standmut_scribe "a drain core that runs no baton while it waits never ends the pass it waits for" \
            'stand_scribe_waiter_keeps_pumping failed at [^:]+:[0-9]+: by the probe, which the drain core ran while it waited \(0, 1\)'
    fi
    stand_mech_off

    stand_mech_on flushsum
    if stand_mut_boot standmut_flushsum CORES=4 MEM=4G; then
        stand_red standmut_flushsum "a count of kept flushes credits a need with a flush begun before it" \
            'stand_flush_ticket failed at [^:]+:[0-9]+: and proves nothing about that need \(0, 1\)'
        stand_red standmut_flushsum "a Give that never lets the scheduler go leaves the core held" \
            'stand_scribe_covers_only_what_was_asked failed at [^:]+:[0-9]+: given back, the core may be switched away from again \([0-9]+, [0-9]+\)'
    fi
    stand_mech_off

    stand_mech_on flushneed
    if stand_mut_boot standmut_flushneed CORES=4 MEM=4G; then
        stand_red standmut_flushneed "a need that is the last flush begun is covered by that flush" \
            'stand_flush_ticket failed at [^:]+:[0-9]+: and proves nothing about that need \(0, 1\)'
    fi
    stand_mech_off

    # A lifted caveat's defect put back: the legacy-IDE DMA wait's `sti`, on
    # the legacy channel, where its test is asked. The test holds no caveat
    # any more, so seeing the defect is a failure that names it.
    stand_mech_on dmawait
    if stand_mut_boot standmut_dmawait CORES=4 MEM=4G; then
        stand_red standmut_dmawait "a DMA wait that turns interrupts on under its caller again is the defect back" \
            'stand_dma_wait_keeps_the_callers_interrupts failed: the defect it stands for is back — a legacy-IDE DMA wait turns interrupts on under its caller.s lock because only the BSP.s interrupt can finish it \(DMA reads that turned interrupts on under a caller that had them off 1\)'
    fi
    stand_mech_off

    # The file table's growth as step 1.1 found it: both of the defects it
    # lifted are back, and each test that stands for one says so, number and all.
    stand_mech_on table
    if stand_mut_boot standmut_table CORES=4 MEM=4G; then
        stand_red standmut_table "a new block of the file table linked with no stamp and no flush behind it runs the chain into unstamped ground" \
            "stand_table_grows_onto_a_stamp failed: the defect it stands for is back — .* \\(file table chains on the medium that run into a block without the table's magic 1\\)"
        stand_red standmut_table "the memory for a link asked only after its block is taken strands the block when it is refused" \
            'stand_table_growth_gives_back_its_ground failed: the defect it stands for is back — .* \(blocks a growth of the file table took and left named by nothing 1\)'
    fi
    stand_mech_off

    # The tag registry's step 1.1 undone, each defect in the place the fix
    # took it out of, and each test must say its defect is back. A refused
    # pass that forgets its block has a boot of its own: the growth that links
    # before it stamps takes no flush at all, and the test of the block kept
    # refuses exactly that flush.
    stand_mech_on registry
    if stand_mut_boot standmut_registry CORES=4 MEM=4G; then
        stand_red standmut_registry "a registry that links its new block before stamping it runs its chain into unstamped ground" \
            'stand_registry_growth_links_what_landed failed: the defect it stands for is back — .* \(registry chains on the medium that run into a block without the registry.s magic 1\)'
        stand_red standmut_registry "a tail written holding the registry's lock crosses the medium under it" \
            'stand_registry_flush_holds_no_lock failed: the defect it stands for is back — .* \(registry flushes of one new name that crossed the medium holding the registry.s lock 1\)'
        stand_red standmut_registry "a registry beside the volume's that shares its mark of what is owed moves the volume's chain" \
            'stand_registry_scratch_leaves_the_volume_alone failed: the defect it stands for is back — .* \(registry chains on the medium a scratch registry moved by living and dying 1\)'
        stand_red standmut_registry "a load that drops what it cannot hold answers OK" \
            'stand_registry_load_refuses_what_it_cannot_hold failed: the defect it stands for is back — .* \(registry loads with an allocation refused that answered OK, stopping at the first 1\)'
        stand_red standmut_registry "a key's ids handed out as the registry's own array are not a copy" \
            'stand_registry_key_ids_are_copies failed: the defect it stands for is back — .* \(copies of a key.s tag ids that were the registry.s own array 1\)'
    fi
    stand_mech_off

    stand_mech_on registrykeep
    if stand_mut_boot standmut_registrykeep CORES=4 MEM=4G; then
        stand_red standmut_registrykeep "a refused pass that forgets the block it took leaves it owned by nothing" \
            'stand_registry_flush_keeps_its_block failed: the defect it stands for is back — .* \(blocks a refused registry growth took that its retry did not link 1\)'
    fi
    stand_mech_off

    # Room on its way (R6), on the legacy channel, where the border's own test
    # is asked — a STRICT kernel stands it down — and every wait for room but
    # the port's is.
    stand_mech_on room
    if stand_mut_boot standmut_room CORES=4 MEM=4G; then
        stand_red standmut_room "a border that lets ERR_WOULD_BLOCK through hands a program the word its boxlib drops" \
            'stand_room_never_an_answer failed: the defect it stands for is back — .* \(answers of ERR_WOULD_BLOCK the border let through to a program 1\)'
        stand_red standmut_room "the Settle's own holder refused its own cycle answers a write in the kernel the word" \
            'stand_room_kernel_write_between_the_steps failed at [^ ]+: a write in the kernel on that core .* \(9, 0\)'
        stand_red standmut_room "a job that runs the cycles in its caller's call is answered before any cycle ends" \
            'stand_room_job_between_the_steps failed at [^ ]+: it waits for the cycle.s end as a waiter of the Settle, unanswered \(0, 1\)'
        stand_red standmut_room "a caller in the kernel answered the word is told ERR_WOULD_BLOCK" \
            'stand_room_create_grows_the_registry failed at [^ ]+: a create whose name grows the registry .* \(9, 0\)'
        stand_red standmut_room "an op waited for and answered without being asked again made nothing" \
            'stand_room_create_parked failed at [^ ]+: and says the new file.s id \(0, 1\)'
    fi
    stand_mech_off

    # And through the port, on a STRICT kernel: a W_SPARE that answers the
    # allocator's word meets the border, which stops the machine naming it.
    stand_mech_on roomport
    if stand_mut_boot standmut_roomport UEFI=on STRICT=on CORES=4 MEM=4G; then
        stand_red standmut_roomport "a W_SPARE that answers the allocator's word stops a STRICT kernel at the border" \
            'KERNEL PANIC:.*\[CHIT\] DEFECT: storage\.write answered pid 0.s token 0x000000 ERR_WOULD_BLOCK'
    fi
    stand_mech_off

    # The mirror's own check (spec_v2 §16.1 p.1) is only worth its silence if a
    # record memory holds, broken, is said: one per kind of Ink — a Plot whose
    # last splice is not conveyed, a rename whose record says another time, a
    # new file whose record carries a flag the packed one does not. Not STRICT,
    # so each is said by its own line and none stops the machine first.
    stand_mech_on mirror
    if stand_mut_boot standmut_mirror CORES=4 MEM=4G; then
        stand_red standmut_mirror "a Plot's last splice not conveyed leaves memory a record that is not the Plot's" \
            '\[TagFS\] DEFECT: the record memory holds of file [0-9]+ does not say what its Plot did — its Strips are not the Plot.s runs'
        stand_red standmut_mirror "a rename's record that says another time is not the one format v1 packed" \
            '\[TagFS\] DEFECT: the record memory holds of file [0-9]+ is not the one format v1 packed — its times'
        stand_red standmut_mirror "a new file's record that carries a flag the packed one does not is not it" \
            '\[TagFS\] DEFECT: the record memory holds of file [0-9]+ is not the one format v1 packed — its flags'
    fi
    stand_mech_off

    # ── a test that leaks a block: caught as it runs, and by the next roll call
    stand_leak_on
    if stand_mut_boot standmut_leak CORES=4 MEM=4G; then
        L="$SCRATCH/serial.standmut_leak.log"
        before=$(grep -aoE 'cow_moves_only_the_block_written failed: it left residue — [0-9]+ block\(s\) in use outside the volume.s own chains before it and [0-9]+ after' "$L" | grep -oE '[0-9]+' | head -1)
        after=$(grep -aoE 'cow_moves_only_the_block_written failed: it left residue — [0-9]+ block\(s\) in use outside the volume.s own chains before it and [0-9]+ after' "$L" | grep -oE '[0-9]+' | tail -1)
        [ -n "$before" ] && [ "$after" = "$((before + 1))" ]
        chk $? "standmut: a test that takes a block and never gives it back is counted by the runner — ${before:-no} block(s) before it, ${after:-no} after"
        if grep -aq '^\[HALT\] Storage sync complete' "$L"; then
            img="$SCRATCH/stand.leak.img"
            cp -c build/boxos.img "$img" 2>/dev/null || cp build/boxos.img "$img"
            stand_boot standmut_leak_again BOOTDISK="$img" CORES=4 MEM=4G
            if [ $? != 2 ]; then
                stand_red standmut_leak_again "and the next boot's roll call finds the block it left as an orphan" \
                    '\[TagFS FSCK\] Bitmap mismatch: 1 orphan, 0 missing'
                stand_red standmut_leak_again "named by where it is" \
                    '\[TagFS FSCK\] orphan run at data block [0-9]+, 1 block\(s\)'
            fi
            rm -f "$img"
        else
            bad "standmut: the leaking boot never said its halt's sync wrote the volume, so the next roll call would judge nothing"
        fi
    fi
    stand_leak_off

    buildkey_restore
}

# ===========================================================================
# quill — the red tests of the Quill write core, asked of a machine
# ===========================================================================
#
# quilltest (src/userspace/apps) asks the questions the Quill plan exists to
# answer, through the user ABI, and says only what it saw — PASS, DEFECT,
# PENDING or FAIL, with the numbers. This gives every case the stand it needs
# and judges what came back, beside what the kernel said and what the medium
# holds afterwards:
#
#   boot 1  on QUILL_BOOT1_MEM: longquery bigwrite sequential interleave
#           coldread coldwrite usememory; then the host lists the volume and
#           counts the extents each file was given
#   boot 2  usememory's second half, when the first said PENDING; fidhistory
#   boot 3  systemtag; then the host lists the volume again: is kernel.bin on it
#
# on a throwaway image given a gigabyte more ground, so that once the steps
# land no case is refused for want of space — built VOLUME_TESTS=on, the one
# build whose quilltest carries the cases that harm a volume past their own
# files. So the kernel's own startup tests run too, after every mount and
# before the shell, and what the judge reads of a boot is its first mount
# line and what follows the case's own echo: the file ids those tests spend
# on boot 2 lie between the two mounts fidhistory is weighed by, and are
# counted as dead ids like the case's own. Each case stands for a defect
# found red on HEAD, and caveat_judge (stand.sh) holds it under the step that
# discharges it: the defect seen, with its number, is held; the correct
# behaviour seen is a FAIL until that step lifts the caveat; anything else is
# a plain FAIL. fidhistory is judged from the mount line of two boots a known
# number of dead file ids apart. coldread and coldwrite carry no caveat: the
# defect they stand for — a read into pages its program had not touched yet
# reported every byte and delivered only those before the first such page —
# was fixed with them, so behaving is a PASS and the defect back is a FAIL.
#
# QUILL_STAND is the machine. The default is UEFI on q35 with four cores —
# AHCI and the asynchronous WriteJob, the way a userspace write goes on the
# owner's boards — and every verdict about a write names the path it took.
# Its keys are read as a shell reads words, so a key whose value holds a
# blank is quoted inside it: QUILL_STAND="STRICT=on CORES=4 MEM=2G
# QEMUEXTRA='-machine pc'" is the PIIX stand, one key reaching make whole.
#
# Boot 1 runs on less memory than the stand: QUILL_BOOT1_MEM in place of its
# MEM. bigwrite writes 256 MiB in one fwrite, and what it proves — that a
# write is not copied through one kernel-heap allocation of its whole size —
# is only proven where that allocation could not be had beside the program's
# own 256 MiB: on a machine with less than twice the write. On the stand's
# 2 GiB the copy would be had, and bigwrite would pass with the defect back.
QUILL_STAND=${QUILL_STAND:-"UEFI=on STRICT=on CORES=4 MEM=2G"}
QUILL_BOOT1_MEM=448M
QUILL_GROW_SECTORS=2097152   # a gigabyte more ground than the image was made with
QUILL_NEED_FREE=131072       # blocks the grown volume must have free: 512 MiB

# quill_keys — QUILL_STAND into QUILL_KEYS, its words; says so and answers 1
# when it does not read as words.
quill_keys() {
    if ! eval "QUILL_KEYS=($QUILL_STAND)" 2>/dev/null; then
        bad "quill: QUILL_STAND does not read as shell words: $QUILL_STAND"
        return 1
    fi
}

# The core count QUILL_STAND boots with — the Makefile's own default when the
# stand does not say — because a write's path depends on it (stand_path).
quill_cores() {
    local key c=1
    for key in ${QUILL_KEYS[@]+"${QUILL_KEYS[@]}"}; do
        case "${key#CORES=}" in "$key"|''|*[!0-9]*) ;; *) c=${key#CORES=} ;; esac
    done
    echo "$c"
}

quill_keep() {    # quill_keep NAME — stop the machine and keep its log
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.quill.$1.log"
}

# quill_boot NAME [MEM] — boot the stretched image, and refuse to go on from a
# boot that stands on any other ground: a repacked image is a fresh one, and
# every verdict taken on it would be about a volume nobody stretched.
# VOLUME_TESTS goes on run-bg as well: it owns a rebuild marker, and a run-bg
# without it would rebuild the tree plain and repack. MEM, when given, is the
# machine's memory in place of the stand's; no build key reads it, so the
# image is booted as it was stretched.
quill_boot() {
    local stood key keys=()
    for key in ${QUILL_KEYS[@]+"${QUILL_KEYS[@]}"}; do
        [ -n "${2:-}" ] && [ "${key#MEM=}" != "$key" ] && continue
        keys+=("$key")
    done
    [ -n "${2:-}" ] && keys+=("MEM=$2")
    make run-stop >/dev/null 2>&1
    make run-bg VOLUME_TESTS=on ${keys[@]+"${keys[@]}"} >/dev/null 2>&1
    if ! wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"; then
        quill_keep "$1"
        bad "quill: boot $1 never reached a shell"
        return 1
    fi
    wait_for_quiet "$BOOT_BURST_QUIET_S"
    if ! stood=$(grow_stood_on build/serial.log "$QUILL_START" "$QUILL_COUNT"); then
        quill_keep "$1"
        harness "quill: boot $1 stood on $stood — the stretched image was not the one booted, so it judges nothing"
        return 1
    fi
}

# quill_run CASE [QUIET_S] — one case: typed, its verdict line, and the shell
# back at its prompt. The prompt is counted as lines, not read as the last two
# bytes of the log: a Use Context makes it `[tags] ~ `, and a kernel line may
# follow it.
quill_run() {
    local mark
    mark=$(wc -l < build/serial.log)
    type_line "quilltest $1" || return 1
    wait_for_line -E build/serial.log "\[QUILL\] $1: (PASS|DEFECT|PENDING|FAIL) " \
        "${2:-$WAIT_QUIET_S}" $((mark + 1)) || return 1
    wait_until "$WAIT_QUIET_S" "the shell's prompt after quilltest $1" \
        '[ "$(tail -n +$((mark + 1)) build/serial.log | tr -d "\r" | grep -cE "^(\[[^]]*\] )?~ ")" -ge 2 ]' \
        || return 1
    wait_for_quiet 3
}

# quill_session TAG — the three boots, their logs and the two listings, on a
# tree built VOLUME_TESTS=on (the caller puts the plain one back).
quill_session() {
    local tag=$1 grown c
    rm -f "$SCRATCH/serial.quill.$tag"[123].log "$SCRATCH/quill.$tag.list"[13]
    quill_keys || return 1
    rm -f build/boxos.img
    if ! build_with_key VOLUME_TESTS=on "$(strict_key ${QUILL_KEYS[@]+"${QUILL_KEYS[@]}"})"; then
        echo "BUILD FAILED (VOLUME_TESTS=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "quill: VOLUME_TESTS=on does not build"
        return 1
    fi
    make run-stop >/dev/null 2>&1
    mutate_product build/boxos.img
    if ! grown=$(grow_stretch build/boxos.img "$QUILL_GROW_SECTORS"); then
        bad "quill: the image has no BoxOS partition to stretch"
        return 1
    fi
    QUILL_START=${grown% *}
    QUILL_COUNT=${grown#* }

    quill_boot "${tag}1" "$QUILL_BOOT1_MEM" || return 1
    for c in longquery bigwrite sequential interleave coldread coldwrite usememory; do
        quill_run "$c" "$WAIT_QUIET_SLOW_S" || break
    done
    quill_keep "${tag}1"
    python3 tools/volume-read.py build/boxos.img list >"$SCRATCH/quill.$tag.list1" 2>&1

    quill_boot "${tag}2" || return 1
    if grep -aq "\[QUILL\] usememory: PENDING " "$SCRATCH/serial.quill.${tag}1.log"; then
        quill_run usememory
    fi
    quill_run fidhistory "$WAIT_QUIET_SLOW_S"
    quill_keep "${tag}2"

    quill_boot "${tag}3" || return 1
    quill_run systemtag
    quill_keep "${tag}3"
    python3 tools/volume-read.py build/boxos.img list >"$SCRATCH/quill.$tag.list3" 2>&1
    return 0
}

# quill_judge TAG BIG SEQ WEAVE USE_BYTES USE_TAGS FIDS — what the three logs
# and the two listings show, one line per finding into $SCRATCH/quill.TAG.verdict:
#   check<TAB>ok|bad<TAB>sentence            the stand itself
#   selftest<TAB>passed|failed|missing<TAB>line
#   case<TAB>NAME<TAB>defect|behaves|fail<TAB>what
# The sizes are the ones quilltest was built with, which quillmut shrinks.
quill_judge() {
    python3 - "$SCRATCH" "$@" "$QUILL_NEED_FREE" >"$SCRATCH/quill.$1.verdict" 2>&1 <<'QUILL_EOF'
import math, re, sys
scratch, tag = sys.argv[1], sys.argv[2]
BIG, SEQ, WEAVE, USE_BYTES, USE_TAGS, FIDS, NEED_FREE = (int(x) for x in sys.argv[3:10])
COLD = 1 << 20                          # quilltest's QUILL_COLD_BYTES
# The extents a record holds with one stem tag and a 10-byte name (quillS.bin,
# quillA.bin): 42 bytes of header, 2 per tag, 6 per extent and the name in
# 4080 (plot/plot_width.h RECORD_HEADER_SIZE, PlotFits). A file of one
# extent per block is refused at exactly EMAX blocks.
EMAX = (4080 - 42 - 2 - 10) // 6

def text(path):
    try:
        return open(path, errors="replace").read().replace("\r", "")
    except OSError:
        return ""

logs = [text("%s/serial.quill.%s%d.log" % (scratch, tag, i)) for i in (1, 2, 3)]
lists = [text("%s/quill.%s.list%d" % (scratch, tag, i)) for i in (1, 3)]

def out(*fields):
    print("\t".join(str(f) for f in fields))

def verdict(log, case):
    found = re.findall(r"\[QUILL\] %s: (PASS|DEFECT|PENDING|FAIL) — (.*)" % case, log)
    return found[-1] if found else (None, "")

def said(log, case):
    word, rest = verdict(log, case)
    return "the machine said %s — %s" % (word, rest) if word else "the machine gave %s no verdict" % case

def host(listing, name):
    m = re.search(r"^ +%s +(\d+) bytes +(\d+) extent\(s\), (\d+) block\(s\)$" % re.escape(name),
                  listing, re.M)
    return tuple(int(x) for x in m.groups()) if m else None

# The most extents a file grown a block at a time can have: its Allotment
# windows, each sized to the file when it has none left — 1, 1, 2, 4, … — so
# 1 + ceil(log2 blocks), fewer wherever a window begins where the last ended.
def ceiling(blocks):
    return (blocks - 1).bit_length() + 1 if blocks > 0 else 0

# The boot's own mount: the first such line, before the startup tests.
def keeps(log):
    found = re.findall(r"this mount keeps (\d+) bytes of kernel heap for (\d+) file id\(s\) "
                       r"ever issued, (\d+) of them live", log)
    return tuple(int(x) for x in found[0]) if found else None

def after_echo(log, case):
    at = log.rfind("~ quilltest " + case)
    return log[at:] if at >= 0 else log

# The medium said it failed a command while the case ran: whatever the case
# says of a refusal, the disk may have made it.
def medium_fault(log, case):
    m = re.search(r"\[AHCI\] port \d+ (?:went past its deadline|reported a fatal error)[^\n]*",
                  after_echo(log, case))
    return m.group(0) if m else None

# --- the stand ---------------------------------------------------------------
m = re.search(r"\[KLIB\] Total RAM: (\d+) MB; the kernel heap starts from a (\d+) KB boot region", logs[0])
ram = int(m.group(1)) if m else None
out("check", "ok" if ram is not None and ram * 1024 * 1024 < 2 * BIG else "bad",
    "boot 1 has %s MB of RAM: a class of cabins is refused the %d bytes bigwrite asks for in one "
    "piece only while less than twice that is free — below %d MB" % (ram, BIG, 2 * BIG >> 20))
m = re.search(r"this volume runs (\d+) sectors and the ground under it runs (\d+) — it takes the rest",
              logs[0])
out("check", "ok" if m else "bad", "boot 1's volume took the ground behind it: %s"
    % ("%s sectors to %s" % m.groups() if m else "no line says so"))
said_free = re.findall(r"\[TagFS\] (\d+) data blocks, (\d+) free, (\d+) files", logs[0])
free = int(said_free[0][1]) if said_free else None
out("check", "ok" if free is not None and free >= NEED_FREE else "bad",
    "boot 1 has %s blocks free, %d needed for everything the cases write" % (free, NEED_FREE))
for i, log in enumerate(logs, 1):
    k = keeps(log)
    out("check", "ok" if k else "bad", "boot %d's mount says what it keeps: %s"
        % (i, "%d bytes for %d file ids, %d live" % k if k else "no line"))
    fault = re.findall(r"^.*(?:VERDICT|PANIC|\[EXCEPTION\]).*$", log, re.M)
    out("check", "bad" if fault or not log else "ok", "boot %d: %s" % (i,
        "no log" if not log else fault[0][:160] if fault else "no panic, no exception, no verdict"))

m = re.search(r"\[KLIB\] heap accounting self-test (passed|FAILED)(.*)", logs[0])
if not m:
    out("selftest", "missing", "boot 1 printed no heap accounting self-test line")
else:
    rise = re.search(r"a 8192-byte pool block, a 65536-byte large piece and 64 slab objects of 64 "
                     r"bytes were counted \(\+(\d+), \+(\d+), \+(\d+)\)", m.group(2))
    good = m.group(1) == "passed" and rise and int(rise.group(1)) >= 8192 and \
        int(rise.group(2)) >= 65536 and int(rise.group(3)) >= 4096
    out("selftest", "passed" if good else "failed", "[KLIB] heap accounting self-test %s%s" % m.groups())

# --- the cases -----------------------------------------------------------------
def longquery():
    word, rest = verdict(logs[0], "longquery")
    m = re.match(r"a query for one tag 256 bytes long returned all (\d+) of the volume's (\d+) files$", rest)
    if word == "DEFECT" and m and m.group(1) == m.group(2) and int(m.group(1)) > 0:
        return "defect", "a query for one tag 256 bytes long answered all %s of the volume's %s files" % m.groups()
    if word == "PASS" and re.match(r"a query for one tag 256 bytes long was refused \(error \d+\)$", rest):
        return "behaves", rest
    return "fail", said(logs[0], "longquery")

def bigwrite():
    word, rest = verdict(logs[0], "bigwrite")
    seen = [(int(s), int(p)) for s, p in re.findall(
        r"\[KLIB\] refused (\d+) bytes to [^:]+: they make the kernel hold \d+ KB already, and the "
        r"machine has (\d+) KB left", after_echo(logs[0], "bigwrite"))]
    witness = [(s, p) for s, p in seen if BIG <= s < BIG + 4096]
    m = re.match(r"one fwrite of (\d+) bytes was refused with error (\d+) and nothing reached the file$", rest)
    if word == "DEFECT" and m and int(m.group(1)) == BIG and m.group(2) == "101" and witness:
        return "defect", ("one fwrite of %d bytes was refused with error 101 and nothing reached the file; "
                          "the kernel refused %d bytes to its class with %d KB left" % ((BIG,) + witness[-1]))
    m = re.match(r"one fwrite of (\d+) bytes landed whole in (\d+) ms and read back exactly$", rest)
    if word == "PASS" and m and int(m.group(1)) == BIG and not witness:
        return "behaves", rest
    return "fail", "%s; the kernel's heap refusals for it: %s" % (said(logs[0], "bigwrite"), seen or "none")

def at_width(files):
    """Every file whole on the host at the record's width: EMAX blocks in EMAX
    extents, one per write."""
    return all(h and h == (EMAX * 4096, EMAX, EMAX) for h in files)

def sequential():
    word, rest = verdict(logs[0], "sequential")
    h = host(lists[0], "quillS.bin")
    fault = medium_fault(logs[0], "sequential")
    if fault:
        return "fail", "%s; the medium said, while it ran: %s" % (said(logs[0], "sequential"), fault)
    if word == "PASS" and rest.startswith("quillS.bin took %d bytes in " % SEQ) and h:
        c = ceiling(h[2])
        if h[0] == SEQ and h[2] == SEQ // 4096 and h[1] <= c:
            return "behaves", "quillS.bin took %d bytes in a row; the host counts %d extent(s) for %d blocks, " \
                              "the ceiling %d" % (SEQ, h[1], h[2], c)
        return "fail", "the file is whole but quillS.bin has %d extents where %d is the ceiling (%d bytes, " \
                       "%d blocks)" % (h[1], c, h[0], h[2])
    m = re.match(r"quillS\.bin, with no other writer, refused its write at byte (\d+) of (\d+) \(error (\d+)\) "
                 r"after (\d+) writes of 4096 in a row, the byte its record's width puts the refusal at "
                 r"\((\d+)\)", rest)
    if word == "DEFECT" and m and int(m.group(2)) == SEQ and m.group(3) == "200" and \
            int(m.group(1)) == int(m.group(5)) == EMAX * 4096 and at_width([h]):
        return "defect", "quillS.bin, its only writer, was refused at byte %s (error 200) after %s writes, " \
                         "the record's width; the host counts %d extents for %d blocks — one per write" \
                         % (m.group(1), m.group(4), h[1], h[2])
    return "fail", "%s; the host lists quillS.bin as %s" % (said(logs[0], "sequential"), h)

def interleave():
    word, rest = verdict(logs[0], "interleave")
    ha, hb = host(lists[0], "quillA.bin"), host(lists[0], "quillB.bin")
    fault = medium_fault(logs[0], "interleave")
    if fault:
        return "fail", "%s; the medium said, while it ran: %s" % (said(logs[0], "interleave"), fault)
    if word == "PASS" and rest.startswith("quillA.bin and quillB.bin took %d bytes each" % WEAVE) and ha and hb:
        c = ceiling(WEAVE // 4096)
        if all(h[0] == WEAVE and h[2] == WEAVE // 4096 and h[1] <= c for h in (ha, hb)):
            return "behaves", "quillA.bin and quillB.bin took %d bytes each, in turn; the host counts %d and %d " \
                              "extents, the ceiling %d" % (WEAVE, ha[1], hb[1], c)
        return "fail", "the files are whole but quillA.bin has %d extents and quillB.bin %d where %d is the " \
                       "ceiling" % (ha[1], hb[1], c)
    m = re.match(r"(quill[AB]\.bin) refused its write at byte (\d+) of (\d+) \(error (\d+)\) after (\d+) "
                 r"alternating writes of 4096, the byte its record's width puts the refusal at \((\d+)\); "
                 r"the (\d+) and (\d+) bytes", rest)
    if word == "DEFECT" and m and m.group(1) == "quillA.bin" and int(m.group(3)) == WEAVE and \
            m.group(4) == "200" and int(m.group(2)) == int(m.group(6)) == EMAX * 4096 and \
            int(m.group(7)) == int(m.group(8)) == EMAX * 4096 and at_width([ha, hb]):
        return "defect", "%s was refused at byte %s (error 200) after %s alternating writes, the record's " \
                         "width; the host counts %d and %d extents for %d and %d blocks — one per write, as for " \
                         "the lone writer of sequential" % (m.group(1), m.group(2), m.group(5), ha[1], hb[1],
                                                           ha[2], hb[2])
    return "fail", "%s; the host lists quillA.bin as %s and quillB.bin as %s" % (said(logs[0], "interleave"), ha, hb)

def coldread():
    word, rest = verdict(logs[0], "coldread")
    m = re.match(r"a read of (\d+) bytes into memory this program never touched delivered all (\d+), each one "
                 r"the file's$", rest)
    if word == "PASS" and m and int(m.group(1)) == int(m.group(2)) == COLD:
        return "behaves", rest
    m = re.match(r"a read of (\d+) bytes into memory this program never touched reported all (\d+), and (\d+) of "
                 r"them are not the file's, from byte (\d+) on \((\d+) of those are zeros\)$", rest)
    if word == "DEFECT" and m and int(m.group(1)) == int(m.group(2)) == COLD and int(m.group(3)) > 0:
        return "defect", "a read of %d bytes into memory its program never touched reported all of them, and %s " \
                         "are not the file's from byte %s on, %s of them zeros" % (COLD, m.group(3), m.group(4),
                                                                                  m.group(5))
    return "fail", said(logs[0], "coldread")

def coldwrite():
    word, rest = verdict(logs[0], "coldwrite")
    m = re.match(r"a write of (\d+) bytes out of memory this program never touched wrote all (\d+), and the file "
                 r"reads back as zeros, every byte$", rest)
    if word == "PASS" and m and int(m.group(1)) == int(m.group(2)) == COLD:
        return "behaves", rest
    m = re.match(r"a write of (\d+) bytes out of memory this program never touched reported all (\d+), and (\d+) "
                 r"bytes of the file are not zeros, from byte (\d+) on$", rest)
    if word == "DEFECT" and m and int(m.group(1)) == int(m.group(2)) == COLD and int(m.group(3)) > 0:
        return "defect", "a write of %d bytes out of memory its program never touched reported all of them, and " \
                         "%s bytes of the file are not zeros from byte %s on" % (COLD, m.group(3), m.group(4))
    return "fail", said(logs[0], "coldwrite")

def usememory():
    w1, t1 = verdict(logs[0], "usememory")
    w2, t2 = verdict(logs[1], "usememory")
    # The clear right after is the control: remembered, it says the volume took
    # a Ledger write beside the probe's, so the length is what was refused.
    m = re.match(r"a Use Context of (\d+) bytes in (\d+) tags was set and the volume did not remember it "
                 r"\(remembered=0; cleared with remembered=1, anchor 0\)$", t1)
    if w1 == "DEFECT" and m and (int(m.group(1)), int(m.group(2))) == (USE_BYTES, USE_TAGS):
        return "defect", "a Use Context of %d bytes in %d tags was set and the volume did not remember it " \
                         "(remembered=0), and the clear beside it was remembered" % (USE_BYTES, USE_TAGS)
    p = re.match(r"a Use Context of (\d+) bytes in (\d+) tags is remembered \(remembered=1\)", t1)
    q = re.match(r"after a reboot the machine gave back all (\d+) tags, (\d+) bytes, in the order they were "
                 r"said; the probe is cleared \(remembered=1\)", t2)
    if w1 == "PENDING" and w2 == "PASS" and p and q and \
            (int(p.group(1)), int(p.group(2))) == (USE_BYTES, USE_TAGS) == (int(q.group(2)), int(q.group(1))):
        return "behaves", "a Use Context of %d bytes in %d tags was remembered and came back whole after a " \
                          "reboot" % (USE_BYTES, USE_TAGS)
    return "fail", "boot 1: %s; boot 2: %s" % (said(logs[0], "usememory"), said(logs[1], "usememory"))

def fidhistory():
    word, rest = verdict(logs[1], "fidhistory")
    m = re.match(r"spent (\d+) file ids, (\d+) to (\d+), with (-?\d+) live files before and after", rest)
    if word != "PENDING" or not m or int(m.group(1)) != FIDS:
        return "fail", said(logs[1], "fidhistory")
    first, last = int(m.group(2)), int(m.group(3))
    b, c = keeps(logs[1]), keeps(logs[2])
    if not b or not c:
        return "fail", "a mount did not say what it keeps: boot 2 %s, boot 3 %s" % (b, c)
    # Between the two mounts: boot 2's startup tests, then the case's ids,
    # the last of them the last id issued before boot 3.
    if b[2] != c[2] or last - first + 1 != FIDS or c[1] != last or first <= b[1]:
        return "fail", "the two mounts do not stand around the case's %d ids, %d to %d: %d then %d ids " \
                       "issued, %d then %d live" % (FIDS, first, last, b[1], c[1], b[2], c[2])
    dead = c[1] - b[1]
    slope = (c[0] - b[0]) / dead
    what = "%d dead file ids — the case's %d, and %d the startup tests spent before it — took the next " \
           "mount from %d to %d bytes of kernel heap, %.1f bytes per id" \
           % (dead, FIDS, dead - FIDS, b[0], c[0], slope)
    if slope >= 64:
        return "defect", what
    if slope <= 8:
        return "behaves", what
    return "fail", what + " — neither HEAD's arrays sized by every id nor an index of the living"

def systemtag():
    word, rest = verdict(logs[2], "systemtag")
    listed = len(re.findall(r"^ +\S+ +\d+ bytes +\d+ extent\(s\)", lists[1], re.M))
    present = host(lists[1], "kernel.bin") is not None
    m = re.match(r"kernel\.bin \(file (\d+)\) gave up its system tag to untag and was then deleted by erase "
                 r"trashed, programs that do not wear system \(exit 0 each\); asked about it now, the volume "
                 r"answers error (\d+)", rest)
    if word == "DEFECT" and m and listed and not present:
        return "defect", "kernel.bin (file %s) gave up its system tag to untag and was deleted by erase trashed, " \
                         "programs that do not wear system (error %s when asked after); the host lists %d files " \
                         "and no kernel.bin" % (m.group(1), m.group(2), listed)
    p = re.match(r"kernel\.bin \(file (\d+)\) kept its system tag against untag \(exit (\d+)\), and its place "
                 r"against erase trashed with it in the trash \(trash and erase exit 0\), programs that do not "
                 r"wear system", rest)
    if word == "PASS" and p and present and p.group(2) != "0":
        return "behaves", "kernel.bin (file %s) refused untag (exit %s) and stayed through erase trashed, programs " \
                          "that do not wear system, and the host still lists it" % p.groups()
    return "fail", "%s; the host lists %d files, kernel.bin %s" % (said(logs[2], "systemtag"), listed,
                                                                 "among them" if present else "not among them")

for name, judge in (("longquery", longquery), ("bigwrite", bigwrite), ("sequential", sequential),
                    ("interleave", interleave), ("coldread", coldread), ("coldwrite", coldwrite),
                    ("usememory", usememory), ("fidhistory", fidhistory), ("systemtag", systemtag)):
    seen, what = judge()
    out("case", name, seen, what)
QUILL_EOF
    [ $? = 0 ] && return 0
    printf 'check\tbad\tthe judge could not read the quill logs: %s\n' \
        "$(tail -1 "$SCRATCH/quill.$1.verdict")" >"$SCRATCH/quill.$1.verdict"
}

# The path a case's writes took: the boot it ran on, and its cores.
quill_path() {
    local boot=1
    case "$2" in fidhistory) boot=2 ;; systemtag) boot=3 ;; esac
    stand_path "$SCRATCH/serial.quill.$1$boot.log" "$(quill_cores)"
}

# The path a case's verdict names; a query writes nothing and takes none.
quill_said_path() {   # quill_said_path CASE SEEN
    case "$1:$2" in
        longquery:*)     echo "" ;;
        *)               echo " [path: $(quill_path quill "$1")]" ;;
    esac
}

run_quill() {
    echo "== quill: the Quill write core's red tests, each held under the step that discharges it =="
    echo "   stand: $QUILL_STAND (boot 1 MEM=$QUILL_BOOT1_MEM), built VOLUME_TESTS=on"
    local kind a b c
    # A session cut short has said why; three boots or none are judged.
    if ! quill_session quill; then
        make run-stop >/dev/null 2>&1; restore build/boxos.img; buildkey_restore; return
    fi
    quill_judge quill 268435456 3145728 67108864 1000 91 50000
    while IFS=$'\t' read -r kind a b c; do
        case "$kind" in
            check)    if [ "$a" = ok ]; then ok "quill: $b"; else bad "quill: $b"; fi ;;
            selftest) if [ "$a" = passed ]; then ok "quill: $b"; else bad "quill: $b"; fi ;;
            case)     caveat_judge "$a" "$b" "$c$(quill_said_path "$a" "$b")" ;;
        esac
    done < "$SCRATCH/quill.quill.verdict"
    lockrank_judge "$SCRATCH/serial.quill.quill1.log $SCRATCH/serial.quill.quill2.log $SCRATCH/serial.quill.quill3.log" "quill"
    make run-stop >/dev/null 2>&1
    restore build/boxos.img
    buildkey_restore
}

# ---------------------------------------------------------------------------
# quillmut — every quill oracle, shown the behaviour it waits for, must turn
# ---------------------------------------------------------------------------
#
# longquery's fix is the tree's own since step 1.1b, and bigwrite, sequential
# and interleave are the tree's own since step 1.3 — each must behave with
# nothing put in. Two fixes that used to be emulated here are the tree's own
# as well, and so they are taken OUT, and their oracle must see the defect
# come back: the Ink guard (step 1.4, ink.c) — down, untag takes the system
# tag off kernel.bin and erase deletes it (systemtag); the Use Context's
# chain (step 1.6B, use_memory.c) — never staked, a context of 1000 bytes is
# carried by nothing and the volume does not remember it (usememory).
# fidhistory keeps one file in a thousand, so its guest must count them. The
# slab stops counting, so the kernel's heap self-test must say FAILED. The
# kernel's copies stop bringing in a page the program has not touched
# (VmmUserCopyWindow), so coldread and coldwrite must turn to a refusal that
# reports 0 bytes — never a count, which is the half of the fix that stands
# without the other. And first, with no machine at all, caveat_judge's three
# answers in their approved words.
quill_mutants_on() {
    mutate src/kernel/tagfs/ink/ink.c
    mutate src/kernel/tagfs/use_memory/use_memory.c
    mutate src/userspace/apps/quilltest_throwaway.c
    python3 - <<'EOF2'
for p, a, b in (
    ("src/kernel/tagfs/ink/ink.c",
     "        if (warrant != INK_WARRANTED && InkTagGuarded(f.dropped[i]))\n",
     "        if (0 && warrant != INK_WARRANTED && InkTagGuarded(f.dropped[i]))   /* logcheck mutation: the guard is down, a guarded tag comes off */\n"),
    ("src/kernel/tagfs/ink/ink.c",
     "    if (boot || (warrant != INK_WARRANTED && InkWearsGuarded(tags, tag_count))) {\n",
     "    if (0 && (boot || (warrant != INK_WARRANTED && InkWearsGuarded(tags, tag_count)))) {   /* logcheck mutation: the guard is down, the kernel's file changes */\n"),
    ("src/kernel/tagfs/use_memory/use_memory.c",
     "    error_t rc   = UseChainStake(s_cycle.need, &s_cycle.chain, &s_cycle.got);\n",
     "    error_t rc   = ERR_TAGFS_NO_SPACE;   /* logcheck mutation: no chain is staked, a Use Context rides on nothing */\n"),
    ("src/userspace/apps/quilltest_throwaway.c", "        int drc = delete((file_id_t)fid);\n",
     "        int drc = (i % 1000u == 999u) ? 0 : delete((file_id_t)fid);   /* logcheck mutation: one in a thousand stays */\n"),
):
    s = open(p).read()
    assert s.count(a) == 1, "quillmut anchor missing in %s: %s" % (p, a.strip().splitlines()[0])
    open(p, "w").write(s.replace(a, b, 1))
EOF2
    mut_done src/kernel/tagfs/ink/ink.c "logcheck mutation" 2
    mut_done src/kernel/tagfs/use_memory/use_memory.c "logcheck mutation" 1
    mut_done src/userspace/apps/quilltest_throwaway.c "logcheck mutation" 1

    mutate src/lib/kernel/slab.c
    python3 - <<'EOF2'
p = "src/lib/kernel/slab.c"
s = open(p).read()
a = "    __atomic_store_n(&cls->live_bytes, cls->live_bytes + cls->obj_size, __ATOMIC_RELAXED);\n"
assert s.count(a) == 1, "quillmut slab-count anchor missing"
s = s.replace(a, "    (void)0;   /* logcheck mutation: the slab stops counting what it hands out */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/lib/kernel/slab.c "logcheck mutation"

    mutate src/kernel/core/memory/vmm/vmm.c
    python3 - <<'EOF2'
p = "src/kernel/core/memory/vmm/vmm.c"
s = open(p).read()
a = "    int touched = VmmFirstTouch(ctx, user_vaddr);\n"
assert s.count(a) == 1, "quillmut first-touch anchor missing"
s = s.replace(a, "    int touched = 0;   /* logcheck mutation: a copy never brings a page in */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/memory/vmm/vmm.c "logcheck mutation"
}

quill_mutants_off() {
    restore src/kernel/tagfs/ink/ink.c
    restore src/kernel/tagfs/use_memory/use_memory.c
    restore src/userspace/apps/quilltest_throwaway.c
    restore src/lib/kernel/slab.c
    restore src/kernel/core/memory/vmm/vmm.c
}

# The judge with no machine: a caveated test that shows its defect is held, one
# that behaves is lifted early, and a test with no caveat that behaves passes —
# each in the approved words, and each counted where it must be. Since step
# 1.6B no quill case is held under a step, so the first of them, longquery, is
# given its step back for the length of the check — in the file (restore puts
# it back), read by subshells that source the table again — the way
# lockrankmut shows its judge a held place.
quill_step_given() {   # quill_step_given NAME STEP — NAME's row given STEP back
    mutate tools/stand_caveat.sh
    python3 - "$1" "$2" <<'EOF'
import re, sys
p = "tools/stand_caveat.sh"
s = open(p).read()
s, n = re.subn(r'(\n\s*' + re.escape(sys.argv[1]) + r'\)\s+echo ")(\|[^\n]*;;)',
               lambda m: m.group(1) + sys.argv[2] + m.group(2) + "   # logcheck mutation: the step given back",
               s, count=1)
assert n == 1, "quillmut: the row of " + sys.argv[1] + " was not found"
open(p, "w").write(s)
EOF
    mut_done tools/stand_caveat.sh "logcheck mutation" 1
}

quill_judge_selfcheck() {
    local said step defect held=longquery
    quill_step_given "$held" 1.1b
    step=$(. tools/stand_caveat.sh; caveat_step "$held")
    defect=$(. tools/stand_caveat.sh; caveat_defect "$held")
    said=$(. tools/stand_caveat.sh
           PASS=0; FAIL=0; CAVEAT=0; HARNESS=0
           caveat_judge "$held" defect "seen 1"
           caveat_judge "$held" behaves "seen 2"
           caveat_judge quillnocaveat behaves "seen 3"
           echo "counted $PASS $FAIL $CAVEAT")
    printf '%s\n' "$said" | grep -qF "caveat held: $held — $defect — discharged by step $step (seen 1)"
    chk $? "quillmut: a caveated test that shows its defect is held, under step $step"
    printf '%s\n' "$said" | grep -qF "caveat lifted early: $held — the test does not test what it claims, or step $step already landed — lift the caveat (seen 2)"
    chk $? "quillmut: a caveated test that behaves is lifted early, and that is a FAIL"
    printf '%s\n' "$said" | grep -qF "quillnocaveat: seen 3"
    chk $? "quillmut: a test with no caveat that behaves passes"
    printf '%s\n' "$said" | grep -qx "counted 1 1 1"
    chk $? "quillmut: the three were counted as one PASS, one FAIL and one held caveat ($(printf '%s\n' "$said" | tail -1))"
    restore tools/stand_caveat.sh
}

run_quillmut() {
    echo "== quillmut: every quill oracle, shown the behaviour it waits for, must turn =="
    local kind a b c want line
    quill_judge_selfcheck
    quill_mutants_on
    if ! quill_session quillmut; then
        make run-stop >/dev/null 2>&1; quill_mutants_off; restore build/boxos.img; buildkey_restore; return
    fi
    make run-stop >/dev/null 2>&1
    quill_mutants_off
    restore build/boxos.img
    quill_judge quillmut 268435456 3145728 67108864 1000 91 50000
    while IFS=$'\t' read -r kind a b c; do
        case "$kind" in
            check)    if [ "$a" = ok ]; then ok "quillmut: $b"; else bad "quillmut: $b"; fi ;;
            selftest) if [ "$a" = failed ]; then
                          ok "quillmut: the slab stopped counting and the kernel's self-test said so — $b"
                      else
                          bad "quillmut: the slab stopped counting and the kernel's self-test said $a — $b"
                      fi ;;
            case)     case "$a" in
                          fidhistory)            want="fail:[0-9]+ files were live before and [0-9]+ after" ;;
                          coldread)              want="fail:a read of [0-9]+ bytes into memory this program never touched answered -[0-9]+ \(error [0-9]+\); 0 of the bytes" ;;
                          coldwrite)             want="fail:a write of [0-9]+ bytes out of memory this program never touched answered -[0-9]+ \(error [0-9]+\); the file holds 0 bytes" ;;
                          systemtag)             want="defect:kernel\.bin \(file [0-9]+\) gave up its system tag to untag and was deleted by erase trashed" ;;
                          usememory)             want="defect:a Use Context of 1000 bytes in 91 tags was set and the volume did not remember it" ;;
                          *)                     want="behaves:" ;;
                      esac
                      line="$b:$c"
                      if [ "$b" = "${want%%:*}" ] && printf '%s\n' "$c" | grep -qE "${want#*:}"; then
                          ok "quillmut: $a ${b} — $c — and the oracle would have caught it"
                      else
                          bad "quillmut: $a should have turned to ${want%%:*} and did not — $line"
                      fi ;;
        esac
    done < "$SCRATCH/quill.quillmut.verdict"
    quill_fid_kept
    buildkey_restore   # every put-back discards the image; leave one built plain from clean sources
}

# fidhistory's guest must have counted exactly the files the mutant kept.
quill_fid_kept() {
    local nums before after
    nums=$(tr -d '\r' < "$SCRATCH/serial.quill.quillmut2.log" 2>/dev/null \
           | sed -nE 's/.*fidhistory: FAIL.* ([0-9]+) files were live before and ([0-9]+) after.*/\1 \2/p' \
           | tail -1)
    before=${nums% *}
    after=${nums#* }
    if [ -n "$nums" ] && [ $((after - before)) = 50 ]; then
        ok "quillmut: fidhistory counted the 50 files the mutant kept ($before live before, $after after)"
    else
        bad "quillmut: fidhistory did not count the 50 files the mutant kept (${nums:-no FAIL line})"
    fi
}

# ===========================================================================
# stackguard — a block buffer on the kernel stack stops the link
# ===========================================================================
#
# tools/check_stack_usage.py stands in front of the kernel link and holds the
# frames tools/stack_usage_held.txt names, each under the step that removes it.
# Host only, no boot. Each leg puts into the tree one thing the gate exists to
# refuse, builds as a person at the desk builds — a plain make, without
# HARNESS=on, because that is the one build the held numbers are judged on —
# and requires the build to stop before the first link pass, in the gate's
# own words and with its numbers:
#   M1 a 4 KiB block on the stack of a function that holds none: new offender
#   M2 that block held 512 bytes short of what M1 measured it at: grew
#   M3 the line kept and the block taken back off the stack: lifted early
#   M4 a reach line while the reach is within its budget: lifted early, and
#      nothing else is said to be wrong
#   M5 the budget lowered one byte below the reach: over its budget
# Since steps 1.9 and 1.10 the tree holds no frame, so M2 and M3 ask the held
# rules of a line the scenario writes itself, for tagfs_close.
# Then M2 once more through the harness, where the same mutant must LINK, the
# gate must say what it did not judge and hold the frame, and no stamp may be
# left: every *mut scenario in this file builds a mutated tree with
# HARNESS=on, and one that failed to link over a frame's few bytes would fail
# for the wrong reason.
# ---------------------------------------------------------------------------

STACKGUARD_HELD=tools/stack_usage_held.txt

# stackguard_red NAME ERE — a plain make must stop at the gate, saying ERE,
# before any link. Returns 0 when it did; the build log is kept either way.
stackguard_red() {
    local log="$SCRATCH/build.stackguard.$1.log" rc
    mut_stamp_passed
    command make >"$log" 2>&1
    rc=$?
    [ $rc -ne 0 ] && grep -qE "$2" "$log" && ! grep -q "Nameplate pass 1" "$log"
}

stackguard_said() {   # the gate's first refusal in NAME's build, for a verdict line
    grep -E '^\[stack\] FAIL' "$SCRATCH/build.stackguard.$1.log" | head -1 | cut -c1-220
}

# stackguard_block_on — a block on the stack of tagfs_close, which holds none.
stackguard_block_on() {
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF2'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "void tagfs_close(TagFSFileHandle *handle)\n{\n"
assert s.count(anchor) == 1, "stackguard tagfs_close anchor missing"
s = s.replace(anchor, anchor +
    "    volatile uint8_t logcheck_scratch[TAGFS_BLOCK_SIZE];   /* logcheck mutation: a block on the stack */\n"
    "    logcheck_scratch[handle ? handle->file_id % TAGFS_BLOCK_SIZE : 0] = 1;\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation" 1
}

# stackguard_hold_on BYTES — a line holding tagfs_close at BYTES, under a step
# that has not landed.
stackguard_hold_on() {
    mutate "$STACKGUARD_HELD"
    printf 'src/kernel/tagfs/tagfs.c:tagfs_close  %s  9.99 logcheck mutation: a block held on the stack\n' "$1" \
        >>"$STACKGUARD_HELD"
    mut_done "$STACKGUARD_HELD" "logcheck mutation" 1
}

# stackguard_reach_on BYTES — a reach line held at BYTES, when the reach is
# within its budget and no line is due.
stackguard_reach_on() {
    mutate "$STACKGUARD_HELD"
    printf 'reach  %s  9.99 logcheck mutation: a reach held that is within its budget\n' "$1" \
        >>"$STACKGUARD_HELD"
    mut_done "$STACKGUARD_HELD" "logcheck mutation" 1
}

# stackguard_budget_on BYTES — the budget the reach is judged against, lowered
# to BYTES: no reach line, so the budget itself is the rule.
stackguard_budget_on() {
    mutate tools/check_stack_usage.py
    python3 - "$1" <<'EOF2'
import sys
p = "tools/check_stack_usage.py"
s = open(p).read()
anchor = "BUDGET = 4096\n"
assert s.count(anchor) == 1, "stackguard BUDGET anchor missing"
open(p, "w").write(s.replace(anchor, "BUDGET = %s   # logcheck mutation: the budget below the reach\n" % sys.argv[1], 1))
EOF2
    mut_done tools/check_stack_usage.py "logcheck mutation" 1
}

run_stackguard() {
    echo "== stackguard: a block buffer on the kernel stack stops the link =="
    local held_frames n said reach_now over held L

    rm -f build/.stack_usage.ok
    build
    held_frames=$(grep -c '^caveat held: stack src/' "$SCRATCH/build.log")
    said=$(grep -E '^\[stack\] [0-9]+ frame\(s\) over 1024 B' "$SCRATCH/build.log")
    over=$(echo "$said" | sed -nE 's/^\[stack\] ([0-9]+) frame\(s\).*/\1/p')
    held=$(echo "$said" | sed -nE 's/.*, ([0-9]+) held;.*/\1/p')
    [ -n "$over" ] && [ "$over" = "$held" ] && [ "$held_frames" = "$held" ] \
        && [ "$held" = "$(grep -cE '^src/' "$STACKGUARD_HELD")" ]
    chk $? "stackguard: the clean tree holds every frame over the limit, and says each ($held_frames said; ${said:-no summary})"
    # Since 1.10 the reach is within its budget and no line holds it.
    reach_now=$(echo "$said" | sed -nE 's/.*; reach ([0-9]+) B against a budget of 4096 B.*/\1/p')
    [ -n "$reach_now" ] && [ "$reach_now" -le 4096 ] \
        && ! awk '$1 == "reach"' "$STACKGUARD_HELD" | grep -q . \
        && ! grep -q '^caveat held: stack reach' "$SCRATCH/build.log"
    chk $? "stackguard: the reach is ${reach_now:-?} bytes, within its budget of 4096, and no caveat holds it"
    # A recursion is named when the heaviest way to the medium runs through
    # one — the chain then carries it as {its frames} — and never otherwise.
    if grep -E '^\[stack\]   ' "$SCRATCH/build.log" | grep -q '{'; then
        grep -qE '^\[stack\] recursion on the way to the medium, each frame counted once: ' "$SCRATCH/build.log"
        chk $? "stackguard: the recursion on the way to the medium is named"
    else
        ! grep -qE '^\[stack\] recursion on the way to the medium' "$SCRATCH/build.log"
        chk $? "stackguard: the heaviest way to the medium runs through no recursion, and none is named"
    fi
    [ -f build/.stack_usage.ok ]
    chk $? "stackguard: the clean tree is judged in full through the harness too, and stamped"

    # M1 — a new offender.
    stackguard_block_on
    stackguard_red block "^\[stack\] FAIL new offender: src/kernel/tagfs/tagfs.c:tagfs_close stands on [0-9]+ bytes, over 1024"
    chk $? "stackguard: a 4 KiB block in tagfs_close stops the link — $(stackguard_said block)"
    n=$(sed -nE 's/.*new offender: src\/kernel\/tagfs\/tagfs\.c:tagfs_close stands on ([0-9]+) bytes.*/\1/p' \
        "$SCRATCH/build.stackguard.block.log" | head -1)

    # M2 — the same block, its line 512 bytes short of what M1 measured: a
    # held frame that stands over its line grew, by exactly those bytes.
    held=$(( ${n:-512} - 512 ))
    stackguard_hold_on "$held"
    stackguard_red grow "^\[stack\] FAIL src/kernel/tagfs/tagfs.c:tagfs_close grew to ${n:-?} bytes while its caveat holds it at $held "
    chk $? "stackguard: tagfs_close held at $held, 512 bytes under the ${n:-?} it stands on, stops the link — $(stackguard_said grow)"

    # The same mutant through the harness: said, not judged, held, linked, no
    # stamp.
    L="$SCRATCH/build.stackguard.harness.log"
    make >"$L" 2>&1
    n=$?
    [ $n -eq 0 ] && grep -q "Nameplate pass 1" "$L" && [ ! -f build/.stack_usage.ok ] \
        && grep -qE "^\[stack\] under the harness, said and not judged: src/kernel/tagfs/tagfs.c:tagfs_close grew to [0-9]+ bytes while its caveat holds it at $held " "$L" \
        && grep -qE "^caveat held: stack src/kernel/tagfs/tagfs.c:tagfs_close [0-9]+ B — logcheck mutation: a block held on the stack — discharged by step 9.99$" "$L"
    chk $? "stackguard: under HARNESS=on the same mutant links, the growth is said and not judged, the frame is held, and no stamp is left (make $n)"
    restore src/kernel/tagfs/tagfs.c

    # M3 — the line kept, the block back off the stack: the caveat must be
    # lifted.
    stackguard_red lift "^\[stack\] FAIL caveat lifted early: stack src/kernel/tagfs/tagfs.c:tagfs_close — logcheck mutation: a block held on the stack — is [0-9]+ bytes, no longer over 1024: the test does not test what it claims, or step 9.99 already landed — lift the caveat$"
    chk $? "stackguard: tagfs_close held with no block on its stack is a caveat lifted early — $(stackguard_said lift)"
    restore "$STACKGUARD_HELD"

    # M4 — a reach line where the reach is within its budget is a caveat
    # lifted early, and it is the only thing said to be wrong.
    stackguard_reach_on "${reach_now:-4096}"
    stackguard_red reach "^\[stack\] FAIL caveat lifted early: stack reach — logcheck mutation: a reach held that is within its budget — is [0-9]+ bytes, within 4096: "
    [ "$(grep -c '^\[stack\] FAIL' "$SCRATCH/build.stackguard.reach.log")" = 1 ]
    chk $? "stackguard: a reach line held within the budget stops the link as lifted early, and nothing else fails — $(stackguard_said reach)"
    restore "$STACKGUARD_HELD"

    # M5 — no reach line: the budget itself is the rule. Lowered below the
    # reach, it stops the link.
    stackguard_budget_on $(( ${reach_now:-4096} - 1 ))
    stackguard_red noreach "^\[stack\] FAIL reach [0-9]+ bytes, over its budget of $(( ${reach_now:-4096} - 1 )): "
    chk $? "stackguard: with no reach line, a reach of ${reach_now:-?} bytes against a budget one byte short stops the link — $(stackguard_said noreach)"
    restore tools/check_stack_usage.py

    build   # every put-back discards the image; leave one built from clean sources
    [ -f build/.stack_usage.ok ]
    chk $? "stackguard: the tree put back is judged in full again, and stamped"
}

# ===========================================================================
# lockrankmut — the LockRank judge, and the rule that no TagFS lock goes
# unranked, each shown what it must refuse. No machine.
# ===========================================================================
#
# lockrank_judge (tools/stand_caveat.sh) holds every place the kernel names
# under its row's step, and is only worth believing if it refuses what it
# must: a place no row holds, a place whose row lost its step, a place one boot
# named twice, a line that is no place at all (the self-deadlock, the table
# full). And it must hold what it must hold — once, however many boots named
# the place — with the self-tests' own places held and not counted as caveats.
# Each shown on a log written for it, every verdict counted where it must be,
# and lockrank_unheld (what the matrix counts) agreeing with it.
#
# The kernel's types keep every TagFS lock a TagFSLock: spin_lock on one does
# not compile, so no lock can be taken without a rank. The host holds the same
# rule over the text — no bare spinlock_t, and no bare spin_lock, in TagFS or
# the storage deck outside LockRank's own module — and a bare spinlock_t put
# into tagfs.c must turn it.

# No place LockRank names has been held under a step since step 1.5 took the
# last TagFS lock held across the medium away (the file's writer lock, now its
# Helm), so the judge is shown what it holds on the table as it stood before
# that step: the held place's row given its step back.
LOCKRANK_HELD_LINE='[LockRank] writer is held across a medium write: taken in tagfs_write_inside, the write asked for by tagfs_alloc_blocks+0x1f5'
LOCKRANK_HELD_KEY='writer@tagfs_write_inside:write'
LOCKRANK_HELD_STEP='1.5'

# lockrankmut_log NAME LINE... — a boot's log holding exactly LINE..., the way
# the serial line ends them; prints its path.
lockrankmut_log() {
    local f="$SCRATCH/lockrankmut.$1.log"; shift
    printf '%s\r\n' "$@" > "$f"
    echo "$f"
}

# lockrankmut_said LOGS — what lockrank_judge says of LOGS on the table
# tools/stand_caveat.sh holds now, then "counted PASS FAIL CAVEAT", in a
# subshell whose counters start at nothing.
lockrankmut_said() {
    (. tools/stand_caveat.sh
     PASS=0; FAIL=0; CAVEAT=0; HARNESS=0
     lockrank_judge "$1" "lockrankmut"
     echo "counted $PASS $FAIL $CAVEAT")
}

# lockrankmut_step_given — the held place's row given its step back, in the
# file (restore puts it back).
lockrankmut_step_given() {
    mutate tools/stand_caveat.sh
    python3 - "$LOCKRANK_HELD_KEY" "$LOCKRANK_HELD_STEP" <<'EOF'
import re, sys
p = "tools/stand_caveat.sh"
s = open(p).read()
key = re.escape("lockrank:" + sys.argv[1])
s, n = re.subn(r'(' + key + r'[|)][^"]*?\n\s*echo ")(\|[^\n]*;;)',
               lambda m: m.group(1) + sys.argv[2] + m.group(2) + "   # logcheck mutation: the step given back",
               s, count=1, flags=re.S)
assert n == 1, "lockrankmut: the row of " + sys.argv[1] + " was not found"
open(p, "w").write(s)
EOF
    mut_done tools/stand_caveat.sh "logcheck mutation" 1
}

# lockrankmut_saw WHAT LOGS COUNTED TEXT — the judge counted COUNTED ("P F C")
# and said TEXT (fixed string) of LOGS.
lockrankmut_saw() {
    local said
    said=$(lockrankmut_said "$2")
    printf '%s\n' "$said" | grep -qx "counted $3" && printf '%s\n' "$said" | grep -qF -- "$4"
    chk $? "lockrankmut: $1 ($(printf '%s\n' "$said" | tail -1))"
}

# lockrank_bare — every line of TagFS or the storage deck that declares or
# takes a bare spinlock, outside LockRank's own module. A line whose code is
# only a comment is not code.
lockrank_bare() {
    grep -rnE '\b(spinlock_t|spin_lock|spin_unlock|spin_trylock|spinlock_init|spin_force_release)\b' \
        src/kernel/tagfs src/kernel/core/decks/storage --include='*.c' --include='*.h' |
        grep -v '^src/kernel/tagfs/lock_rank/' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|/\*|\*)'
}

run_lockrankmut() {
    echo "== lockrankmut: the LockRank judge, shown what it must refuse and what it must hold =="
    local held held2 unknown twice full deadlock proof said n bare step defect
    step=$LOCKRANK_HELD_STEP
    defect=$(caveat_defect "lockrank:$LOCKRANK_HELD_KEY")
    held=$(lockrankmut_log held "$LOCKRANK_HELD_LINE")
    held2=$(lockrankmut_log held2 "$LOCKRANK_HELD_LINE")
    unknown=$(lockrankmut_log unknown \
        '[LockRank] registry is held across a medium write: taken in lockrankmut_nowhere, the write asked for by write_block+0x3f')
    twice=$(lockrankmut_log twice "$LOCKRANK_HELD_LINE" \
        '[LockRank] writer is held across a medium write: taken in tagfs_write_inside, the write asked for by write_block+0x3f')
    full=$(lockrankmut_log full '[LockRank] 256 places have been named; later ones are counted, not named')
    deadlock=$(lockrankmut_log deadlock \
        '[LockRank] registry taken in tag_registry_lookup while this core already holds it, taken in tag_registry_flush — this core now waits for itself')
    proof=$(lockrankmut_log proof \
        '[LockRank] proof is held across a medium read: taken in TestStandLockRankNamesTheMedium, the read asked for by TestStandLockRankNamesTheMedium+0x12a')

    # On the table as it stood before the held place's step landed.
    lockrankmut_step_given
    lockrankmut_saw "a place its row holds is a caveat held under step $step" "$held" "1 0 1" \
        "caveat held: lockrank:$LOCKRANK_HELD_KEY — $defect — discharged by step $step (lockrankmut, named in 1 of 1 boot(s): $LOCKRANK_HELD_LINE)"
    lockrankmut_saw "and one caveat, however many boots named it" "$held $held2" "1 0 1" \
        "named in 2 of 2 boot(s)"
    lockrankmut_saw "a place no row holds is a FAIL" "$unknown" "0 1 0" \
        "LockRank names a place no row holds: registry@lockrankmut_nowhere:write"
    lockrankmut_saw "a place named twice in one boot is a FAIL" "$twice" "0 1 1" \
        "names one place twice in one boot"
    lockrankmut_saw "the table full is a FAIL" "$full" "0 1 0" \
        "says what no place of a row can be: [LockRank] 256 places have been named"
    lockrankmut_saw "a core that waits for itself is a FAIL" "$deadlock" "0 1 0" \
        "says what no place of a row can be: [LockRank] registry taken in tag_registry_lookup while this core already holds it"
    lockrankmut_saw "the self-tests' own place is held, and is no caveat" "$proof" "1 0 0" \
        "(0 under a step, 1 the self-tests' own)"

    # What the matrix counts is what the judge refuses.
    n=$(. tools/stand_caveat.sh
        for f in "$held" "$unknown" "$twice" "$full" "$proof"; do
            printf '%s ' "$(lockrank_unheld "$f" | wc -l | tr -d ' ')"
        done)
    restore tools/stand_caveat.sh
    [ "$n" = "0 1 1 1 0 " ]
    chk $? "lockrankmut: the matrix counts nothing held and every refusal (held, unknown, twice, full, proof: $n)"

    # And on the table as it stands, the step landed and the row lost it: the
    # place seen again is the defect back.
    said=$(lockrankmut_said "$held"; echo "unheld $(lockrank_unheld "$held" | wc -l | tr -d ' ')")
    printf '%s\n' "$said" | grep -qx "counted 0 1 0" &&
        printf '%s\n' "$said" | grep -qF "lockrank:$LOCKRANK_HELD_KEY failed: the defect it stands for is back — $defect"
    chk $? "lockrankmut: a place whose row lost its step is the defect back, and a FAIL ($(printf '%s\n' "$said" | grep '^counted'))"
    printf '%s\n' "$said" | grep -qx "unheld 1"
    chk $? "lockrankmut: and the matrix counts it ($(printf '%s\n' "$said" | grep '^unheld'))"

    # No lock in TagFS without a rank.
    bare=$(lockrank_bare)
    [ -z "$bare" ]
    chk $? "lockrankmut: TagFS and the storage deck hold no bare spinlock outside LockRank (${bare:-none})"
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
a = "static WellKnownTags g_well_known = { TAGFS_INVALID_TAG_ID, TAGFS_INVALID_TAG_ID };\n"
assert s.count(a) == 1, "lockrankmut: the bare-lock anchor is missing"
open(p, "w").write(s.replace(a, a + "static spinlock_t g_lockrankmut_bare;   /* logcheck mutation: a TagFS lock with no rank */\n", 1))
EOF
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation" 1
    bare=$(lockrank_bare)
    restore src/kernel/tagfs/tagfs.c
    printf '%s\n' "$bare" | grep -qF "src/kernel/tagfs/tagfs.c:" && [ "$(printf '%s\n' "$bare" | grep -c .)" = 1 ]
    chk $? "lockrankmut: a bare spinlock_t put into tagfs.c is found ($(printf '%s\n' "$bare" | cut -c1-120))"
}

# ===========================================================================
# abba — the tag registry and the file table grown on sixteen cores while
# three other cabins rename, create and tag: the machine must come back
# ===========================================================================
#
# MP-D4, the two ABBAs. A chain that grew took the volume's state lock under
# its own — the registry's in tag_registry_flush, the file table's in
# file_table_flush, both through tagfs_alloc_blocks — while a rename, a create,
# a delete, a tag and an untag take those locks under state. Every TagFS lock
# keeps its core's interrupts off, so two K-Cores that met there the two ways
# round never moved again. Step 1.1 took the block out from under both locks
# (a Spare stamped with none held, scribe.h), and LockRank's order line — the
# red that does not depend on timing — is now demanded NOT to come. This gate
# is the guard beside it (the owner, 2026-09-28): `quilltest abba` on UEFI
# STRICT with sixteen cores — four K-Cores — grows both chains while its three
# workers call the other way round, each in a cabin of its own, and the run
# must reach the case's verdict. On a tree where the crossing strikes, it does
# not: that is this gate going red on evidence, not on a flake.
#
# ‼ A STALL IS MEASURED AGAINST THE RUN'S OWN PACE, AND THE BOUND IS THE
# HARNESS'S, NEVER THE KERNEL'S. The driver prints a progress line every 25 new
# tags, each saying when by the machine's own clock. Once two have come, a
# stall is no new progress line for ABBA_STALL_TIMES × (the slowest interval
# between two so far + 1 s, the resolution the log is looked at with) — the
# interval the larger of the machine's clock and the gaps the host saw the
# lines arrive in, since a console may hand over a burst at once — however
# much else the log says meanwhile, since a kernel that is wedged on two cores
# may still print on a third. Before two lines there is no pace to hold the
# machine to, and the wait is the stand's own: the log quiet for
# WAIT_QUIET_S, or the runaway guard. The verdict says the bound and the
# numbers it came from.
#
# ‼ THE ORDER LINES ARE DEMANDED BY THEIR ROWS, NOT BY THIS BLOCK. Each of the
# two places in ABBA_ORDER_KEYS is judged by its row in stand_caveat.sh: while
# the row carries a step the place must be named in this boot — a chain grew
# under its own lock — and a place not named is the workload failing to ask
# its question, or the step already landed; once the step is lifted the place
# must not be named at all. The commit that fixes a chain lifts this demand by
# deleting the step in the row, as it lifts every caveat, and nothing here
# changes.
#
# Aimed where the case starts: enough new tags to fill the registry's last
# block and reach ABBA_EDGES blocks past it, and the table's edge — its rows,
# 510 to a block. Built VOLUME_TESTS=on, because abba spends tags and file ids
# for good, so the startup tests run first — and they grow both chains, the
# table by whole blocks, past any edge read off the fresh image. So the kernel
# says, after its last test, how long each chain is on the medium and how full
# the registry's last block is ("[TESTS] the tests leave ..."), and the aim is
# taken from that line with volume-read's figures of the format. After the
# verdict the machine halts through `bye` and volume-read reads the volume
# again: against that same line, the registry must have grown by at least the
# blocks any packing of the case's own records takes past the room its last
# block had, and the table by a block — growth nothing but the case made.
#
# ABBA_STAND is the machine: the gate is sixteen cores, and a look at the same
# workload on another machine names it here, as QUILL_STAND does for quill
# (one K-Core never crosses, so on a single core the run must always end).
# ---------------------------------------------------------------------------

ABBA_STAND=${ABBA_STAND:-"UEFI=on STRICT=on CORES=16 MEM=8G"}
ABBA_NAME=_qa
ABBA_STEM=31          # quilltest_locks.c: every new tag's name is 31 bytes long
ABBA_EDGES=3          # registry blocks the new tags reach, the first the last one's room
ABBA_STALL_TIMES=5
ABBA_ORDER_KEYS="registry@tag_registry_flush/state@tagfs_alloc_blocks_inside table@file_table_flush/state@tagfs_alloc_blocks_inside"
ABBA_PROGRESS='^\[QUILL\] abba: (workers rename pid [0-9]+|[0-9]+ of [0-9]+ new tags made).* at [0-9]+ ms[;,]'
ABBA_VERDICT='^\[QUILL\] abba: (PASS|FAIL) — '

# abba_aim JSON TABLE REGISTRY TAIL — "TAGS EDGE REGISTRY TABLE NEED": the
# format's figures off volume-read's check of the image (a registry record's
# head, a block's data, a table block's rows), the chains as the startup tests
# left them (TABLE and REGISTRY blocks, TAIL bytes in the registry's last),
# and from them the new tags that fill that last block and reach ABBA_EDGES
# blocks past it, the table's edge, both chains' lengths, and the least number
# of blocks any packing of those tags' records takes past the room the last
# block has.
abba_aim() {
    python3 - "$1" "$ABBA_STEM" "$ABBA_EDGES" "$2" "$3" "$4" <<'ABBA_AIM_EOF'
import json, sys
d = json.load(open(sys.argv[1]))
ch, stem, edges = d["chains"], int(sys.argv[2]), int(sys.argv[3])
table, registry, tail = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
record = ch["registry_record_header"] + stem
room_bytes = ch["registry_block_data"] - tail
room, per_block = room_bytes // record, ch["registry_block_data"] // record
tags = room + (edges - 1) * per_block + 1
need = -(-(tags * record - room_bytes) // ch["registry_block_data"])
print(tags, ch["table_rows_per_block"] * table, registry, table, need)
ABBA_AIM_EOF
}

# The kernel's own word on the chains once its startup tests are done
# (stand_chain_tests.c, StandSayChainsLeft): "TABLE REGISTRY TAIL".
ABBA_LEFT='^\[TESTS\] the tests leave the file table ([0-9]+) block\(s\) long on the medium and the registry ([0-9]+), its last block holding ([0-9]+) byte\(s\)'
abba_left() {
    tr -d '\r' < "$1" | grep -aE "$ABBA_LEFT" | tail -1 | sed -E "s/$ABBA_LEFT.*/\1 \2 \3/"
}

# abba_chains JSON — "REGISTRY TABLE": both chains' lengths after the run.
abba_chains() {
    python3 -c 'import json, sys; c = json.load(open(sys.argv[1]))["chains"]; print(len(c["registry"]), len(c["table"]))' "$1"
}

# abba_lines MARK — the case's own lines since line MARK of the serial log.
abba_lines() {
    tail -n +$(($1 + 1)) build/serial.log | tr -d '\r' | grep -a '^\[QUILL\] abba: '
}

# abba_pace MARK — the slowest interval between two consecutive progress
# lines since line MARK, in ms by the machine's own clock (each line says when
# it was printed); 0 while fewer than two have come.
abba_pace() {
    abba_lines "$1" | grep -E "$ABBA_PROGRESS" | sed -nE 's/.* at ([0-9]+) ms[;,].*/\1/p' |
        awk 'NR > 1 && $1 - p > m { m = $1 - p } { p = $1 } END { print m + 0 }'
}

# abba_watch MARK — follow quilltest abba from line MARK until its verdict
# (answers 0) or a stall (answers 1). The pace is the slowest interval between
# two progress lines, taken the larger of two ways: by the machine's clock,
# which the lines carry, and by the gaps in which the host saw them arrive —
# a console may hand over a burst of lines at once, and a host may fall
# behind. Leaves ABBA_SEEN (progress lines), ABBA_PACE_MS (the machine's
# slowest interval), ABBA_SLOWEST (the pace, whole s), ABBA_BOUND (the stall
# bound it came to, s; 0 while fewer than two lines had come), ABBA_SPENT (s
# since the echo) and ABBA_WHY.
abba_watch() {
    local mark=$1 t0 now n at size last="" still=0 ms
    t0=$(date +%s); at=$t0
    ABBA_SEEN=0; ABBA_PACE_MS=0; ABBA_SLOWEST=0; ABBA_BOUND=0; ABBA_SPENT=0
    while :; do
        now=$(date +%s)
        ABBA_SPENT=$((now - t0))
        n=$(abba_lines "$mark" | grep -cE "$ABBA_PROGRESS")
        if [ "$n" -gt "$ABBA_SEEN" ]; then
            [ "$ABBA_SEEN" -gt 0 ] && [ $((now - at)) -gt "$ABBA_SLOWEST" ] && ABBA_SLOWEST=$((now - at))
            ABBA_PACE_MS=$(abba_pace "$mark")
            ms=$(((ABBA_PACE_MS + 999) / 1000))
            [ "$ms" -gt "$ABBA_SLOWEST" ] && ABBA_SLOWEST=$ms
            [ "$n" -ge 2 ] && ABBA_BOUND=$((ABBA_STALL_TIMES * (ABBA_SLOWEST + 1)))
            ABBA_SEEN=$n; at=$now
        fi
        if abba_lines "$mark" | grep -qE "$ABBA_VERDICT"; then
            ABBA_WHY=verdict; return 0
        fi
        if [ "$ABBA_BOUND" -gt 0 ] && [ $((now - at)) -ge "$ABBA_BOUND" ]; then
            ABBA_WHY="no progress line for $((now - at)) s, past the bound of $ABBA_BOUND s — $ABBA_STALL_TIMES × (the slowest interval between two, $ABBA_SLOWEST s, + 1), the machine's own clock saying $ABBA_PACE_MS ms"
            return 1
        elif [ "$ABBA_BOUND" = 0 ]; then
            size=$(wc -c < build/serial.log | tr -d ' ')
            if [ "$size" = "$last" ]; then still=$((still + 1)); else still=0; last=$size; fi
            if [ "$still" -ge "$WAIT_QUIET_S" ]; then
                ABBA_WHY="the log went quiet for $WAIT_QUIET_S s with $ABBA_SEEN progress line(s), fewer than the two a pace needs"
                return 1
            fi
        fi
        if [ "$ABBA_SPENT" -ge "$WAIT_RUNAWAY_S" ]; then
            ABBA_WHY="no verdict in the runaway guard's $WAIT_RUNAWAY_S s, the progress lines still coming"
            return 1
        fi
        sleep 1
    done
}

# abba_session TAG — a fresh image built VOLUME_TESTS=on, aimed, booted on
# ABBA_STAND, `quilltest abba` typed and watched, and the machine halted
# through bye once the case answered. Leaves $SCRATCH/serial.abba.TAG.log, the
# checks abba.TAG.before.json and (after an orderly halt) abba.TAG.after.json,
# the aim (ABBA_TAGS ABBA_EDGE ABBA_R0 ABBA_T0 ABBA_NEED), ABBA_HALTED and
# abba_watch's figures. Answers 1, having said why, when there is no run to
# judge.
abba_session() {
    local tag=$1 mark aim
    ABBA_HALTED=0; ABBA_WHY="not watched"; ABBA_SEEN=0; ABBA_SLOWEST=0; ABBA_BOUND=0; ABBA_SPENT=0
    rm -f "$SCRATCH/serial.abba.$tag.log" "$SCRATCH/abba.$tag.before.json" "$SCRATCH/abba.$tag.after.json"
    make run-stop >/dev/null 2>&1
    rm -f build/boxos.img
    # shellcheck disable=SC2086
    if ! build_with_key VOLUME_TESTS=on "$(strict_key $ABBA_STAND)"; then
        echo "BUILD FAILED (VOLUME_TESTS=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "abba: $tag — VOLUME_TESTS=on does not build"
        return 1
    fi
    python3 tools/volume-read.py build/boxos.img check --json >"$SCRATCH/abba.$tag.before.json" 2>/dev/null
    if ! python3 -c 'import json, sys; json.load(open(sys.argv[1]))["chains"]' \
            "$SCRATCH/abba.$tag.before.json" >/dev/null 2>&1; then
        harness "abba: $tag — volume-read's figures of the fresh image would not read"
        return 1
    fi
    # shellcheck disable=SC2086
    if ! make run-bg VOLUME_TESTS=on $ABBA_STAND >"$SCRATCH/runbg.abba.$tag.log" 2>&1 || [ ! -S build/qemu.mon ]; then
        harness "abba: $tag — no machine was started: $(tail -1 "$SCRATCH/runbg.abba.$tag.log" | tr -d '\r')"
        make run-stop >/dev/null 2>&1
        return 1
    fi
    if ! wait_for_line -E build/serial.log 'TagFS: [0-9]+ run, |KERNEL PANIC' "$WAIT_QUIET_S" ||
       ! grep -aqE 'TagFS: [0-9]+ run, ' build/serial.log || ! wait_for_prompt; then
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$SCRATCH/serial.abba.$tag.log"
        bad "abba: $tag — the machine never came to its shell after the startup tests ($(grep -aE 'KERNEL PANIC|TagFS: [0-9]+ run, ' "$SCRATCH/serial.abba.$tag.log" | tr -d '\r' | tail -1))"
        return 1
    fi
    local left
    left=$(abba_left build/serial.log)
    if [ -z "$left" ] || ! aim=$(abba_aim "$SCRATCH/abba.$tag.before.json" $left 2>&1); then
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$SCRATCH/serial.abba.$tag.log"
        harness "abba: $tag — the kernel did not say where its startup tests left the chains, so there is nothing to aim from${aim:+: $(echo "$aim" | tail -1)}"
        return 1
    fi
    read -r ABBA_TAGS ABBA_EDGE ABBA_R0 ABBA_T0 ABBA_NEED <<ABBA_AIM
$aim
ABBA_AIM
    echo "   aim: the startup tests left the registry $ABBA_R0 block(s) and the table $ABBA_T0; $ABBA_TAGS new tags reach $ABBA_EDGES registry blocks past its last one's room, and the table's edge is fid $ABBA_EDGE"
    mark=$(wc -l < build/serial.log)
    if type_line "quilltest abba $ABBA_NAME $ABBA_TAGS $ABBA_EDGE"; then
        abba_watch "$mark"
    else
        ABBA_WHY="the machine never took the command"
    fi
    if [ "$ABBA_WHY" = verdict ] &&
       wait_until "$WAIT_QUIET_S" "the shell's prompt after quilltest abba" \
           '[ "$(tail -n +$((mark + 1)) build/serial.log | tr -d "\r" | grep -cE "^(\[[^]]*\] )?~ ")" -ge 2 ]' &&
       type_line bye && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" $((mark + 1)) &&
       grep -aq '^\[HALT\] Storage sync complete' build/serial.log; then
        ABBA_HALTED=1
    fi
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.abba.$tag.log"
    if [ "$ABBA_HALTED" = 1 ]; then
        python3 tools/volume-read.py build/boxos.img check --json >"$SCRATCH/abba.$tag.after.json" 2>/dev/null
    fi
    return 0
}

# abba_order LOG WHAT ENDED — each place of ABBA_ORDER_KEYS as its row
# demands: named in this boot while the row holds it under a step, not named
# once the step is lifted. The row is read where this is asked, so a caller
# that sources a changed stand_caveat.sh gets the demand that row makes. A
# held place a run that did not reach its end (ENDED 0) never named is said
# and not counted: that run never came to the question, and its stall is
# the finding.
abba_order() {
    local log=$1 what=$2 ended=$3 key step line
    for key in $ABBA_ORDER_KEYS; do
        step=$(caveat_step "lockrank:$key")
        line=$(lockrank_rows "$log" | awk -F'|' -v k="$key" '$2 == k { print $3; exit }')
        if [ -n "$step" ] && [ -n "$line" ]; then
            ok "abba: $what — the order place $key is held under step $step, and this boot named it: $line"
        elif [ -n "$step" ] && [ "$ended" = 0 ]; then
            echo "  (abba: $what — the order place $key, held under step $step, was not named before the run stopped: not asked, and not counted)"
        elif [ -n "$step" ]; then
            bad "abba: $what — the order place $key is held under step $step and this boot did not name it: the workload did not grow that chain under its lock, or step $step already landed — lift the caveat"
        elif [ -z "$line" ]; then
            ok "abba: $what — the order place $key, its step lifted, is not named"
        else
            bad "abba: $what — the order place $key, its step lifted, is named again: the defect it stands for is back — $(caveat_defect "lockrank:$key") ($line)"
        fi
    done
}

# abba_judge TAG WHAT — the session's run, the case's verdict, the chains and
# the order demand.
abba_judge() {
    local tag=$1 what=$2 L="$SCRATCH/serial.abba.$1.log" said fault verdict made r1 t1
    said=$(stand_summary "$L")
    echo "$said" | grep -qE "^TagFS: $STAND_RUN run, [0-9]+ passed, 0 failed"
    chk $? "abba: $what — the boot's $STAND_RUN startup tests ran before the case and none failed (${said:-no summary})"
    fault=$(grep -aE 'KERNEL PANIC|\[EXCEPTION\]' "$L" | tr -d '\r' | head -1 | cut -c1-160)
    [ -z "$fault" ]
    chk $? "abba: $what — no panic, no exception${fault:+: $fault}"
    if [ "$ABBA_WHY" = verdict ] && [ "$ABBA_BOUND" -gt 0 ]; then
        ok "abba: $what — the run reached its verdict in $ABBA_SPENT s after $ABBA_SEEN progress lines; the slowest interval between two was $ABBA_PACE_MS ms by the machine's clock and $ABBA_SLOWEST s the larger way, so a stall was no line for $ABBA_BOUND s ($ABBA_STALL_TIMES × ($ABBA_SLOWEST + 1))"
    elif [ "$ABBA_WHY" = verdict ]; then
        ok "abba: $what — the run reached its verdict in $ABBA_SPENT s after $ABBA_SEEN progress line(s), too few for a pace, so the wait was the stand's quiet rule ($WAIT_QUIET_S s)"
    else
        bad "abba: $what — the run did not reach its verdict: $ABBA_WHY; $ABBA_SEEN progress lines in $ABBA_SPENT s, the last: $(tr -d '\r' < "$L" | grep -aE "$ABBA_PROGRESS" | tail -1)"
    fi
    verdict=$(tr -d '\r' < "$L" | grep -aE "$ABBA_VERDICT" | tail -1)
    case "$verdict" in
        *"abba: PASS"*) ok "abba: $what — ${verdict#\[QUILL\] abba: }" ;;
        ?*)             bad "abba: $what — ${verdict#\[QUILL\] abba: }" ;;
    esac
    made=$(printf '%s\n' "$verdict" | sed -nE 's/.*PASS — made ([0-9]+) new tags.*/\1/p')
    if [ -n "$made" ]; then
        if [ "$ABBA_HALTED" = 1 ] && read -r r1 t1 <<ABBA_AFTER
$(abba_chains "$SCRATCH/abba.$tag.after.json" 2>/dev/null)
ABBA_AFTER
        then
            [ $((r1 - ABBA_R0)) -ge "$ABBA_NEED" ] && [ "$t1" -gt "$ABBA_T0" ]
            chk $? "abba: $what — from where the startup tests left them, the registry went from $ABBA_R0 to $r1 blocks, $ABBA_NEED at least for the case's $made new tags past the room its last block had, and the file table from $ABBA_T0 to $t1"
        else
            bad "abba: $what — the chains were not read after the run: the machine $([ "$ABBA_HALTED" = 1 ] && echo "halted and volume-read could not read the volume" || echo "did not halt through bye ($(grep -aE "$STAND_HALT_ENDS" "$L" | tr -d '\r' | tail -1 | cut -c1-120))")"
        fi
    fi
    abba_order "$L" "$what" "$([ "$ABBA_WHY" = verdict ] && echo 1 || echo 0)"
}

run_abba() {
    echo "== abba: the tag registry and the file table grown on $ABBA_STAND while three cabins rename, create and tag =="
    if abba_session abba; then
        abba_judge abba "the guard"
        lockrank_judge "$SCRATCH/serial.abba.abba.log" "abba"
    fi
    make run-stop >/dev/null 2>&1
    rm -f build/boxos.img
    buildkey_restore
}

# ---------------------------------------------------------------------------
# abbamut — a block taken under the registry's lock, and the demand must turn
# ---------------------------------------------------------------------------
#
# The order demand is only worth its PASS if it can see the place come back.
# So the registry takes a block for nothing — taken and given straight back —
# under its own lock at the start of every flush, the allocation the fix of
# its chain takes out from under that lock, put back where it cannot miss:
# the first registry flush of the boot names the place, grown chain or not.
# The session is abba's own, typed and watched as the gate does it, and its
# guard is not judged here — a tree that takes the state lock under the
# registry at every flush is expected to stall. Then the row is read as it
# will stand the day that fix lands, its step lifted, and abba's demand must
# answer that the defect it stands for is back.
abbamut_kernel_on() {
    mutate src/kernel/tagfs/tag_registry/tag_registry.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/tag_registry/tag_registry.c"
s = open(p).read()
a = "    TagFSLockTake(&reg->lock);\n    bool owed = reg->written_next < reg->next_id || reg->tail_owed;\n"
assert s.count(a) == 1, "abbamut registry-flush anchor missing"
s = s.replace(a, "    TagFSLockTake(&reg->lock);\n"
                 "    uint64_t logcheck_taken;   /* logcheck mutation: a block taken under the registry's lock at every flush */\n"
                 "    if (tagfs_alloc_blocks(1, &logcheck_taken) == 0)\n"
                 "        tagfs_free_blocks(logcheck_taken, 1);\n"
                 "    bool owed = reg->written_next < reg->next_id || reg->tail_owed;\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/tagfs/tag_registry/tag_registry.c "logcheck mutation" 1
}

# abbamut_lift_on KEY — KEY's row with its step lifted, as the commit that
# fixes its place will leave it; a row already lifted is left as it is.
abbamut_lift_on() {
    mutate tools/stand_caveat.sh
    python3 - "$1" <<'EOF'
import re, sys
p = "tools/stand_caveat.sh"
s = open(p).read()
key = re.escape("lockrank:" + sys.argv[1])
s, n = re.subn(r'(' + key + r'[|)][^"]*?\n\s*echo ")[0-9.]*[a-z]*(\|[^\n]*;;)',
               r'\1\2   # logcheck mutation: the step lifted', s, count=1, flags=re.S)
assert n == 1, "abbamut: the row of " + sys.argv[1] + " was not found"
open(p, "w").write(s)
EOF
    mut_done tools/stand_caveat.sh "logcheck mutation" 1
}

run_abbamut() {
    echo "== abbamut: a block taken under the registry's lock at every flush, the step lifted — abba's demand must turn =="
    local key=registry@tag_registry_flush/state@tagfs_alloc_blocks_inside L="$SCRATCH/serial.abba.abbamut.log"
    local rc line said defect at typed
    # Since step 1.5B a STRICT kernel stops the machine at every place LockRank
    # names (lock_rank.c, LockRankStops), and this mutant's place has to be
    # NAMED, at the first flush, for abba's judge to be shown it: the session
    # runs on the same stand with STRICT's kernel half off.
    local ABBA_STAND="${ABBA_STAND//STRICT=on/STRICT=off}"
    abbamut_kernel_on
    abba_session abbamut; rc=$?
    make run-stop >/dev/null 2>&1
    restore src/kernel/tagfs/tag_registry/tag_registry.c
    if [ "$rc" = 0 ]; then
        line=$(lockrank_rows "$L" | awk -F'|' -v k="$key" '$2 == k { print $3; exit }')
        at=$(tr -d '\r' < "$L" | grep -anF -- "$line" | head -1 | cut -d: -f1)
        typed=$(tr -d '\r' < "$L" | grep -an '^~ quilltest abba ' | head -1 | cut -d: -f1)
        [ -n "$line" ] && [ -n "$at" ] && [ -n "$typed" ] && [ "$at" -lt "$typed" ]
        chk $? "abbamut: the registry taking a block under its lock is named at the boot's first flush, before the case began to grow anything (log line ${at:-none}, the case's echo ${typed:-none}) — ${line:-not named}; the run: $ABBA_WHY"
        defect=$(caveat_defect "lockrank:$key")
        abbamut_lift_on "$key"
        said=$(. tools/stand_caveat.sh
               PASS=0; FAIL=0; CAVEAT=0; HARNESS=0
               abba_order "$L" "abbamut" "$([ "$ABBA_WHY" = verdict ] && echo 1 || echo 0)"
               echo "counted $PASS $FAIL")
        restore tools/stand_caveat.sh
        printf '%s\n' "$said" | grep -qF "abba: abbamut — the order place $key, its step lifted, is named again: the defect it stands for is back — $defect ($line)"
        chk $? "abbamut: with its step lifted, abba's demand for $key answers that the defect is back ($(printf '%s\n' "$said" | tail -1))"
    fi
    rm -f build/boxos.img
    buildkey_restore
}

# ---------------------------------------------------------------------------
# turns — the boundary is the pocket, and on one core Helm work is owed the
# quantum (RT-11)
# ---------------------------------------------------------------------------
#
# A K-Core served one program's pocket ring until it was empty, and popped its
# queue until IT was empty: a program that handed pockets in faster than they
# were served kept its K-Core for good, and everything below the pop — the
# batons, the services, the volume's Helm work and its Settle — waited behind
# it. quilltest flood is that program: the cheapest call there is, handed in
# without waiting for it. Measured on the tree before the fix: on UEFI 4c the
# flood held K1, the committer, and the anchor beside it waited 571 s with
# Nightwatch naming a pocket queued behind the flood; on three cores the one
# K-Core is also the drain core, and the write beside it was never served at
# all. And on one core the idle context — where a write's next window runs —
# was owed the quantum beside a program that never parks only while a Settle
# cycle was: 16 MiB that took 376 ms alone took 5387 ms beside quilltest hog,
# its windows waiting out the Settle window (X1-2).
#
# quilltest beside writes TURNS_MIB of a file of its own and anchors it, alone
# and then beside COUNT cabins of KIND, and says both times and how many calls
# the floods had answered meanwhile — a flood that got nothing fails the case.
# Three machines: UEFI 4c, one flood (AHCI, the committer a K-Core of its own,
# which the flood lands on); BIOS 3c, two floods (the legacy channel, whose
# windows are Helm work run at the service point of the one K-Core, and two
# programs whose pockets keep that K-Core's queue from ever being empty — a
# single flood empties it whenever its own notify outruns its push); and UEFI
# 1c beside a hog. On each the write beside them may take no more than
# TURNS_PACE times what it took alone, and a second: the machine's turns are
# shared, and a write that waits for its turns — the K-Core's queue to empty
# beside two floods, the Settle window beside a hog — takes many times that.
# The wait ends at the case's verdict, a Nightwatch proof that something owed
# went unserved, a panic, or a log gone quiet.
# ---------------------------------------------------------------------------

TURNS_MIB=16
TURNS_PACE=4
TURNS_VERDICT='^\[QUILL\] beside: (PASS|FAIL) — '
TURNS_STOP='^\[QUILL\] beside: (PASS|FAIL) — |^\[NIGHTWATCH\] VERDICT: .* a defect|KERNEL PANIC'

# turns_leg TAG KIND COUNT KEYS... — boot KEYS, type `quilltest beside KIND
# COUNT TURNS_MIB`, wait as above and keep the log as serial.turns.TAG.log. Leaves
# TURNS_LINE (the verdict, none when it never came), TURNS_FAULT (a panic, an
# exception or a Nightwatch defect after the command), TURNS_ALONE and
# TURNS_BESIDE (ms); answers 1, having said why, when there is no run to judge.
turns_leg() {
    local tag=$1 kind=$2 count=$3 mark L="$SCRATCH/serial.turns.$1.log"
    shift 3
    TURNS_LINE=""; TURNS_FAULT=""; TURNS_ALONE=""; TURNS_BESIDE=""
    rm -f "$L"
    make run-stop >/dev/null 2>&1
    if ! make run-bg "$@" >"$SCRATCH/runbg.turns.$tag.log" 2>&1 ||
       ! wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"; then
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$L" 2>/dev/null
        bad "turns: $tag ($*) never reached a shell"
        return 1
    fi
    wait_for_quiet "$BOOT_BURST_QUIET_S"
    mark=$(wc -l < build/serial.log)
    if type_line "quilltest beside $kind $count $TURNS_MIB"; then
        wait_for_line -E build/serial.log "$TURNS_STOP" "$WAIT_QUIET_S" $((mark + 1))
    fi
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$L"
    TURNS_LINE=$(tail -n +$((mark + 1)) "$L" | tr -d '\r' | grep -aE "$TURNS_VERDICT" | tail -1)
    TURNS_FAULT=$(tail -n +$((mark + 1)) "$L" | tr -d '\r' |
                  grep -aE 'KERNEL PANIC|\[EXCEPTION\]|^\[NIGHTWATCH\] VERDICT: .* a defect' |
                  head -1 | cut -c1-200)
    TURNS_ALONE=$(printf '%s\n' "$TURNS_LINE" | sed -nE 's/.* in ([0-9]+) ms alone and in [0-9]+ ms beside .*/\1/p')
    TURNS_BESIDE=$(printf '%s\n' "$TURNS_LINE" | sed -nE 's/.* in [0-9]+ ms alone and in ([0-9]+) ms beside .*/\1/p')
    return 0
}

# turns_paced — 0 when the write beside the cabins took no more than
# TURNS_PACE times what it took alone, and a second.
turns_paced() {
    [ -n "$TURNS_ALONE" ] && [ -n "$TURNS_BESIDE" ] &&
        [ "$TURNS_BESIDE" -le $((TURNS_PACE * TURNS_ALONE + 1000)) ]
}

# turns_judge TAG WHAT KIND — the leg just run.
turns_judge() {
    local what=$2 kind=$3
    [ -z "$TURNS_FAULT" ]
    chk $? "turns: $what — no panic, no exception, nothing owed proved unserved${TURNS_FAULT:+: $TURNS_FAULT}"
    case "$TURNS_LINE" in
        *"beside: PASS"*) ok "turns: $what — ${TURNS_LINE#\[QUILL\] beside: }" ;;
        ?*)               bad "turns: $what — ${TURNS_LINE#\[QUILL\] beside: }" ;;
        *)                bad "turns: $what — the write beside the ${kind}s was never answered (the last said: $(tr -d '\r' < "$SCRATCH/serial.turns.$1.log" | grep -a '^\[QUILL\]' | tail -1))" ;;
    esac
    if [ -n "$TURNS_LINE" ]; then
        turns_paced
        chk $? "turns: $what — beside the $kind cabin(s) the write took ${TURNS_BESIDE:-?} ms, no more than $TURNS_PACE × the ${TURNS_ALONE:-?} ms it took alone + 1000 ms: the machine's turns are shared with it"
    fi
}

run_turns() {
    echo "== turns: a write and its anchor beside floods of calls on UEFI 4c and BIOS 3c, beside a hog on UEFI 1c =="
    build STRICT=on
    turns_leg uefi4 flood 1 UEFI=on STRICT=on CORES=4 MEM=4G &&
        turns_judge uefi4 "UEFI 4c, the flood on the committer's K-Core" flood
    turns_leg uefi1 hog 1 UEFI=on STRICT=on CORES=1 MEM=2G &&
        turns_judge uefi1 "UEFI 1c, beside a hog" hog
    build
    turns_leg bios3 flood 2 CORES=3 MEM=4G &&
        turns_judge bios3 "BIOS 3c, the legacy channel and one K-Core" flood
    lockrank_judge "$SCRATCH/serial.turns.uefi4.log $SCRATCH/serial.turns.uefi1.log $SCRATCH/serial.turns.bios3.log" "turns"
}

# ---------------------------------------------------------------------------
# turnsmut — the old boundaries put back, and every leg must turn
# ---------------------------------------------------------------------------
#
# Three mutants, each the line one of turns' legs guards: the guide serving a
# program's ring until it is empty (F04-1) — the flood keeps the K-Core it
# lands on, on UEFI 4c the committer's and on BIOS 3c the only one; the K-Core
# popping its queue until it is empty (F04-2) — two floods, each pocket of one
# queued behind the other's, keep BIOS 3c's one K-Core in its pop; and idle
# owed the quantum on one core only for the Settle (X1-2) — beside the hog the
# write waits out the Settle window. Each leg must then fail: no verdict, or one
# slower than the pace allows.
turnsmut_guide_on() {
    mutate src/kernel/core/guide/guide.c
    python3 - <<'EOF'
p = "src/kernel/core/guide/guide.c"
s = open(p).read()
a = "    if (KPocketIsEmpty(proc))\n        return;\n    guide_process_pocket(proc);\n"
assert s.count(a) == 1, "turnsmut guide anchor missing"
s = s.replace(a, "    while (!KPocketIsEmpty(proc))   /* logcheck mutation: the ring served until it is empty */\n"
                 "        guide_process_pocket(proc);\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/core/guide/guide.c "logcheck mutation" 1
}

turnsmut_loop_on() {
    mutate src/kernel/arch/x86-64/amp/kcore.c
    python3 - <<'EOF'
p = "src/kernel/arch/x86-64/amp/kcore.c"
s = open(p).read()
a = "        struct process_t* proc = kcore_queue_pop(q);\n        if (proc)\n            kcore_process_entry(proc);\n"
assert s.count(a) == 1, "turnsmut loop anchor missing"
s = s.replace(a, "        struct process_t* proc;\n"
                 "        while ((proc = kcore_queue_pop(q)) != NULL)   /* logcheck mutation: the queue popped until it is empty */\n"
                 "            kcore_process_entry(proc);\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/arch/x86-64/amp/kcore.c "logcheck mutation" 1
}

turnsmut_idle_on() {
    mutate src/kernel/tagfs/settle/cadence.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/settle/cadence.c"
s = open(p).read()
a = "    return HelmReadyListed() || SettleIdleOwed();\n"
assert s.count(a) == 1, "turnsmut idle anchor missing"
s = s.replace(a, "    return SettleIdleOwed();   /* logcheck mutation: idle owed the quantum for the Settle alone */\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/tagfs/settle/cadence.c "logcheck mutation" 1
}

# turnsmut_turned TAG WHAT KIND — the leg just run must have failed.
turnsmut_turned() {
    local said
    if [ -z "$TURNS_LINE" ]; then
        said="no verdict${TURNS_FAULT:+, and $TURNS_FAULT}"
    elif ! turns_paced; then
        said="beside the $3 cabin(s) ${TURNS_BESIDE} ms against ${TURNS_ALONE} ms alone, past $TURNS_PACE × + 1000 ms"
    elif case "$TURNS_LINE" in *"beside: FAIL"*) true ;; *) false ;; esac; then
        said=${TURNS_LINE#\[QUILL\] beside: }
    fi
    [ -n "$said" ]
    chk $? "turnsmut: $2 — the leg turned: ${said:-it passed: ${TURNS_LINE#\[QUILL\] beside: }}"
}

run_turnsmut() {
    echo "== turnsmut: the ring served whole, the queue popped whole, idle owed for the Settle alone — every turns leg must fail =="
    turnsmut_guide_on; build STRICT=on
    turns_leg mutguide4 flood 1 UEFI=on STRICT=on CORES=4 MEM=4G &&
        turnsmut_turned mutguide4 "the ring served whole, UEFI 4c" flood
    build
    turns_leg mutguide3 flood 2 CORES=3 MEM=4G &&
        turnsmut_turned mutguide3 "the ring served whole, BIOS 3c" flood
    restore src/kernel/core/guide/guide.c
    turnsmut_loop_on; build
    turns_leg mutloop3 flood 2 CORES=3 MEM=4G &&
        turnsmut_turned mutloop3 "the queue popped whole, BIOS 3c" flood
    restore src/kernel/arch/x86-64/amp/kcore.c
    turnsmut_idle_on; build STRICT=on
    turns_leg mutidle1 hog 1 UEFI=on STRICT=on CORES=1 MEM=2G &&
        turnsmut_turned mutidle1 "idle owed for the Settle alone, UEFI 1c" hog
    restore src/kernel/tagfs/settle/cadence.c
    build   # leave the tree built from clean sources
}

# ===========================================================================
# cores — the volume asked from every core at once, on UEFI STRICT 16c
# ===========================================================================
#
# Four claims of the write core that carry into format v2, each asked by
# quilltest (quilltest_cores.c, quilltest_ground.c) from many cabins at once,
# each cabin's calls a K-Core's and each write going through the AHCI port, and
# each judged three ways: what the guest read back through the kernel, what
# the host reads off the medium once the machine is stopped (volume-read check
# — no block owned twice, none used and owned by nobody — and the files' own
# bytes), and the next boot's mount.
#
#   writers      16 cabins each write and rewrite a file of their own, 16
#                rounds, a new size each — some grown by the write, some cut by
#                a span first — every round read back; halted through bye; the
#                host gets each file and holds it against its stream; a reboot
#                reads them all back again (writersback). Every Ink of them
#                lays its record in the pool's tail: two laid at one place is a
#                file that comes back as an older round, or not at all.
#   appenders    two cabins append records to one file — STORAGE_WRITE_APPEND,
#                the asynchronous WriteJob — while a third grows it by zeros,
#                STORAGE_SPAN_GROW, the synchronous one, all through one Helm;
#                then two appenders and no spanner on a USB stick that is the
#                volume. Every append must lie where its answer put it, whole,
#                and nothing else be in the file but the spans' zeros: the size
#                the appends and the zeros, to the byte. The host parses the
#                file off the medium record by record as well.
#   readdelete   a file of 64 MiB read in four windows of 16 MiB while another
#                cabin deletes it in the second, runs four Settle cycles and
#                writes a filler of the same size — the reads held to
#                READDELETE_BPS by QEMU, so the window is still being read when
#                all of that is done. The window must deliver the file's own
#                bytes, every later one a refusal as a file not found; after a
#                halt and a reboot the volume is whole.
#   defragdelete 8 slots, each deleted and made again 10 times — written 4 KiB
#                at a time beside its pair, so it lies in several runs — while
#                two cabins defrag every slot over and over; then the power
#                button, a reboot, and every slot holds its last gen, whole, one
#                file of its name: nothing deleted came back.
#
# THE MUTATIONS THEY EXIST FOR, each put in by hand and the scenario run on it,
# whole, on this tree (2026-10-07):
#
#   writers      PoolAppend (meta_pool_tail.c) without
#                `g_pool_tail->used_bytes = (uint16_t)(pos + bytes);` — every
#                record of the tail laid at one place. The running machine
#                reads every round back (the mirror is in memory); the medium
#                after the halt has lost the sixteen files. MEASURED: 7 passed,
#                5 failed — check (16 damage, FILE_LOST), the host's get,
#                writersback, the next mount's census, the second check. The
#                literal D1 — TagFSInkPlot's lock around InkPlotLocked taken
#                out — did NOT turn it in the one run it was given: every
#                asynchronous write is inked on the one drain core, and the
#                spans' Inks on the K-Cores met them too rarely to tear the tail.
#   appenders    wjob_load (write_job_port.c) beginning an append at the size
#                the job's open saw, `j->handle->file_size`, not the size of the
#                record loaded under the Helm, `j->plot.size`. MEASURED: 8
#                passed, 4 failed — the case's DEFECT and the host's parse, on
#                AHCI and on the stick alike.
#   readdelete   FallowRipen (fallow.c) without condition 3,
#                `(n->kind == FALLOW_BY_RECORD && n->gen > HelmViewFloor(n->fid)) ||`
#                — the deleted file's ground goes back after the cycles, the
#                filler takes the lowest free run, where the file lay, and the
#                window being read answers 9304382 of its bytes with the
#                filler's. MEASURED: 7 passed, 1 failed (the case's DEFECT).
#   defragdelete InkQuitclaimLocked (ink.c) without
#                `if (fallow) FallowAdd(fallow, gen);` — a deleted file's ground
#                never comes to rest, so it never goes back, and the medium
#                keeps it taken with nothing owning it. MEASURED: 9 passed, 3
#                failed — both checks (ORPHAN) and the next mount's census
#                ("Bitmap mismatch: 6335 orphan"). Taking the defrag's own line
#                out instead — PlotGatherLay's `PlotLetGoLater` of the old
#                ground — leaks so fast that the volume fills and a defragger
#                is refused ERR_TAGFS_NO_SPACE in the first round: red too, for
#                a reason the scenario does not name, so it is not the one
#                written here.
# ---------------------------------------------------------------------------

CORES_STAND=${CORES_STAND:-"UEFI=on STRICT=on CORES=16 MEM=8G"}
CORES_WRITERS=16
CORES_ROUNDS=16
CORES_APPEND_RECORDS=60
READDELETE_MIB=64
READDELETE_ROUNDS=2
READDELETE_BPS=2097152       # reads at 2 MiB/s: a window of 16 MiB is eight seconds
READDELETE_GROW=2097152      # a gigabyte more ground: the file and its filler, twice over
DEFRAG_SLOTS=8
DEFRAG_ROUNDS=10
CORES_VERDICT='^\[QUILL\] [a-z]+: (PASS|DEFECT|PENDING|FAIL) — '
CORES_FAULT='KERNEL PANIC|\[EXCEPTION\]|^\[NIGHTWATCH\] VERDICT: .* a defect'

# cores_keys — CORES_STAND into CORES_KEYS, its words.
cores_keys() {
    if ! eval "CORES_KEYS=($CORES_STAND)" 2>/dev/null; then
        bad "cores: CORES_STAND does not read as shell words: $CORES_STAND"
        return 1
    fi
}

# cores_boot TAG WHAT KEYS... — the machine started and at its shell. 1, said,
# when not, the log kept as serial.cores.TAG.log.
cores_boot() {
    local tag=$1 what=$2; shift 2
    make run-stop >/dev/null 2>&1
    if ! make run-bg "$@" >"$SCRATCH/runbg.cores.log" 2>&1 || [ ! -S build/qemu.mon ]; then
        harness "$what: no machine was started — $(tail -1 "$SCRATCH/runbg.cores.log" | tr -d '\r')"
        make run-stop >/dev/null 2>&1
        return 1
    fi
    if ! wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"; then
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$SCRATCH/serial.cores.$tag.log"
        bad "$what: the machine never reached a shell ($*)"
        return 1
    fi
    wait_for_quiet "$BOOT_BURST_QUIET_S"
}

# cores_case LINE [QUIET_S] — LINE typed (cores_type), its verdict waited for
# — or a fault — and then the shell's prompt (cores_answer). Leaves CORES_SAID,
# the verdict line (none when it never came), and CORES_MARK, the log's length
# before the line was typed.
cores_case() {
    cores_type "$1" && cores_answer "$1" "${2:-$WAIT_QUIET_S}"
}

cores_type() {
    CORES_SAID=""
    CORES_MARK=$(wc -l < build/serial.log)
    type_line "$1"
}

cores_answer() {
    local line=$1 quiet=$2
    wait_for_line -E build/serial.log "$CORES_VERDICT|$CORES_FAULT" "$quiet" $((CORES_MARK + 1)) || return 1
    CORES_SAID=$(tail -n +$((CORES_MARK + 1)) build/serial.log | tr -d '\r' | grep -aE "$CORES_VERDICT" | tail -1)
    [ -n "$CORES_SAID" ] || return 1
    wait_until "$WAIT_QUIET_S" "the shell's prompt after $line" \
        '[ "$(tail -n +$((CORES_MARK + 1)) build/serial.log | tr -d "\r" | grep -cE "^(\[[^]]*\] )?~ ")" -ge 2 ]'
}

# cores_halt TAG HOW — the machine stopped through `bye`, or the power button
# (HOW button), and its log kept as serial.cores.TAG.log; 0 once the halt said
# its sync wrote the volume and QEMU went away by itself.
cores_halt() {
    local tag=$1 how=$2 mark pid synced=1
    mark=$(wc -l < build/serial.log)
    pid=$(cat build/qemu.pid 2>/dev/null)
    if [ "$how" = button ]; then
        mon_raw system_powerdown
    else
        type_line bye
    fi
    if wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" $((mark + 1)) &&
       tail -n +$((mark + 1)) build/serial.log | grep -aq '^\[HALT\] Storage sync complete' &&
       wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 ${pid:-0} 2>/dev/null"; then
        synced=0
    fi
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.cores.$tag.log"
    return $synced
}

# cores_said WHAT — the verdict CORES_SAID judged: PASS (or PENDING, where the
# case's answer is read after a reboot) is a PASS; anything else a FAIL.
cores_said() {
    local what=$1 want=${2:-PASS}
    case "$CORES_SAID" in
        *": $want — "*) ok "$what — ${CORES_SAID#\[QUILL\] }" ;;
        ?*)             bad "$what — ${CORES_SAID#\[QUILL\] }" ;;
        *)              bad "$what — no verdict came (the last said: $(tr -d '\r' < build/serial.log | grep -aE '^\[QUILL\]|KERNEL PANIC' | tail -1 | cut -c1-200))" ;;
    esac
}

# cores_faults LOG WHAT — no panic, no exception, no Nightwatch proof of
# something owed and never served, anywhere in the boot.
cores_faults() {
    local fault
    fault=$(tr -d '\r' < "$1" | grep -aE "$CORES_FAULT" | head -1 | cut -c1-200)
    [ -z "$fault" ]
    chk $? "$2 — no panic, no exception, nothing owed proved unserved${fault:+: $fault}"
}

# cores_check IMG WHAT — volume-read's census of the stopped volume: no block
# owned twice, none used and owned by nobody, nothing the mount would lose.
# Notes are said, not counted: a row the mount corrects is one.
cores_check() {
    local said
    said=$(python3 tools/volume-read.py "$1" check 2>&1)
    printf '%s\n' "$said" >"$SCRATCH/cores.check.log"
    printf '%s\n' "$said" | grep -q '^check: consistent — 0 damage, 0 leak'
    chk $? "$2 — volume-read check: $(printf '%s\n' "$said" | grep -E '^check: (consistent|inconsistent)' | tail -1 | sed 's/^check: //')$(printf '%s\n' "$said" | grep -E '^check: (damage|leak) ' | head -1 | cut -c1-200 | sed 's/^check: /; first: /')"
}

# cores_mounted LOG WHAT — the next mount's own census: no block named twice,
# none orphaned or missing, no file lost. Table rows it corrects from the pool
# are said, not counted: the pool is where a file's newest record is.
CORES_FSCK_CORRECTED="^\[TagFS FSCK\] [0-9]+ file table entry\(s\) corrected from this volume's metadata pool, 0 file\(s\) lost$"
cores_mounted() {
    local fsck corrected
    fsck=$(tr -d '\r' < "$1" | grep -aE '^\[TagFS FSCK\] ' | grep -avE "$CORES_FSCK_CORRECTED" | head -1 | cut -c1-200)
    corrected=$(tr -d '\r' < "$1" | grep -aE "$CORES_FSCK_CORRECTED" | head -1 | sed -nE 's/^\[TagFS FSCK\] ([0-9]+) .*/\1/p')
    grep -aqE 'MOUNTED — [0-9]+ file' "$1" && [ -z "$fsck" ]
    chk $? "$2 — the volume mounted again and its census named no block twice, nothing orphaned or missing, no file lost (table rows corrected from the pool: ${corrected:-0})${fsck:+: $fsck}"
}

# cores_streams IMG FILES... — each NAME:SIZE:SEED got off the medium and held
# against quilltest's stream (QuillWord): "ok N BYTES", or the first file that
# is not its stream.
cores_streams() {
    local img=$1 f; shift
    for f in "$@"; do
        python3 tools/volume-read.py "$img" get "${f%%:*}" -o "$SCRATCH/cores.get.${f%%:*}" >/dev/null 2>&1 ||
            rm -f "$SCRATCH/cores.get.${f%%:*}"
    done
    python3 - "$SCRATCH/cores.get." "$@" <<'CORES_STREAMS_EOF'
import struct, sys
M = (1 << 64) - 1
def stream(seed, base, size):
    out = bytearray()
    for i in range((size + 7) // 8):
        x = ((base + 8 * i) ^ ((seed & 0xFF) << 56)) & M
        x = (x * 0x9E3779B97F4A7C15) & M
        out += struct.pack("<Q", x ^ (x >> 29))
    return bytes(out[:size])
total = 0
for f in sys.argv[2:]:
    name, size, seed = f.split(":")
    try:
        got = open(sys.argv[1] + name, "rb").read()
    except OSError:
        print("%s is not on the medium" % name)
        sys.exit(1)
    want = stream(int(seed), 0, int(size))
    if got != want:
        at = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))
        print("%s holds %d bytes where %d of stream %s are due, the first wrong at byte %d" % (name, len(got), len(want), seed, at))
        sys.exit(1)
    total += len(got)
print("ok %d %d" % (len(sys.argv) - 2, total))
CORES_STREAMS_EOF
}

# ---------------------------------------------------------------------------
# writers
# ---------------------------------------------------------------------------

# writers_files — NAME:SIZE:SEED of every writer's file as its last round
# leaves it: quilltest_cores.c's QuillWriterSize and QuillWriterSeed, said
# again. Worked out here rather than read off the writers' own lines: sixteen
# cabins print at once, and one printf of one of them was measured split in
# two by the console with another's line between the halves.
writers_files() {
    local i r=$((CORES_ROUNDS - 1))
    for i in $(seq 0 $((CORES_WRITERS - 1))); do
        printf '_qw%02d.bin:%d:%d\n' "$i" \
            $(( (1 + (i + 3 * r) % 5) * 131072 + 4096 * ((i * 7 + r) % 9) + 8 * ((i * 131 + r * 17) % 500) + (i + r) % 8 )) \
            $(( (i << 4) | r ))
    done
}

run_writers() {
    echo "== writers: $CORES_WRITERS cabins on $CORES_STAND each write and rewrite a file of their own, $CORES_ROUNDS rounds =="
    local L1="$SCRATCH/serial.cores.writers1.log" L2="$SCRATCH/serial.cores.writers2.log"
    local said
    cores_keys || return
    rm -f "$L1" "$L2"
    make run-stop >/dev/null 2>&1
    rm -f build/boxos.img
    build "$(strict_key "${CORES_KEYS[@]}")"
    mutate_product build/boxos.img

    if cores_boot writers1 "writers" "${CORES_KEYS[@]}"; then
        cores_case "quilltest writers $CORES_WRITERS $CORES_ROUNDS" "$WAIT_QUIET_SLOW_S"
        cores_said "writers: the $CORES_WRITERS cabins"
        cores_halt writers1 bye
        chk $? "writers: the halt through bye said its sync wrote the volume ($(grep -aE "$STAND_HALT_ENDS" "$L1" | tr -d '\r' | tail -1 | cut -c1-100))"
        cores_faults "$L1" "writers: boot 1"
        cores_check build/boxos.img "writers: after the halt"
        # shellcheck disable=SC2046
        said=$(cores_streams build/boxos.img $(writers_files))
        case "$said" in
            "ok $CORES_WRITERS "*) ok "writers: the host got all $CORES_WRITERS files off the medium, ${said##* } bytes, each its last round's stream byte for byte" ;;
            *)                     bad "writers: the medium does not hold the writers' files as they said them — ${said:-the host could not read them}" ;;
        esac
    fi

    if cores_boot writers2 "writers: the reboot" "${CORES_KEYS[@]}"; then
        cores_case "quilltest writersback $CORES_WRITERS $CORES_ROUNDS" "$WAIT_QUIET_SLOW_S"
        cores_said "writers: after the reboot"
        cores_halt writers2 bye
        chk $? "writers: the second boot halted through bye and said its sync wrote the volume"
        cores_mounted "$L2" "writers: the reboot"
        cores_faults "$L2" "writers: boot 2"
        cores_check build/boxos.img "writers: after the second halt"
    fi
    lockrank_judge "$L1 $L2" "writers"
    make run-stop >/dev/null 2>&1
    restore build/boxos.img
}

# ---------------------------------------------------------------------------
# appenders
# ---------------------------------------------------------------------------

# cores_appended IMG WRITERS RECORDS — _qap.bin off the medium, parsed record
# by record from its first byte: "ok RECORDS APPENDED ZEROS SIZE" when every
# append of every writer is there once, its head and its stream whole, and
# nothing lies between two but zeros; else the first thing that is not so.
cores_appended() {
    python3 tools/volume-read.py "$1" get _qap.bin -o "$SCRATCH/cores.get._qap.bin" >/dev/null 2>&1 ||
        { echo "_qap.bin is not on the medium"; return; }
    python3 - "$SCRATCH/cores.get._qap.bin" "$2" "$3" <<'CORES_APPENDED_EOF'
import struct, sys
M = (1 << 64) - 1
MAGIC = struct.pack("<Q", 0x3143455250504151)
def word(seed, off):
    x = (off ^ ((seed & 0xFF) << 56)) & M
    x = (x * 0x9E3779B97F4A7C15) & M
    return x ^ (x >> 29)
def check(w, k):
    x = (((w << 32) | k) * 0x9E3779B97F4A7C15) & M
    return (x ^ (x >> 29)) & 0xFFFFFFFF
data = open(sys.argv[1], "rb").read()
writers, records = int(sys.argv[2]), int(sys.argv[3])
seen, pos, zeros, appended = set(), 0, 0, 0
while pos < len(data):
    if data[pos:pos + 8] == MAGIC and pos + 24 <= len(data):
        _m, w, k, ln, c = struct.unpack_from("<QIIII", data, pos)
        if w >= writers or k >= records or ln < 24 or (ln - 24) % 8 or pos + ln > len(data) or c != check(w, k):
            print("byte %d begins a head that names no append of this run (writer %d, append %d, %d bytes)" % (pos, w, k, ln))
            sys.exit(0)
        if (w, k) in seen:
            print("append %d of appender %d lies in the file twice, the second at byte %d" % (k, w, pos))
            sys.exit(0)
        want = b"".join(struct.pack("<Q", word(0xA0 + w, (k << 32) + 8 * j)) for j in range((ln - 24) // 8))
        if data[pos + 24:pos + ln] != want:
            print("append %d of appender %d at byte %d is not whole" % (k, w, pos))
            sys.exit(0)
        seen.add((w, k))
        appended += ln
        pos += ln
    elif data[pos] == 0:
        zeros += 1
        pos += 1
    else:
        print("byte %d lies in no append and is not a span's zero" % pos)
        sys.exit(0)
lost = [(w, k) for w in range(writers) for k in range(records) if (w, k) not in seen]
if lost:
    print("%d append(s) are not in the file, the first append %d of appender %d" % (len(lost), lost[0][1], lost[0][0]))
    sys.exit(0)
print("ok %d %d %d %d" % (len(seen), appended, zeros, len(data)))
CORES_APPENDED_EOF
}

# cores_appended_judge IMG WHAT WRITERS SPANS — the host's parse held to the
# claim: every append there once, whole, and the size the appends and the
# zeros — no zeros at all where no span ran.
cores_appended_judge() {
    local said n appended zeros size
    said=$(cores_appended "$1" "$3" "$CORES_APPEND_RECORDS")
    read -r _ok n appended zeros size <<CORES_APPENDED
$said
CORES_APPENDED
    if [ "${said%% *}" = ok ] && [ "$n" = $(($3 * CORES_APPEND_RECORDS)) ] && [ $((appended + zeros)) = "$size" ] &&
       { [ "$4" != 0 ] || [ "$zeros" = 0 ]; }; then
        ok "$2 — the host parsed _qap.bin off the medium: all $n appends once each and whole, $appended bytes, and $zeros zeros between them; $size bytes in all"
    else
        bad "$2 — the host's parse of _qap.bin off the medium: $said"
    fi
}

run_appenders() {
    echo "== appenders: two cabins append to one file beside a spanner on $CORES_STAND, and two on a USB stick =="
    local L1="$SCRATCH/serial.cores.append.log" L2="$SCRATCH/serial.cores.appendusb.log"
    cores_keys || return
    rm -f "$L1" "$L2"
    make run-stop >/dev/null 2>&1
    rm -f build/boxos.img
    build "$(strict_key "${CORES_KEYS[@]}")"
    mutate_product build/boxos.img
    if cores_boot append "appenders" "${CORES_KEYS[@]}"; then
        cores_case "quilltest append 2 $CORES_APPEND_RECORDS 1" "$WAIT_QUIET_SLOW_S"
        cores_said "appenders: AHCI, two appenders and a spanner"
        cores_halt append bye
        chk $? "appenders: the halt through bye said its sync wrote the volume"
        cores_faults "$L1" "appenders: AHCI"
        cores_check build/boxos.img "appenders: AHCI, after the halt"
        cores_appended_judge build/boxos.img "appenders: AHCI" 2 1
    fi
    make run-stop >/dev/null 2>&1
    restore build/boxos.img

    # The stick is the volume: the internal disk declared not this machine's
    # (latearrival_on), the stick a copy of the image taken once the machine is
    # up (stick_from_boot_image). BIOS, because that mutation stands on the
    # legacy channel's seat kind — STRICT's q35 puts the disk behind AHCI.
    latearrival_on
    build
    make run-stop >/dev/null 2>&1
    if make run-bg USB=on CORES=16 MEM=8G >"$SCRATCH/runbg.cores.log" 2>&1 && stick_from_boot_image &&
       wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" &&
       mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw" &&
       mon_raw "device_add usb-storage,drive=stick,id=usbstick" &&
       wait_for_line build/serial.log "hands over" "$WAIT_QUIET_SLOW_S" && wait_for_quiet "$BOOT_BURST_QUIET_S"; then
        cores_case "quilltest append 2 $CORES_APPEND_RECORDS 0" "$WAIT_QUIET_SLOW_S"
        cores_said "appenders: a USB stick, two appenders"
        grep -aq "a medium arrived carrying a volume, and this machine had none" build/serial.log
        chk $? "appenders: the stick is the volume the appends went to"
        cores_halt appendusb bye
        chk $? "appenders: the USB halt through bye said its sync wrote the volume"
        cores_faults "$L2" "appenders: USB"
        cores_check "$SCRATCH/stick.img" "appenders: the stick, after the halt"
        cores_appended_judge "$SCRATCH/stick.img" "appenders: the stick" 2 0
    else
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$L2" 2>/dev/null
        bad "appenders: the stick never became the volume at a shell ($(tr -d '\r' < "$L2" 2>/dev/null | grep -aE 'hands over|mass storage|KERNEL PANIC' | tail -1 | cut -c1-120))"
    fi
    latearrival_off
    rm -f "$SCRATCH/stick.img"
    lockrank_judge "$L1 $L2" "appenders"
}

# ---------------------------------------------------------------------------
# readdelete
# ---------------------------------------------------------------------------

run_readdelete() {
    echo "== readdelete: a file of $READDELETE_MIB MiB read on $CORES_STAND while another cabin deletes it, reads held to $READDELETE_BPS B/s =="
    local L1="$SCRATCH/serial.cores.readdelete1.log" L2="$SCRATCH/serial.cores.readdelete2.log" grown start count stood
    cores_keys || return
    rm -f "$L1" "$L2"
    make run-stop >/dev/null 2>&1
    rm -f build/boxos.img
    build "$(strict_key "${CORES_KEYS[@]}")"
    mutate_product build/boxos.img
    if ! grown=$(grow_stretch build/boxos.img "$READDELETE_GROW"); then
        bad "readdelete: the image has no BoxOS partition to stretch"
        restore build/boxos.img; return
    fi
    start=${grown% *}; count=${grown#* }

    if cores_boot readdelete1 "readdelete" "${CORES_KEYS[@]}"; then
        if stood=$(grow_stood_on build/serial.log "$start" "$count"); then
            # ‼ THE GROWN VOLUME SAYS ITS INTEGRITY MAP STANDS AFTER THE PROMPT.
            # The map is laid over the new ground once the shell is up, and
            # its line lands after the `~ `: a line typed then echoes with no
            # prompt before it, type_line does not see it taken and types it
            # again — measured, the case ran twice. So the map's line is
            # waited for, and a fresh prompt asked for, before the case.
            wait_for_line build/serial.log '^\[Integrity\] the map stands' "$WAIT_QUIET_S"
            wait_for_quiet "$BOOT_BURST_QUIET_S"
            key_press ret
            wait_for_quiet "$BOOT_BURST_QUIET_S"
            # The reads are held once the line is taken: the case writes and
            # anchors its file before its first read, and only the second
            # window is aimed at.
            if cores_type "quilltest readdelete $READDELETE_MIB $READDELETE_ROUNDS"; then
                mon_raw "block_set_io_throttle ide0-hd0 0 $READDELETE_BPS 0 0 0 0"
                cores_answer "quilltest readdelete" "$WAIT_QUIET_SLOW_S"
                mon_raw "block_set_io_throttle ide0-hd0 0 0 0 0 0 0"
            fi
            cores_said "readdelete"
            tail -n +$((CORES_MARK + 1)) build/serial.log | tr -d '\r' | grep -a '^\[QUILL\] readdelete: round ' |
                sed 's/^\[QUILL\] /       /'
            cores_halt readdelete1 bye
            chk $? "readdelete: the halt through bye said its sync wrote the volume"
            cores_faults "$L1" "readdelete: boot 1"
            cores_check build/boxos.img "readdelete: after the halt"
            if cores_boot readdelete2 "readdelete: the reboot" "${CORES_KEYS[@]}"; then
                cores_halt readdelete2 bye
                chk $? "readdelete: the reboot halted through bye and said its sync wrote the volume"
                cores_mounted "$L2" "readdelete: the reboot"
                cores_check build/boxos.img "readdelete: after the second halt"
            fi
        else
            make run-stop >/dev/null 2>&1
            harness "readdelete: the boot stood on $stood — not the stretched image, so it judges nothing"
        fi
    fi
    lockrank_judge "$L1 $L2" "readdelete"
    make run-stop >/dev/null 2>&1
    restore build/boxos.img
}

# ---------------------------------------------------------------------------
# defragdelete
# ---------------------------------------------------------------------------

# cores_slots IMG — every slot of defragdelete off the medium: one file of its
# name, its gen DEFRAG_ROUNDS — its head, then its stream, whole.
cores_slots() {
    local s name
    python3 tools/volume-read.py "$1" list >"$SCRATCH/cores.slots.list" 2>&1
    for s in $(seq 0 $((DEFRAG_SLOTS - 1))); do
        name=$(printf '_qd%02d.bin' "$s")
        python3 tools/volume-read.py "$1" get "$name" -o "$SCRATCH/cores.get.$name" >/dev/null 2>&1 ||
            rm -f "$SCRATCH/cores.get.$name"
    done
    python3 - "$SCRATCH/cores.slots.list" "$SCRATCH/cores.get." "$DEFRAG_SLOTS" "$DEFRAG_ROUNDS" <<'CORES_SLOTS_EOF'
import re, struct, sys
M = (1 << 64) - 1
def stream(seed, base, size):
    out = bytearray()
    for i in range((size + 7) // 8):
        x = ((base + 8 * i) ^ ((seed & 0xFF) << 56)) & M
        x = (x * 0x9E3779B97F4A7C15) & M
        out += struct.pack("<Q", x ^ (x >> 29))
    return out[:size]
listing = open(sys.argv[1]).read()
slots, g = int(sys.argv[3]), int(sys.argv[4])
total = 0
for s in range(slots):
    name = "_qd%02d.bin" % s
    n = len(re.findall(r"^\s+%s\s" % re.escape(name), listing, re.M))
    if n != 1:
        print("the medium lists %d file(s) called %s, where one is due" % (n, name))
        sys.exit(0)
    size = (192 << 10) + 4096 * ((s * 37 + g * 11) % 64) + 8 * ((s + g) % 100) + s % 8
    want = stream(0xD0 + s, g << 40, size)
    want[0:32] = struct.pack("<QIIQQ", 0x3147415246454451, s, g, size, (((s << 32) | g) * 0x9E3779B97F4A7C15) & M)
    try:
        got = open(sys.argv[2] + name, "rb").read()
    except OSError:
        got = b""
    if got != bytes(want):
        head = struct.unpack_from("<QII", got, 0) if len(got) >= 16 else (0, 0, 0)
        print("%s holds %d bytes, its head saying gen %d, where gen %d in %d bytes is due" % (name, len(got), head[2], g, size))
        sys.exit(0)
    total += size
print("ok %d %d" % (slots, total))
CORES_SLOTS_EOF
}

run_defragdelete() {
    echo "== defragdelete: $DEFRAG_SLOTS slots deleted and made again $DEFRAG_ROUNDS times beside two defraggers on $CORES_STAND, then the power button =="
    local L1="$SCRATCH/serial.cores.defrag1.log" L2="$SCRATCH/serial.cores.defrag2.log" said gathered gone
    cores_keys || return
    rm -f "$L1" "$L2"
    make run-stop >/dev/null 2>&1
    rm -f build/boxos.img
    build "$(strict_key "${CORES_KEYS[@]}")"
    mutate_product build/boxos.img

    if cores_boot defrag1 "defragdelete" "${CORES_KEYS[@]}"; then
        cores_case "quilltest defragdelete $DEFRAG_SLOTS $DEFRAG_ROUNDS" "$WAIT_QUIET_SLOW_S"
        cores_said "defragdelete" PENDING
        gathered=$(tail -n +$((CORES_MARK + 1)) build/serial.log | tr -d '\r' | grep -acE '^\[TagFS\] file [0-9]+: [0-9]+ extent\(s\) of it, [0-9]+ block\(s\), gathered into one run')
        gone=$(printf '%s\n' "$CORES_SAID" | sed -nE 's/.* found the file deleted under them ([0-9]+) times.*/\1/p')
        [ "${gathered:-0}" -gt 0 ] && [ "${gone:-0}" -gt 0 ]
        chk $? "defragdelete: the defraggers gathered files the kernel named $gathered time(s) and found a file deleted under them ${gone:-no} time(s) — both are what the case asks"
        cores_halt defrag1 button
        chk $? "defragdelete: the power button's halt said its sync wrote the volume ($(grep -aE "$STAND_HALT_ENDS" "$L1" | tr -d '\r' | tail -1 | cut -c1-100))"
        cores_faults "$L1" "defragdelete: boot 1"
        cores_check build/boxos.img "defragdelete: after the power button"
        said=$(cores_slots build/boxos.img)
        case "$said" in
            "ok $DEFRAG_SLOTS "*) ok "defragdelete: the host lists one file of each slot's name and got each off the medium as its gen $DEFRAG_ROUNDS, ${said##* } bytes in all — none of the $((DEFRAG_SLOTS * DEFRAG_ROUNDS)) files deleted came back" ;;
            *)                    bad "defragdelete: off the medium after the power button — $said" ;;
        esac
    fi
    if cores_boot defrag2 "defragdelete: the reboot" "${CORES_KEYS[@]}"; then
        cores_case "quilltest defragback $DEFRAG_SLOTS $DEFRAG_ROUNDS" "$WAIT_QUIET_SLOW_S"
        cores_said "defragdelete: after the reboot"
        cores_halt defrag2 bye
        chk $? "defragdelete: the reboot halted through bye and said its sync wrote the volume"
        cores_mounted "$L2" "defragdelete: the reboot"
        cores_faults "$L2" "defragdelete: boot 2"
        cores_check build/boxos.img "defragdelete: after the second halt"
    fi
    lockrank_judge "$L1 $L2" "defragdelete"
    make run-stop >/dev/null 2>&1
    restore build/boxos.img
}

run_cores() {
    run_writers; echo; run_appenders; echo; run_readdelete; echo; run_defragdelete
}

case "${1:-both}" in
    healthy)  run_healthy ;;
    novolume) run_novolume ;;
    badpool)  run_badpool ;;
    framerefuse) run_framerefuse ;;
    uefi)     run_uefi ;;
    footing)  run_footing ;;
    stranger) run_stranger ;;
    latearrival) run_latearrival ;;
    replug)   run_replug ;;
    replugwrite) run_replugwrite ;;
    nofsgsbase) run_nofsgsbase ;;
    logsave)  run_logsave ;;
    mountfail) run_mountfail ;;
    yank)     run_yank ;;
    slowdisk) run_slowdisk ;;
    haltslow) run_haltslow ;;
    haltcycle) run_haltcycle ;;
    fullvolume) run_fullvolume ;;
    gpt)      run_gpt ;;
    stillthere) run_stillthere ;;
    sleeps)     run_sleeps ;;
    lines)      run_lines ;;
    linesmut)   run_linesmut ;;
    rollcall)   run_rollcall ;;
    rollcallmut) run_rollcallmut ;;
    handset)    run_handset ;;
    handsetmut) run_handsetmut ;;
    handsetdeaf) run_handsetdeaf ;;
    handsetdeafmut) run_handsetdeafmut ;;
    brigade)    run_brigade ;;
    brigademut) run_brigademut ;;
    headcount)  run_headcount ;;
    headcountmut) run_headcountmut ;;
    deadline)   run_deadline ;;
    deadlinemut) run_deadlinemut ;;
    byname)     run_byname ;;
    bynamemut)  run_bynamemut ;;
    stash)      run_stash ;;
    stashmut)   run_stashmut ;;
    wire)       run_wire ;;
    wiremut)    run_wiremut ;;
    knock)      run_knock ;;
    knockmut)   run_knockmut ;;
    knockstorm) run_knockstorm ;;
    knockstormmut) run_knockstormmut ;;
    unattended) run_unattended ;;
    unattendedmut) run_unattendedmut ;;
    sleepsmut)  run_sleepsmut ;;
    kcoreclaim) run_kcoreclaim ;;
    kcoreclaimmut) run_kcoreclaimmut ;;
    runnable)   run_runnable ;;
    runnablemut) run_runnablemut ;;
    lostwake)   run_lostwake ;;
    lostwakemut) run_lostwakemut ;;
    chit)       run_chit ;;
    chitmut)    run_chitmut ;;
    chitmissmut) run_chitmissmut ;;
    turnin)     run_turnin ;;
    turninmut)  run_turninmut ;;
    twoctrl)  run_twoctrl ;;
    manyports) run_manyports ;;
    usbrecover) run_usbrecover ;;
    holdground) run_holdground ;;
    returnfail) run_returnfail ;;
    seal)     run_seal ;;
    ctrlgiveup) run_ctrlgiveup ;;
    enumsilence) run_enumsilence ;;
    isoch)    run_isoch ;;
    noexec)   run_noexec ;;
    earlyirq) run_earlyirq ;;
    intx)     run_intx ;;
    iommu)    run_iommu ;;
    lastsaid) run_lastsaid ;;
    draft)    run_draft ;;
    draftmut) run_draftmut ;;
    paint)    run_paint ;;
    paintmut) run_paintmut ;;
    finish)   run_finish ;;
    finishmut) run_finishmut ;;
    overtake) run_overtake ;;
    overtakemut) run_overtakemut ;;
    orderlife) run_orderlife ;;
    orderlifemut) run_orderlifemut ;;
    batchfail) run_batchfail ;;
    batchfailmut) run_batchfailmut ;;
    caret)    run_caret ;;
    caretmut) run_caretmut ;;
    grow)     run_grow ;;
    growcut)  run_growcut ;;
    growmut)  run_growmut ;;
    census)   run_census ;;
    censusmut) run_censusmut ;;
    stand)    run_stand ;;
    standusb) run_standusb ;;
    standmut) run_standmut ;;
    quill)    run_quill ;;
    quillmut) run_quillmut ;;
    stackguard) run_stackguard ;;
    lockrankmut) run_lockrankmut ;;
    abba)     run_abba ;;
    abbamut)  run_abbamut ;;
    turns)    run_turns ;;
    turnsmut) run_turnsmut ;;
    writers)  run_writers ;;
    appenders) run_appenders ;;
    readdelete) run_readdelete ;;
    defragdelete) run_defragdelete ;;
    cores)    run_cores ;;
    both)     run_healthy; echo; run_novolume ;;
    all)      run_healthy; echo; run_novolume; echo; run_stranger; echo; run_latearrival; echo; run_replug; echo; run_replugwrite; echo; run_holdground; echo; run_returnfail; echo; run_nofsgsbase; echo; run_logsave; echo; run_yank; echo; run_stillthere; echo; run_sleeps; echo; run_lines; echo; run_rollcall; echo; run_handset; echo; run_handsetdeaf; echo; run_brigade; echo; run_headcount; echo; run_deadline; echo; run_byname; echo; run_stash; echo; run_wire; echo; run_knock; echo; run_knockstorm; echo; run_unattended; echo; run_kcoreclaim; echo; run_runnable; echo; run_lostwake; echo; run_chit; echo; run_turnin; echo; run_slowdisk; echo; run_haltslow; echo; run_haltcycle; echo; run_fullvolume; echo; run_gpt; echo; run_twoctrl; echo; run_manyports; echo; run_usbrecover; echo; run_ctrlgiveup; echo; run_enumsilence; echo; run_isoch; echo; run_seal; echo; run_uefi; echo; run_noexec; echo; run_earlyirq; echo; run_intx; echo; run_iommu; echo; run_lastsaid; echo; run_draft; echo; run_paint; echo; run_finish; echo; run_overtake; echo; run_orderlife; echo; run_batchfail; echo; run_caret; echo; run_grow; echo; run_growcut; echo; run_quill; echo; run_mountfail; echo; run_badpool; echo; run_framerefuse; echo; run_census; echo; run_censusmut; echo; run_stand; echo; run_standusb; echo; run_standmut; echo; run_quillmut; echo; run_stackguard; echo; run_lockrankmut; echo; run_abba; echo; run_abbamut; echo; run_turns; echo; run_turnsmut; echo; run_cores ;;
    *) echo "usage: $0 [healthy|novolume|uefi|noexec|earlyirq|intx|iommu|lastsaid|draft|draftmut|paint|paintmut|finish|finishmut|overtake|overtakemut|orderlife|orderlifemut|batchfail|batchfailmut|caret|caretmut|grow|growcut|growmut|census|censusmut|stand|standusb|standmut|quill|quillmut|stackguard|abba|abbamut|turns|turnsmut|writers|appenders|readdelete|defragdelete|cores|mountfail|holdground|returnfail|stranger|latearrival|replug|replugwrite|nofsgsbase|badpool|framerefuse|manyports|usbrecover|stillthere|sleeps|sleepsmut|lines|linesmut|rollcall|rollcallmut|knock|knockmut|knockstorm|knockstormmut|unattended|unattendedmut|handset|handsetmut|kcoreclaim|kcoreclaimmut|runnable|runnablemut|lostwake|lostwakemut|turnin|turninmut|slowdisk|haltcycle|fullvolume|gpt|seal|ctrlgiveup|enumsilence|isoch|both|all]"; exit 2 ;;
esac

STAND_FINISHED=1
echo
echo "logcheck: $PASS passed, $FAIL failed"
if [ "$CAVEAT" != 0 ]; then
    echo "logcheck: $CAVEAT caveat(s) held — each is a known defect reproduced on purpose, and names the step that discharges it"
fi
if [ "$HARNESS" != 0 ]; then
    echo "logcheck: $HARNESS harness failure(s) — this run could not talk to the"
    echo "          machine it was judging, so its verdicts are not evidence."
fi
[ "$FAIL" = 0 ] && [ "$HARNESS" = 0 ]
