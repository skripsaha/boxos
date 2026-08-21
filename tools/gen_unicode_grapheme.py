#!/usr/bin/env python3
# ============================================================================
# gen_unicode_grapheme.py — offline generator for the UAX #29 tables behind
# boxcxx <__bits/unicode_grapheme> (Ф43-e-2: [format.string.std]/13).
#
# NOT a build dependency. The build compiles the baked constexpr arrays; run
# this only to (re)generate them against a newer Unicode release.
#
#   python3 tools/gen_unicode_grapheme.py                    # fetch latest UCD
#   python3 tools/gen_unicode_grapheme.py --ucd /path/to/ucd # use a local copy
#
# ── Why <format> needs this at all ──────────────────────────────────────────
# [format.string.std]/13 defines a field's width as the number of extended
# GRAPHEME CLUSTERS in the string, not the number of code units and not the
# number of code points. "é" written as e + U+0301 is one column wide however
# many bytes it takes; a flag emoji is one cluster made of two code points and
# four bytes. Until Ф43-e-2 this library counted code units, which is exact for
# ASCII and wrong for everything else.
#
# Segmenting into clusters needs three properties: Grapheme_Cluster_Break for
# most of the rules, Extended_Pictographic for GB11 (which holds an emoji-ZWJ
# sequence together), and Indic_Conjunct_Break for GB9c (which holds a
# consonant-linker-consonant conjunct together). Those three are all this file
# carries. GB9c is here because BOTH reference implementations apply it —
# measured: a devanagari conjunct is one cluster in each of them.
#
# ── Storage: two class tables, one edge array ───────────────────────────────
# Grapheme_Cluster_Break partitions the whole code space, so it is stored as
# the sorted STARTS of its runs plus the class each run has: a binary search
# for the last start <= cp gives the class, with no "unassigned" hole to
# special-case. Extended_Pictographic is an ordinary set and uses the sorted
# edge array + InRuns that <__bits/unicode_runs> already reads for two other
# generated headers.
#
# The rules themselves (GB1..GB999) are NOT here. They are hand-written, in
# <__bits/format_width>, for the same reason the binary search is not in the
# generated files: a rule is not a UCD fact, and this file may hold nothing but
# UCD facts.
# ============================================================================
import argparse
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src/userspace/boxcxx/include/std/__bits/unicode_grapheme"

UCD_BASE = "https://www.unicode.org/Public/UCD/latest/ucd/"
GCB_FILE = "auxiliary/GraphemeBreakProperty.txt"
EMOJI_FILE = "emoji/emoji-data.txt"
DCP_FILE = "DerivedCoreProperties.txt"

MAXCP = 0x10FFFF

# The order here IS the emitted enumerator order; the reader names them.
CLASSES = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
           "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"]
CLASS_INDEX = {name: i for i, name in enumerate(CLASSES)}

LINE = re.compile(r"^\s*([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*([A-Za-z_]+)")
# DerivedCoreProperties spells Indic_Conjunct_Break as a two-part value:
#   0915..0939    ; InCB; Consonant
INCB_LINE = re.compile(
    r"^\s*([0-9A-Fa-f]+)(?:\.\.([0-9A-Fa-f]+))?\s*;\s*InCB;\s*([A-Za-z]+)")

# GB9c joins a consonant to a consonant across a linker. Three values matter;
# everything else is None and takes no part.
INCB_CLASSES = ["None", "Consonant", "Extend", "Linker"]
INCB_INDEX = {n: i for i, n in enumerate(INCB_CLASSES)}


def parse_incb(text):
    for raw in text.splitlines():
        line = raw.split("#", 1)[0]
        m = INCB_LINE.match(line)
        if not m:
            continue
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        yield lo, hi, m.group(3)


def fetch(path, ucd_dir):
    """Return the text of one UCD file, from disk if given, else from unicode.org."""
    if ucd_dir:
        return (Path(ucd_dir) / path).read_text(encoding="utf-8")
    with urllib.request.urlopen(UCD_BASE + path, timeout=120) as r:
        return r.read().decode("utf-8")


def parse(text):
    for raw in text.splitlines():
        line = raw.split("#", 1)[0]
        m = LINE.match(line)
        if not m:
            continue
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        yield lo, hi, m.group(3)


