# logcheck_room — a book's roomback by the kernel's word:
# the GiB written the moment a GiB was deleted on a full book, judged by what
# the kernel says it keeps and holds at each refusal (fullvolume), and the
# judge turned by a write that waits on no Tally (roombackmut).
#
# SOURCED, NEVER RUN — by logcheck.sh; ok, bad, chk, harness, build, mutate,
# mut_done, restore and SCRATCH are stand.sh's, fullvolume_leg,
# FULLVOLUME_STRETCH and FULLVOLUME_MIB logcheck.sh's. Its own file for the
# reason logcheck_mkfs.sh has one: logcheck.sh is twelve thousand lines.

# The kernel's refusal at the wall, said as a write's wait for a Tally ends —
# of a file's data (allotment.c, AllotmentBookShort) or of the room its record
# takes on the ribbon (ribbon.c, RibbonRefusedSaid): what was free, what the
# book keeps and holds itself, named, and how the reserve was measured. Either
# may end the write: the one the ribbon ends is weighed by its own word, never
# by a line of the data's said before it, nor taken for none. The ribbon run
# out under the Settle's own stitch (RibbonOutSaid) ends no change's wait and
# is no refusal of one.
ROOMBACK_REFUSED='\[TagFS\] no (block can be had for [0-9]+ block\(s\) of a file.s data|room on the ribbon for a change of the volume): .*'

# roomback_exact MEASURE — 0 when a refusal's reserve was measured as a write
# may be refused at the wall (§16.6 п.8, binder.c BinderRoomMeasured): to the
# block by the last Tally, or from above once a turn of the ring was waited for.
roomback_exact() {
    case "$1" in
        "to the block by its last Tally"|*", a turn of the ring waited for") return 0 ;;
    esac
    return 1
}

