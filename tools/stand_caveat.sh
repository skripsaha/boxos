# stand_caveat — the Caveat table, its judge, and the path every verdict about
# a write names (stand_path), for every host harness.
#
# SOURCED BY stand.sh, NEVER RUN, and never sourced by a harness itself: a
# harness has it the moment it has stand.sh. Its own file because the table
# grows with every red test a step of the plan is written against, and the
# machinery beside it should not have to move out of its way.

# ===========================================================================
# A KNOWN DEFECT, REPRODUCED ON PURPOSE, IS HELD — NOT PASSED, NOT SKIPPED
# ===========================================================================
#
# A test written for a defect that a later step of the plan discharges has two
# honest answers before that step: the defect was SEEN, with a number measured
# at run time, or the test is broken. Counting the first as a FAIL keeps every
# run red until the step lands and teaches everyone to stop reading the
# colour; counting it as a PASS, or skipping it, lets the test rot unwatched,
# and a test that has quietly stopped seeing its defect is then green on the
# day the step claims to have fixed it.
#
# So a test OBSERVES and this JUDGES, in the same words as the kernel's own
# startup tests. The observation is one of three: `defect` (the named defect,
# seen positively), `behaves` (the correct behaviour, seen), or `fail` (anything
# else — a broken test must never hide behind a caveat). A caveated test that
# behaves is a FAIL as well: it does not test what it claims, or its step has
# landed and the caveat has to go. Held caveats are counted in CAVEAT and never
# change a run's exit status.
CAVEAT=0

