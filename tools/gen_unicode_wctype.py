#!/usr/bin/env python3
# ============================================================================
# gen_unicode_wctype.py — offline generator for the Unicode tables behind
# boxcxx <__bits/unicode_wctype> (Ф42: [cwctype.syn]).
#
# NOT a build dependency. The kernel build compiles the baked constexpr arrays;
# run this only to (re)generate them against a newer Unicode release.
#
#   python3 tools/gen_unicode_wctype.py                    # fetch latest UCD
#   python3 tools/gen_unicode_wctype.py --ucd /path/to/ucd # use a local copy
#
# ── Why <cwctype> has tables and <cctype> does not ──────────────────────────
# <cctype> classifies BYTES of the "C" locale and writes the answers out as
# range tests, because the "C" locale's execution character set is ASCII and
# nothing can change that at run time. <cwctype> classifies Unicode scalar
# values, because BoxOS decided in Ф41-b that the encoding of its one locale is
# UTF-8 with MB_CUR_MAX 4. The two headers are not in tension: 0xD0, the first
# byte of 'ж', is not a character, and isalpha() is right to say so; L'ж' is a
# character, and iswalpha() is right to say so too.
#
# ── What each predicate is, and why ─────────────────────────────────────────
# The normative source is the UCD, not another libc. Two answers were measured
# against macOS libc in en_US.UTF-8 and deliberately differ:
#
#   iswalpha(U+4E2D) — macOS says 0. Lo IS Alphabetic in DerivedCoreProperties,
#                      so a CJK ideograph is a letter here. macOS classifies
#                      only what its own locale tables carry.
#   iswprint(U+00AD) — macOS says 1 for a SOFT HYPHEN, a Cf format character
#                      with no glyph. Cf is excluded from print here, which
#                      also forces iswpunct(U+00AD) to 0, since punct must
#                      imply print for the classification to be self-consistent.
#
#   iswalpha   Alphabetic                     (DerivedCoreProperties)
#   iswupper   Uppercase                      (DerivedCoreProperties)
#   iswlower   Lowercase                      (DerivedCoreProperties)
#   iswspace   White_Space                    (PropList)
#   iswblank   Zs union U+0009                (DerivedGeneralCategory)
#   iswcntrl   Cc                             (DerivedGeneralCategory)
#   iswpunct   P* union S*                    (DerivedGeneralCategory)
#   iswprint   assigned, and not Cc/Cf/Cs/Cn/Zl/Zp. Zs IS printable (a space
#              prints); Co IS printable (a private-use code point is assigned a
#              glyph by the agreement that uses it); Cs and Cn are not scalar
#              values anyone can draw.
#
# Three predicates are NOT tables and are derived in the header, because the
# standard defines them by derivation rather than by a set:
#   iswdigit  the ten ASCII digits, and only those. C fixes this: [cwctype.syn]
#             inherits C's "decimal-digit character as defined in 5.2.1", which
#             is 0-9. Both reference libraries agree, measured: iswdigit of
#             U+0660 ARABIC-INDIC DIGIT ZERO is 0 in a UTF-8 locale.
#   iswxdigit the ASCII hexadecimal digits, same reasoning.
#   iswgraph  print and not space.
#   iswalnum  alpha or digit — so U+0660 is neither alnum nor punct, which is
#             exactly what a UTF-8 locale answers.
#
# Case mapping is the SIMPLE mapping of UnicodeData.txt fields 12 (uppercase)
# and 13 (lowercase). towupper/towlower are single-code-point functions and
# cannot express the full mappings (SpecialCasing.txt), where one character
# becomes several: towupper(U+00DF LATIN SMALL LETTER SHARP S) stays U+00DF
# rather than becoming "SS". Every implementation draws this line here.
#
# Storage is <__bits/unicode_runs>: sorted edge arrays for the sets, strided
# runs for the mappings. Both encodings are verified against the raw UCD over
# all 1 114 112 code points below, and nothing is written if either disagrees.
# ============================================================================
import argparse
import bisect
import sys
import urllib.request
from pathlib import Path

MAXCP = 0x10FFFF

UCD_BASE = "https://www.unicode.org/Public/UCD/latest/ucd/"
GC_FILE = "extracted/DerivedGeneralCategory.txt"
DCP_FILE = "DerivedCoreProperties.txt"
PL_FILE = "PropList.txt"
UD_FILE = "UnicodeData.txt"

# Not printable: control, format, surrogate, unassigned, line and paragraph
# separators. Zs and Co are deliberately absent — see the preamble.
NONPRINT = {"Cc", "Cf", "Cs", "Cn", "Zl", "Zp"}
PUNCT_CATS = {"Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po",
              "Sm", "Sc", "Sk", "So"}

HEADER = Path(__file__).resolve().parent.parent / (
    "src/userspace/boxcxx/include/std/__bits/unicode_wctype")


def fetch(path, ucd_dir):
    """Return the text of one UCD file, from disk if given, else from unicode.org."""
    if ucd_dir:
        return (Path(ucd_dir) / Path(path).name).read_text(encoding="utf-8")
    with urllib.request.urlopen(UCD_BASE + path, timeout=120) as r:
        return r.read().decode("utf-8")


