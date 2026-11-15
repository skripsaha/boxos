#!/bin/bash
# gates — every gate this tree is held to, one after another, one line each.
#
#     tools/gates.sh                  all of them
#     tools/gates.sh healthy stand    only these, in the order given
#     tools/gates.sh --list           their names, one to a line
#     tools/gates.sh loggerhour       a long gate (GATES_LONG), run by name only:
#                                     a logger of a line a second for an hour,
#                                     about 65 minutes, on a book's record;
#                                     LOGGER_SECONDS=120 for a short one
#     tools/gates.sh census           CENSUS_LANDINGS="B59 B65": a book's
#                                     census of those landings alone, said in
#                                     a HARNESS line — never green
#
#     OUT=DIR     where each gate's output (NAME.out), whatever it left changed
#                 in the tree (NAME.dirty) and the SUMMARY go. Unset, a fresh
#                 directory under TMPDIR, named on the first line.
#     TMPDIR=DIR  the harnesses' scratch: stand.sh keeps its books under it.
#     TREE=DIR    the checkout the gates run in, judged by its OWN harnesses.
#                 Unset, the checkout this file is in.
#
# The GATES, every one on the book the build makes: the logcheck and PowerCut
# scenarios — the book's PowerCut matrix among them (overwrite, logger, tipin,
# sessions, replug, machineb) — then framerefuse, bookboot (the kernel BOOT
# names held to the book by Gangway before the jump), dedup (a copy laid on its
# original by the pass after a Tally), and volume-read's self-test over the
# image the build lays.
# Beside them the host's own stands, which no volume touches: boxasm, the
# in-BoxOS assembler and linker held to GNU ld and tools/nameplate; format,
# BoxLib's printf conversions and snprintf's bound held to the host's C
# library and to exact arithmetic; form, BoxLib's Form — its text, its image,
# the sorting of a line into fields, the Luggage — held to exact outcomes;
# recipe, BoxLib's Recipe and its SHA-256 held to exact outcomes and to the
# host's hashlib; elf, the kernel's ELF reader held to an exact verdict for
# every way a program file can be wrong, and to accepting every executable at
# a fixed address the tree builds; box, the one reader and the one pen of a box
# built as the kernel and BoxLib build them, a box of every kind of part
# written and read back, and broken one way at a time, each to its exact
# verdict; pack, every program the tree builds packed into a box and held to
# its ELF by a reading of the ELF that shares nothing with pack, with what pack
# must refuse refused; inside, what tools/inside says of a box of every
# kind of part and of that box damaged one way at a time; and settings, the
# one reader and the one pen of a program's settings held to exact outcomes —
# a declaration of every kind and each way one is wrong, every kind's values,
# the person's file line by line, the pen's changes byte for byte — and to
# bytes made at random.
#
# The list is here, in the tree it judges, and not beside a session: a list
# kept out of the tree can lose a gate without a diff saying so.
#
# ‼ ONE AFTER ANOTHER, NEVER TWO AT ONCE ON ONE TREE. The mutation scenarios
# edit the checkout in place and build it; a gate running beside one of them
# would be measuring its mutant. Two runs at once take two trees and two
# TMPDIRs.
#
# ‼ A GATE THAT LEAVES THE TREE CHANGED HAS FAILED, whatever it printed: the
# next gate would be measured on what it left. So every gate is followed by a
# look at src, tools and the Makefile; a change there is kept in NAME.dirty,
# the SUMMARY says so, and the tree is put back before the next gate. That is
# also why a tree with changes of its own is refused before the first gate —
# putting the tree back would take them with it.

set -u

GATES_LOGCHECK="healthy unattended byname quill quillmut census lockrankmut replug holdground returnfail
                turns turnsmut cores replugwrite latelaunchmut haltcycle halthog halthogmut panicstop panicstopmut fullvolume roombackmut
                stand standusb standmut mkfs crewcrash fare faremut"
GATES_POWERCUT="selftest noanchor noanchormut nextfid hog halt race overwrite logger tipin sessions
                replug machineb spared undonecut engross dedup firstflushmut lastflushmut booknextfidmut stitchmut"
