# stand_caveat — the Caveat table, its judge, the path every verdict about a
# write names (stand_path) and the medium a volume was seated on
# (stand_seat_kind), for every host harness.
#
# SOURCED, NEVER RUN — by stand.sh, so every stand harness has it the moment
# it has stand.sh, and by tools/stress_matrix.sh directly, which has no
# stand.sh and none of its helpers (ok, bad, harness). So lockrank_unheld and
# all it calls — lockrank_places, lockrank_rows, caveat_row — use none of
# them. Its own file because the table grows with every red test a step of the
# plan is written against, and the machinery beside it should not have to move
# out of its way.

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
# its own tests here. One STEP names no step: `limit`, a row that holds a
# limit the owner accepted and no step will discharge — never lifted, its
# DEFECT the limit and the owner's word for it, given only to a run the limit
# is a limit on, so a test under it that behaves was given the row where it
# stands for nothing (caveat_row_book).
#
# PowerCut (tools/powercut.sh) gives a scenario none of the (scenario, code)
# rows: the book's census (bookcensus.py) knows no third state between a block
# an owner names and a free one, so a code a cut shows is a FAIL until the
# owner assigns it a step. What PowerCut holds are the rows of noanchor's and
# hog's verify — the cadence settles a write — and those of a book's own
# oracles (caveat_row_book).
caveat_row() {
    case "$1" in
        longquery)  echo "|a query of 256 bytes or more is dropped unread, and the answer is every file in scope" ;;
        bigwrite)   echo "|a write is copied through one kernel-heap allocation of its whole size, so one bigger than the heap is refused and nothing reaches the file" ;;
        usememory)  echo "|a Use Context longer than 432 bytes is forgotten, remembered=0" ;;
        fidhistory) echo "|a mount spends kernel heap on every file id ever issued, the dead ones included" ;;
        coldread)   echo "|a read into pages its program had not touched yet reported every byte and delivered only those before the first such page" ;;
        coldwrite)  echo "|a write out of pages its program had not touched yet reported every byte, and the file holds other than the zeros those pages are" ;;
        noanchor)   echo "|a file written without anchor has no name after a power cut" ;;
        book:*)     caveat_row_book "$1" ;;
        hog)        echo "|on one core under a CPU hog nothing settles a write before the power goes" ;;
        lockrank:*) caveat_row_lockrank "$1" ;;
    esac
}

# caveat_row_lockrank NAME — LockRank's places (lockrank_judge, below): every
# key a gate measured, under the step R1 section 1.7 names for it.
caveat_row_lockrank() {
    case "$1" in
        # The writer lock is gone since step 1.5: a write, span and defrag
        # hold the file's Helm, which is no lock and nobody spins for.
        lockrank:writer@tagfs_write_inside:read|\
        lockrank:writer@tagfs_write_inside:write)
                    echo "|a file's writer holds its IRQ-off lock across every medium command of its write, so a second writer of the file spins with interrupts off for the whole of the first one's write" ;;
        lockrank:writer@tagfs_write_inside:flush|\
        lockrank:writer@tagfs_span_file_inside:read|\
        lockrank:writer@tagfs_span_file_inside:write|\
        lockrank:writer@tagfs_span_file_inside:flush|\
        lockrank:writer@tagfs_defrag_file_inside:read|\
        lockrank:writer@tagfs_defrag_file_inside:write|\
        lockrank:writer@tagfs_defrag_file_inside:flush)
                    echo "|a file's writer holds its IRQ-off lock across the medium commands of a span, a defrag and a gather of its extents — the gather's flush among them — so a second writer of the file spins with interrupts off for all of them" ;;
        lockrank:state@tagfs_defrag_file_inside:read|\
        lockrank:state@tagfs_defrag_file_inside:write)
                    echo "|defragmenting a file copies its blocks under the volume's state lock, so every create, delete, rename and tag waits with interrupts off while the medium reads and writes the whole file" ;;
        lockrank:proof@TestStandLockRankNamesTheMedium:read|\
        lockrank:proof@TestStandLockRankNamesTheOrder/proofstate@TestStandLockRankNamesTheOrder)
                    echo "proof|LockRank's self-tests hold a lock of their own across a read, and take one out of order, on purpose" ;;
    esac
}