# THE table, and the only one: NAME -> "STEP|DEFECT". A row names a defect
# found red on HEAD, never one that might be. It is lifted by deleting its STEP
# alone, in the commit of the step it names, and the DEFECT stays for good as
# what the test stands for: the day the defect is back, the FAIL says which
# one it was ("|DEFECT" is a test with no caveat). A harness adds the rows of
# its own tests here.
#
# PowerCut's census (tools/powercut.sh): every (scenario, code) pair a
# scenario was measured showing is its own row, held under 1.6 — except the
# ORPHAN an unanchored create leaves (noanchor, hog) and race's anchored file
# lost by a later torn sync, under 1.4 (the owner, 2026-09-27). chaingrow
# keeps its census by kind, (scenario, prefix|subset, code), and a chain
# refused by the chain's name: its refusals are MP-D5, under 1.1 — the subset
# rows too, because step 1.1 links a new block only once a flush has covered
# its stamp (the owner, 2026-09-28), which closes the epoch they fall in.
caveat_row() {
    case "$1" in
        longquery)  echo "1.1b|a query of 256 bytes or more is dropped unread, and the answer is every file in scope" ;;
        bigwrite)   echo "1.3|a write is copied through one kernel-heap allocation of its whole size, so one bigger than the heap is refused and nothing reaches the file" ;;
        sequential|interleave)
                    echo "1.3|a file's record takes one extent per 4 KiB written and never merges them, so the 4080-byte record of a 10-byte name refuses its write at byte 2748416, after 671 extents" ;;
        usememory)  echo "1.6|a Use Context longer than 432 bytes is forgotten, remembered=0" ;;
        fidhistory) echo "1.3b|a mount spends kernel heap on every file id ever issued, the dead ones included" ;;
        systemtag)  echo "1.4|any program may take the system tag off kernel.bin and then delete it — the kernel the volume boots is gone" ;;
        coldread)   echo "|a read into pages its program had not touched yet reported every byte and delivered only those before the first such page" ;;
        coldwrite)  echo "|a write out of pages its program had not touched yet reported every byte, and the file holds other than the zeros those pages are" ;;
        noanchor)   echo "1.7|a file written without anchor has no name after a power cut" ;;
        nextfid)    echo "1.6|a table written ahead of its ledger hands the same file id out twice" ;;
        hog)        echo "1.7|on one core under a CPU hog nothing settles a write before the power goes" ;;
        settlecut:FID_PAST_NEXT)
                    echo "1.6|a sync stopped by SETTLECUT=ledger before its ledger leaves a table row past the ledger's next_file_id" ;;
        noanchor:ORPHAN|hog:ORPHAN)
                    echo "1.4|a create never anchored, cut by the power, leaves a block marked used that nothing owns — the map is written only inside Settle" ;;
        race:durable)
                    echo "1.4|an anchored file is lost by a later torn sync — the shutdown sync retires its record in place" ;;
        nextfid:FID_PAST_NEXT|halt:FID_PAST_NEXT|race:FID_PAST_NEXT|chaingrow:prefix:FID_PAST_NEXT|chaingrow:subset:FID_PAST_NEXT)
                    echo "1.6|a sync cut between its table and its ledger leaves a table row past the ledger's next_file_id" ;;
        nextfid:FILE_LOST|halt:FILE_LOST|race:FILE_LOST|chaingrow:subset:FILE_LOST)
                    echo "1.6|a sync cut between its writes leaves a table row naming a record the pool never got, and the mount deletes the file" ;;
        nextfid:MISSING|halt:MISSING|race:MISSING|chaingrow:subset:MISSING)
                    echo "1.6|a sync cut between its writes leaves a named block reading free, for the allocator to hand out again" ;;
        nextfid:ORPHAN|halt:ORPHAN|race:ORPHAN)
                    echo "1.6|a sync cut between its writes leaves blocks marked used that nothing owns" ;;
        nextfid:TAG_ABSENT|halt:TAG_ABSENT|race:TAG_ABSENT|chaingrow:subset:TAG_ABSENT)
                    echo "1.6|a sync cut between its writes leaves a file wearing a tag the registry does not define" ;;
        nextfid:UNNAMED_RECORD|halt:UNNAMED_RECORD|race:UNNAMED_RECORD)
                    echo "1.6|a sync cut between its writes leaves an ACTIVE record no table row names, its blocks leaked" ;;
        halt:CHAIN_REFUSED|race:CHAIN_REFUSED)
                    echo "1.6|a cut inside the shutdown sync keeps a pool link to a block none of whose writes landed, and the kernel refuses to mount the volume" ;;
        halt:SPARE_UNNAMED|race:SPARE_UNNAMED)
                    echo "1.6|a cut keeps a stamped spare header whose link was lost, a block nothing names" ;;
        race:TAG_DUPLICATE)
                    echo "1.6|a registry write torn by the cut leaves a record repeating another's key, and it is dropped" ;;
        chaingrow:prefix:CHAIN_REFUSED:table)
                    echo "1.1|the file table writes its last block's link to a block it has just taken before that block, so a power cut between the two leaves a chain into unstamped ground and the kernel refuses the volume at every mount" ;;
        chaingrow:prefix:CHAIN_REFUSED:registry)
                    echo "|the tag registry writes its last block's link to a block it has just taken before that block, so a power cut between the two leaves a chain into unstamped ground and the kernel refuses the volume at every mount" ;;
        chaingrow:subset:CHAIN_REFUSED:table)
                    echo "1.1|a cut that keeps the file table's link to its new block and loses that block's own write, both in one flush epoch, leaves a chain into unstamped ground the kernel refuses to mount" ;;
        chaingrow:subset:CHAIN_REFUSED:registry)
                    echo "|a cut that keeps the tag registry's link to its new block and loses that block's own write, both in one flush epoch, leaves a chain into unstamped ground the kernel refuses to mount" ;;
        chaingrow:prefix:ORPHAN|chaingrow:subset:ORPHAN)
                    echo "1.6|a cut that keeps the bitmap of a block the registry or the file table has just taken and loses the link to it leaves that block marked used and owned by nothing" ;;
        chaingrow:prefix:SPARE_UNNAMED|chaingrow:subset:SPARE_UNNAMED)
                    echo "1.6|a cut after the registry's new block is stamped and before the link to it is written keeps a stamped spare header nothing names, a block marked used and owned by nothing" ;;
        # LockRank's places (lockrank_judge, below): every key a gate measured,
        # under the step R1 section 1.7 names for it.
        lockrank:registry@tag_registry_load:read)
                    echo "|the tag registry is read from the medium under its IRQ-off lock, so a core that looks up a tag spins with interrupts off for as long as the medium takes" ;;
        lockrank:registry@tag_registry_flush:read|\
        lockrank:registry@tag_registry_flush:write)
                    echo "|the tag registry's flush holds its IRQ-off lock across every medium read and write it makes, so a core that needs the registry spins with interrupts off for as long as the medium takes" ;;
        lockrank:registry@tag_registry_load/readahead@ReadAheadLookup|\
        lockrank:registry@tag_registry_load/integrity@IntegrityVerify|\
        lockrank:registry@tag_registry_load/braid@BraidIsHealthy|\
        lockrank:registry@tag_registry_flush/readahead@ReadAheadLookup|\
        lockrank:registry@tag_registry_flush/readahead@ReadAheadInvalidate|\
        lockrank:registry@tag_registry_flush/integrity@IntegrityUpdate|\
        lockrank:registry@tag_registry_flush/braid@BraidIsHealthy)
                    echo "|a block is read or written for the tag registry with the registry's lock held, so the read-ahead cache's, the integrity map's and Braid's locks are taken under that leaf" ;;
        lockrank:registry@tag_registry_flush/state@tagfs_alloc_blocks_inside|\
        lockrank:registry@tag_registry_flush/state@MapOwedSettle)
                    echo "|the tag registry takes a block for its chain under its own lock, so the volume's state lock is taken under the registry while a rename takes the registry under state: the two orders of one ABBA (MP-D4), which stops both cores" ;;
        lockrank:table@file_table_flush:read|\
        lockrank:table@file_table_flush:write)
                    echo "1.1|the file table's flush holds its IRQ-off lock across every medium read and write it makes, so a core that looks up a file spins with interrupts off for as long as the medium takes" ;;
        lockrank:table@file_table_flush/readahead@ReadAheadLookup|\
        lockrank:table@file_table_flush/readahead@ReadAheadInvalidate|\
        lockrank:table@file_table_flush/integrity@IntegrityUpdate|\
        lockrank:table@file_table_flush/braid@BraidIsHealthy)
                    echo "1.1|a block is read or written for the file table with the table's lock held, so the read-ahead cache's, the integrity map's and Braid's locks are taken under that leaf" ;;
        lockrank:table@file_table_flush/state@tagfs_alloc_blocks_inside|\
        lockrank:table@file_table_flush/state@MapOwedSettle)
                    echo "1.1|the file table takes a block for its chain under its own lock, so the volume's state lock is taken under the table while a create takes the table under state: the two orders of one ABBA (MP-D4), which stops both cores" ;;
        lockrank:integrity@IntegrityVerify:read|\
        lockrank:integrity@IntegrityVerify:write|\
        lockrank:integrity@IntegrityUpdate:read|\
        lockrank:integrity@IntegrityUpdate:write|\
        lockrank:integrity@IntegrityFlush:write)
                    echo "1.1|a page of the integrity map is read or written under the map's IRQ-off lock, from inside every block read and write, so every core that reads or writes a block spins with interrupts off for as long as the medium takes" ;;
        lockrank:integrity@IntegrityVerify/readahead@ReadAheadLookup|\
        lockrank:integrity@IntegrityVerify/readahead@ReadAheadInvalidate|\
        lockrank:integrity@IntegrityUpdate/readahead@ReadAheadInvalidate|\
        lockrank:integrity@IntegrityUpdate/readahead@ReadAheadLookup|\
        lockrank:integrity@IntegrityFlush/readahead@ReadAheadInvalidate|\
        lockrank:integrity@IntegrityVerify/braid@BraidIsHealthy|\
        lockrank:integrity@IntegrityUpdate/braid@BraidIsHealthy|\
        lockrank:integrity@IntegrityFlush/braid@BraidIsHealthy)
                    echo "1.1|a page of the integrity map is read or written with the map's lock held, so the read-ahead cache's and Braid's locks are taken under that leaf" ;;
        lockrank:ledger@tagfs_write_ledger:write|\
        lockrank:ledger@tagfs_remember_use_context:write)
                    echo "1.1|the Ledger is written to the medium under its IRQ-off lock, so a core that reads the Ledger spins with interrupts off for as long as the medium takes" ;;
        lockrank:diskbook@DiskBookCheckpoint:write)
                    echo "1.1|the DiskBook's checkpoint writes its header to the medium under its IRQ-off lock, and a volume being taken down asks for it though nothing else of the take-down writes" ;;
        lockrank:cow@TagFS_CowIsActive:read)
                    echo "1.1|with no snapshot of a file standing, asking whether it is copied on write reads its metadata off the medium under the copy-on-write lock, so every core that writes spins with interrupts off for as long as the medium takes (CR-10)" ;;
        lockrank:cow@TagFS_CowAfterWrite:flush)
                    echo "1.1|every 64th redirect the log takes pushes the medium's cache from inside the copy-on-write lock, with interrupts off, on the drain core that waits for completions only its own interrupt returns (CR-6)" ;;
        lockrank:cow@TagFS_CowBeforeWrite:read|\
        lockrank:cow@TagFS_CowBeforeWrite:write)
                    echo "1.3|a write into a block a snapshot keeps copies the block under the copy-on-write lock, so every core that writes spins with interrupts off while the medium reads and writes the copy" ;;
        lockrank:cow@TagFS_CowAfterWrite:write|\
        lockrank:diskbook@DiskBookLogRedirect:write)
                    echo "1.3|the copy a snapshot keeps is logged in the DiskBook's redirect log under the copy-on-write and DiskBook locks, a medium write with both held" ;;
        lockrank:cow@cow_resync_redirect_journal:write|\
        lockrank:diskbook@DiskBookResetRedirects:write)
                    echo "1.3|deleting a snapshot rewrites the DiskBook's header under the copy-on-write and DiskBook locks, a medium write with both held" ;;
        lockrank:state@meta_pool_flush_retires:read|\
        lockrank:state@meta_pool_flush_retires:write)
                    echo "1.4|the sync gives retired metadata records back under the volume's state lock, reading and writing pool blocks while every create, delete, rename and tag waits with interrupts off" ;;
        lockrank:state@tagfs_create_file_inside:read|\
        lockrank:state@tagfs_create_file_inside:write|\
        lockrank:state@tagfs_delete_file_inside:read|\
        lockrank:state@tagfs_remove_tag_inside:read|\
        lockrank:state@tagfs_remove_tag_inside:write|\
        lockrank:state@tagfs_rename_file_inside:read|\
        lockrank:state@tagfs_rename_file_inside:write|\
        lockrank:state@tagfs_add_tag_inside:read|\
        lockrank:state@tagfs_add_tag_inside:write)
                    echo "1.4|a create, a delete, a rename, a tag or its removal reads and writes metadata pool blocks under the volume's state lock, so every other create, delete, rename and tag waits with interrupts off for as long as the medium takes" ;;
        lockrank:writer@tagfs_write_inside:read|\
        lockrank:writer@tagfs_write_inside:write)
                    echo "1.5|a file's writer holds its IRQ-off lock across every medium command of its write, so a second writer of the file spins with interrupts off for the whole of the first one's write" ;;
        lockrank:state@tagfs_defrag_file_inside:read|\
        lockrank:state@tagfs_defrag_file_inside:write)
                    echo "1.5|defragmenting a file copies its blocks under the volume's state lock, so every create, delete, rename and tag waits with interrupts off while the medium reads and writes the whole file" ;;
        lockrank:proof@TestStandLockRankNamesTheMedium:read|\
        lockrank:proof@TestStandLockRankNamesTheOrder/proofstate@TestStandLockRankNamesTheOrder)
                    echo "proof|LockRank's self-tests hold a lock of their own across a read, and take one out of order, on purpose" ;;
    esac
}