GATES_OTHER="framerefuse bookboot dedup volread_selftest boxasm format form recipe elf box pack inside settings"
GATES_LONG="loggerhour loggerhourmut"

gate_names() {
    local g
    for g in $GATES_LOGCHECK; do echo "$g"; done
    for g in $GATES_POWERCUT; do echo "pc_$g"; done
    for g in $GATES_OTHER; do echo "$g"; done
}

# Every gate that can be asked for by name: the list a plain run goes through,
# and the long ones, which only a name asks for.
gate_known() {
    gate_names
    for g in $GATES_LONG; do echo "$g"; done
}

# The command a gate is: its harness and its name there.
gate_run() {
    case "$1" in
        pc_*)             tools/powercut.sh "${1#pc_}" ;;
        volread_selftest) make && python3 tools/volume-read.py build/boxos.img self-test ;;
        boxasm)           tools/boxasm_stand.sh ;;
        format)           tools/format_stand.sh ;;
        form)             tools/form_stand.sh ;;
        recipe)           tools/recipe_stand.sh ;;
        elf)              tools/elf_stand.sh ;;
        box)              tools/box_stand.sh ;;
        pack)             tools/pack_stand.sh ;;
        inside)           tools/inside_stand.sh ;;
        settings)         tools/settings_stand.sh ;;
        *)                tools/logcheck.sh "$1" ;;
    esac
}

if [ "${1:-}" = "--list" ]; then gate_names; exit 0; fi

TREE=${TREE:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$TREE" || { echo "gates: no tree at $TREE"; exit 2; }

ALL=$(gate_names)
if [ $# -gt 0 ]; then
    for want in "$@"; do
        gate_known | grep -qx -- "$want" || { echo "gates: no gate is called '$want' (tools/gates.sh --list)"; exit 2; }
    done
    WANTED="$*"
else
    WANTED=$ALL
fi

tree_changes() { git status --short -- src tools Makefile | grep -v '^??'; }

if [ -n "$(tree_changes)" ]; then
    echo "gates: REFUSING TO RUN — $TREE has changes of its own in src, tools or the Makefile:"
    tree_changes | sed 's/^/          /'
    echo "       A gate that leaves the tree changed is caught by the change, and the tree"
    echo "       is put back after it; on this tree that would take these with it."
    echo "       Commit them, or run the gates on a checkout of the commit."
    exit 2
fi

OUT=${OUT:-${TMPDIR:-/tmp}/gates.$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT" || { echo "gates: cannot make $OUT"; exit 2; }
echo "gates: $TREE ($(git rev-parse --short HEAD)) → $OUT"
: > "$OUT/SUMMARY"

failed=0; dirty=0; ran=0
for name in $WANTED; do
    t0=$(date +%s)
    gate_run "$name" > "$OUT/$name.out" 2>&1
    rc=$?
    # The harness's own closing words: its count, and the caveats and harness
    # failures it owns to — each cut at its explanation.
    verdict=$(sed $'s/\x1b\\[[0-9;]*m//g' "$OUT/$name.out" \
              | grep -E '^(logcheck|powercut|boxasm|format|form|recipe|elf|box|pack|inside|settings): [0-9]+ (passed|caveat|harness)|^self-test: [0-9]+ of [0-9]+ cases' \
              | sed 's/ — .*//' | paste -sd '|' - | sed 's/|/; /g')
    line="$name rc=$rc $(( $(date +%s) - t0 ))s ${verdict:-(no verdict line)}"
    echo "$line" | tee -a "$OUT/SUMMARY"
    ran=$((ran + 1))
    [ $rc -eq 0 ] || failed=$((failed + 1))
    tree_changes > "$OUT/$name.dirty"
    if [ -s "$OUT/$name.dirty" ]; then
        echo "$name LEFT THE TREE DIRTY" | tee -a "$OUT/SUMMARY"
        dirty=$((dirty + 1))
        git checkout -- src tools Makefile
    else
        rm -f "$OUT/$name.dirty"
    fi
    make run-stop > /dev/null 2>&1
done

echo "gates: $ran run, $failed failed, $dirty left the tree dirty" | tee -a "$OUT/SUMMARY"
[ $failed -eq 0 ] && [ $dirty -eq 0 ]