# caveat_row_book NAME — the rows of a book's own oracles in PowerCut
# (powercut_judge_book.py, powercut_lie.py; spec_v2 §3.10, §16.5 п.21), each
# the defect it stands for: no step — on a book none is known — so each is a
# FAIL that names what came back the day one shows. One row is a limit, not a
# defect a step discharges: book:wiped:fixed, a smudge's wipe on a fixed medium
# (§16.6 п.5) — the settled stitches a spoilt page holds with nothing past
# them, the newest and those before it on its page, up to three in a 16 KiB
# page of 4 KiB atoms (C18) — given only to a run whose volume the Boardroom
# seated as fixed (powercut.sh pc_caveats, stand_seat_kind) and asked only of a
# wipe seen: a fixed medium no smudge wiped behaves under book:wiped.
caveat_row_book() {
    case "$1" in
        book:whole)   echo "|a power cut leaves a file of a book as no flush left it — its old bytes and new mixed, or rotten (C1, C15, C3)" ;;
        book:order)   echo "|a stitch is written before the flush its own word promises its data stands behind, or two stitches share one (§3.3)" ;;
        book:lines)   echo "|a logger's file comes back cut inside a line, or with fewer lines than a flush had made durable (C21)" ;;
        book:slip)    echo "|a logger's tail is written as a block of data for every line instead of riding its stitches as a Slip (C21)" ;;
        book:tipin)   echo "|a logger's Tallies are not laid in, or the newest one breaks the rule of binding (C7-C10)" ;;
        book:spared)  echo "|a stitch whose toll spared its data's flush and lost data it names is taken as though the data were there (C3)" ;;
        book:engross) echo "|a tail engrossed into a block before its stitch is named by somebody when the power goes first (C22)" ;;
        book:chain)   echo "|the verdict a mount lays down on a book is not laid by the session's first stitch, or a block it rolled back is given twice (C16)" ;;
        book:dedup)   echo "|a power cut in the middle of the pass that lays a copy on its original leaves the copy on neither its own blocks nor the original's, or either file not as its write left it (§8.2)" ;;
        book:owners)  echo "|a medium that lost writes a kept flush stood behind leaves a book with two owners of one block (C6, C17)" ;;
        book:lost)    echo "|a medium lost what a stitch named, the evidence on it, and the book says nothing (C5, C13, C17, C18)" ;;
        book:wiped)   echo "|a medium that spoils the page around a stitch in flight wipes a settled stitch whole with nothing past it, and the book cannot say what settled is lost (C18)" ;;
        book:wiped:fixed)
                      echo "limit|on a fixed medium the ribbon keeps the medium's own atom, so a cut that spoils the page around the stitch in flight can wipe the settled stitches that page holds whole with nothing past them — the newest, and those before it on the page — and no reader can tell them from cycles that never wrote — the format's limit on such a medium, as ext4's and NTFS's on the same disks (spec_v2 §16.6 п.5, C18)" ;;
        book:boots)   echo "|a book a medium lied about is mounted otherwise than predicted, or a session on it gives a block twice (C16, C26)" ;;
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
                if [ "$step" = limit ]; then
                    printf '  \033[36mCAVEAT\033[0m caveat held: %s — %s — a limit the owner accepted, which no step discharges (%s)\n' \
                           "$name" "$defect" "$what"
                else
                    printf '  \033[36mCAVEAT\033[0m caveat held: %s — %s — discharged by step %s (%s)\n' \
                           "$name" "$defect" "$step" "$what"
                fi
                CAVEAT=$((CAVEAT+1)) ;;
            behaves)
                if [ "$step" = limit ]; then
                    bad "limit given where it is none: $name — a test held under a limit behaves, so the row was given to a run the limit is not a limit on ($what)"
                else
                    bad "caveat lifted early: $name — the test does not test what it claims, or step $step already landed — lift the caveat ($what)"
                fi ;;
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
# synchronously on one machine and the WriteJob through the port on another,
# and a verdict that does not say which cannot be compared with the next one.
# Read off the kernel's own mount line ("[TagFS] volume on seat N: <seat>",
# tagfs.c) and the WriteJob's own fork (write_job.c, WriteJobSeatRunsAsync: an
# AHCI port or a USB disk, more than one core and a writer in userspace go
# through the port; everything else writes synchronously). Prints one of
# AHCI async (WriteJob) | AHCI sync | ATA sync | USB async (WriteJob) |
# USB sync. A log that mounted nothing, or a seat this does not know, prints
# what it found instead and answers 1, so the path in a verdict is never a
# guess.
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
        usb*)
            if [ "$cores" -gt 1 ]; then echo "USB async (WriteJob)"; else echo "USB sync"; fi ;;
        "")   echo "no path: $log mounted no volume"; return 1 ;;
        *)    echo "no path: seat '$seat' is none this knows"; return 1 ;;
    esac
}

# stand_seat_kind LOG — the medium the volume of the machine that wrote LOG
# was mounted on, as the Boardroom calls its seat: `removable` for a seat it
# calls so ("[Boardroom]   seat N: NAME (removable)", boardroom.c — every USB
# stick, the seat a book's mount lays its ribbon in flash pages on), `fixed`
# for one it seats plainly, read for the seat of the kernel's own mount line
# ("[TagFS] volume on seat N: NAME", tagfs.c). A log that mounted nothing, or
# a seat the Boardroom never named, prints what it found instead and answers
# 1, so the medium in a verdict is never a guess (powercut.sh pc_sign).
stand_seat_kind() {
    local log=$1 line seat name
    [ -f "$log" ] || { echo "no medium: there is no log at $log"; return 1; }
    line=$(tr -d '\r' < "$log" | sed -nE 's/^\[TagFS\] volume on seat ([0-9]+): (.*)$/\1|\2/p' | tail -1)
    [ -n "$line" ] || { echo "no medium: $log mounted no volume"; return 1; }
    seat=${line%%|*} name=${line#*|}
    if tr -d '\r' < "$log" | grep -aqxF "[Boardroom]   seat $seat: $name (removable)"; then
        echo removable
    elif tr -d '\r' < "$log" | grep -aqxF "[Boardroom]   seat $seat: $name"; then
        echo fixed
    else
        echo "no medium: the Boardroom never named seat $seat, $name"; return 1
    fi
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