# caveat_step NAME / caveat_defect NAME — the two halves of NAME's row; both
# empty for a test with no row, the step alone empty for one whose caveat was
# lifted.
caveat_step()   { local row; row=$(caveat_row "$1"); echo "${row%%|*}"; }
caveat_defect() { local row; row=$(caveat_row "$1"); echo "${row#*|}"; }

# caveat_judge NAME OBSERVED WHAT — the one verdict. WHAT carries the numbers
# the run measured, and for a test that writes it ends with " [path: …]" from
# stand_path, because a verdict about a write is a verdict about one path.
caveat_judge() {
    local name=$1 seen=$2 what=$3 step defect
    case "$seen" in
        defect|behaves|fail) ;;
        *) harness "caveat_judge $name: '$seen' is not an observation — defect, behaves or fail ($what)"
           return ;;
    esac
    step=$(caveat_step "$name")
    defect=$(caveat_defect "$name")
    if [ -n "$step" ]; then
        case "$seen" in
            defect)
                printf '  \033[36mCAVEAT\033[0m caveat held: %s — %s — discharged by step %s (%s)\n' \
                       "$name" "$defect" "$step" "$what"
                CAVEAT=$((CAVEAT+1)) ;;
            behaves)
                bad "caveat lifted early: $name — the test does not test what it claims, or step $step already landed — lift the caveat ($what)" ;;
            fail)
                bad "$name: $what" ;;
        esac
        return
    fi
    case "$seen" in
        behaves) ok "$name: $what" ;;
        defect)  bad "$name failed: the defect it stands for is back — ${defect:+$defect (}$what${defect:+)}" ;;
        fail)    bad "$name: $what" ;;
    esac
}