def parse(text):
    """Yield (first, last, property) for every data line of a UCD property file."""
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(";")]
        if len(fields) < 2:
            continue
        lo, _, hi = fields[0].partition("..")
        yield int(lo, 16), int(hi or lo, 16), fields[1]


def merge(pairs):
    """Sort and coalesce (first,last) pairs into disjoint, adjacent-free runs."""
    out = []
    for lo, hi in sorted(pairs):
        if out and lo <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return [(lo, hi) for lo, hi in out]


def complement(runs):
    """Everything in [0, MAXCP] that `runs` does not cover."""
    out, prev = [], 0
    for lo, hi in runs:
        if lo > prev:
            out.append((prev, lo - 1))
        prev = hi + 1
    if prev <= MAXCP:
        out.append((prev, MAXCP))
    return out


def edges(runs):
    flat = []
    for lo, hi in runs:
        flat.append(lo)
        flat.append(hi + 1)
    return flat


def check_set(name, runs, flat):
    """Membership by run must agree with membership by edge parity, everywhere."""
    inside = set()
    for lo, hi in runs:
        inside.update(range(lo, hi + 1))
    for cp in range(0, MAXCP + 1):
        if (bisect.bisect_right(flat, cp) & 1) != (cp in inside):
            raise SystemExit("%s: edge encoding disagrees with the runs at U+%05X"
                             % (name, cp))
    return len(inside)


def case_runs(mapping):
    """Compress {cp: mapped} into non-overlapping (first, last, stride, delta).

    Walked in increasing code-point order, so a new run always starts past the
    last code point the previous one absorbed — overlap is impossible.
    """
    out = []
    for cp in sorted(mapping):
        delta = mapping[cp] - cp
        if out:
            run = out[-1]
            if run[3] == delta:
                gap = cp - run[1]
                if run[0] == run[1] and gap in (1, 2):
                    run[1], run[2] = cp, gap        # second element fixes the stride
                    continue
                if run[0] != run[1] and gap == run[2]:
                    run[1] = cp
                    continue
        out.append([cp, cp, 1, delta])              # stride 1, never 0
    return [tuple(r) for r in out]


def check_case(name, mapping, runs):
    """The encoded runs must reproduce the raw mapping at every code point."""
    firsts = [r[0] for r in runs]
    for cp in range(0, MAXCP + 1):
        i = bisect.bisect_right(firsts, cp)
        got = cp
        if i:
            first, last, stride, delta = runs[i - 1]
            if cp <= last and (cp - first) % stride == 0:
                got = cp + delta
        want = mapping.get(cp, cp)
        if got != want:
            raise SystemExit("%s: run encoding gives U+%05X at U+%05X, UCD says U+%05X"
                             % (name, got, cp, want))


def set_array(name, flat):
    out = ["inline constexpr char32_t %s[] = {" % name]
    for i in range(0, len(flat), 8):
        out.append("    " + ", ".join("0x%05X" % v for v in flat[i:i + 8]) + ",")
    out.append("};")
    out.append("inline constexpr unsigned %sCount = %d;" % (name, len(flat)))
    return "\n".join(out)


def case_array(name, runs):
    out = ["inline constexpr CaseRun %s[] = {" % name]
    for first, last, stride, delta in runs:
        out.append("    {0x%05X, 0x%05X, %u, %d}," % (first, last, stride, delta))
    out.append("};")
    out.append("inline constexpr unsigned %sCount = %d;" % (name, len(runs)))
    return "\n".join(out)


PREAMBLE = '''// boxcxx — <__bits/unicode_wctype>
//
// The Unicode facts behind <cwctype>, and nothing else.
//
// THIS WHOLE FILE IS GENERATED by tools/gen_unicode_wctype.py — not just the
// arrays. Re-running that script overwrites every line here, so nothing
// hand-written may live in this file: it holds UCD facts and nothing else. The
// reasoning about what each predicate MEANS lives in that generator's preamble;
// the functions that read these tables live in <__bits/unicode_runs>; the
// header that presents them lives in <cwctype>.
//
// Source data: %(gc)s
//              %(dcp)s
//              %(pl)s
//              UnicodeData.txt of the same release
//
// Sets are sorted edge arrays; case mappings are strided runs. Both encodings
// were verified against the raw UCD over all 1114112 code points by the
// generator, which refuses to write on any disagreement.
#ifndef BOXCXX_BITS_UNICODE_WCTYPE
#define BOXCXX_BITS_UNICODE_WCTYPE

#include <__bits/unicode_runs>

namespace std {
namespace __unicode {

'''

