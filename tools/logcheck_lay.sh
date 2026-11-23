# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the lay scenario.

# ---------------------------------------------------------------------------
# lay: a volume laid on a BoxOS partition by the kernel, whole with the
# system or not at all (tagfs/volume/volume_lay.c, utilities volumes and
# disks).
#
# Four sticks beside the machine's own disk. A blank one is laid anew, given
# a BoxOS partition of 400M and a volume called Fresh — asked once and told
# no, then yes — given what came with the system on the home volume, refused
# a second volume over the one standing on it, and grown into the rest of
# the stick. A blank one of 4096-byte blocks is laid anew and given a volume
# with no name. One the host laid with an ESP and a BoxOS partition too small
# for the system refuses a volume on either, as it refuses one on an entry it
# has not and a disk that is not there. The fourth, a BoxOS partition the host
# laid, is given a volume called Cut through blklogwrites, so every write and
# flush of the laying is on a record. The halt ends.
#
# On the host volume-read finds every volume laid consistent and the kernel
# its Deed names attested by its book; Fresh holds the home volume's files
# that came with the system and the ones it keeps in a role — the same bytes,
# roles, tags and passports — and none of the person's; the stick of 4096-byte
# blocks the ones in a role; the small stick is as it was, byte for byte; and
# Fresh's partition runs to the end of its stick.
#
# Every medium a power cut could leave of the laying is built from the record
# — every flush, every subset of the writes between two (every prefix, every
# single omission and a sample past ten), every torn last write — and judged:
# each holds no volume at all, or the whole volume, the very files the laying
# ended with; none holds a volume before the Deed's own writes, every one
# after them does. Two are booted: the last with no Deed, its book and files
# written, which takes a volume laid again; and one whose Deed landed at its
# head alone, mounted on one Deed with every file.
#
# A second boot of Fresh alone takes the ground its partition grew by, by
# its name, with every file it was given.
# ---------------------------------------------------------------------------

LAY_SCENARIO_STICKS="d512 d4k small cut"

# lay_stick NAME — the -drive and -device of the stick NAME; `cut` through
# blklogwrites onto $SCRATCH/lay.cut.log.
lay_stick() {
    local img="$SCRATCH/lay.$1.img"
    case "$1" in
        cut)
            printf -- '-blockdev driver=file,node-name=lc-file,filename=%s,cache.no-flush=on ' "$img"
            printf -- '-blockdev driver=file,node-name=lc-log-file,filename=%s,cache.no-flush=on ' "$SCRATCH/lay.cut.log"
            printf -- '-blockdev driver=blklogwrites,node-name=lc-log,file=lc-file,log=lc-log-file,log-sector-size=512,log-super-update-interval=1 '
            printf -- '-blockdev driver=raw,node-name=lc-disk,file=lc-log -device usb-storage,drive=lc-disk,id=lcstick ' ;;
        d4k)
            printf -- '-drive if=none,id=%s,file=%s,format=raw -device usb-storage,drive=%s,id=%sstick,logical_block_size=4096,physical_block_size=4096 ' \
                "$1" "$img" "$1" "$1" ;;
        *)
            printf -- '-drive if=none,id=%s,file=%s,format=raw -device usb-storage,drive=%s,id=%sstick ' \
                "$1" "$img" "$1" "$1" ;;
    esac
}

# lay_boot TAG STICKS... — the machine run to the shell with the sticks in.
lay_boot() {
    local tag=$1 extra="" s
    shift
    for s; do extra+=$(lay_stick "$s"); done
    make run-stop >/dev/null 2>&1
    if ! make run-bg USB=on CORES=4 MEM=4G QEMUEXTRA="$extra" >"$SCRATCH/lay.$tag.run-bg" 2>&1; then
        harness "lay ($tag): run-bg would not start: $(tail -1 "$SCRATCH/lay.$tag.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt
}

# lay_halt TAG — bye, the halt waited out, the log kept as serial.lay.TAG.log.
lay_halt() {
    local tag=$1 rc=0
    type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" || rc=1
    sleep 1
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log >"$SCRATCH/serial.lay.$tag.log"
    return $rc
}