# stand_path LOG CORES — the way a userspace write went down to the medium on
# the machine that wrote LOG, booted with CORES cores. A red test that writes
# is evidence about ONE path: the same test takes the legacy channel
# synchronously on one machine and the AHCI WriteJob on another, and a verdict
# that does not say which cannot be compared with the next one. Read off the
# kernel's own mount line ("[TagFS] volume on seat N: <seat>", tagfs.c) and
# ObjWrite's own fork (storage_ops.c: AHCI, more than one core and a writer in
# userspace go through the WriteJob; everything else writes synchronously).
# Prints one of AHCI async (WriteJob) | AHCI sync | ATA sync | USB sync. A log
# that mounted nothing, or a seat this does not know, prints what it found
# instead and answers 1, so the path in a verdict is never a guess.
stand_path() {
    local log=$1 cores=$2 seat
    case "$cores" in
        ''|*[!0-9]*) echo "no path: '$cores' is not a number of cores"; return 1 ;;
    esac
    [ -f "$log" ] || { echo "no path: there is no log at $log"; return 1; }
    seat=$(tr -d '\r' < "$log" | sed -nE 's/^\[TagFS\] volume on seat [0-9]+: (.*)$/\1/p' | tail -1)
    case "$seat" in
        "AHCI port "*)
            if [ "$cores" -gt 1 ]; then echo "AHCI async (WriteJob)"; else echo "AHCI sync"; fi ;;
        ATA*) echo "ATA sync" ;;
        usb*) echo "USB sync" ;;
        "")   echo "no path: $log mounted no volume"; return 1 ;;
        *)    echo "no path: seat '$seat' is none this knows"; return 1 ;;
    esac
}

