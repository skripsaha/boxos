#!/usr/bin/env python3
# ============================================================================
# gen_freestanding_manifest.py — offline generator for tools/freestanding_manifest.txt,
# the entity list that tools/cxx_freestanding_audit.sh checks boxcxx against.
#
# NOT a build dependency, and not run by any gate. Run it only to re-derive the
# manifest against a newer working draft:
#
#   python3 tools/gen_freestanding_manifest.py
#
# ── What a freestanding macro actually claims ───────────────────────────────
# __cpp_lib_freestanding_X says "this implementation provides the freestanding
# subset of header X". The subset is not written out anywhere as a list: it is
# spelled INSIDE each header's synopsis, as a `// freestanding` comment on the
# declarations that belong to it. So the only way to audit the claim is to read
# all 25 synopses and check every marked declaration against the library — and
# the only way to keep that audit honest a year from now is to fetch the marks
# rather than transcribe them. A transcription is a second source of truth that
# nothing re-derives; Ф41 found seven holes in exactly such a transcription
# (tools/version_syn_owners.txt) by diffing it against the draft.
#
# Three markers matter and they are not the same thing:
#   // freestanding           the declaration IS in the subset -> must exist
#   // freestanding-deleted   a freestanding implementation MAY delete it, so
#                             its absence is conforming and its presence is
#                             conforming; the audit must ignore it
#   // hosted                 explicitly outside the subset
# A header whose opening comment says "all freestanding" or "mostly
# freestanding" puts every declaration in the subset except those marked
# otherwise; for those the manifest records the header, not a name list,
# because "every name in <algorithm>" is what the in-tree suite already tests.
#
# ── Why the exposition-only names are dropped ───────────────────────────────
# A synopsis contains names no program may write — `see below _M;`, the
# exposition-only `bits-available`, the `c-atexit-handler` parameter types.
# They carry the marker like anything else and they are not entities. The
# filter below drops names containing a hyphen (the draft's own convention for
# exposition-only identifiers) and the ones declared as `see below`.
# ============================================================================
import argparse
import html
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tools/freestanding_manifest.txt"
BASE = "https://eel.is/c++draft/"

# (draft page, macro stem, header to include when probing).
# The stem is the __cpp_lib_freestanding_<stem> the entities belong to; two
# stems share a macro (errc lives half in <cerrno> and half in <system_error>)
# and are spelled with a suffix here, joined again by the audit script.
TARGETS = [
    ("cstdlib.syn",         "cstdlib",       "<cstdlib>"),
    ("cstring.syn",         "cstring",       "<cstring>"),
    ("cwchar.syn",          "cwchar",        "<cwchar>"),
    ("cerrno.syn",          "errc.cerrno",   "<cerrno>"),
    ("system.error.syn",    "errc.system",   "<system_error>"),
    ("new.syn",             "operator_new",  "<new>"),
    ("ratio.syn",           "ratio",         "<ratio>"),
    ("charconv.syn",        "charconv",      "<charconv>"),
    ("array.syn",           "array",         "<array>"),
    ("optional.syn",        "optional",      "<optional>"),
    ("variant.syn",         "variant",       "<variant>"),
    ("string.view.synop",   "string_view",   "<string_view>"),
    ("expected.syn",        "expected",      "<expected>"),
    ("tuple.syn",           "tuple",         "<tuple>"),
    ("utility.syn",         "utility",       "<utility>"),
    ("memory.syn",          "memory",        "<memory>"),
    ("functional.syn",      "functional",    "<functional>"),
    ("iterator.synopsis",   "iterator",      "<iterator>"),
    ("ranges.syn",          "ranges",        "<ranges>"),
    ("algorithm.syn",       "algorithm",     "<algorithm>"),
    ("numeric.ops.overview", "numeric",      "<numeric>"),
    ("rand.synopsis",       "random",        "<random>"),
    ("execution.syn",       "execution",     "<execution>"),
    ("mdspan.syn",          "mdspan",        "<mdspan>"),
    ("string.syn",          "char_traits",   "<string>"),
]

# Names a using-declaration must not be built from: keywords and type names
# that a regex will pick out of a declaration whose real name sits elsewhere.
KEYWORD = re.compile(
    r"^(see|below|class|struct|typename|const|constexpr|void|int|long|char|bool|"
    r"unsigned|signed|short|double|float|auto|template|operator|noexcept|inline|"
    r"explicit|virtual|friend|using|namespace|return|if|else|for|while|decltype|"
    r"requires|concept|enum|public|private|protected|static|extern|typedef|new|"
    r"delete)$")


def text_of(page):
    """One draft page as plain text, comments and all."""
    with urllib.request.urlopen(BASE + page, timeout=120) as r:
        raw = r.read().decode("utf-8", "replace")
    raw = re.sub(r"<script.*?</script>", "", raw, flags=re.S)
    return html.unescape(re.sub(r"<[^>]+>", "", raw))


def declared_name(line):
    """(kind, name) declared by one synopsis line, or None."""
    s = line.split("//")[0].strip()
    if not s:
        return None
    # `see below _M;` declares an exposition-only member whose type the draft
    # leaves unwritten. It carries the marker and it is not an entity.
    if s.startswith("see below"):
        return None
    m = re.match(r"#define\s+([A-Za-z_]\w*)", s)
    if m:
        return ("macro", m.group(1))
    m = re.match(r"using\s+([A-Za-z_]\w*)\s*=", s)
    if m:
        return ("name", m.group(1))
    s = re.sub(r"^template\s*<.*?>\s*", "", s)
    m = re.match(r"(?:class|struct|enum(?:\s+class)?)\s+([A-Za-z_]\w*)", s)
    if m:
        return ("name", m.group(1))
    if s.startswith("namespace"):
        return None
    m = re.search(r"([A-Za-z_]\w*)\s*\(", s) or re.search(r"\b([A-Za-z_]\w*)\s*(?:=|;)", s)
    return ("name", m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(OUT))
    args = ap.parse_args()

    rows = ["# GENERATED by tools/gen_freestanding_manifest.py — do not edit.",
            "# Source: the working draft's header synopses at " + BASE,
            "#",
            "# One record per macro stem:",
            "#   header <stem> <#include> <whole|partial>",
            "#   name   <stem> <identifier>      an entity the subset requires",
            "#   macro  <stem> <identifier>      a macro the subset requires",
            "# 'whole' means the synopsis says all/mostly freestanding: the subset is",
            "# the header minus what is marked hosted, which is what the in-tree suite",
            "# already covers, so no name rows follow.",
            ""]
    for page, stem, header in TARGETS:
        lines = text_of(page).splitlines()
        whole = any("all freestanding" in l or "mostly freestanding" in l for l in lines)
        names, macros = set(), set()
        for l in lines:
            if "freestanding" not in l or "freestanding-deleted" in l:
                continue
            if "all freestanding" in l or "mostly freestanding" in l:
                continue
            got = declared_name(l)
            if not got:
                continue
            kind, name = got
            if KEYWORD.match(name) or "-" in name:
                continue
            (macros if kind == "macro" else names).add(name)
        rows.append("header %s %s %s" % (stem, header, "whole" if whole else "partial"))
        for n in sorted(names):
            rows.append("name %s %s" % (stem, n))
        for n in sorted(macros):
            rows.append("macro %s %s" % (stem, n))
        rows.append("")
        print("%-14s %-8s names=%-4d macros=%d"
              % (stem, "whole" if whole else "partial", len(names), len(macros)))

    Path(args.out).write_text("\n".join(rows) + "\n")
    print("wrote", args.out)


if __name__ == "__main__":
    sys.exit(main())
