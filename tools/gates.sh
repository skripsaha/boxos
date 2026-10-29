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
#     tools/gates.sh --formats        no boot: every gate in its part and a
#                                     scenario its harness runs on that format,
#                                     no harness leaning on the default format
#                                     (the gate `formats` too)
#
#     OUT=DIR     where each gate's output (NAME.out), whatever it left changed
#                 in the tree (NAME.dirty) and the SUMMARY go. Unset, a fresh
#                 directory under TMPDIR, named on the first line.
#     TMPDIR=DIR  the harnesses' scratch: stand.sh keeps its books under it.
#     TREE=DIR    the checkout the gates run in, judged by its OWN harnesses.
#                 Unset, the checkout this file is in.
#
# Two parts (spec_v2 §16.5 п.22). The GATES, run on a book — every one under
# VOLUME_FORMAT=v2: the logcheck and PowerCut scenarios of the format the machine
# makes — the book's PowerCut matrix among them (overwrite, logger, tipin,
# sessions, replug, machineb: what answers format v1's halt, race, noanchor and
# hog on a book) — a scenario of format v1's own machinery answered on a book
# by its own (settlecut by spared, undonecut and engross; poolmut and flushmut
# by the book's flush and stitch mutants; nextfidmut by booknextfidmut). Then
# the BACKLIST, run on format v1 — every one under VOLUME_FORMAT=v1, named
# v1_*: what holds format v1's own structures alone — the census of its
# plantings and its mutants, the stand of format v1, chaingrow, settlecut's
# Ledger, next_fid and the mutants of its pool, flush and next_fid, abba's
# chains, and the heirloom: a volume kernel N laid down and wrote, read and
# written by this kernel and read back by N (bindery.sh, N b9962f55 unless
# HEIRLOOM_FROM names another). Then framerefuse, Concordance (every test
# written for format v1 names its book's analog), `formats`, and volume-read's
# self-test over the image each format lays. No gate leans on the format the
# build makes when nobody names one: `tools/gates.sh --formats` holds every
# gate to a part and to a scenario its harness runs on that part's format, and
# every harness to stand_format (stand.sh).
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
                turns turnsmut cores replugwrite latelaunchmut haltcycle fullvolume stand standusb mkfs"
GATES_POWERCUT="selftest noanchor noanchormut nextfid hog halt race overwrite logger tipin sessions
                replug machineb spared undonecut engross firstflushmut lastflushmut booknextfidmut stitchmut"
GATES_OTHER="framerefuse concordance formats volread_selftest"
GATES_LONG="loggerhour loggerhourmut"
BACKLIST_LOGCHECK="census censusmut stand standusb standmut abba abbamut"
BACKLIST_POWERCUT="settlecut chaingrow nextfid nextfidmut poolmut flushmut"
BACKLIST_OTHER="heirloom volread_selftest"
HEIRLOOM_N=b9962f55   # the last kernel before the book (bindery.sh, receipt (в))

gate_names() {
    local g
    for g in $GATES_LOGCHECK; do echo "$g"; done
    for g in $GATES_POWERCUT; do echo "pc_$g"; done
    for g in $BACKLIST_LOGCHECK; do echo "v1_$g"; done
    for g in $BACKLIST_POWERCUT; do echo "v1_pc_$g"; done
    for g in $GATES_OTHER; do echo "$g"; done
    for g in $BACKLIST_OTHER; do echo "v1_$g"; done
}

# Every gate that can be asked for by name: the list a plain run goes through,
# and the long ones, which only a name asks for.
gate_known() {
    gate_names
    for g in $GATES_LONG; do echo "$g"; done
}

in_list() {
    local want=$1 x
    shift
    for x in "$@"; do [ "$x" = "$want" ] && return 0; done
    return 1
}

# The format a gate runs on: the part whose list names it — the Backlist's v1,
# the gates' v2 — and "none" for a name no list holds.
gate_format() {
    case "$1" in
        v1_pc_*) in_list "${1#v1_pc_}" $BACKLIST_POWERCUT && { echo v1; return; } ;;
        v1_*)    in_list "${1#v1_}" $BACKLIST_LOGCHECK $BACKLIST_OTHER && { echo v1; return; } ;;
        pc_*)    in_list "${1#pc_}" $GATES_POWERCUT && { echo v2; return; } ;;
        *)       in_list "$1" $GATES_LOGCHECK $GATES_OTHER $GATES_LONG && { echo v2; return; } ;;
    esac
    echo none
}

# The command a gate is: its harness and its name there, under its format.
gate_run() {
    local name=${1#v1_}
    case "$name" in
        pc_*)             VOLUME_FORMAT=$2 tools/powercut.sh "${name#pc_}" ;;
        volread_selftest) make VOLUME_FORMAT="$2" && python3 tools/volume-read.py build/boxos.img self-test ;;
        concordance)      python3 tools/concordance.py --self-test && python3 tools/concordance.py ;;
        formats)          gate_formats ;;
        heirloom)         HEIRLOOM_FORMAT=$2 HEIRLOOM_FROM=${HEIRLOOM_FROM:-$HEIRLOOM_N} tools/bindery.sh heirloom ;;
        *)                VOLUME_FORMAT=$2 tools/logcheck.sh "$name" ;;
    esac
}