def merge(runs):
    """Sort and coalesce touching/overlapping [lo, hi] runs."""
    out = []
    for lo, hi in sorted(runs):
        if out and lo <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return [(lo, hi) for lo, hi in out]


def edges(runs):
    """[lo, hi] runs -> sorted boundaries; membership is odd parity."""
    flat = []
    for lo, hi in runs:
        flat.append(lo)
        flat.append(hi + 1)
    return flat


def class_table(gcb):
    """A start-sorted (start, class) table covering U+0000..U+10FFFF."""
    cls = bytearray(MAXCP + 1)          # 0 == Other everywhere by default
    for lo, hi, prop in gcb:
        if prop not in CLASS_INDEX:
            raise SystemExit("unknown Grapheme_Cluster_Break value: %s" % prop)
        v = CLASS_INDEX[prop]
        for cp in range(lo, hi + 1):
            cls[cp] = v

    starts, values = [0], [cls[0]]
    for cp in range(1, MAXCP + 1):
        if cls[cp] != values[-1]:
            starts.append(cp)
            values.append(cls[cp])
    return starts, values, cls


def incb_table(incb):
    """A start-sorted (start, class) table for Indic_Conjunct_Break."""
    cls = bytearray(MAXCP + 1)          # 0 == None everywhere by default
    for lo, hi, prop in incb:
        if prop not in INCB_INDEX:
            raise SystemExit("unknown InCB value: %s" % prop)
        v = INCB_INDEX[prop]
        for cp in range(lo, hi + 1):
            cls[cp] = v
    starts, values = [0], [cls[0]]
    for cp in range(1, MAXCP + 1):
        if cls[cp] != values[-1]:
            starts.append(cp)
            values.append(cls[cp])
    return starts, values, cls


def verify_class(starts, values, cls, what):
    import bisect
    for cp in range(0, MAXCP + 1):
        i = bisect.bisect_right(starts, cp) - 1
        if values[i] != cls[cp]:
            raise SystemExit("%s table disagrees at U+%05X" % (what, cp))


def verify(starts, values, cls, ext_edges, ext_runs):
    """Every code point must read back the way the raw table says it is."""
    import bisect
    for cp in range(0, MAXCP + 1, 1):
        i = bisect.bisect_right(starts, cp) - 1
        if values[i] != cls[cp]:
            raise SystemExit("class table disagrees at U+%05X" % cp)
    inside = set()
    for lo, hi in ext_runs:
        inside.update(range(lo, hi + 1))
    for cp in range(0, MAXCP + 1):
        if (bisect.bisect_right(ext_edges, cp) & 1) != (cp in inside):
            raise SystemExit("Extended_Pictographic edges disagree at U+%05X" % cp)


def u32_array(name, values, fmt="0x%05X"):
    out = ["inline constexpr char32_t %s[] = {" % name]
    for i in range(0, len(values), 8):
        out.append("    " + ", ".join(fmt % v for v in values[i:i + 8]) + ",")
    out.append("};")
    return "\n".join(out)


def u8_array(name, values):
    out = ["inline constexpr unsigned char %s[] = {" % name]
    for i in range(0, len(values), 16):
        out.append("    " + ", ".join("%d" % v for v in values[i:i + 16]) + ",")
    out.append("};")
    return "\n".join(out)


PREAMBLE = '''// boxcxx — <__bits/unicode_grapheme>
//
// The three Unicode properties UAX #29 segmentation needs, and nothing else.
//
// THIS WHOLE FILE IS GENERATED by tools/gen_unicode_grapheme.py — not just the
// arrays. Re-running that script overwrites every line here, so nothing
// hand-written may live in this file: it holds UCD facts and nothing else.
// The segmentation RULES (GB1..GB999) and the width estimate they feed live in
// <__bits/format_width>, next to the algorithm that drives them.
//
// Source data: %(gcb)s
//              %(emoji)s
//              %(dcp)s
//
// Grapheme_Cluster_Break partitions the whole code space, so it is stored as
// the sorted STARTS of its runs plus each run's class: one binary search for
// the last start <= cp, and no unassigned hole to special-case.
// Extended_Pictographic is an ordinary set, stored as sorted edges and read by
// InRuns from <__bits/unicode_runs> — the same reader two other generated
// headers use.
#ifndef BOXCXX_BITS_UNICODE_GRAPHEME
#define BOXCXX_BITS_UNICODE_GRAPHEME

#include <__bits/unicode_runs>

namespace std {
namespace __unicode {

// The Grapheme_Cluster_Break property values, in the order the tables encode.
enum class Gcb : unsigned char {
%(enum)s
};

// Indic_Conjunct_Break, which rule GB9c needs. Both reference implementations
// apply GB9c — measured, not assumed: a devanagari conjunct is ONE cluster in
// each of them — so leaving it out would have made this library the odd one.
enum class Incb : unsigned char {
%(incbenum)s
};

'''