# roomback_book_judge L VERDICT — the guest's VERDICT of roomback on a book
# (`PASS — …` or `PENDING — …`, the word taken off) weighed by the log L: the
# GiB written the moment a GiB was deleted waits for the Tally that makes the
# reserve's measure exact, or for a turn of the ring, and is refused no-space
# only past them — so the last refusal after the delete says its reserve
# measured to the block, or a turn of the ring waited for, with no more free
# than the book keeps and the tails are promised — and the write took all of
# the GiB but the ground that was not data's grew by since the volume was
# first full: what was free and what the book holds itself, each as the
# kernel named it at its last refusal before the volume was full and its last
# after, up to the guest's verdict — the refusal said as the wait for a Tally
# ends, never a line of the halt's past it. The next Tally
# measured to the block, the Tallies, their pair and the ribbon of a book that
# took in a file laid finer in the ground of the one deleted (§16.4 п.7) —
# named by the Binder, each a Quire it holds (binder_slots.c,
# BinderGroundHeld), never what is left of the data run past the free and the
# files, which held a leak as well as the book and made any refusal its own
# excuse. Free, not kept: what the book keeps is a measure, and the ground
# under it is what was free — at the first wall a Tally's pair taken out of the
# reserve between two of the fill's refusals left 4352 free under a reserve
# of 4864, and a judge by the reserve held that write 512 blocks short of the
# ground the second wall had back, or not, as the Tally fell before the last
# refusal or after it. The MiB _qra.bin was laid past the GiB is the room for
# the rest. What it took reads back whole.
#
# ‼ THE FILL IS A WRITE AT THE WALL TOO, held to the same law: the volume is
# first full only where the fill's last refusal was measured to the block or
# past a turn of the ring, with no more free than the book keeps and promises.
# The fill writes _qrb.bin without a pause, so every refusal it meets comes
# with the piece it inked just before past the last Tally's slice — growth the
# measure holds (endpaper_fact.c, SinceSaid) — and the reserve kept from above
# counts each block handed since its kept measure at its widest, a run of its
# own in FILES and an element of its own in DIGESTS (endpaper.c,
# EndpaperKeptCost): the fill is short of room only by the measure of growth
# whenever it reaches the wall, the very write §16.6 п.8 makes wait. Measured
# on this volume: from above the book keeps 8448 and 8960 where the Tally's
# exact measure keeps 4864, and a fill refused there without its wait stops
# 3584 to 4096 blocks short of a volume that had them — the margin the kernel
# whose write waits on no Tally (roombackmut) then wrote the whole GiB into,
# twice, while a judge of the GiB's write alone found nothing short. A first
# wall measured exactly leaves that kernel no margin, and the GiB's own write
# reaches the wall growing by the same measure. 0 when it holds, 1 when it was
# weighed and does not, 2 when the guest or the kernel never said what it
# weighs; ROOMBACK_SAID what was weighed, ROOMBACK_TOOK the bytes it took.
roomback_book_judge() {
    local L="$1" verdict="$2" at end w1 w2 f1 k1 h1 p1 m1 f2 k2 h2 p2 m2 took want refused good anchor allowed
    local named='s/.* ([0-9]+)( block\(s\))? free, the book keeps ([0-9]+) for its ribbon and its Tally and holds ([0-9]+) itself in its Tallies and its ribbon, and ([0-9]+) are promised.*/\1 \3 \4 \5/p'
    local measured='s/.* — the reserve measured (.*) \(ERR_TAGFS_NO_SPACE\)$/\1/p'
    ROOMBACK_TOOK=$((FULLVOLUME_MIB << 20))
    at=$(tr -d '\r' < "$L" | grep -anE '\[QUILL\] roomback: the volume is full — ' | tail -1 | cut -d: -f1)
    if [ -z "$at" ]; then
        ROOMBACK_SAID="the guest never said the volume was full, so there is no first wall to weigh (${verdict#*— })"
        return 2
    fi
    w1=$(tr -d '\r' < "$L" | head -n "$at" | grep -aoE "$ROOMBACK_REFUSED" | tail -1)
    read -r f1 k1 h1 p1 <<<"$(echo "$w1" | sed -nE "$named")"
    m1=$(echo "$w1" | sed -nE "$measured")
    if [ -z "$p1" ] || [ -z "$m1" ]; then
        ROOMBACK_SAID="the volume was full and the kernel's word of what was free, what the book keeps and holds and how it was measured at its first wall is not there (${w1:-none})"
        return 2
    fi
    if ! roomback_exact "$m1"; then
        ROOMBACK_SAID="the volume was first full with $f1 free where the book keeps $k1 and $p1 are promised, its reserve measured $m1 — the fill refused by the measure of its growth with no Tally waited for to make it exact, and the GiB after it weighed against the ground the fill left (${verdict#*— })"
        return 1
    fi
    if [ "$f1" -gt $((k1 + p1)) ]; then
        ROOMBACK_SAID="the volume was first full with $f1 free, more than the $k1 the book keeps and the $p1 promised, its reserve measured $m1 — the fill refused room it had (${verdict#*— })"
        return 1
    fi
    ROOMBACK_SAID="every byte, read back whole, the volume first full by its reserve measured $m1 (${verdict#*— })"
    case "$verdict" in
        PASS*) return 0 ;;
        FAIL*) ROOMBACK_SAID="the guest's own verdict of the GiB is FAIL, past a first wall measured $m1 — ${verdict#*— }"
               return 2 ;;
    esac
    end=$(tr -d '\r' < "$L" | grep -anE '\[QUILL\] roomback: (PASS|PENDING|FAIL) — ' | tail -1 | cut -d: -f1)
    w2=$(tr -d '\r' < "$L" | head -n "${end:-0}" | tail -n +"${at:-1}" | grep -aoE "$ROOMBACK_REFUSED" | tail -1)
    read -r f2 k2 h2 p2 <<<"$(echo "$w2" | sed -nE "$named")"
    m2=$(echo "$w2" | sed -nE "$measured")
    read -r took want refused good anchor <<EOF_ROOMBACK