# lay_said TAG PATTERN WHAT — boot TAG's log says PATTERN (grep -E).
lay_said() {
    grep -aqE "$2" "$SCRATCH/serial.lay.$1.log"
    chk $? "lay: $3"
}

# lay_laid DISK — the number the house gave the volume `volumes new` laid on DISK.
lay_laid() {
    tr -d '\r' < build/serial.log | sed -nE "s/^Volume ([0-9]+) is laid on disk $1, .*/\1/p" | tail -1
}

# lay_whole IMG BLOCK WHAT — volume-read finds the volume on IMG consistent,
# and the kernel its Deed names attested by its book.
lay_whole() {
    python3 tools/volume-read.py "$1" --block "$2" check >"$SCRATCH/lay.check.log" 2>&1 &&
        grep -q '^check: consistent — 0 damage' "$SCRATCH/lay.check.log"
    chk $? "lay: $3 is consistent ($(tail -1 "$SCRATCH/lay.check.log"))"
    python3 tools/volume-read.py "$1" --block "$2" boot 2>&1 | grep -q '^boot: the kernel the deed names is attested by the book'
    chk $? "lay: and the kernel its Deed names is attested by its book"
}

# lay_holds IMG BLOCK WHICH — the files of the volume on IMG are the home
# volume's WHICH — `roles`, the ones it keeps in a role, or `fresh`, every one
# no program made — each with its bytes, role, tags and passport.
lay_holds() {
    python3 - "$1" "$2" "$3" build/boxos.img <<'EOF'
import json, subprocess, sys
def Files(img, block):
    out = subprocess.check_output([sys.executable, "tools/volume-read.py", img, "--block", block, "check", "--json"],
                                  stderr=subprocess.DEVNULL)
    return {f["name"]: f for f in json.loads(out)["files"] if f["visible"]}
laid, home = Files(sys.argv[1], sys.argv[2]), Files(sys.argv[4], "512")
want = {n for n, f in home.items() if (f["flags"] & 12 if sys.argv[3] == "roles" else not f.get("made_by"))}
def Card(f):
    return (f["sha256"], f["size"], f["flags"], sorted(f.get("tag_text") or []), f.get("passport"))
differ = sorted(n for n in want & set(laid) if Card(laid[n]) != Card(home[n]))
if set(laid) != want or differ:
    print("laid %d, wanted %d; missing %s, beyond %s, differing %s" % (len(laid), len(want), sorted(want - set(laid))[:4],
          sorted(set(laid) - want)[:4], differ[:4]))
    sys.exit(1)
print("%d file(s)" % len(laid))
EOF
}