EPILOGUE = '''
inline constexpr unsigned kGcbRunCount  = %(runs)d;
inline constexpr unsigned kIncbRunCount = %(incbruns)d;

// The Grapheme_Cluster_Break value of a code point.
constexpr Gcb GcbOf(char32_t cp) noexcept
{
    unsigned lo = 0, hi = kGcbRunCount;   // last start <= cp
    while (hi - lo > 1) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (kGcbStarts[mid] <= cp) lo = mid;
        else hi = mid;
    }
    return static_cast<Gcb>(kGcbValues[lo]);
}

// The Indic_Conjunct_Break value of a code point.
constexpr Incb IncbOf(char32_t cp) noexcept
{
    unsigned lo = 0, hi = kIncbRunCount;
    while (hi - lo > 1) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (kIncbStarts[mid] <= cp) lo = mid;
        else hi = mid;
    }
    return static_cast<Incb>(kIncbValues[lo]);
}

// The Unicode property Extended_Pictographic=Yes, which rule GB11 needs.
constexpr bool IsExtendedPictographic(char32_t cp) noexcept
{
    return InRuns(kExtPictEdges, kExtPictEdgeCount, cp);
}

} // namespace __unicode
} // namespace std

#endif // BOXCXX_BITS_UNICODE_GRAPHEME
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ucd", help="directory holding the three UCD files")
    ap.add_argument("--out", default=str(HEADER))
    args = ap.parse_args()

    gcb_text = fetch(GCB_FILE, args.ucd)
    emoji_text = fetch(EMOJI_FILE, args.ucd)
    dcp_text = fetch(DCP_FILE, args.ucd)
    gcb_name = gcb_text.splitlines()[0].strip("# ")
    emoji_name = emoji_text.splitlines()[0].strip("# ")
    dcp_name = dcp_text.splitlines()[0].strip("# ")

    gcb = list(parse(gcb_text))
    ext = merge([(lo, hi) for lo, hi, p in parse(emoji_text)
                 if p == "Extended_Pictographic"])
    if not gcb or not ext:
        raise SystemExit("UCD parse produced an empty table — refusing to write")

    incb = list(parse_incb(dcp_text))
    if not incb:
        raise SystemExit("DerivedCoreProperties has no InCB — refusing to write")

    starts, values, cls = class_table(gcb)
    istarts, ivalues, icls = incb_table(incb)
    ext_e = edges(ext)
    verify(starts, values, cls, ext_e, ext)
    verify_class(istarts, ivalues, icls, "InCB")

    enum_lines = ",\n".join("    %s = %d" % (n, i) for i, n in enumerate(CLASSES))
    incb_lines = ",\n".join("    %s = %d" % (n, i)
                            for i, n in enumerate(INCB_CLASSES))
    text = (PREAMBLE % {"gcb": gcb_name, "emoji": emoji_name, "dcp": dcp_name,
                        "enum": enum_lines, "incbenum": incb_lines}
            + u32_array("kGcbStarts", starts) + "\n\n"
            + u8_array("kGcbValues", values) + "\n\n"
            + u32_array("kIncbStarts", istarts) + "\n\n"
            + u8_array("kIncbValues", ivalues) + "\n\n"
            + u32_array("kExtPictEdges", ext_e) + "\n"
            + "inline constexpr unsigned kExtPictEdgeCount = %d;\n" % len(ext_e)
            + EPILOGUE % {"runs": len(starts), "incbruns": len(istarts)})
    Path(args.out).write_text(text)

    print("%s: %d GCB runs, %d InCB runs, %d Extended_Pictographic runs, "
          "%d bytes of rodata"
          % (args.out, len(starts), len(istarts), len(ext),
             4 * len(starts) + len(values) + 4 * len(istarts) + len(ivalues) +
             4 * len(ext_e)))


if __name__ == "__main__":
    sys.exit(main())