# ===========================================================================
# LOCKRANK — every place the kernel names is held by a row, under its step
# ===========================================================================
#
# The kernel names, once per boot, each place where a TagFS lock is held
# across a command to the medium or a wait for a Scribe, and each take that
# breaks the lock order (src/kernel/tagfs/lock_rank). A place is a key:
#
#   <name>@<fn>:<op>                 a lock held across the medium (read, write,
#                                    flush) or a wait for a Scribe (wait)
#   <outer>@<fn1>/<inner>@<fn2>      <inner> taken in <fn2> under <outer>
#
# and every key has its row in the table above, "lockrank:<key>", under the
# step of the plan that removes the place — rows written only for keys a gate
# MEASURED. A key with no row is a place nobody knew of, and a FAIL; a key
# whose row lost its step is a place that came back, and a FAIL; any other
# kind of line (the self-deadlock, a lock with no rank, a ninth lock, a
# let-go of a lock nobody took, the table full), and a key said twice in one
# boot, are the mechanism itself broken, and a FAIL. A row with the step
# "proof" holds the kernel's own self-tests of LockRank (the stand suite). A
# row that is not seen says nothing: a path not walked names nothing.

# lockrank_rows LOGS — "LOG|KEY|LINE" for every [LockRank] line of each log
# (LOGS is one argument, the paths separated by spaces); KEY is empty for a
# line that names no place a row can hold.
lockrank_rows() {
    local f
    for f in $1; do
        [ -f "$f" ] || continue
        tr -d '\r' < "$f" | grep -a '^\[LockRank\] ' | sed -E \
            -e 's/^(\[LockRank\] ([a-z]+) is held across a medium (read|write|flush): taken in ([A-Za-z0-9_]+), the (read|write|flush) asked for by [^ ]+)$/\2@\4:\3|\1/' -e t \
            -e 's/^(\[LockRank\] ([a-z]+) is held across a wait for a Scribe: taken in ([A-Za-z0-9_]+), the wait asked for by [^ ]+)$/\2@\3:wait|\1/' -e t \
            -e 's/^(\[LockRank\] ([a-z]+) taken in ([A-Za-z0-9_]+) under ([a-z]+), taken in ([A-Za-z0-9_]+) — out of order)$/\4@\5\/\2@\3|\1/' -e t \
            -e 's/^/|/' | awk -v f="$f" '{ print f "|" $0 }'
    done
}