$(echo "$verdict" | sed -nE 's/^PENDING — .*took ([0-9]+) of ([0-9]+) bytes .* refused (-?[0-9]+); read back good to byte ([0-9]+) \(read -?[0-9]+, anchor (-?[0-9]+)\).*/\1 \2 \3 \4 \5/p')
EOF_ROOMBACK
    if [ -z "$took" ] || [ -z "$p2" ] || [ -z "$m2" ]; then
        ROOMBACK_SAID="the write of a GiB on the book came short, and the guest's verdict or the kernel's word of what was free, what the book keeps and holds and how it was measured (before the volume was full: ${w1:-none}; after: ${w2:-none}) does not say by how much (${verdict#*— })"
        return 2
    fi
    ROOMBACK_TOOK=$took
    allowed=$(( f2 + h2 > f1 + h1 ? (f2 + h2 - f1 - h1) * 4096 : 0 ))
    ROOMBACK_SAID="a GiB written the moment a GiB was deleted on a book took $took of $want bytes, all but the $(( (want - took) / 4096 )) block(s) of the $(( allowed / 4096 )) the free and what the book holds grew by since the volume was first full ($f2 free and $h2 held, against $f1 and $h1; it keeps $k2, $p2 promised), refused no-space ($refused) with its reserve measured $m2, and read back whole to byte $good"
    roomback_exact "$m2" && [ "$f2" -le $((k2 + p2)) ] && [ "$refused" = -852 ] && [ "$good" = "$took" ] && [ "$anchor" = 0 ] &&
        [ "$took" -ge $((want - allowed)) ]
}

# fullvolume_roomback_book L VERDICT — roomback on a book, judged
# (roomback_book_judge): its bytes in the caller's TOOK.
fullvolume_roomback_book() {
    roomback_book_judge "$1" "$2"
    chk $? "fullvolume: roomback — $ROOMBACK_SAID"
    TOOK=$ROOMBACK_TOOK
}

# roombackmut — the judge of a book's roomback turns on a write that waits on
# no Tally: BinderRoomWait answering false, a write short of room only by the
# measure of growth past the last Tally is refused no-space at once — the
# fill at the first wall, whose refusal says the reserve measured from above
# with no Tally waited for, and the GiB after the delete wherever the fill's
# wall left it no margin — the very kernel §16.6 п.8 put the wait in, whose
# GiB came 1280 blocks short of a volume that had them.
roomback_wait_on() {
    mutate src/kernel/tagfs/settle/binder.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/settle/binder.c"
s = open(p).read()
a = "bool BinderRoomWait(void)\n{\n"
assert s.count(a) == 1, "roombackmut anchor missing"
s = s.replace(a, a + "    return false;   /* logcheck mutation: a write short of room waits on no Tally */\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/tagfs/settle/binder.c "logcheck mutation"
}

run_roombackmut() {
    echo "== roombackmut: a write at a full book's wall that waits on no Tally turns roomback's judge =="
    local L="$SCRATCH/serial.fullvolume.room.log" verdict
    roomback_wait_on
    build STRICT=on
    fullvolume_leg room "$FULLVOLUME_STRETCH" "quilltest roomback $FULLVOLUME_MIB" roomback
    restore src/kernel/tagfs/settle/binder.c
    rm -f "$SCRATCH/fullvolume.room.img"
    verdict=$(tr -d '\r' < "$L" | grep -aoE '\[QUILL\] roomback: (PASS|PENDING|FAIL) — .*' | tail -1)
    if [ -z "$verdict" ]; then
        harness "roombackmut: roomback gave no verdict on the kernel whose write waits on no Tally (last: $(tr -d '\r' < "$L" | grep -aE '^\[(QUILL|TagFS)\]' | tail -1 | cut -c1-160))"
    else
        roomback_book_judge "$L" "${verdict#\[QUILL\] roomback: }"
        case $? in
            0) bad "roombackmut: a write that waits on no Tally holds roomback's judge — $ROOMBACK_SAID" ;;
            1) ok "roombackmut: a write that waits on no Tally turns roomback's judge — $ROOMBACK_SAID" ;;
            *) harness "roombackmut: roomback's judge had nothing to weigh on the kernel whose write waits on no Tally — $ROOMBACK_SAID" ;;
        esac
    fi
    build   # leave the tree built plain, as every scenario here does
}