# lay_cuts — every medium a power cut could leave of the recorded laying
# planned and judged (see the scenario); the two to boot left in
# $SCRATCH/lay.cutboots as `again EPOCH KEEP` and `onedeed EPOCH KEEP`.
lay_cuts() {
    local d="$SCRATCH/lay.cuts" vr=tools/volume-read.py
    rm -rf "$d" && mkdir -p "$d" || return 1
    python3 "$vr" "$SCRATCH/lay.cut.base" cut "$SCRATCH/lay.cut.log" --against "$SCRATCH/lay.cut.img" >"$d/against.log" 2>&1 ||
        { cat "$d/against.log"; return 1; }
    python3 "$vr" "$SCRATCH/lay.cut.base" cut "$SCRATCH/lay.cut.log" --list --json >"$d/list.json" || return 1
    python3 "$vr" "$SCRATCH/lay.cut.img" check --json >"$d/final.json" 2>/dev/null || return 1
    # Every write inside the partition is the laying's: the base holds no
    # volume yet for the record's classifier to name them by.
    python3 - "$d/list.json" "$d/final.json" >"$d/list.lay.json" <<'EOF' || return 1
import json, sys
d, final = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
lo = final["ground"]["start"]
hi = lo + final["ground"]["sectors"]
for w in d["writes"]:
    if lo <= w["lba"] and w["lba"] + w["sectors"] <= hi:
        w["touches"] = [{"first_lba": w["lba"], "sectors": w["sectors"], "kind": "lay", "what": "the laying"}]
json.dump(d, sys.stdout)
EOF
    python3 tools/powercut.py plan "$d/list.lay.json" --seed 1a7 >"$d/plan.json" || return 1
    echo '{"files": []}' >"$d/base.check.json"
    python3 tools/powercut.py checks "$d/plan.json" "$SCRATCH/lay.cut.base" "$SCRATCH/lay.cut.log" "$d/base.check.json" \
        --jobs "${PC_CHECK_JOBS:-6}" >"$d/checks.jsonl" 2>"$d/checks.err" || return 1
    python3 - "$d/plan.json" "$d/checks.jsonl" "$d/final.json" "$SCRATCH/lay.cutboots" <<'EOF'
import json, sys
cuts = {c["id"]: c for c in json.load(open(sys.argv[1]))["cuts"]}
final = json.load(open(sys.argv[3]))
whole = sorted([f["fid"], f.get("name"), f.get("size"), f.get("sha256")] for f in final["files"] if f["visible"])
allowed = {"DEED_HEAD_BAD", "DEED_TAIL_BAD", "RIBBON_TORN"}
nothing, mounted, wrong, onedeed = [], [], [], None
for line in open(sys.argv[2]):
    r = json.loads(line)
    c = cuts[r["id"]]
    if "error" in r:
        if "no deed this kernel could stand on" in r["error"]:
            nothing.append(c)
        else:
            wrong.append("%s: %s" % (r["id"], r["error"]))
        continue
    codes = {f[0] for f in r["findings"]}
    if not r["mounts"] or sorted(r["files"]) != whole or codes - allowed:
        wrong.append("%s: mounts %s, %d file(s), %s" % (r["id"], r["mounts"], len(r["files"]), sorted(codes)))
        continue
    mounted.append(c)
    if codes & {"DEED_TAIL_BAD"} and onedeed is None:
        onedeed = c
deed = min((c["epoch"] for c in mounted), default=None)
if deed is None or not nothing:
    wrong.append("no cut holds %s" % ("the volume" if deed is None else "no volume"))
else:
    wrong += ["%s holds no volume after the Deed's writes" % c["id"] for c in nothing if c["epoch"] > deed]
    wrong += ["%s holds a volume before the Deed's writes" % c["id"] for c in mounted if c["epoch"] < deed]
again = max((c for c in nothing if c["epoch"] == deed - 1 and c["id"].endswith("-all")), key=lambda c: c["epoch"],
            default=None)
if again is None or onedeed is None:
    wrong.append("no cut %s to boot" % ("with its book and no Deed" if again is None else "on its head's Deed alone"))
for w in wrong[:8]:
    print("  " + w)
if wrong:
    sys.exit(1)
with open(sys.argv[4], "w") as out:
    for name, c in (("again", again), ("onedeed", onedeed)):
        out.write("%s %d %s\n" % (name, c["epoch"], c["keep"] or "none"))
print("%d medium(s): %d hold no volume, every one before the Deed's writes; %d the whole volume, %d file(s)"
      % (len(nothing) + len(mounted), len(nothing), len(mounted), len(whole)))
EOF
}