# lockrank_places LOGS — the rows folded: "form|LOG|LINE" for a line with no
# place, "twice|LOG|LINE" for a place one boot named again, and one
# "key|KEY|BOOTS|LINE" per place: how many of the logs named it, and the first
# line that did.
lockrank_places() {
    lockrank_rows "$1" | awk -F'|' '
        $2 == ""           { print "form|" $1 "|" $3; next }
        (($1, $2) in seen) { print "twice|" $1 "|" $3; next }
                           { seen[$1, $2] = 1; boots[$2]++
                             if (!($2 in first)) { first[$2] = $3; order[++n] = $2 } }
        END { for (i = 1; i <= n; i++) print "key|" order[i] "|" boots[order[i]] "|" first[order[i]] }'
}

# lockrank_judge LOGS WHAT — every line of the logs judged: a place held under
# its step is a CAVEAT, once per place however many of the boots named it;
# anything else is a FAIL, said with its line.
lockrank_judge() {
    local logs=$1 what=$2 f boots=0 held=0 proof=0 fail0=$FAIL kind a b c row
    for f in $logs; do
        if [ -f "$f" ]; then boots=$((boots + 1)); else harness "lockrank_judge: there is no log at $f ($what)"; fi
    done
    while IFS='|' read -r kind a b c; do
        case "$kind" in
            form)  bad "LockRank: $what — ${a##*/} says what no place of a row can be: $b" ;;
            twice) bad "LockRank: $what — ${a##*/} names one place twice in one boot: $b" ;;
            key)
                row=$(caveat_row "lockrank:$a")
                if [ -z "$row" ]; then
                    bad "LockRank names a place no row holds: $a — $c ($what, named in $b of $boots boot(s))"
                elif [ "${row%%|*}" = proof ]; then
                    proof=$((proof + 1))
                else
                    held=$((held + 1))
                    caveat_judge "lockrank:$a" defect "$what, named in $b of $boots boot(s): $c"
                fi ;;
        esac
    done <<LOCKRANK_EOF
$(lockrank_places "$logs")
LOCKRANK_EOF
    if [ "$FAIL" = "$fail0" ]; then
        ok "LockRank: $what — every place named in $boots boot(s) is held by its row ($held under a step, $proof the self-tests' own)"
    fi
}

# lockrank_unheld LOG — for the matrix, which has no ok or bad: every line of
# LOG that no row holds — a place with no row or whose row lost its step, a
# line with no place, a place named twice — one per line, for it to count.
lockrank_unheld() {
    local kind a b c row
    lockrank_places "$1" | while IFS='|' read -r kind a b c; do
        case "$kind" in
            form|twice) echo "$b" ;;
            key) row=$(caveat_row "lockrank:$a")
                 [ -n "${row%%|*}" ] || echo "$c" ;;
        esac
    done
}