EPILOGUE = '''
// The eight tabled predicates. Everything else <cwctype> answers is derived
// from these plus ASCII range tests — see the header.
constexpr bool IsAlphabetic(char32_t cp) noexcept
{
    return InRuns(kAlphabeticEdges, kAlphabeticEdgesCount, cp);
}
constexpr bool IsUppercase(char32_t cp) noexcept
{
    return InRuns(kUppercaseEdges, kUppercaseEdgesCount, cp);
}
constexpr bool IsLowercase(char32_t cp) noexcept
{
    return InRuns(kLowercaseEdges, kLowercaseEdgesCount, cp);
}
constexpr bool IsWhiteSpace(char32_t cp) noexcept
{
    return InRuns(kWhiteSpaceEdges, kWhiteSpaceEdgesCount, cp);
}
constexpr bool IsBlank(char32_t cp) noexcept
{
    return InRuns(kBlankEdges, kBlankEdgesCount, cp);
}
constexpr bool IsControl(char32_t cp) noexcept
{
    return InRuns(kControlEdges, kControlEdgesCount, cp);
}
constexpr bool IsPunct(char32_t cp) noexcept
{
    return InRuns(kPunctEdges, kPunctEdgesCount, cp);
}
constexpr bool IsPrint(char32_t cp) noexcept
{
    return InRuns(kPrintEdges, kPrintEdgesCount, cp);
}

constexpr char32_t ToUpper(char32_t cp) noexcept
{
    return MapCase(kToUpperRuns, kToUpperRunsCount, cp);
}
constexpr char32_t ToLower(char32_t cp) noexcept
{
    return MapCase(kToLowerRuns, kToLowerRunsCount, cp);
}

} // namespace __unicode
} // namespace std

#endif // BOXCXX_BITS_UNICODE_WCTYPE
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ucd", help="directory holding the four UCD files")
    ap.add_argument("--out", default=str(HEADER))
    args = ap.parse_args()

    gc_text = fetch(GC_FILE, args.ucd)
    dcp_text = fetch(DCP_FILE, args.ucd)
    pl_text = fetch(PL_FILE, args.ucd)
    ud_text = fetch(UD_FILE, args.ucd)

    gc = list(parse(gc_text))
    dcp = list(parse(dcp_text))
    pl = list(parse(pl_text))

    listed = merge([(lo, hi) for lo, hi, _ in gc])
    unassigned = complement(listed)          # anything the file omits is Cn

    sets = {
        "Alphabetic": merge([(l, h) for l, h, p in dcp if p == "Alphabetic"]),
        "Uppercase":  merge([(l, h) for l, h, p in dcp if p == "Uppercase"]),
        "Lowercase":  merge([(l, h) for l, h, p in dcp if p == "Lowercase"]),
        "WhiteSpace": merge([(l, h) for l, h, p in pl if p == "White_Space"]),
        "Blank":      merge([(l, h) for l, h, p in gc if p == "Zs"] + [(0x09, 0x09)]),
        "Control":    merge([(l, h) for l, h, p in gc if p == "Cc"]),
        "Punct":      merge([(l, h) for l, h, p in gc if p in PUNCT_CATS]),
    }
    sets["Print"] = complement(
        merge([(l, h) for l, h, p in gc if p in NONPRINT] + unassigned))

    for name, runs in sets.items():
        if not runs:
            raise SystemExit("UCD parse produced an empty %s table — refusing to write" % name)

    upper, lower = {}, {}
    for line in ud_text.splitlines():
        f = line.split(";")
        if len(f) < 15:
            continue
        cp = int(f[0], 16)
        if f[12]:
            upper[cp] = int(f[12], 16)
        if f[13]:
            lower[cp] = int(f[13], 16)
    if not upper or not lower:
        raise SystemExit("UnicodeData parse produced no case mappings — refusing to write")

    up_runs, lo_runs = case_runs(upper), case_runs(lower)
    check_case("towupper", upper, up_runs)
    check_case("towlower", lower, lo_runs)

    order = ["Alphabetic", "Uppercase", "Lowercase", "WhiteSpace",
             "Blank", "Control", "Punct", "Print"]
    blocks, counts, rodata = [], {}, 0
    for name in order:
        flat = edges(sets[name])
        counts[name] = check_set(name, sets[name], flat)
        rodata += 4 * len(flat)
        blocks.append(set_array("k%sEdges" % name, flat))
    rodata += 16 * (len(up_runs) + len(lo_runs))
    blocks.append(case_array("kToUpperRuns", up_runs))
    blocks.append(case_array("kToLowerRuns", lo_runs))

    text = (PREAMBLE % {"gc": gc_text.splitlines()[0].strip("# "),
                        "dcp": dcp_text.splitlines()[0].strip("# "),
                        "pl": pl_text.splitlines()[0].strip("# ")}
            + "\n\n".join(blocks) + "\n" + EPILOGUE)
    Path(args.out).write_text(text)

    print("%s: %d bytes of rodata" % (args.out, rodata))
    for name in order:
        print("  %-11s %4d runs  %8d code points" % (name, len(sets[name]), counts[name]))
    print("  towupper    %4d runs  %8d mappings" % (len(up_runs), len(upper)))
    print("  towlower    %4d runs  %8d mappings" % (len(lo_runs), len(lower)))


if __name__ == "__main__":
    sys.exit(main())
