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
                    echo "1.1|the tag registry writes its last block's link to a block it has just taken before that block, so a power cut between the two leaves a chain into unstamped ground and the kernel refuses the volume at every mount" ;;
        chaingrow:subset:CHAIN_REFUSED:table)
                    echo "1.1|a cut that keeps the file table's link to its new block and loses that block's own write, both in one flush epoch, leaves a chain into unstamped ground the kernel refuses to mount" ;;
        chaingrow:subset:CHAIN_REFUSED:registry)
                    echo "1.1|a cut that keeps the tag registry's link to its new block and loses that block's own write, both in one flush epoch, leaves a chain into unstamped ground the kernel refuses to mount" ;;
        chaingrow:prefix:ORPHAN|chaingrow:subset:ORPHAN)
                    echo "1.6|a cut that keeps the bitmap of a block the registry or the file table has just taken and loses the link to it leaves that block marked used and owned by nothing" ;;
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
