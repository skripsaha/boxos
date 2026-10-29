#!/bin/bash
# gates — every gate this tree is held to, one after another, one line each.
#
#     tools/gates.sh                  all of them
#     tools/gates.sh healthy stand    only these, in the order given
#     tools/gates.sh --list           their names, one to a line
#
#     OUT=DIR     where each gate's output (NAME.out), whatever it left changed
#                 in the tree (NAME.dirty) and the SUMMARY go. Unset, a fresh
#                 directory under TMPDIR, named on the first line.
#     TMPDIR=DIR  the harnesses' scratch: stand.sh keeps its books under it.
#     TREE=DIR    the checkout the gates run in, judged by its OWN harnesses.
#                 Unset, the checkout this file is in.
#
# Thirty-four gates — twenty-two of logcheck's and twelve of PowerCut's — then
# framerefuse, the stand, volume-read's self-test over the image a plain
# `make` lays down (logcheck's mkfs runs it over the one `make VOLUME_FORMAT=v2`
# lays, whose book mkfs bound), boxasm, the in-BoxOS assembler and linker
# held to GNU ld and tools/nameplate, and format, BoxLib's printf conversions
# held to the host's C library and to exact arithmetic, and form, BoxLib's Form
# — its text, its image, the sorting of a line into fields, the Luggage — held
# to exact outcomes. The list is here, in the tree it judges, and
# not beside a session: a list kept out of the tree can lose a gate without a
# diff saying so.
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

GATES_LOGCHECK="healthy unattended byname quill quillmut census censusmut lockrankmut
                replug holdground returnfail standmut abba abbamut turns turnsmut cores
                replugwrite haltcycle fullvolume standusb mkfs"
GATES_POWERCUT="selftest noanchor noanchormut nextfid nextfidmut hog settlecut chaingrow
                poolmut flushmut halt race"

gate_names() {
    local g
    for g in $GATES_LOGCHECK; do echo "$g"; done
    for g in $GATES_POWERCUT; do echo "pc_$g"; done
    echo framerefuse; echo stand; echo volread_selftest; echo boxasm; echo format; echo form
}

# The command a gate is: its harness and its name there.
gate_run() {
    case "$1" in
        pc_*)             tools/powercut.sh "${1#pc_}" ;;
        volread_selftest) make && python3 tools/volume-read.py build/boxos.img self-test ;;
        boxasm)           tools/boxasm_stand.sh ;;
        format)           tools/format_stand.sh ;;
        form)             tools/form_stand.sh ;;
        *)                tools/logcheck.sh "$1" ;;
    esac
}

if [ "${1:-}" = "--list" ]; then gate_names; exit 0; fi

TREE=${TREE:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$TREE" || { echo "gates: no tree at $TREE"; exit 2; }

ALL=$(gate_names)
if [ $# -gt 0 ]; then
    for want in "$@"; do
        echo "$ALL" | grep -qx -- "$want" || { echo "gates: no gate is called '$want' (tools/gates.sh --list)"; exit 2; }
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
              | grep -E '^(logcheck|powercut|boxasm|format|form): [0-9]+ (passed|caveat|harness)|^self-test: [0-9]+ of [0-9]+ cases' \
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