run_lay() {
    echo "== lay: a volume laid by the kernel, whole with the system or not at all =="
    local s rc d512 d4k small cut fresh shipped n line name epoch keep again onedeed t
    build
    for s in $LAY_SCENARIO_STICKS again onedeed; do rm -f "$SCRATCH/lay.$s.img"; done
    rm -f "$SCRATCH/lay.cut.log" "$SCRATCH/lay.cut.base" "$SCRATCH/lay.small.before" "$SCRATCH/lay.cutboots"
    python3 -c "import sys; [open(p, 'wb').truncate(int(n) << 20) for p, n in zip(sys.argv[1::2], sys.argv[2::2])]" \
        "$SCRATCH/lay.d512.img" 1024 "$SCRATCH/lay.d4k.img" 700
    if ! python3 tools/disks.py lay "$SCRATCH/lay.small.img" 200M --part esp:32M --part boxos:16M:BoxOS >/dev/null ||
       ! python3 tools/disks.py lay "$SCRATCH/lay.cut.img" 400M --part boxos:300M:BoxOS >/dev/null; then
        harness "lay: the sticks could not be laid"
        return
    fi
    cp "$SCRATCH/lay.small.img" "$SCRATCH/lay.small.before"
    cp "$SCRATCH/lay.cut.img" "$SCRATCH/lay.cut.base"
    : >"$SCRATCH/lay.cut.log"

    rc=1
    if lay_boot first $LAY_SCENARIO_STICKS && disks_ask "disks"; then
        d512=$(disks_number "1 GB") d4k=$(disks_number "734 MB") small=$(disks_number "210 MB") cut=$(disks_number "419 MB")
        if [ -n "$d512" ] && [ -n "$d4k" ] && [ -n "$small" ] && [ -n "$cut" ] &&
           disks_ask "disks $d512 new" yes && disks_ask "disks $d512 add boxos 400M" yes &&
           disks_ask "volumes new $d512 0 Fresh" no &&
           disks_ask "volumes new $d512 0 Fresh" yes && fresh=$(lay_laid "$d512") && [ -n "$fresh" ] &&
           disks_ask "volumes shipped 0" && disks_ask "carry them $fresh copy" &&
           disks_ask "volumes new $d512 0 Again" &&
           disks_ask "disks $d512 grow 0" yes &&
           disks_ask "disks $d4k new" yes && disks_ask "disks $d4k add boxos" yes &&
           disks_ask "volumes new $d4k 0" yes && [ -n "$(lay_laid "$d4k")" ] &&
           disks_ask "volumes new $small 0" && disks_ask "volumes new $small 1" yes &&
           disks_ask "volumes new $small 7" && disks_ask "volumes new 99 0" &&
           disks_ask "volumes new $cut 0 Cut" yes && [ -n "$(lay_laid "$cut")" ] &&
           disks_ask "volumes"; then
            rc=0
        fi
    fi
    lay_halt first || rc=1
    [ $rc = 0 ]
    chk $? "lay (first boot): every volume laid, given, grown and refused as asked, and the halt ends"

    shipped=$(sed -nE 's/^([0-9]+) file\(s\) came with the system on volume 0 .*/\1/p' "$SCRATCH/serial.lay.first.log" | tail -1)
    lay_said first '^Nothing was laid$' "a volume the person says no to is not laid"
    lay_said first "^Volume [0-9]+ is laid on disk $d512, [0-9]+ file\\(s\\) of the system on it$" \
        "a volume is laid on the blank stick's BoxOS partition, with the system"
    lay_said first '\[Deed\] seat [0-9]+: packed [0-9]+ — shipment [0-9a-f]+ over [0-9]+ file\(s\), tree [0-9a-f]+, (clean|‼ UNDER TEST)$' \
        "the laid volume's Deed says what it was packed with, and the kernel's tree"
    lay_said first "^Copied ${shipped:-x} file\\(s\\) to Fresh$" "and it is given every file that came with the system (${shipped:-none})"
    lay_said first "^A volume of the house stands on entry 0 of disk $d512: no volume is laid over it$" \
        "a volume standing on a partition is never laid over"
    lay_said first "^\\[DiskTable\\] seat [0-9]+: entry 0 grows from [0-9]+ to [0-9]+ block\\(s\\)" \
        "the partition grows into the rest of its stick"
    lay_said first "^Volume [0-9]+ is laid on disk $d4k, " "a volume is laid on a stick of 4096-byte blocks"
    lay_said first "^Entry 0 of disk $small is EFI system, not BoxOS's" "a partition not BoxOS's takes no volume"
    lay_said first "^No volume was laid on disk $small: the partition is too small for a volume and the system$" \
        "a partition too small for the system takes none"
    lay_said first "^Disk $small has no partition in entry 7$" "an entry a disk has not is said"
    lay_said first '^There is no disk numbered 99$' "a disk that is not there is said"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$SCRATCH/serial.lay.first.log"
    chk $? "lay: no panic, no fault, no defect said"

    lay_whole "$SCRATCH/lay.d512.img" 512 "Fresh"
    t=$(lay_holds "$SCRATCH/lay.d512.img" 512 fresh)
    chk $? "lay: Fresh holds every file of the home volume no program made, as it is there ($t)"
    t=$(disks_read "$SCRATCH/lay.d512.img" 2>/dev/null) || t='{}'
    case "$t" in *'"parts"'*) disks_holds "$t" 't["parts"][0]["last"] >= t["last_usable"] - 2048' ;; *) false ;; esac
    chk $? "lay: Fresh's partition runs to the end of its stick"
    lay_whole "$SCRATCH/lay.d4k.img" 4096 "the volume on the stick of 4096-byte blocks"
    t=$(lay_holds "$SCRATCH/lay.d4k.img" 4096 roles)
    chk $? "lay: it holds every file the home volume keeps in a role, as it is there ($t)"
    cmp -s "$SCRATCH/lay.small.img" "$SCRATCH/lay.small.before"
    chk $? "lay: the small stick refused is as it was, byte for byte"

    t=$(lay_cuts 2>&1)
    chk $? "lay: every power cut of the laying leaves no volume or the whole of it — $(echo "$t" | tail -1)"
    [ -n "$t" ] && echo "$t" | sed -n '1,8p' | grep -v "^[0-9]* medium" || true

    # The two cuts booted: one laid again, one mounted on its head's Deed.
    rc=1
    if [ -s "$SCRATCH/lay.cutboots" ]; then
        while read -r name epoch keep; do
            python3 tools/volume-read.py "$SCRATCH/lay.cut.base" cut "$SCRATCH/lay.cut.log" --epoch "$epoch" \
                --keep "$keep" --out "$SCRATCH/lay.$name.img" >/dev/null || break
        done <"$SCRATCH/lay.cutboots"
        if [ -f "$SCRATCH/lay.again.img" ] && [ -f "$SCRATCH/lay.onedeed.img" ] &&
           lay_boot cuts again onedeed && disks_ask "disks"; then
            again=$(tr -d '\r' < build/serial.log | sed -nE 's/^Disk ([0-9]+) — [^,]*, 419 MB, GPT, removable$/\1/p' | tail -1)
            [ -n "$again" ] && disks_ask "volumes new $again 0 Again" yes && [ -n "$(lay_laid "$again")" ] &&
                disks_ask "volumes" && rc=0
        fi
        lay_halt cuts || rc=1
    fi
    [ $rc = 0 ]
    chk $? "lay (the cuts booted): the one with no Deed takes a volume laid again, and the halt ends"
    lay_said cuts 'it is mounted on one deed$' "the one whose Deed landed at its head alone is mounted on it"
    n=$(sed -nE 's/^  ([0-9]+)  Cut — ([0-9]+) file\(s\), .*/\2/p' "$SCRATCH/serial.lay.cuts.log" | tail -1)
    [ "${n:-0}" -gt 0 ] && [ "$n" = "$(python3 -c 'import json, sys
print(sum(f["visible"] for f in json.load(open(sys.argv[1]))["files"]))' "$SCRATCH/lay.cuts/final.json" 2>/dev/null)" ]
    chk $? "lay: with every file the laying ended with (${n:-none})"
    lay_whole "$SCRATCH/lay.again.img" 512 "the volume laid again"

    # Fresh alone: the ground it grew by taken, by its name.
    rc=1
    if lay_boot grown d512 &&
       wait_for_line -E build/serial.log 'volume [0-9]+ of the house is called "Fresh"' "$WAIT_QUIET_S" && disks_ask "volumes"; then
        rc=0
    fi
    lay_halt grown || rc=1
    [ $rc = 0 ]
    chk $? "lay (second boot): Fresh is taken in again, and the halt ends"
    lay_said grown 'seat [0-9]+: the volume now runs [0-9]+ sectors; its data run is [0-9]+ blocks, [0-9]+ more than it had' \
        "Fresh takes the ground its partition grew by"
    n=$(lay_holds "$SCRATCH/lay.d512.img" 512 fresh | sed -nE 's/^([0-9]+) .*/\1/p')
    lay_said grown "^  [0-9]+  Fresh — ${n:-x} file\\(s\\)" "with every file it was given (${n:-none})"
    lay_whole "$SCRATCH/lay.d512.img" 512 "Fresh, grown"
}