# --formats: no gate leans on the format a build makes when nobody names one
# (critic K4.18 — the default moves to v2 in one line of the Makefile, and a
# Backlist scenario that leaned on it would go on silently on a book). Every
# gate is named by the list of its part (gate_format), and a PowerCut gate is a
# scenario its harness runs on that part's format — none format v1's own
# machinery on a book (pc_book's refusals), none a book's own on format v1;
# gate_run names the part's format to every harness; no harness keeps a
# default of its own nor hands an unnamed one on — the format a scenario asks
# is stand_format's, the Makefile's — create_tagfs is told it by name, and a
# check that an image is format vN stands where the scenario names vN.
gate_formats() {
    local bad=0 g line refused book
    refused=$(awk '/^pc_book\(\)/,/^}/' tools/powercut_book.sh | grep -B1 "stands on format v1's own machinery" |
              grep -oE 'v2:[a-z0-9]+' | sed 's/^v2://')
    book=$(awk '/^pc_book\(\)/,/^}/' tools/powercut_book.sh | grep -oE 'v2:[a-z0-9]+' | sed 's/^v2://' | sort -u |
           grep -vxF -e all $(printf -- '-e %s ' $refused))
    for g in $(gate_known); do
        case "$(gate_format "$g")" in
            v1) ! in_list "${g#v1_pc_}" $book || [ "${g#v1_pc_}" = "$g" ] ||
                    { echo "gates --formats: $g — a book's own PowerCut scenario, in the Backlist"; bad=1; } ;;
            v2) ! in_list "${g#pc_}" $refused || [ "${g#pc_}" = "$g" ] ||
                    { echo "gates --formats: $g — format v1's own PowerCut scenario, among the gates on a book"; bad=1; } ;;
            *)  echo "gates --formats: $g — no part's list names it"; bad=1 ;;
        esac
    done
    while IFS= read -r line; do
        echo "gates --formats: a harness keeps a format of its own — $line"; bad=1
    done < <(grep -nE '\$\{VOLUME_FORMAT:-v[12]\}|VOLUME_FORMAT \?= v[12]' tools/*.sh Makefile)
    while IFS= read -r line; do
        echo "gates --formats: a harness hands a format on that may be none, the default's — $line"; bad=1
    done < <(grep -nE '[A-Z_]*FORMAT="?\$\{[A-Z_]*FORMAT:-\}|\$\{[A-Z_]*FORMAT:\+VOLUME_FORMAT=' tools/*.sh |
             grep -vE '^tools/stand\.sh:[0-9]+:\s*command make HARNESS=on \$\{STAND_FORMAT:\+')
    while IFS= read -r line; do
        echo "gates --formats: create_tagfs asked without a format — $line"; bad=1
    done < <(grep -nE '(tools/create_tagfs|\$\(TAGFS_TOOL\))( |$)' tools/*.sh Makefile |
             grep -vE '^[^:]+:[0-9]+:\s*#' | grep -vE -- '--v[12]|--frame-only|--ground \$\(VOLUME_FORMAT_FLAG\)|\$\(VOLUME_FORMAT_FLAG\)|"\$mode"|^Makefile:[0-9]+:(TAGFS_TOOL|\$\(TAGFS_TOOL\):|\$\(IMAGE\):)|rm -f')
    while IFS= read -r line; do
        echo "gates --formats: $line"; bad=1
    done < <(python3 - tools/*.sh <<'EOF'
import re, sys
for path in sys.argv[1:]:
    lines = open(path).read().split("\n")
    heads = [i for i, l in enumerate(lines) if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\(\)\s*\{", l)]
    for i, l in enumerate(lines):
        m = re.search(r"format +: (v[12])", l)
        if not m or l.lstrip().startswith("#"):
            continue
        v = m.group(1)
        head = max([h for h in heads if h <= i], default=0)
        end = next((k for k in range(i, len(lines)) if lines[k].startswith("}")), len(lines))
        if not re.search(r'(VOLUME|STAND|HEIRLOOM|PC)_FORMAT="?' + v + r"\b|--" + v + r"\b", "\n".join(lines[head:end])):
            print(f"{path}:{i + 1}: a check that the image is format {v} where the scenario never names {v} — it holds the default")
EOF
)
    [ $bad -eq 0 ] && echo "gates --formats: $(gate_known | wc -l | tr -d ' ') gates, each named by its part and a scenario its harness runs on that format; no harness leans on the default format"
    return $bad
}

if [ "${1:-}" = "--list" ]; then gate_names; exit 0; fi
if [ "${1:-}" = "--formats" ]; then cd "$(dirname "$0")/.." && gate_formats; exit $?; fi

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
    gate_run "$name" "$(gate_format "$name")" > "$OUT/$name.out" 2>&1
    rc=$?
    # The harness's own closing words: its count, and the caveats and harness
    # failures it owns to — each cut at its explanation.
    verdict=$(sed $'s/\x1b\\[[0-9;]*m//g' "$OUT/$name.out" \
              | grep -E '^(logcheck|powercut|bindery): [0-9]+ (passed|caveat|harness)|^self-test: [0-9]+ of [0-9]+ cases|^concordance: |^gates --formats: [0-9]+ gates' \
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
