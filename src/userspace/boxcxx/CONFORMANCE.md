# boxcxx — C++23 conformance

`boxcxx` is BoxOS's own implementation of the C++ standard library and of the
Itanium C++ ABI runtime. It is not a port of libstdc++ or of libc++: every
header under `include/std/` was written for this kernel, and the runtime
(exceptions, RTTI, unwinding, TLS) is implemented in `src/`.

This document is the honest inventory of where boxcxx stands against the
standard. It records what is missing, what deviates and why, where boxcxx is
deliberately stricter, and — because it matters when you are deciding whom to
trust — the places where boxcxx is right and the reference implementations are
wrong.

**Working reference:** ISO/IEC 14882 C++23, document N4950. Where a rule comes
from a later draft (C++26) and was adopted anyway, that is stated at the entry.

**C++26 on top of that baseline.** From Ф32 boxcxx implements named C++26
features and *claims their feature-test macros*, unconditionally — there is no
`__cplusplus` gate, and the library is still compiled at `-std=gnu++23`. Both
reference implementations hide every C++26 macro behind `__cplusplus > 202302L`;
this is a deliberate deviation, and the reasoning is in §1.3. Each such macro
names its paper where it is defined. C++23 remains the baseline: a macro whose
C++26 feature is *not* implemented keeps its C++23 value.

## How to read the per-header entries

| Mark | Meaning |
|---|---|
| `!` | **Silently wrong.** boxcxx compiles and runs, but the result is one the standard forbids. These are the entries that can cost you a bug. |
| `+` | **Stricter than the standard.** boxcxx rejects or bounds something the standard permits. Portable code is unaffected; code that relied on the slack is not. |
| `~` | **Deviation by decision.** Deliberate, with the reason recorded. |
| `–` | **Absent.** The header exists; this part of it does not. |
| `?` | **Unspecified by the standard.** This records the choice boxcxx made, so you can rely on it here without believing it is portable. |
| `✓` | **Closed.** It used to deviate and no longer does. Kept, with what it used to do, because that is what explains why code written against the old behaviour changed — and because a list that quietly deletes its own history is not an inventory. |

## What the library is, in numbers

| | |
|---|---|
| Standard headers provided | **109** — 104 of the 105 C++23 [headers] name (1 absent, §1), plus `<stdatomic.h>` and four of C++26: `<inplace_vector>`, `<debugging>`, `<stdbit.h>`, `<stdckdint.h>` |
| Internal implementation leaves (`include/std/__bits/`) | 165 |
| Header source | ~119 000 lines |
| Feature-test macros defined | 239 — 166 at their C++23 value, 73 carrying a later one (measured against libstdc++ 16.1 at `-std=c++23`) |
| BoxOS-native headers (`include/box/cxx/`) | 33 (§5) |
| In-tree conformance suite | `src/userspace/apps/cxxtest.cpp` — 256 phases (237 of them the numbered `PhaseN` series), 6 665 runtime checks, 2 069 `static_assert`s |
| Gate run on every commit | BIOS and UEFI × 1 and 16 cores, `-cpu max` |

The four counted rows drifted three times before the rule was written down, so
here it is: headers and leaves are the *regular files* in `include/std` and
`include/std/__bits` (`ls -1 include/std` returns one more than the header
count, because `__bits` is an entry too — the rule used to say plain `ls -1`,
and Ф38 re-derived every row and found that one off by exactly that
directory); macros are `#define __cpp_lib_` lines from `-dM -E` on a
translation unit containing only `#include <version>`; phases are ROWS OF THE
`kPhases` TABLE in `cxxtest.cpp` (see the drift note below); checks are
occurrences of `Check(`, `CheckText(` and `CheckTextW(` in `cxxtest.cpp` and
`static_assert`s are occurrences of the bare token `static_assert` (which is 17
more than `static_assert(`, the difference being the times the keyword is
named in a comment — the two rows were never counted the same way, and saying
so is cheaper than renumbering both; the difference was 24 at Ф45 and is
re-derived, not carried forward, every time this row moves). `tools/cxx_ftm_audit.sh` re-derives the
macro count and checks it against [version.syn] on every run.

The phase count has drifted four times, in both directions, so it is stated
with the rule that produces it — and as of Ф43 the rule stopped being a grep.
`main` is a TABLE now: one `{name, function}` row per phase, walked in order,
with the launch args selecting which rows run. The count is therefore
`sizeof(kPhases)/sizeof(kPhases[0])`, and the program PRINTS it — every run
ends in `ALL PASS` or `SUBSET PASS: N of M phases`, so the number in this table
can be checked against a boot log instead of against a grep that has to be
maintained. That is **252** — 233 purely numbered, 18 suffixed (`Phase4a`,
`Phase7b`, `Phase9a2` and the rest) and `PhaseCurrent`. It rose by four across
Ф43: `Phase225`, `Phase226` and `Phase227` are new, and `phase2` — the compile-time header
torture, which used to be an unnumbered tail call after the loop — became an
ordinary row.

**The sixth drift was the header row against its own breakdown, and Ф44-f
found it by counting the files.** The row said 108 while the words beside it
said 102 + 5, which is 107, and the tree held 108 — the total had been
corrected at Ф45 and the breakdown that produces it had not. Both are re-derived
here, and they now agree at 109 because `<regex>` and `<valarray>` are in the
tree: the reversal of an exclusion has to move six numbers, and Ф44 shipped
`<regex>` three commits before this document admitted it existed.

**The fifth drift was this paragraph against the table again, and Ф45's door
found it by re-deriving both.** The table said 249 phases while this paragraph
said 245, so the document contradicted itself about the one number it had
already recorded four corrections on — and both were wrong, because the rule
above produces 252. The suite row's other two numbers were stale by 165 checks
and 24 `static_assert`s at the same time, which is what happens when a session
adds phases and copies the previous session's row. All six rows are re-derived
here from the commands the rule names, and the three that move together —
phases, checks, `static_assert`s — were taken in one pass rather than three. The 166 recorded at Ф33 was the numbered series alone, which is
why both numbers are given above: neither can drift without the other
contradicting it.

**The third drift was this paragraph against the table two rows up.** Ф41
raised the table to 221 and left the sentence here saying 217, so the document
disagreed with itself about a number it had already been corrected on twice.
Ф42 re-derived both from the same two commands and they now differ by exactly
the suffixed phases, which is the only difference they are allowed to have.

**The fourth drift was the whole table, and Ф42 caused it by fixing the
prose.** Ф42-g rewrote every sentence that named a wide feature and left the
counted rows exactly as it found them, so on the day it shipped the document
said 102 headers five lines above §1.1 saying 102 of 105 are IN the tree and 3
are not — 107 files described as 102, and a header count contradicting its own
breakdown. The suite row was stale by two phases, 66 checks and 26
`static_assert`s at the same time. Ф43 re-derived all six rows from the
commands above; **the lesson is that this table has now drifted every time
someone edited the document without running them**, which is why the numbers
here are worth exactly as much as the last person's willingness to type six
shell commands, and no more.

Built freestanding: `-nostdinc++ -nostdlib -ffreestanding -fno-builtin`, with
`-fexceptions -frtti -fcoroutines -fasynchronous-unwind-tables
-fcf-protection=full`. Exceptions and RTTI are fully supported and are used by
the library itself; there is no "no-exceptions" configuration.

> **One measured fact worth knowing before you read anything about performance:**
> `src/userspace/apps/Makefile` carries no `-O` flag at all. Every BoxOS
> application, including the conformance suite, is compiled at `-O0`. The
> library archive `libboxcxx.a` is built at `-O2`, but inline code from the
> headers lands in the application's translation unit at `-O0`.

## If something surprised you, start here

| You wrote | What happens | Why / what to use |
|---|---|---|
| `while (std::getline(std::cin, s))` | never ends | The keyboard Current is a live console with no terminator — there is no Ctrl-D here to make one, so `std::cin` never sets `eofbit`. Read a known number of lines, or read from a file. (§2 `<iostream>`) |
| `std::cerr << "..."` | goes to the **screen**, like `cout` | BoxOS's separate diagnostic road is the serial `box::current::log`, under its own name. A `cerr` that only reached a serial cable would be invisible on a machine without one. (§2 `<iostream>`) |
| `std::ofstream f(path)` with a path | opens a file literally named `"a/b.txt"` | TagFS is tag-addressed: the argument is a NAME, not a path. There are no directories to walk and no separator to parse. |
| `std::ofstream f(name)` on a snapshotted file | fails to open | `out` means truncate, and truncating a file a snapshot still reads would corrupt the snapshot. (§2 `<fstream>`) |
| `#include <filesystem>`, `std::filesystem::path` | no such header, no such name | There is no hierarchical path namespace to model, so the `path` overloads of `fstream`'s constructors and `open()` are absent with it. |
| `os << u8"text"`, `os << L"text"` | does not compile | The inserters are deleted, as [ostream.inserters.character] requires. A narrow stream does not transcode; convert explicitly. (Until Ф31e-a this compiled and printed the pointer address.) |
| `for (auto& [k, v] : m)` over `flat_map` or `box::flat_hash_map` | does not compile | The iterator hands out a proxy, not a reference to a pair. Use `auto` or `auto&&`. |
| `constexpr std::string s = "a long one…";` | not a constant expression | A string that outgrows the inline buffer owns an allocation, and no allocation outlives constant evaluation. Up to 15 characters it works and the object lives in the image. Both mainstream libraries draw the line in the same place. (§2 `<string>`) |

---

# 1. What is absent entirely

## 1.1 Headers that do not exist (1)

**These counts are now derived, not maintained by hand.** The two tables of
[headers] name 105 headers in C++23; 104 of them are in the tree and 1 is not,
which accounts for the whole list apart from the deprecated `<codecvt>`. Five
more files sit beside them: `<stdatomic.h>`, which C++23 specifies outside
those tables ([stdatomic.h.syn]), and four C++26 headers — `<inplace_vector>`,
`<debugging>`, `<stdbit.h>`, `<stdckdint.h>` (all §2). 104 + 5 = the 109 files in
`include/std`.

Deriving them found something a hand-maintained list had been hiding since the
document was written: **`<ctime>` was in neither column.** It was not listed as
absent and it was not in the tree; it simply never appeared, and the totals
still added up because they were adjusted to each other rather than to the
standard. Ф41-e-2 built it, so it is now in the tree rather than in neither
column — and the count above is the output of a diff between [headers] and
`ls include/std`, not a number anyone maintains.

### C library wrappers — 21 of 21 provided

**None is absent.** Ф42 finished the set that Ф41 began, and the sentence that
used to open this section — "BoxOS has no libc" — has been retired rather than
qualified: it was true, and it was the reason given for headers it said nothing
about.

Provided since Ф41: `<cassert>` `<cctype>` `<cerrno>` `<cfloat>` `<climits>`
`<cstdarg>` `<csignal>` (Ф41-a), `<cstring>` `<cstdlib>` (Ф41-b), `<cfenv>`
(Ф41-c), `<csetjmp>` (Ф41-d) and `<cinttypes>` `<clocale>` `<cuchar>` (Ф41-e-1)
`<ctime>` (Ф41-e-2) and `<cstdio>` (Ф41-f/g) — all §2. `<cwctype>` joined them
in Ф42-a (§2).

**The entry here used to name all seventeen and give one reason for all of
them — "BoxOS has no libc" — and that reason was doing two different jobs.**
It is true, and it says something real about `<cstdio>`, which needs a `FILE*`
that nothing below it implements. It says nothing at all about `<cctype>`,
whose fourteen functions are the classification of the only locale that exists;
about `<cfloat>` and `<climits>`, which are the compiler's own macros; about
`<cstdarg>`, whose contents only the compiler can provide in the first place;
or about `<cassert>` and `<csignal>`, which are two dozen lines over a
termination path the runtime already had to have. Seven headers were absent
because of a sentence about the eighth.

What each of the seven turned out to rest on is in §2. Two of them cost more
than they looked: `<climits>` cannot be the one-line wrapper `<cfloat>` is
(the compiler's `<limits.h>` withholds the long long limits from C++), and
`<csignal>` is the reason `std::abort` exists at all here — which is the reason
the library's fatal path stopped running teardown on the way out (§2
`<csignal>`).

Userspace links `boxlib`, the native BoxOS library, whose vocabulary is
Manifests, Crates, Touch and TagFS rather than POSIX. That remains the shape of
everything underneath these headers: `<cerrno>`'s `errno` is a C contract and
not a BoxOS one, and nothing in boxlib or `box::` will ever set it (§2
`<cerrno>`).

**`<cwchar>` used to be absent for a different reason than the rest of that
list:** it was said to belong to the wide-character exclusion of §1.2 because
it "carries an entire second formatted-I/O engine". Half of that was right and
the half that mattered was not. It does carry the wide formatted-I/O surface —
and that surface turned out to need no second engine at all. Every conversion
except the character and string ones produces ASCII, so a directive is
formatted by the narrow engine and widened, which is how the correctly-rounded
floating-point path of Ф27 arrived without being ported. What the wide layer
owns is the four conversions whose width is counted in characters rather than
bytes, and that is a page of code, not an engine. It shipped in Ф42-b (§2).

`<cwctype>` used to be named here beside it, on the grounds that it "carries
`wctype`/`wctrans`, which are locales". **That reason was wrong, and building
the header is what showed it.** `wctype("alpha")` and `wctrans("tolower")` are
not a locale database: C fixes the twelve property names and the two mapping
names itself, and every answer behind them is a Unicode property. The header
needs no locale to exist and does not consult one — which is fortunate, because
BoxOS has exactly one and it can never change. It shipped first in Ф42-a, ahead
of the streams, for exactly that reason: nothing in it depends on the rest of
the wide layer.

`<stdatomic.h>` used to be listed here as absent. It is **provided** as of Ф34:
it is a pure using-declaration header over `<atomic>`, so `_Atomic(T)` means
`std::atomic<T>` and every name [atomics.syn] declares is reachable from the
global namespace — there is no second implementation to keep in step. Writing
it is what surfaced the missing `atomic_int_least*_t` / `atomic_int_fast*_t`
aliases, which [atomics.syn] has listed since C++11 and `<atomic>` never had.
The two C23 headers C++26 adopts, `<stdbit.h>` and `<stdckdint.h>`, are
provided for the same reason and on the same terms (§2).

`<cstddef>` and `<cstdint>` were provided long before Ф41, because they are pure
type and macro headers with no runtime behind them, and `<cmath>` since Ф10;
fourteen of the twenty-one C headers are therefore in the tree today. Independently, the compiler's own
freestanding C headers remain available and are used by the library itself:
`<stdint.h>`, `<stddef.h>`, `<stdarg.h>`, `<limits.h>`, `<float.h>` come from
GCC, not from a libc, and resolve normally — with one measured hole, recorded
in §2 `<climits>`, where the compiler's header is deliberately C-only.

**The `<name.h>` forms are not provided.** [depr.c.headers] keeps `<string.h>`,
`<stdlib.h>` and the rest as deprecated compatibility headers that put their
names in the global namespace; boxcxx ships only the `<cX>` spellings. The
names still reach the global namespace — every `<cX>` header here declares them
in `std` and then makes them visible unqualified, which [headers]/5 explicitly
leaves free — so ported code that calls `isdigit(c)` compiles; code that
`#include <ctype.h>` does not.

### Excluded by decision — 1

| Header | Why |
|---|---|
| `<filesystem>` | There is no hierarchical path namespace to model. TagFS is tag-addressed: a file is found by the tags it carries, not by where it sits. This is also why `<fstream>`'s `filesystem::path` overloads are absent (§2 `<fstream>`). |

**`<regex>` and `<valarray>` were on that list until Ф44, and what stood in the
"why" column for both of them was the sentence "Excluded by plan".** That is
not a reason; it is a record that nobody had written one. The entry beside them
is a reason — TagFS is addressed by tags, so there is no path to model, and no
amount of work would make `<filesystem>` mean anything here. Ф0 excluded five
headers in one line in June, and by Ф43 three of the five had been reversed
(`<fstream>`/`<iostream>` in Ф36, the wide streams in Ф42, the locale facets in
Ф43), each time because the stated reason turned out to be about something
else. These two were the remainder.

**What made `<regex>` cheap was paid for by other phases.** Its cost was never
the engine: `regex_traits` sits on `ctype` and `collate`, and `wregex` on the
whole wide layer, and at Ф0 neither existed. By Ф44 both did.

**The engine is a Pike VM, and that is the interesting part.** Both reference
libraries hand the application a backtracking matcher, which is the `timeout`
paradigm in the world of strings: it hopes it will finish. Measured against
them, `/(a+)+b/` doubles per character in libstdc++ — 26 characters take five
seconds and there is no ceiling at all — while libc++ refuses with
`error_complexity` at thirteen characters on a question whose answer is a
trivial *no*. Here the linear machine answers both in microseconds, because the
time a match takes is set by the shape of the input rather than by luck; only
back-references, which are not regular, take the second path, a bounded
backtracker with an explicit step budget that reports `error_complexity` when
it is spent rather than hanging. [re.err] names that error for exactly this.
See §2 `<regex>` for the normative forks that were decided from the text
against both libraries.

**`<valarray>` is in the tree as of Ф44-f, and is the only header here whose
arithmetic is run by a crew** — see §2 `<valarray>`.

**`<iostream>` and `<fstream>` used to be on that list, and no longer are.**
The entry for them read, in substance, that BoxOS does not have the Unix
stdin/stdout/stderr model and does not reach files through a path-opened byte
stream — both of which are still true, and neither of which turned out to be a
reason the headers could not exist. What the two headers actually needed was a
channel to bind to, and BoxOS has one: the Current spine. `std::cout`, `cerr`
and `clog` are the standard's names for the `"screen"` Current; `std::cin` is
the `"keyboard"` Current; a `basic_filebuf` is a `"file:"` Current. None of
them is a descriptor, and the `s` a filebuf opens is a TagFS name, not a path.

Reversing the exclusion cost one thing that was not a C++ problem at all.
[filebuf.members] Table 122 says plain `ios_base::out` means `"w"`, and `"w"`
truncates — and TagFS could not truncate. `tagfs_write` only ever grew a file,
so **every shorter rewrite in the system left the old tail readable behind the
new content**, and had done since TagFS was written. Ф36 built the primitive
(`tagfs_truncate_file` → `STORAGE_OBJ_TRUNCATE` → `file_truncate` →
`current_resize`); the C++ header is what finally asked for it. Its two
refusals are recorded in §2 `<fstream>`.

### C++23 features not implemented — 0

Every C++23 header this library ever intended to provide now exists. What
remains absent is the C-library wrappers above and the three excluded by
decision; the feature list that used to sit here is empty.

**`<execution>` left it in Ф40.** Its entry said "not implemented; the parallel
overloads of the algorithms are absent with it", and the header was the easy
half: four empty classes and a trait. The half that took the work is what
`par` runs ON — a brigade of strands that lives in the cabin, sleeps on the
kernel's address-park between jobs and is reused by every call. See
`<execution>` in §2 for what it does, what it deliberately does not, and which
overloads exist so far.

**`<stdfloat>` left that list in Ф40, and its entry — "no extended
floating-point types" — was the most wrong thing in this document.** The
compiler predefines all five `__STDCPP_*_T__` macros, so `_Float16`,
`_Float32`, `_Float64`, `_Float128` and `__bf16` had been standard
floating-point types by [basic.extended.fp] in every translation unit for as
long as this toolchain has been in use. The header was five aliases of work;
what was missing was underneath it, and had been failing silently: see
`<stdfloat>` in §2 for what that turned out to be.

**`<syncstream>` left that list in Ф40.** Its entry had been honest about
having no blocker left ("what is left is the work itself"), and the work took
one header — but it did not take it alone. The three manipulators
[ostream.manip] gives the feature live in `<ostream>`, and they had been
shipping as no-ops, with a comment that said so and was true: nothing in the
tree could answer `dynamic_cast<basic_syncbuf*>`, so `emit_on_flush` had
nothing to flip. A header that only holds characters would have been half the
feature.

**`<scoped_allocator>` left that list in Ф38 as well**, and its "Not
implemented." was hiding a dependency rather than an absence: the adaptor's
`construct` is *specified* in terms of `uses_allocator_construction_args`
([allocator.adaptor.members]/5), and [allocator.uses.construction] did not
exist in this tree at all. What existed were two partial hand-written copies
of the rule — one in `<__bits/flat_engine>` with no `pair` case whatsoever, one
in `<memory_resource>` whose last branch was commented "best effort" and
constructed the object **without the allocator** where the standard says the
program is ill-formed. Both are gone; the rule is in `<__bits/uses_allocator>`
and everything calls it. Details in §2 `<memory>`, `<memory_resource>` and
`<scoped_allocator>`.

**`<spanstream>` left that list in Ф38 too**, and its entry — "Not
implemented." — was the honest one: unlike `<stacktrace>`'s identically brief
line, nothing was in the way. `<span>`, `<streambuf>`, `<istream>` and
`<ostream>` had all been here since Ф30e, and a buffer that cannot grow needs
less machinery than one that can. Writing it found a defect one layer down,
recorded in §2 `<ostream>`: every formatted inserter shares one width/fill
engine, and that engine discarded what `sputn`/`sputc` told it. No sink in the
tree could refuse before — a stringbuf grows, a filebuf grows, the screen
always accepts — so a short write had never been possible. A spanbuf can
refuse, and until this commit it was refused silently.

**`<typeindex>` left that list in Ф38.** Its entry read "there is no
`std::type_index`", which was true and said nothing about why the header had
never been worth an entry of its own: everything it needs — `type_info::before`
and `type_info::hash_code` — had been in `<typeinfo>` since Ф5. What the header
adds is an *order*, and boxcxx's `before` compares mangled-name pointers, so the
question worth answering was whether that order is a strict total one. It is,
and for a reason that holds by construction rather than by luck: distinct types
have distinct mangled names, and a linker that merges identical string contents
cannot merge contents that differ, so distinct `type_info` objects never share a
name pointer and `==` (pointer identity) and `before` (pointer order) cannot
disagree. Phase192 pins it the only way worth pinning it — irreflexivity,
asymmetry, transitivity and `<=>`/`==` agreement over every pair and triple of a
twelve-type zoo, plus the `set`/`map`/`unordered_map` that would silently lose
keys if any of those failed. Comparing unrelated pointers with `<` is
*unspecified* rather than undefined, and on one flat address space it is a total
order; libstdc++ makes the same trade whenever it may assume merged typeinfo
names.

**`<stacktrace>` used to be on that list, and no longer is.** Its entry named a
real blocker rather than an absence: symbolization needs a symbol table in the
address space, and the loader maps `PT_LOAD` and nothing else, so an image's
`.symtab` is not in it. Ф37 answered that below C++ — Nameplate
(`src/include/nameplate_format.h`) is a table generated at link time from the
image's own symbols and linked back in as an allocatable section, so naming an
address is a binary search through the process's own memory: no syscall, no
allocation, no lock, nothing that has to still be working. The header is §2
`<stacktrace>`, and what it deliberately does not carry — demangled names, file
and line — is recorded there.

## 1.2 Excluded by decision inside headers that do exist

- **Wide characters — CLOSED as of Ф42-g.** This was a flat exclusion until
  Ф42 and is now nothing at all: the bullet stays because the ground it covers
  is worth naming, not because anything on it is still missing. The honest way
  to state it is by what has landed.
  **Landed:** `<cwctype>`, so the classification of a wide character is
  answered from the Unicode Character Database rather than from ASCII;
  `<cwchar>`, so wide strings, the restartable conversions, the seven `wcsto*`,
  the wide character I/O and the whole of `fwprintf`/`fwscanf` work (§2); and
  the `codecvt` facets, so a wide character has a defined way to become bytes
  and back — see the `<locale>` entry below, which is where that stopped being
  a flat exclusion at all.
  **Landed in Ф42-f: the streams themselves.** Every `w` typedef of
  [iosfwd.syn] exists — `wios`, `wstreambuf`, `wistream`, `wostream`,
  `wiostream`, `wstringbuf`, `wistringstream`, `wostringstream`,
  `wstringstream`, `wsyncbuf`, `wosyncstream`, `wfilebuf`, `wifstream`,
  `wofstream`, `wfstream` — along with `wcin`/`wcout`/`wcerr`/`wclog`, the
  free character and string inserters and extractors in all three tiers the
  synopsis declares, and the `wchar_t` half of `<string>`'s `operator<<` /
  `operator>>` / `getline`. **A `wofstream` writes UTF-8**, through the
  `codecvt` Ф42-e built; see `<fstream>` for what that cost and what it took
  away.

  **What that closed, and how it was made to close:** with no
  `operator<<(basic_ostream<wchar_t>&, wchar_t)`, a wide character offered to
  `<<` used to be promoted to `int` and its NUMBER printed — `wos << L'A'`
  wrote `65`, silently wrong output rather than a compile error. Ф42-d pinned
  that WRONG answer in `Phase215` on purpose, so that the commit adding the
  inserter would fail there and be forced to rewrite it. It did, on the first
  run after the inserter landed.

  **Landed in Ф42-g: formatting, which was the last of it.** `<format>` now
  answers for both character types — `basic_format_context` (a class template
  that had been missing outright, see §2), `wformat_context`, `wformat_args`,
  `wformat_parse_context`, `wformat_string`, `make_wformat_args`, the wide
  `format`/`format_to`/`format_to_n`/`formatted_size`/`vformat`/`vformat_to`,
  and every `formatter<T, wchar_t>` the standard asks for: the scalars, the
  strings, the pointers, [format.range], [format.tuple], all twenty-four of
  [time.format]'s, and `formatter<thread::id, wchar_t>`. The wide half of
  [string.conversions] came with it — `to_wstring` and the eight
  `sto*(const wstring&)`, none of which existed.

  **What made it one commit's worth of work rather than a second library** is
  the observation the three wide layers before it had already made: the text
  is produced in ASCII by the engine that exists. `to_chars` writes digits,
  `<chrono>`'s renderer writes "Jan" and "+0300", `"true"` is a literal — and
  ASCII is its own code point. So `CharT` enters at exactly three boundaries:
  the format string on the way in, the sink on the way out, and the arguments
  that really are characters or strings. There is no second parser, no second
  floating-point formatter, no second Unicode table, and `<chrono>`'s
  conversion renderer was not touched at all: it still returns a narrow string,
  and the wide walk around it owns only the literal text BETWEEN conversions,
  which is the one part of a chrono-spec that can be anything.

  **This subphase's oracle was the library's own narrow half**, not a table of
  expected strings: `Phase218` runs every std-format-spec the grammar can build
  through both halves over twenty-one argument shapes — 20,286 spec/argument
  pairs — and requires the same text widened, or the same refusal. A table can
  only say what someone thought to write down. The one presentation allowed to
  disagree is `'c'`, because [tab:format.type.int] makes its answer depend on
  the character type, and the check requires that disagreement to actually
  occur rather than merely permitting it.

  **What Ф42-d did close** is the numeric engine. `<ostream>` and `<istream>`
  formatted every number through helpers declared as `basic_ostream<char>&`
  and `basic_istream<char>&` — 52 and 39 lines bound to one character type,
  and the reason a wide stream could not simply be instantiated. The
  formatting itself did not change: `to_chars` and `from_chars` work in ASCII,
  which is what a number is written in. What became generic is the emission,
  where a run of ASCII widens on the way to the buffer, and the ingestion,
  where a stream character narrows to ASCII before the grammar looks at it and
  anything outside ASCII narrows to a character that is a digit in no base.
  Both conversions are casts rather than facet calls, for the reason §2
  `<cwctype>` gives: one locale, UTF-8, and ASCII values that are their own
  code points.

  A wide `basic_ostringstream` is therefore a working wide stream today —
  width, fill, `adjustfield`, `showbase`, `boolalpha` and the whole of the
  correctly-rounded floating-point path included, with `setfill(L'ж')` filling
  in a character no byte can hold.

- **Locales beyond `"C"`.** There is one locale, and now there is a way to
  build variants of it: Ф43-a made `locale` a container, so a program can
  install its own facet with `locale(loc, new my_facet)`, combine two locales
  by category, and imbue a stream with the result. **All six categories are
  populated as of Ф43-d-3** — `ctype`, the numeric family, `collate`,
  `messages`, the monetary family and the time family, each with its `_byname`
  form. What is still absent is a second locale to *name*: `locale("de_DE")`
  throws rather than answering as `"C"`, because there is one locale here and
  saying otherwise would be the lie. The `L` format specifier WORKS as of
  Ф43-e-1 — it reads the `numpunct` of whichever locale the formatting context
  carries — and `<format>` gained the six locale-taking overloads of
  [format.functions] it had never had. Details and reasoning are in §2
  `<locale>` and §2 `<format>`.
- **Time zones and leap seconds — CLOSED as of Ф45.** This was the last flat
  exclusion in this section. `<chrono>` now carries IANA tzdata 2026c baked
  into the image, the whole of [time.zone], and the 27 leap seconds — see §2
  `<chrono>` for what that cost and what it chose. `chrono::parse` and
  `from_stream` came with it, so the library reads back every format it writes,
  and **`__cpp_lib_chrono` is defined** at last (§1.3). The `clock:tzdb` door
  and the `clock:zone` writer followed, so [time.zone.db.remote] is a working
  clause here rather than a truthful stub — see §2 `<chrono>`.

## 1.3 Feature-test macros

boxcxx defines **239** `__cpp_lib_*` macros. Two properties were verified across
the whole set, not sampled.

> This section said **201** until Ф41, and the number at the top of the document
> said 209 in the same breath, because Ф40 updated the summary table and not the
> prose. Every count below is now the output of the audit script rather than a
> figure carried forward by hand — the same correction §1.1 needed, for the same
> reason.

- **Every C++23 macro carries its N4950 value**, and none is defined at a later
  revision's value. The exceptions are the macros of *implemented C++26
  features*, which carry their C++26 value and are listed at the end of this
  section; there are forty-seven so far, measured rather than counted by hand: every
  macro whose value here exceeds what libstdc++ 16.1 reports at `-std=c++23`,
  plus every macro it does not define there at all.
- **Every one is visible both from `<version>` and from every header
  [version.syn] names as an owner**, as [support.limits.general] requires —
  checked over the full cross-product of 239 macros × 109 headers by
  `tools/cxx_ftm_audit.sh`, against a transcription of [version.syn]'s ownership
  lists kept beside it in `tools/version_syn_owners.txt`.

  **That transcription had seven holes, and Ф41 found them by diffing it
  against [version.syn] rather than reading it.** `__cpp_lib_chrono`,
  `filesystem`, `format`, `formatters`, `is_implicit_lifetime`, `ranges` and
  `result_of_sfinae` were simply not in the file. No false green came of it —
  the audit skips macros boxcxx does not define, and at the time boxcxx defined
  none of the seven — but the gap was a trap armed for the day any one of them
  is defined, at which point its ownership would have gone unchecked in
  silence. **Two have since been defined and the trap did not fire, because
  Ф41 had already disarmed it:** `__cpp_lib_formatters` in Ф42-g and
  `__cpp_lib_format` in Ф43-e-2, both checked against the file the day they
  landed. The one
  macro still absent from the file, `__cpp_lib_modules`, is absent on purpose:
  [version.syn] gives it no "also in" list at all, so `<version>` is its only
  owner and there is nothing for the file to say.

  This entry used to claim the same thing without the tool, on the strength of
  one hand-check. **By Ф32-i it was false**: `__cpp_lib_nonmember_container_access`
  was missing from seven of the twelve headers that own it — `<forward_list>`,
  `<list>`, `<map>`, `<set>`, `<string>`, `<unordered_map>`, `<unordered_set>` —
  because it lives in the `<iterator>` leaf and those seven include only the
  container one. A claim nothing re-checks has a shelf life, so it is now
  re-checked; the macros are declared by name at the top of each owning header
  (`BOXCXX_OWNS_<stem>`) and each `__bits/version_*` leaf defines only what the
  including header declared.

- **The converse does not hold, and cannot.** 193 of the 239 macros are also
  reachable from some header that does not own them. That is not a conformance
  defect — [support.limits.general] sets a floor, not a ceiling — and it is not
  fixable by gating: a header that includes another inherits its macros, so
  `<vector>` sees `__cpp_lib_allocate_at_least` because it includes `<memory>`.
  libstdc++ leaks for exactly the same reason and is only lower because its
  headers include less of each other; its `<vector>` exposes 52 macros where
  ours exposes 101. The gating still narrowed it (161 → 147, and `<string>`
  81 → 73), but the floor is set by the include graph, and closing that is a
  different piece of work than this one. The number is pinned in the audit
  script as a ratchet: if it grows, the gate fails. Ф33 re-pinned it from 147
  to 153, and the delta is accounted for macro by macro: six of its eleven new
  macros live in headers that half the library includes (`<utility>`,
  `<functional>`, `<type_traits>`, `<optional>`, `<ranges>`, `<string>`), and
  the other five do not leak at all because nothing else includes their
  headers. Ф34 re-pinned it 153 → 158 on the same accounting: `constrained_
  equality`, `algorithm_default_value_type`, `copyable_function`,
  `smart_ptr_owner_equality` and `format_uchar` each own a widely-included
  header; `stdatomic_h`, `stdbit_h` and `stdckdint_h` leak nowhere, because no
  header in the tree includes a C-compatibility header. Ф35 re-pinned it
  158 → 161: `indirect` and `polymorphic` are owned by `<memory>`, and the
  third is `__cpp_lib_span`, which had never leaked anywhere at all because
  nothing included `<span>` — `<mdspan>` now does, since `extents` and
  `mdspan` both take one. The three macros `<mdspan>` itself owns leak
  nowhere: nothing includes `<mdspan>`. Ф36 added two headers and one macro
  and did **not** move the pin: it stayed at 161, which is what the accounting
  predicts rather than a coincidence. The metric counts distinct MACROS that
  reach somewhere they are not owned, not (macro, header) pairs — so
  `<fstream>` and `<iostream>` seeing everything `<ios>` and `<string>`
  already leaked adds nobody new, and the one macro Ф36 defines,
  `__cpp_lib_fstream_native_handle`, leaks nowhere because nothing in the tree
  includes `<fstream>`. Ф37 did not move it either, for the same reason:
  `__cpp_lib_stacktrace` is owned by `<stacktrace>`, and nothing includes
  `<stacktrace>`.

  **The accounting stops at Ф37 and the pin does not.** It stands at 193 today,
  re-pinned by the phases in between without the delta being written down here
  — the same failure the counted table above records five times, in a different
  column. The RULE is unchanged and is stated in the audit script: a growth has
  to be accounted for by NEW macros whose owning header is widely included,
  never by a header that started including more than it used to. What is
  missing is the arithmetic for 161 → 192, not the rule. Ф44-f added
  `<valarray>` and did **not** move the pin, which is what the rule predicts:
  the header owns no macro of its own, and the ones it inherits from `<cmath>`
  and `<type_traits>` already reach everywhere. Ф47 DID move it, 192 → 193, and
  the delta is accounted for the way the rule demands: one new macro,
  `__cpp_lib_constexpr_cmath`, whose owner `<cmath>` is included by `<complex>`,
  `<valarray>` and `<random>` — measured, visible from all three, none of which
  owns it. Ф46 moved nothing, because it raised a value and added no macro.

**9 of the 248 macros [version.syn] names are not defined** — measured as a
set difference between the transcription and what a translation unit including
only `<version>` reports, not counted by hand. They divide cleanly:

| Why undefined | Count | Which |
|---|---|---|
| whole-clause requirements relaxations, unprovable by inspection | 2 | `ranges`, `algorithm_iterator_requirements` |
| the feature is excluded or absent here | 5 | `filesystem`, `char8_t`, `result_of_sfinae`, `is_implicit_lifetime`, `modules` |

**`constexpr_cmath` left it in Ф47, and it was the last C++23 LIBRARY FEATURE
this document had to record as absent.** Of the five names still in that row,
`filesystem` is a decision (§1.1) and `char8_t` is blocked by it, and
`is_implicit_lifetime` and `modules` are the compiler's to answer, not the
library's. What is left in the row is nothing anyone here can build.

Half of P0533R9 was already done and nobody had noticed: `<__bits/c_arith>` has
had `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv` and the three `*div_t` as
`inline constexpr` since it was written. **libstdc++ 16.1 does not** — measured,
`static_assert(std::div(7,2).quot == 3)` does not compile there at any `-std`,
while its whole `<cmath>` is constant-evaluable — and that is precisely why it
defines this macro nowhere. Neither does libc++ 22. See §2 `<cmath>` for what
the other half cost.

**`chrono` left that row in Ф45**, and it took the whole clause to do it: the
zone database, the leap seconds, `zoned_time`'s formatter and `chrono::parse`.
It is claimed at **201907L**, the C++23 value. C++26 raises it to 202306L for
hashing the chrono value classes, which boxcxx does not do — measured against
libstdc++ 16.1, which reports 201907L at `-std=c++23` and 202306L at
`-std=c++26`. **`is_implicit_lifetime` is not ours to define at all**:
[meta.unary.prop] makes it a compiler question, and GCC 15.2 has no
`__builtin_is_implicit_lifetime` (measured — `__has_builtin` reports 0). It
belongs with the toolchain workarounds rather than here, and moves there the
day the trait is a library decision again.
| C++26 draft additions no implementation has | 2 | `initializer_list` (202511L), `ranges_generate_random` (202403L) |

**This table had a fourth row until Ф43-f, and it was the largest of them:**
twenty-six freestanding-subset markers, described here as "not features, an
unrun [compliance] audit". Ф41 is why it was a row at all — the transcription
had eleven of the twenty-five and `<version>` had none — and the entry said the
library was "very likely entitled to most of them; entitled is not audited".
Ф43-f ran the audit and the row is gone. Twenty of the twenty-five subsets were
complete already; four wanted C23 functions that neither reference library has;
one wanted the whole of [ptrtag]. `tools/cxx_freestanding_audit.sh` re-derives
the answer on demand and refuses an overclaim, so the row cannot come back
quietly.

`format` left the "excluded or absent" row in Ф43-e-2 and `formatters` left it
in Ф42-g. The last row is measured too: neither libstdc++ 16.1 nor libc++
defines either macro at any `-std` they accept. The governing rule is that a
macro is defined only when the feature
behind it is *complete*, established by reading the implementation rather than by
checking that the headline function exists. That has two consequences worth
stating plainly:

- Some macros stay undefined even though the everyday use of the feature works.
  `__cpp_lib_ranges` is the widest case: it stands for the whole of [ranges] plus
  [specialized.algorithms]. Since Ф31e-e **every entity it promises exists**; what
  holds it back now is that its C++23 value is contested (LWG 3931) and three of
  the four papers behind that value are whole-clause requirements relaxations
  (§2 `<ranges>`).
- In exchange, a defined macro can be trusted. boxcxx never advertises a feature it
  only partly has.

`__cpp_lib_out_ptr`'s C++26 value is held back on the freestanding rule as well
— the paper that raises it is a freestanding one. Only one macro is now
undefined because its header is missing (`filesystem`); `execution` left that
category in Ф40, when the header was built. The in-tree suite pins the absences
as well as the values, so a macro cannot quietly appear — and when one is
closed, the guard fires and forces the pin to be flipped in the same commit.
Since Ф41 the audit script also refuses to run past a macro boxcxx defines that
the ownership map does not know, which is how a hole in the map stops being
invisible.

**On pinning C++23 rather than "latest".** Under `-std=c++23` the reference
libraries each report at least one post-N4950 value; boxcxx reports what C++23
specifies:

| Macro | boxcxx | libstdc++ 16.1 | libc++ 22 |
|---|---|---|---|
| `__cpp_lib_out_ptr` | **202106** | 202311 (P2833R2, post-N4950) | 202106 |
| `__cpp_lib_flat_map` | **202207** | 202207 | 202511 (a later DR) |
| `__cpp_lib_shift` | **202202** | 202202 | 201806 |

**On claiming C++26 macros anyway.** The rule above is about a macro whose C++26
*feature is not implemented*: it keeps its C++23 value, because raising it would
be a claim the library cannot honour. When the feature *is* implemented the
opposite argument applies, and boxcxx defines the macro — with no `__cplusplus`
gate, even though the library is compiled at `-std=gnu++23`. A feature-test macro
answers "does the library in front of me have this?", not "which `-std` did you
pass?"; `std::stringbuf(sv, alloc)` is genuinely reachable at `-std=gnu++23`
here, so hiding the macro would be a lie by omission, which is the worse of the
two directions. The reference libraries gate because they must not put C++26
names into a strictly-conforming C++23 program; boxcxx is the single library of a
single target and has no such second audience. Three C++26 features were in the
tree before this policy existed (P2510R3's pointer `0` presentation,
`enable_nonlocking_formatter_optimization`, and P2495R3's `basic_stringbuf`
constructors) — the library simply stayed silent about them.

| C++26 macro claimed | Value | Paper | Since |
|---|---|---|---|
| `__cpp_lib_sstream_from_string_view` | 202306 | P2495R3 | Ф32-a |
| `__cpp_lib_constexpr_algorithms` | 202306 | P2562R1 | Ф32-b |
| `__cpp_lib_ranges_reserve_hint` | 202502 | P2846R6 | Ф32-c |
| `__cpp_lib_ranges_concat` | 202403 | P2542R8 | Ф32-d |
| `__cpp_lib_span` | 202311 | P2821R5 | Ф32-e |
| `__cpp_lib_to_chars` | 202306 | P2497R0 | Ф32-e |
| `__cpp_lib_variant` | 202306 | P2637R3 | Ф32-e |
| `__cpp_lib_reference_wrapper` | 202403 | P2944R3 | Ф32-e |
| `__cpp_lib_saturation_arithmetic` | 202311 | P0543R3 | Ф32-e |
| `__cpp_lib_is_sufficiently_aligned` | 202411 | P2897R7 | Ф32-e |
| `__cpp_lib_string_subview` | 202506 | P3044R2 | Ф32-e |
| `__cpp_lib_bind_front` | 202306 | P2714R1 | Ф32-f |
| `__cpp_lib_bind_back` | 202306 | P2714R1 | Ф32-f |
| `__cpp_lib_not_fn` | 202306 | P2714R1 | Ф32-f |
| `__cpp_lib_exception_ptr_cast` | 202506 | P2927R3 | Ф32-f |
| `__cpp_lib_ranges_cache_latest` | 202411 | P3138R5 | Ф32-g |
| `__cpp_lib_ranges_as_input` | 202502 | P3137R3 | Ф32-g |
| `__cpp_lib_atomic_min_max` | 202403 | P0493R5 | Ф32-h |
| `__cpp_lib_optional` | 202506 | P2988R12 | Ф33 |
| `__cpp_lib_optional_range_support` | 202406 | P3168R2 | Ф33 |
| `__cpp_lib_constant_wrapper` | 202606 | P2781R9 + LWG 4383/4500 | Ф33 |
| `__cpp_lib_function_ref` | 202604 | P0792R14 + P3961R1 | Ф33 |
| `__cpp_lib_inplace_vector` | 202603 | P0843R14 | Ф33 |
| `__cpp_lib_debugging` | 202403 | P2546R5 | Ф33 |
| `__cpp_lib_philox_engine` | 202406 | P2075R6 | Ф33 |
| `__cpp_lib_bitset` | 202306 | P2697R1 | Ф33 |
| `__cpp_lib_is_virtual_base_of` | 202406 | P2985R0 | Ф33 |
| `__cpp_lib_ranges_indices` | 202506 | P3060R2 | Ф33 |
| `__cpp_lib_to_string` | 202306 | P2587R3 | Ф33 |
| `__cpp_lib_associative_heterogeneous_insertion` | 202306 | P2363R5 | Ф33 |
| `__cpp_lib_format` | 202311 | P2905R2 + P2918R2 | Ф46 |

Three of those carry a value the current working draft has already moved past,
and deliberately: `__cpp_lib_to_chars` is at P2497R0's 202306 rather than the
draft's 202606, `__cpp_lib_saturation_arithmetic` at P0543R3's 202311 rather
than 202603, and `__cpp_lib_atomic_min_max` at P0493R5's 202403 rather than
202506, which would additionally promise P3309's constexpr atomics. A macro
names the newest feature actually present, so a later paper that is not
implemented does not get to raise it.

Two of Ф33's carry a value **higher** than libstdc++ 16.1's, for the same
reason read the other way: `__cpp_lib_function_ref` is at 202604 rather than
202603 because P3961R1 — build a function_ref from another specialization by
copying its thunk instead of wrapping it — is implemented here and is not
implemented there; `__cpp_lib_constant_wrapper` is at 202606 rather than 202603
because LWG 4383's SFINAE-friendly pseudo-mutators are. In libstdc++ 16.1
`requires { ++std::cw<1>; }` is a hard error; here it is `true`, and the result
is `cw<2>`.

`<ratio>`'s C++26 addition (P2734R0's quetta / ronna / ronto / quecto) is
**not** implemented and its macro is not claimed, because on this target there
is nothing to implement: `intmax_t` is 64 bits, 10^30 does not fit in it, and
the paper defines those four only when `intmax_t` can represent them.
libstdc++ 16.1 does not define them either.

# 2. Per-header deviations

Only headers with something to record appear below. A header's absence from this
section means no deviation is *known* — which is not the same as a proof of
conformance. It means the phase that built it closed its audit findings and
nothing has been found since.

## `<algorithm>`

- `✓` Added in Ф34: **P2248R8 default value types.** Every algorithm that takes
  a value now defaults that parameter's type from the iterator (or, for the
  projected ranges forms, from `projected_value_t`), so `ranges::find(v, {})`
  and `std::fill(f, l, {})` name the element type once instead of twice. Eight
  headers co-own `__cpp_lib_algorithm_default_value_type`; the containers'
  `std::erase(c, {})` is part of it.
- `✓` Closed by the same work: **`ranges::replace` and `ranges::replace_copy`
  forced the searched-for and written values to be the SAME type.**
  [alg.replace] has spelled them `T1` and `T2` since C++20, so
  `ranges::replace(v, 1, 9.5)` over a `vector<double>` did not compile.
- `✓` Closed in Ф31e-b-1: `mismatch` had no four-iterator overloads — the unsafe
  shape N3346 added them to replace, where the second range's end is never
  consulted. The call was not silently accepted, but the diagnostic was poor: the
  fourth argument bound to the `Pred` parameter of the three-iterator overload and
  the failure appeared inside the body. Which overload four same-type iterators
  select is settled by partial ordering, not by a constraint. This closed
  `__cpp_lib_robust_nonmodifying_seq_ops`.
- `~` `stable_sort` and `inplace_merge` are not `constexpr`. This is correct for
  C++23; P2562 (C++26) would change it.
- `✓` Closed in Ф31e-g-3: `stable_partition` was *declared* `constexpr` and could
  never be constant-evaluated — any call with a non-empty range reaches
  `::operator new(size_t, nothrow_t)`. The annotation is gone rather than made
  true: C++23 does not ask for a constexpr `stable_partition` (P0202's original
  exclusion list names it, and P2562R1 is C++26), and `ranges::stable_partition`
  next to it already said so. A specifier that promises what the function cannot
  do is worse than its two honestly-unannotated siblings, because the error
  appears only at the point of use.
- `✓` Closed in Ф31e-g: **`std::sample` and `std::shuffle` did not exist** —
  only the `ranges::` forms did, so the spelling in every pre-ranges algorithm
  text failed to compile. `shuffle`'s absence was recorded nowhere; no
  feature-test macro gates it. The classic forms delegate to the `ranges::`
  engines rather than carrying a second copy of the selection- and
  reservoir-sampling logic. Closed `__cpp_lib_sample`.
- `~` The parallel overloads do not exist — `<execution>` does not exist (§1.1).

### P2562R1 — constexpr stable sorting (Ф32-b)

- `✓` `stable_sort`, `stable_partition`, `inplace_merge` and their three
  `ranges::` forms are constant-evaluable. In a constant expression there is no
  buffer to allocate — `::operator new` is not one — so they take the in-place
  rotate merge, which is exactly the path [stable.sort] and [alg.partitions]
  already specify for "no additional memory available"; only the comparison
  count differs, never the result or the stability. `inplace_merge` never
  needed a buffer in the first place. Ф31e-g-3 had removed `stable_partition`'s
  `constexpr` because it was a lie at the time; it is back, now true. This
  raised `__cpp_lib_constexpr_algorithms` to its C++26 value. P0202's other two
  exclusions, `shuffle` and `sample`, are not part of P2562R1 and are still not
  constexpr.

## `<array>`

- `✓` Closed in Ф31e-b-1: `array<T,0>` was missing its whole element and reverse
  surface — `operator[]`, `front`, `back`, `rbegin`, `rend`, `crbegin`, `crend` —
  all of which the non-zero specialization has. Calling them is undefined;
  declaring them is not optional, and generic code over both sizes needs them.
  (`✓` Closed in Ф31e-a: `get<I>` on a `const array&&` now returns `const T&&` as
  [array.tuple] specifies, instead of binding to the `const array&` overload and
  handing a forwarding layer an lvalue.)

## `<atomic>`

- `✓` Added in Ф34: **`atomic_ref<const T>` and `atomic_ref<volatile T>`**
  (P3323R1) and `address()` (P2835R6). The read-only form carries exactly the
  operations that do not modify — `load`, `wait`, `is_lock_free`, `address`,
  `operator value_type` — so a cell another cabin owns and this one only
  observes is a *type*, not a convention, and `store` on it is a compile error
  rather than a review comment. The volatile form is the MMIO shape and keeps
  the full set; a `static_assert` refuses it when the operations would not be
  lock-free, since a lock-pool fallback reads the object more times than the
  program wrote. `atomic<cv T>` is now ill-formed, as the same paper requires.
- `✓` Closed in Ф34: the **`atomic_int_least*_t` / `atomic_int_fast*_t` alias
  families were missing entirely**, all sixteen of them. [atomics.syn] has listed
  them since C++11; writing `<stdatomic.h>`, which re-exports the whole set, is
  what surfaced it.
- `✓` Closed in Ф31e-d: **the volatile half of the arithmetic surface did not
  exist.** [atomics.types.int], [atomics.types.float] and [atomics.types.pointer]
  each declare every compound assignment and every `++`/`--` twice, volatile and
  not, and only the non-volatile ones were here — so `volatile atomic<int> c;
  ++c;` did not compile, and neither did `volatile atomic<double>::fetch_add`.
  The absence was in neither this document nor `<version>`, which recorded only
  the floating-point half of it. This closed `__cpp_lib_atomic_float`.
- `✓` Closed in Ф31e-d: `atomic_ref`'s unified primary had no `difference_type`
  and none of the five compound-assignment operators, though `fetch_add`/`++` were
  there — so `atomic_ref<int>::difference_type` was ill-formed and `r += 1` did
  not compile, on the one type in the header written for a caller-owned cell.
  Closed `__cpp_lib_atomic_ref`.
- `✓` Closed in Ф31e-d: six of the ten `atomic_flag` free functions were absent —
  `atomic_flag_test`, `_test_explicit` and `_wait` for a `volatile atomic_flag*`,
  both forms of `atomic_flag_wait_explicit`, and volatile `atomic_flag_notify_one`
  / `_notify_all`. The member functions behind them carried both cv-forms the
  whole time. Closed `__cpp_lib_atomic_flag_test` and `__cpp_lib_atomic_wait`.
- `?` **A 16-byte atomic load writes to memory.** `atomic<T>::is_always_lock_free`
  is `true` for a 16-byte `T`, and the operations lower to `__atomic_*_16` calls
  which boxcxx implements itself (`src/runtime/atomic_support.cpp`) with
  `lock cmpxchg16b`. The load is a self-comparing `cmpxchg16b` — a locked
  read-modify-write — so a 16-byte atomic placed in a read-only mapping will
  fault. This is the classic libatomic caveat, and it is the price of the trait
  being true; the operations do link and work, because the tree supplies the
  entry points rather than relying on an external libatomic.
- `~` Atomics that are not lock-free (any size outside 1/2/4/8/16) go through a
  **cabin-private lock pool**. Cross-cabin shared memory therefore supports only
  the lock-free sizes; the header states this contract.

### P0493R5 — atomic fetch_max / fetch_min (Ф32-h)

- `✓` `fetch_max` and `fetch_min` on the integral and pointer specializations of
  both `atomic` and `atomic_ref`, in every cv-form the synopsis lists twice, plus
  `atomic_fetch_max` / `atomic_fetch_min` and their `_explicit` forms. Floating
  point deliberately has none: R5 dropped the `[atomics.types.float]` wording
  outright, because NaN and signed zero do not survive `max` and `min`. `bool`
  has none either — it is not an *integral-type* in `[atomics.types.int]`'s
  sense — and the generic `atomic<T>` never had them.
- `✓` Signed types compare **signed**. Every other `fetch_key` on a signed type
  is computed as if converted to unsigned, and this paper amended that sentence
  to read "except for `fetch_max` and `fetch_min`". Applying the old rule would
  make `-1` the largest number there is; the emitted code is `jle` for
  `atomic<int>` and `jnb` for `atomic<unsigned>` and `atomic<T*>`.
- `+` **The store is conditional below a release, and unconditional at or above
  one.** x86-64 has no max/min instruction and GCC 15 has no builtin, so both
  are a CAS loop, and §5 of the paper is largely about which loop. Skipping the
  store whenever the value already wins is *read-and-conditional-store*, which
  §5.1 writes out and rejects — these are read-modify-write operations like every
  other `fetch_key`. The one relaxation §5.3 grants is that a store is only
  *required* when the caller asked for a release, since a release that stores
  nothing releases nothing; below that the omitted store would only have added a
  modification-order entry carrying the value that was already there. boxcxx takes
  exactly that split, and implements the release side as a single unconditional
  CAS rather than the paper's sketch, which primes with a dummy no-op
  read-modify-write and can then take a second one on top. So
  `fetch_max(v, relaxed)` on a value that already wins executes **no locked
  instruction at all**, and `fetch_max(v, seq_cst)` always executes exactly one —
  which is what keeps a many-core high-water mark off a cache line it never
  needed. libstdc++ 16's fallback loop stores unconditionally for every order.
- `~` The macro is `__cpp_lib_atomic_min_max 202403L`, not the draft's current
  `202506L`. The later value also promises P3309's constexpr atomics, which this
  library does not have; the value names the revision that is implemented, which
  is what a feature-test macro is for. libstdc++ 16 defines 202403L for the same
  reason. `store_max` / `store_min` are a different paper (P3111, under
  `__cpp_lib_atomic_reductions`) and are absent.

## `<barrier>`

- `✓` Corrected in Ф31e-d: this entry used to claim `arrive()`'s `[[nodiscard]]`
  as a boxcxx extension — "the synopsis does not mandate it". **It does.**
  [thread.barrier.class] declares `[[nodiscard]] arrival_token arrive(ptrdiff_t
  update = 1);`, and libc++ 22 carries the attribute for the same reason. The
  claim was recalled, not measured; the code was right and the document was
  wrong.
- `✓` Closed in Ф31e-d: `__cpp_lib_barrier` was withheld on a residual
  `arrive_and_drop` race against a concurrent phase completion. That race was
  disproved during Ф31 and the reasoning in the header was rewritten then, but the
  macro was never released. `arrive_and_drop` arrives at the **current** phase, so
  its arrival is one the drain needs; every successful `arrive()` CAS is
  `acq_rel` on the packed state word, so whoever reads the count down to zero
  reads a value at or after ours in that word's modification order and therefore
  sees the expected-count decrement sequenced before it. cxxtest phase139 drives
  four strands through staggered `arrive_and_drop` rounds against a live
  completion function.
- `~` Precondition violations are loud rather than undefined: `arrive(n)` with
  `n <= 0` or `n` greater than the phase's expected count calls `Panic`. Note the
  asymmetry — the *constructor* does not validate its `expected` argument.

## `<bitset>`

- `✓` Closed in Ф39: `to_string()` and the `basic_string`-taking constructor were
  annotated `constexpr`, correct at run time, and impossible to constant-evaluate,
  because they build a `basic_string` and that was not a literal type. P0980R1
  landed and both fold now, with **no change to this header at all** — which is
  what the note that used to stand here predicted. `__cpp_lib_constexpr_bitset` is
  claimed at 202207L.
- `+` P2697R1's `basic_string_view` constructor is implemented and is likewise
  constant-evaluable; it was the one that already worked while the string
  constructor did not.
- `+` `__cpp_lib_constexpr_bitset` is defined at **202207L**, which is what
  [version.syn] says. libstdc++ 16.1 defines it as 202202L — P2417R2's adoption
  meeting rather than the value the standard settled on. libc++ agrees with the
  draft; measured on both.

## `<cassert>`

The only header in the library with **no include guard around what it
defines**, and that is the specification rather than an oversight:
[assertions.assert]/1 redefines `assert` according to the current state of
`NDEBUG` on *every* inclusion, so a translation unit may switch `NDEBUG` and
include the header again to get the other behaviour. A guard would freeze
whichever state came first. Phase203 is the only test in the suite that
includes a header twice on purpose, and it observes both halves: with `NDEBUG`
the expression must not be evaluated at all, without it exactly once.

- **One parameter, as the standard writes it.** A variadic form would silently
  accept `assert()` and `assert(a, b)`, which are ill-formed programs. Both
  reference implementations take one parameter too, so — here as there — a
  template argument list needs its own parentheses: `assert((is_same_v<A,B>))`.
- **The diagnostic goes to the screen, not the serial log**, on two lines: the
  expression, then file, line and function. Same call Ф36 made for `std::cerr`,
  for the same reason — on a machine with no cable attached, a diagnostic that
  only reaches the serial port is one nobody sees. The format is
  implementation-defined and this one is not glibc's.
- **Failure ends the process through `std::abort`**, so no `atexit` callback
  and no static destructor runs. See `<csignal>` for why that sentence was not
  true of anything in boxcxx before Ф41.

## `<cctype>`

Fourteen functions, all decided by the current C locale — and there is exactly
one, `"C"` (§1.2). So the answers are not a table loaded from anywhere: they are
the classification the standard fixes for that locale, written out as range
tests. Nothing can invalidate them at run time and there is no table pointer to
follow.

- **Not constexpr**, deliberately: [constexpr.functions]/1 forbids an
  implementation from adding `constexpr` to a standard library signature where
  the standard does not, and the standard does not for these. Same rule that
  keeps `hash<vector<bool>>` non-constexpr (§2 `<vector>`).
- Every value from `0` to `255` and `EOF` answers *something* defined: the
  predicates are ranges, so `EOF` (−1) matches none of them, and `tolower`/
  `toupper` return their argument unchanged wherever there is no counterpart —
  which is what makes folding a `getchar()` result safe. Above 127 the `"C"`
  locale has nothing at all, and every predicate says so.
- The names are visible unqualified as well as in `std`, which [headers]/5
  leaves free and both reference implementations also do.

## `<cerrno>`

**BoxOS has no errno register, and this header does not give it one.** That
idiom is deliberately absent from every BoxOS interface — boxlib returns an
`error_t` from the call that failed, `box::error` carries it in C++, and
`box/bay.h`, `box/brook.h` and `box/error.h` each say so in as many words.
Nothing on that path touches the object this header defines, and nothing will.

What the header is, then, is the C standard library's contract and only that:
[cerrno.syn] requires a thread-local modifiable `int` lvalue named `errno`, and
the C functions boxcxx provides (the `strto*` family, and the math functions on
the paths where they choose to) are specified to set it. `errno = 0; x =
strtod(s, &e); if (errno == ERANGE)` is correct C and has to work.

- **`errno` is `thread_local`, as [errno] has required since C++11**, and on a
  system whose whole point is that several strands share a cabin that is not a
  formality. A fresh strand's `errno` starts at zero, which is what `.tbss`
  buys; phase203 pins both properties from a second strand.
- **The macro values are the `std::errc` enumerator values**, and phase203
  static_asserts every single pair. That check is available here and not in
  either reference implementation: theirs come from a libc they do not compile
  against, so the two lists can drift and only a runtime `error_code`
  comparison would notice. Both halves are in this tree.
- The macro set is **exactly** the names `std::errc` has. There is no `EDQUOT`,
  no `ENOTBLK`, no `EUSERS`: a macro naming a condition nothing here can report
  would be an invitation to write a comparison that never comes true.

## `<cfloat>`

[cfloat.syn] says the contents are those of `<float.h>`, and the compiler's
freestanding `<float.h>` is complete for it — measured against the whole
[cfloat.syn] list, including `DECIMAL_DIG`, `FLT_EVAL_METHOD`, the
`*_HAS_SUBNORM`, `*_TRUE_MIN` and `*_DECIMAL_DIG` families and `FLT_ROUNDS`.
So this header is one `#include` and no deviations. Its neighbour `<climits>`
looks identical and is not; see there.

## `<charconv>`

- `~` `__int128` and `unsigned __int128` are deliberately unsupported. The standard
  requires the standard integer types and `char`; adding the extended ones would
  drag in the `make_unsigned<__int128>` question for no gain. `long double` **is**
  supported in both directions, at genuine 80-bit width.
- `~` `from_chars` does not skip whitespace, does not accept `+`, and does not
  accept a `0x` prefix — strictly per [charconv]. The `strtod`-style preamble lives
  in `stof`/`stod` instead.

## `<chrono>`

- `✓` **Time zones exist as of Ф45**, and the three entries that used to stand
  here are gone with them. They used to read: no `tzdb`, `time_zone`,
  `zoned_time` or `leap_second`; `utc_clock` and its siblings as type surface
  with no `now()`; `utc_time` silently equal to `sys_time`; and `clock_cast`
  identity-only. The `!` among them was the only "silently wrong" mark this
  header carried.
  **What is here now:** the whole of [time.zone] — `tzdb`, `tzdb_list`,
  `time_zone`, `time_zone_link`, `leap_second`, `sys_info`, `local_info`,
  `zoned_time`, `zoned_traits`, `nonexistent_local_time`,
  `ambiguous_local_time`, `locate_zone`, `current_zone`, `reload_tzdb`,
  `remote_version` — over IANA **tzdata 2026c**: 598 names, 341 zones, 257
  links, 16 631 transitions and 27 leap seconds. `utc_clock`, `tai_clock`,
  `gps_clock` and `file_clock` have `now()` and their conversions, and
  `clock_cast` joins any pair of them through `clock_time_conversion`.
- `?` **The database is baked into the image, not read from a file.** There is
  no `/usr/share/zoneinfo` to read and no network to fetch from, so
  `tools/gen_tzdb.py` compiles the IANA source with `zic` on a development host
  and emits `<__bits/tzdb_table>` — 116 KB, in exactly one object file, so a
  program that includes `<chrono>` for durations links none of it. This is the
  same shape `<stacktrace>`'s name table takes and for the same reason:
  `locate_zone` makes no syscall, touches no disk and cannot fail for want of
  a file.
- `✓` **[time.zone.db.remote] works, through TagFS rather than through a
  network.** Lay a table under the tag `clock:tzdb` and `remote_version()`
  reports its version and `reload_tzdb()` loads it, pushing a new `tzdb` onto
  the front of the list exactly as the clause describes. With nothing behind
  that door the newest database in reach IS the one in the image, and both
  functions say so — which was already the honest answer before the door
  existed, and is now a case rather than the only case.
  **Both databases stay live.** [time.zone.db.remote]/3 requires a `time_zone`
  handed out before a reload to keep answering after it, so a `time_zone`
  carries the database it came out of rather than an index into a global table:
  there is no longer a "the" table. `tzdb_list::erase_after` really erases, and
  the entry it drops is destroyed, which is what the clause means by
  invalidated.
  **The blob behind the door is the only untrusted input this library has**, so
  `tzdata::Validate` gates it: magic, every section inside the blob, every
  offset inside the section it names, the string pool's terminating NUL, each
  body's varint run decoding to exactly the transitions it claims, a sorted
  name index (`Find` is a binary search — an unsorted index does not fault, it
  answers wrongly) and a leap list that is ordered and carries only ±1. It was
  measured rather than reviewed: on a development host under ASan and UBSan,
  **all 119 052 truncation lengths of the real table were refused, and of
  300 000 byte-level mutations the 29 480 that were admitted were then fully
  exercised without one read outside the blob.** That fuzz is also what found
  three unbounded accumulations in the reader — the POSIX-footer digit runs and
  the varint shift — which no table IANA ships can reach and any file can.
- `~` **The door loads a table whose version DIFFERS, where the clause says
  NEWER.** [time.zone.db.remote]/2 is written for a service that only ever
  moves forward; behind this door is a person, who put that file there on
  purpose, and a deliberate step back to last month's table to undo a bad
  release is a thing a person does. Comparing for difference obeys them;
  comparing for newer would ignore them silently, and they would find out from
  a wrong clock. Owner's decision, recorded here rather than left as a gap.
- `~` **`current_zone()` reads the `clock:zone` tag and answers `Etc/UTC` when
  nothing has set it.** A machine that has not been told where it is has no
  local time to report, and UTC is the same fact `<ctime>` records when it says
  `localtime` IS `gmtime`. Every named zone still works: `locate_zone` answers
  for all 598. The tag is read on every call rather than cached, so the answer
  is current because it looked and not because something remembered to
  invalidate.
  **The other half of that tag is `box::clock::set_zone` (§5) and the
  `timezone` command**, which no standard has a spelling for because setting
  the zone is a question about the machine, not about time. It resolves the
  name against the database before writing and stores the canonical spelling,
  so a Link is stored as its Zone and what is on disk always resolves —
  a name the database does not know would otherwise read back as `Etc/UTC` and
  be discovered an hour later from a clock that is quietly wrong. The write
  multicasts a Touch on `clock:zone` carrying the new name, so a clock already
  drawn can redraw without polling.
- `✓` **`chrono::parse` and `from_stream` exist as of Ф45**, for every type
  [time.parse] names. The ten conversions the locale decides — `%a`, `%A`,
  `%b`, `%B`, `%h`, `%c`, `%x`, `%X`, `%p`, `%r` — are read through the
  `time_get` facet Ф43-d-3 built, not through a second parser, which is both
  what the clause asks for and what keeps one answer to what "Mar" means.
- `~` **`%Ez` and `%z` both accept a colon.** The standard reserves `hh:mm` for
  the modified form; boxcxx takes it either way, because a reader that rejects
  `+03:00` for want of an `E` is useless against real ISO 8601 text and no
  valid input becomes ambiguous by allowing it.
- `!` **This header carried a regression for the length of one commit, and it
  is recorded because of how it arrived.** `formatter<utc_time>` rendered the
  count directly, which was exactly right while there was no leap-second table
  and `utc_time` and `sys_time` were the same clock. Ф45 baked the table and
  every UTC instant started printing 27 seconds late — from a file nobody had
  edited. The code did not change; the truth under it did. It now converts,
  and shows the 60th second inside an insertion.
- `?` `sys_info::save` is derived from the Zone record's STDOFF in the tzdata
  source, because TZif records only a daylight FLAG. Two other derivations were
  tried first and measured wrong — the arithmetic one on 17 927 instants and
  CPython's per-type heuristic on 733, `America/Coyhaique` among them with a
  "daylight saving" of 2 565 seconds. libstdc++ reads the same Rule lines and
  agrees; `zoneinfo` uses the heuristic and does not, on 41 zones.
- `to_time_t` / `from_time_t` work in `std::time_t`, as [time.clock.system]/3
  requires. **This entry used to be a deviation** — they returned `long long`,
  because the name `time_t` belonged to a 20-byte BoxOS structure in
  `box/time.h` and `<chrono>` could not introduce the arithmetic type the
  standard asks for without colliding with it. Ф41-e-2 renamed that structure to
  `BoxTime` (the name `clock_boxtime()` and `rtc_get_boxtime()` had always used
  for it) and built `<ctime>`; the deviation went with the rename rather than
  being worked around again. phase210 asserts the return type, so undoing the
  rename stops the build.
- `?` `file_clock`'s epoch is the Unix epoch (the standard leaves it
  implementation-defined; libstdc++ uses 1601), so a `file_time` renders like a
  `sys_time`.
- `?` `%I`, `%r` and `%p` for durations longer than 12 hours apply **no** reduction:
  `%I` of `hours{300}` is 288. The standard does not define these specifiers beyond
  a day. Both reference libraries now reduce modulo 12/24 and print `12:00:00 PM`
  for the same input; boxcxx does not. All three are defensible; this records ours.
- `?` For calendar values where `ok()` is false the standard leaves the rendering
  unspecified, and the three implementations genuinely differ.
  `year_month_weekday` resolves all date fields through `sys_days` while taking the
  validity of `%b`/`%a` from the stored month and weekday.

## `<climits>`

The header that looks like `<cfloat>` and is not. GCC's `<limits.h>` gates the
three long long macros on

```c
#if defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
```

— a condition no C++ translation unit ever meets, because `__STDC_VERSION__` is
a C macro. So the compiler's freestanding header is **missing `LLONG_MIN`,
`LLONG_MAX` and `ULLONG_MAX` in C++**, all three of which [climits.syn]
requires. boxcxx supplies them from the compiler's own `__LONG_LONG_MAX__`, so
they cannot describe a different target than the rest of the header.

Measured against the entire [climits.syn] list, those three are the only ones
missing; `MB_LEN_MAX`, `CHAR_MIN` and the rest are all present. This was found
the way such things are found here — by writing the static_asserts in phase203
that compare every macro against `std::numeric_limits`, and watching the build
stop.

**And one deliberate deviation from `<limits.h>`: `MB_LEN_MAX` is raised to 4.**
The compiler says 1, and when it does it is not describing this system — a
freestanding implementation with no locale support has single-byte characters by
default. Ф41-b made the `"C"` locale's multibyte encoding UTF-8 (§2
`<cstdlib>`), which fixes `MB_CUR_MAX` at 4, and `MB_CUR_MAX` may never exceed
`MB_LEN_MAX`. The alternative was a second, narrower encoding living inside the
C functions of a system that is UTF-8 everywhere else.

## `<cinttypes>`

Almost entirely macros, and the macros are why it could not be a wrapper: GCC's
freestanding set stops at `<stdint.h>` and never ships `<inttypes.h>`. Before
Ф41-e there was not one `PRI*` or `SCN*` spelling anywhere in the BoxOS tree —
every format string that printed a `uint64_t` was written by hand, per call
site, in whichever modifier the author knew was right.

A `PRI` macro is a promise about the ABI: `PRId64` is `"ld"` only because
`int64_t` **is** `long` here. Everywhere else that promise is made by a vendor
header generated for the target; here it is made by this file, so it is made
checkably. Five `static_assert`s pin the type identities, and phase207 goes
further: a probe carrying `__attribute__((format(printf, 1, 2)))` under a local
`#pragma GCC diagnostic error "-Wformat"` makes **the compiler itself** verify
every macro against the type it is used with. A modifier that stops matching
stops the build, naming the macro and the line, instead of letting `printf` read
the wrong number of bytes off the varargs list and print a plausible lie.

- `~` **`abs(intmax_t)` and `div(intmax_t, intmax_t)` are absent, and their
  absence is required.** [cinttypes.syn] limits those overloads to the case
  where `intmax_t` designates an *extended* integer type. Here `intmax_t` is
  `long`, `<cstdlib>` already declares `abs(long)` and `div(long, long)`, and a
  second declaration would be a redefinition rather than an overload. Both
  reference implementations omit them for the same reason.
- `~` **`wcstoimax` / `wcstoumax` are absent** with the rest of the wide
  library: `<cwchar>` is deferred (§1.1). Naming one is a compile error that
  says which header is missing, not a link failure at the end of the build.
- `?` `strtoimax` and `strtoumax` forward to `<cstdlib>`'s parser rather than
  repeating it, which is exact rather than merely plausible: the `static_assert`
  above pins `intmax_t` to `long`, the type `strtol` already returns. If the
  assumption ever stops holding the build stops with it.
- `?` The exact-, least- and fast-width families coincide at every width, which
  is a fact about this ABI rather than a simplification.
- There is **no `SCNX` family** — `%X` is a printf conversion only. Its absence
  is C's, and phase207 asserts it in the preprocessor, where a macro's existence
  is the kind of question that can actually be asked.

## `<clocale>`

One locale, `"C"`, whose multibyte encoding is UTF-8. That is an answer, not a
shortfall: BoxOS is UTF-8 end to end — the screen, the keyboard and TagFS names
all are — and there is no environment to read a locale name out of (`getenv` is
always `nullptr`, see `<cstdlib>`).

Two consequences are worth stating rather than discovering.

- `+` **`setlocale` never changes anything, so it can never race.** In a hosted
  library this is a global mutable switch that silently changes what `"."` means
  to every number the program prints; glibc's own manual warns that changing the
  locale while another thread formats a number is undefined. Here the `lconv` is
  a constant in read-only storage and `localeconv()` may be called from every
  strand at once. BoxOS gets that property by having refused the switch.
- `~` **An unknown locale name returns `nullptr`.** That is precisely how C
  spells "that selection cannot be honoured", and it is not the same as a quiet
  fallback to `"C"`, which would let a program believe it had got what it asked
  for. `setlocale(LC_ALL, "en_US.UTF-8")` fails; `""` succeeds, because the
  implementation-defined native locale here **is** `"C"`. This entry used to
  note a deliberate ASYMMETRY here — `std::locale` accepted and ignored any
  name while `setlocale` refused it — and that asymmetry is gone as of Ф43-a:
  `std::locale("en_US.UTF-8")` now throws `runtime_error` where `setlocale`
  returns `nullptr`. Two spellings of the same refusal, which is what they
  should always have been.
- `?` Every `char` member of `lconv` is `CHAR_MAX`, which C defines as "this
  locale does not say" — **not** zero, which would be the claim that there are
  no fractional digits. `decimal_point` is `"."`; every other string is empty.
- `?` The member *order* of `lconv` is not fixed by the standard, and the two
  reference implementations genuinely differ (glibc and the BSDs disagree about
  where `int_curr_symbol` goes). boxcxx initialises it with designated
  initializers so a reordering cannot silently put `CHAR_MAX` where a pointer
  belongs.

## `<cfenv>`

The header that made an old claim checkable, and the claim did not survive.

- **Both floating-point units, always.** `float` and `double` report into
  MXCSR; `long double` is genuine 80-bit x87 (Ф27) and reports into the x87
  status word. A `<cfenv>` that read only MXCSR would answer "no exception" for
  every `long double` operation that raised one, so every function here reads
  or writes both, `fenv_t` carries both, and `fesetround` sets both — phase205
  checks the rounding change on a `long double` quotient precisely because a
  MXCSR-only implementation would pass everything else.
- **`feraiseexcept` performs the operation rather than writing the bit.** A
  written status bit is a claim; an executed `1.0/0.0` is the thing itself, and
  it keeps working if a trap is ever unmasked. `fesetexceptflag` is the other
  one — it sets the state WITHOUT raising, which is exactly the distinction
  [cfenv.syn] draws between them. Raising overflow or underflow also raises
  inexact here, which C explicitly makes implementation-defined.
- **No way to unmask an exception.** `feenableexcept` is a glibc extension, not
  [cfenv.syn], and it would be a promise BoxOS cannot keep: an unmasked SIMD
  fault has nowhere to go, because `<csignal>` deliberately has no asynchronous
  half. `FE_DENORMAL` is x86's own extra flag and is named because the hardware
  has it.
- **`#pragma STDC FENV_ACCESS` is not supported by the compiler.** GCC ignores
  it with a warning, so the optimizer is formally entitled to move
  floating-point operations across these calls. Every BoxOS application is
  compiled at `-O0` (see the note in "What the library is, in numbers"), where
  it does not, and the library itself at `-O2` does not reorder across the
  inline asm these functions are built from. It remains a real caveat rather
  than a solved problem.

> **‼ What `<cfenv>` measured on its first run, and what it cost to fix.**
> `math_errhandling == MATH_ERREXCEPT` had been declared since Ф10 — "errors
> are reported by raising the IEEE-754 flags, not through errno" — and until
> this header there was no `stmxcsr` anywhere in the tree, so the claim was
> unverifiable rather than true. The first measurement: `sqrt(-1)` raised
> `FE_INVALID` because boxcxx's `sqrt` IS `sqrtsd`; `log(0)`, `log(-1)` and
> `exp(1000)` raised **nothing at all**, returning the right value as a
> CONSTANT — and a returned constant is not an operation, and only operations
> raise. Ф41-c-2 made every documented error path perform the arithmetic that
> reports it (§2 `<cmath>`), so the claim is now true where [c.math] speaks.
> phase205 checks flag and value together for eighteen cases and checks that an
> ordinary call raises nothing, so neither the behaviour nor this paragraph can
> drift without the suite failing.

## `<cmath>`

- `✓` Closed in Ф47: **P0533R9 — `constexpr` for `<cmath>` and `<cstdlib>`.**
  `__cpp_lib_constexpr_cmath` is defined at 202202L, the C++23 value (§1.3), and
  on this target it could not be a matter of adding a keyword. Every `long
  double` function here is x87 assembly, which no constant evaluation may
  execute, so each carries a twin: `if consteval` takes the `__builtin_*l` form,
  which GCC folds with MPFR at the target's own 80-bit format, and the assembly
  stays for run time — where the builtin would emit a call to a libm this target
  does not have (measured: `__builtin_fmodl` compiles to `call fmodl` at `-O2`).

  **The twin sits AFTER the error branches, never before**, and that is what
  makes [library.c] come out right for free. Every error path performs the
  arithmetic that raises the flag, through a `volatile` operand, and `volatile`
  is not a constant expression — so `logb(0.0L)` and `fmod(1.0, 0.0)` are
  ill-formed in a constant expression, exactly as the standard requires of a
  call that would raise. Hoisting a builtin above them would have answered
  `-inf` or NaN quietly instead.

  **A twin is a second implementation, so phase240 makes the library's own
  run-time half the oracle for it.** Every function P0533R9 lists computes an
  exact result — a remainder, a rounding, a scaling by a power of two, an
  exponent — so there is no rounding choice to differ over legitimately: the two
  halves must agree BIT FOR BIT, sign of zero included, over tables that run from
  the smallest subnormal to the largest finite at all three widths, with the
  run-time side forced through `volatile` so the compiler cannot fold it into the
  same evaluation. 99 call shapes are constant expressions, including
  [cmath.syn]/2's promoting overloads and all five extended floating-point types.

  **Three defects fell out of it, and none of them could have been asked about
  before.**
  - `nextafter(float, float)` read `float(nextafter(double(x), double(y)))` and
    **returned its own argument**: the next `double` after 1.0 rounds back to
    `1.0f`. `nextafterf` carried the same fault through `long double`. Both step
    in `float` now. The boundary of the defect is worth stating, because every
    other `f`-suffixed wrapper here takes the same wide detour and is fine: their
    results are exact, so narrowing is one rounding. `nextafter` is the one
    function whose answer is the SPACING of the narrow type, and a wider type
    does not have it.
  - `fmod` doubled its divisor as `d + d <= r`, which overflows on the largest
    finite value. At run time that is harmless — the comparison against infinity
    answers false and the loop ends correctly, which is why it stood — but an
    overflow is not a constant expression. It now asks `d <= r - d`, the same
    question without computing `2d`; `r - d` cannot overflow and is exact where
    the answer is close, by Sterbenz.
  - `remquo` reached `rint`, and **`rint` must not become `constexpr`**: it reads
    the current rounding mode, which a constant evaluation has none of and would
    answer round-to-nearest whatever the program had set. [cmath.syn] leaves
    `rint`, `nearbyint`, `lrint` and `llrint` out of P0533R9's list for that
    reason; `remquo` takes the builtin at translation time instead, and phase240
    pins all four as *not* constant expressions so a later sweep cannot quietly
    add them.

  C++26's 202306L (P1383R2, `constexpr` `sqrt`/`sin`/`exp`/`log`/`pow`) is **not**
  claimed: those are not constant expressions here, and phase240 pins `sqrt` as
  one of the absences.
- `✓` **Closed in Ф41-c-2: `math_errhandling` used to overpromise.** It expands
  to `MATH_ERREXCEPT` — "errors are reported by raising the IEEE-754 flags" —
  and until `<cfenv>` existed nothing could check it. The measurement (§2
  `<cfenv>`) was that only `sqrt` honoured it, because `sqrt` IS `sqrtsd` and
  the hardware raises; every software path returned `kInf` or `kQNaN`, and
  returning a CONSTANT raises nothing, because only operations raise. Every
  documented error path now produces its value by performing the arithmetic
  that reports it: a pole divides by zero, a domain error divides zero by zero,
  an overflow squares the largest finite, an underflow squares the smallest
  normal. The kernels are untouched — including the correctly-rounded `long
  double` chains of Ф27e — because only the error branches changed, and the
  `long double` helpers do their arithmetic in x87 so the flag lands in the unit
  that computed it. Covered: `log` `log2` `log10` `log1p` `exp` `exp2` `expm1`
  `pow` `asin` `acos` `acosh` `atanh` `tgamma` `lgamma` `fmod` `remainder`
  `remquo` `logb`, in `double` and `long double` (`float` delegates to
  `double`). phase205 checks the flag AND the value for eighteen cases, and
  checks that an ordinary call raises nothing at all.
- `~` **The special functions still do not raise.** The C++17 mathematical
  special functions (`cyl_bessel_*`, `riemann_zeta`, `expint`, the Legendre and
  Laguerre families, …) return the right value on their domain errors without
  raising `FE_INVALID`. [sf.cmath] specifies their domains but not their
  reporting, and `math_errhandling` speaks for [c.math]; recorded here rather
  than left for someone to discover.
- `✓` Closed in Ф31e-b-1: `std::lerp` did not exist. It is now P0811R3's exact
  shape — the only one that keeps exactness at both ends, boundedness inside
  `[0,1]` and monotonicity at once — which with `midpoint` closes
  `__cpp_lib_interpolate`. A NaN `t` is not propagated and yields `b`;
  [c.math.lerp]/2 permits that and both reference libraries do the same.
- `✓` Closed in Ф31e-b-1: the **three-argument `hypot(x, y, z)`** was absent, which
  this document had failed to record — `<version>` had it, §2 did not. Present now
  at all three widths, with power-of-two scaling, closing `__cpp_lib_hypot`.
- `✓` Closed in Ф31e-c: the "sufficient additional overloads" of [cmath.syn]/2 did
  not exist, so a call whose arguments were *all* integers had three equally bad
  candidates and was ambiguous — `std::sqrt(4)`, `std::pow(2, 3)`,
  `std::atan2(1, 1)`, `std::fma(1, 2, 3)` and `std::abs(-3)` did not compile at
  all. Mixing `float` with `double` did work (`float` → `double` is a promotion,
  which outranks the conversion the `float` overload would need), but mixing
  `float` with `long double` did not, because nothing promotes to `long double`.
  [sf.cmath]/2 applies the same rule to the special functions, and they were
  affected identically — `std::riemann_zeta(2)` was ambiguous too.
- `~` `std::abs` for `int`, `long` and `long long` is declared in `<cmath>`.
  [c.math.abs] puts those three in `<cstdlib>`, which they also reach through
  `<__bits/c_arith>` — the sentence here said "which BoxOS does not have" until
  Ф47, and had been untrue since Ф41-e built the header;
  they are provided here rather than left to the promotion rule, because promoting
  would turn `std::abs(-3)` from the `int` `3` every program expects into `3.0`.
  Unsigned arguments remain ill-formed, which is what [c.math.abs]/3 asks for.
- `~` `nan("payload")` ignores the payload string and returns a plain quiet NaN.
- `?` `std::log10` is not exact on one of the 23 exactly-representable powers of
  ten (measured on BoxOS). `box::log(x, 10)` returns 22 of the 23 exactly by
  calling `log10` directly rather than dividing logarithms.

### Ф40 — the extended floating-point types

- Every `<cmath>` function now has the overloads [cmath.syn]/2 asks for at the
  five extended types, and [cmath.syn]/3's promotion is no longer a three-way
  widest-wins: with those types present the conversion ranks are a PARTIAL
  order, and the machinery implements rank first, then subrank, then "no
  common type exists" for the one incomparable pair. See `<stdfloat>` in this
  section for the whole story, including what float128_t deliberately lacks.

## `<compare>` / `<concepts>`

- `✓` Closed in Ф31e-g-3: P2404R3's *comparison-common-type-with* was not
  implemented, so `equality_comparable_with`, `totally_ordered_with` and
  `three_way_comparable_with` still asked C++20's `common_reference_with` — which
  requires the common reference to be **constructible** from each side. When that
  common reference is a prvalue, constructible means *copyable*, so a move-only
  type could not be compared with anything it converts from, however well-formed
  every `==` and `<=>` between them was. The C++23 relaxation asks instead whether
  each side converts to `const C&`, which binds a reference and copies nothing.
  The concept lives in `<compare>` — the lower of the two headers — and is spelled
  with type traits rather than `same_as`/`convertible_to` for the same reason the
  rest of `__cmp` is. `totally_ordered_with` needed no edit: it inherits the
  relaxation through `equality_comparable_with`. Closed `__cpp_lib_concepts`.

- `✓` Closed in Ф31e-b-1: `compare_three_way_result` was declared with two required
  parameters where the standard declares `template<class T, class U = T>`, so
  `compare_three_way_result<T>` did not compile. **This document had the extent of
  it wrong**: it also claimed `compare_three_way_result_t<T>` failed, and that was
  never true — the alias always carried its own default, and `<version>`'s note
  said so correctly. Measured before the fix, not recalled.
- `✓` Closed in Ф31e-f: `__cpp_lib_three_way_comparison` is now defined. Its
  201907L value is P1614R2, the library-wide `operator<=>`, and the last thing
  missing was the four ordered associative containers (see `<map>`). All 26 of
  the `<=>` overloads the paper asks for were re-measured before the macro was
  released, rather than taken from the note claiming they were there.

## `<complex>`

- `✓` Added in Ф34: the **tuple protocol** (P2819R2) — `tuple_size`,
  `tuple_element` and four `get<I>` overloads. Unlike `integer_sequence`'s
  protocol in `<utility>`, this one puts `complex` INTO the tuple-like set, so a
  `complex` is a valid source for `pair`/`tuple`'s tuple-like constructor and for
  `views::elements`. `get<>` returns a reference to the part, which is the whole
  point: `real()`/`imag()` return by value and always have.
- `~` The accessor the four `get` overloads go through, `__part(size_t)`, is a
  public member with a reserved name, for the same reason libstdc++'s
  `__get_part` is: a free function template cannot be befriended without
  repeating `complex`'s own requires-clause on every declaration.

## `<cstdlib>`

- `✓` Closed in Ф43-f: **`memalignment` was missing.** C23's, and a freestanding
  entity of [cstdlib.syn]: the largest power of two a pointer is aligned to.
  Neither libstdc++ 16.1 nor libc++ has it. `v & -v` isolates the lowest set
  bit, which IS that power of two — and the null case needs no branch, which a
  mutation established rather than an argument: `~0 + 1` is `0`, so the
  arithmetic already answers zero for a null pointer. The separate sentence the
  standard spends on null describes the same answer, not a different one.

The header the "BoxOS has no libc" sentence was really about, and building it
was mostly answering questions the system had never been asked.

- **`getenv` always returns `nullptr`, and `system(nullptr)` returns 0.** Both
  are answers rather than stubs. A BoxOS process is a cabin carrying TAGS —
  read with `box::this_process`, not with a string table inherited from a
  parent that may not exist — and there is no command processor to hand a
  shell line to; a program is spawned by name (`box::process::spawn`).
  `system(cmd)` returns −1.
- **`aligned_alloc` is implemented INSIDE the allocator**, because [c.malloc]
  requires the ordinary `free()` to release what it returns. The usual trick —
  over-allocate, return an aligned address inside, stash the real pointer just
  below it — is what boxcxx's aligned `operator new` does, and it works there
  only because the aligned *delete* knows to look for the stash. `free()` reads
  the 32 bytes in front of the payload and expects a block header, so boxlib
  splits the block and puts a **real header** there. phase204 pins the split
  with a conservation law over `heap_get_stats` (payloads plus one header
  apiece can never exceed the bytes taken from sbrk) — a check added because
  the first version of the test, which only checked alignment, writability and
  freeability, passed a mutation that gave the split block 32 bytes it did not
  own.
- **`rand`'s state is per-strand.** [c.math.rand] says nothing about where the
  state lives, and every hosted libc puts it in one object: glibc's `rand()` is
  a data race by construction. Here two strands that both `srand(1)` get the
  same sequence instead of fighting over one. The generator is a 64-bit LCG
  returning its high bits (the low bits of any LCG have short periods, which is
  how `rand()` earned its reputation); `RAND_MAX` is 2147483647.
- **`qsort` is an introsort**, so the worst case is O(n log n). C requires no
  complexity at all and the reference libcs ship a quicksort whose worst case
  is quadratic; a system that must not stall cannot.
- **The multibyte encoding of the `"C"` locale is UTF-8, and `MB_CUR_MAX` is
  4.** C leaves the encoding implementation-defined. BoxOS is UTF-8 end to end
  — screen, keyboard, TagFS names — so a second, narrower encoding inside the C
  functions would have made `mbstowcs` produce mojibake from ordinary BoxOS
  text. `mblen`/`mbtowc`/`wctomb`/`mbstowcs`/`wcstombs` decode and encode it
  strictly: a truncated sequence, a bare continuation byte, an overlong form
  and a surrogate are each rejected with −1. See `<climits>` for the one
  consequence outside this header.
- **The `strto*` grammar is C's, not `<charconv>`'s**, and the two are related
  only underneath: leading whitespace, a sign, the `0x` prefix, hex floats,
  `inf`/`nan`, `endptr`, and overflow reported through `errno` are all things
  [charconv] deliberately forbids `from_chars` from doing. What is shared is
  the magnitude parsing. On overflow `strtod` reports which side by re-parsing
  into `long double` — an 80-bit type covers every `double` that overflowed, so
  the answer is a comparison rather than a second scan of the text.
- `atexit` registers into the **same** list as static destructors (through
  `__cxa_atexit`), because [basic.start.term] orders the two against each other
  by registration and two separate lists could only guess. `at_quick_exit` has
  its own list, as it must: `quick_exit` runs those and nothing else — no
  atexit handlers, no static destructors, no `.fini_array`. phase204 witnesses
  that from a child process, the only place it is observable.
- `abs`/`labs`/`llabs` and the `div` family live in `<__bits/c_arith>`, shared
  with `<cmath>`: [c.math.abs] puts them here, but a TU that includes only
  `<cmath>` and writes `abs(-3)` must not get `3.0`. They are `constexpr`
  because P0533R9 (C++23) made them so, not because the implementation took a
  liberty.

## `<cstring>`

- `✓` Closed in Ф43-f: **`memccpy` and `memset_explicit` were missing.** Both are
  C23 and both are freestanding entities of [cstring.syn]; neither reference
  library has either. `memccpy` is the one mem-copy that stops on a VALUE, and
  its null return is load-bearing — it is how a caller learns the record was
  truncated rather than terminated. `memset_explicit` is memset the optimizer
  may not delete, which is what erasing a key needs and what plain memset does
  not promise.
- `?` **The "may not be deleted" half of `memset_explicit` is not pinned by any
  check, and cannot be.** Proving a store was not elided means reading a dead
  object, which is the undefined behaviour the function exists to make safe. A
  mutation deleting the compiler barrier survived the suite, correctly. It is
  also inert in this build — the function has its own translation unit and
  nothing links with LTO, so the call cannot be inlined and the store cannot be
  seen to be dead. What would pin it is reading generated code.

The functions are boxlib's — one `memcpy` in the system, shared by C and C++ —
so what this header adds is the part C++ adds to C: **the six const-preserving
overload pairs** ([cstring.syn]/3). `strchr` on a `const char *` yields
`const char *` here; the C declaration would have handed back a mutable pointer
into a const string. That is also why the pairs are NOT injected into the
global namespace: `::strchr` is C's, `std::strchr` is C++'s, and a
using-declaration for a function differing only in return type is a
redeclaration conflict rather than an overload. **libc++ makes the same split;
libstdc++ does not manage it** — there, `std::memchr` on a const pointer still
returns a mutable one.

- **The functions are null-tolerant**, which C leaves undefined:
  `strlen(nullptr)` is 0, a copy into `nullptr` returns `nullptr`, a search in
  `nullptr` finds nothing. Nine of them already were before Ф41 and the
  thirteen added match. No conforming program can tell, because every case is
  undefined behaviour; a bare-metal system has no signal to turn the fault into
  a diagnostic, so the strengthening costs nothing.
- **`strtok`'s cursor is per-strand.** C describes one static object, which is
  why `strtok` is the textbook function two threads must not both call.
  [c.strings] leaves the state unspecified, so `__thread` is conformance rather
  than extension — and on a system whose point is that several strands share a
  cabin it is the only defensible reading. phase204 proves it with two strands
  tokenizing concurrently; with the shared cursor the test fails on 16 cores.
- **`strcoll` is `strcmp` and `strxfrm` is a copy**, because the `"C"` locale
  collates by character code and there is no other locale (§1.2).
- **`strerror` answers from `generic_category()`'s table**, not a second one:
  [syserr.errcat.objects] ties errno values to that category, so a program
  comparing `strerror(EDOM)` with `generic_category().message(EDOM)` is
  entitled to the same words. The buffer it returns is per-strand and the next
  call on that strand overwrites it, which C permits and which removes the race
  a shared buffer would have. A condition BoxOS cannot surface gets
  `"generic error N"` rather than a cargo-culted Unix string — the policy
  `<system_error>` already had.

## `<cuchar>`

The six restartable conversions between the `"C"` locale's multibyte encoding
and `char8_t` / `char16_t` / `char32_t`. Since that encoding is UTF-8,
`mbrtoc32` is a decoder, `c32rtomb` an encoder, and `mbrtoc8` the odd one out —
its input and its output are the same bytes, handed back one code unit per call.

The whole design is the letter `r` in the names. `<cstdlib>`'s `mbtowc` is
handed a complete character or it fails; these are *restartable*, so a caller
may feed one byte at a time and they must remember what they have seen. Both
kinds of memory live in `mbstate_t`: input bytes of a character not yet
complete, and output units of a character that produced more than one — the low
surrogate for `mbrtoc16`, the trailing UTF-8 bytes for `mbrtoc8`, returned as
`(size_t)(-3)`.

`(size_t)(-2)` is the answer that makes streaming possible and the one an
implementation is most tempted to get wrong, because it requires knowing that a
prefix is still *possible*. `E0 80` is not: no third byte rescues an overlong
form. That distinction is decided once, in `<__bits/c_utf8>`, and both this
header and `<cstdlib>` read their answer from it — `<cstdlib>` mapping
`Incomplete` back onto `-1`, which is correct for a non-restartable conversion
bounded by `n`.

- `+` **A null `ps` selects state that is per strand, not per process.** C keeps
  one internal `mbstate_t` per function per program, and a BoxOS program runs
  strands over one address space by default, so the hosted answer is a data race
  the standard would have let us ship. Same decision as `rand` and `strtok`
  (Ф41-b) and the `<ctime>` buffers (Ф41-e).
- `?` **`mbstate_t` moved here from `<iosfwd>`, where it had been an eight-byte
  opaque placeholder** since the iostream work — `char_traits::state_type` and
  `fpos<mbstate_t>` are spelled in terms of it. `<iosfwd>`'s own comment said
  the definition would move "the day one of those headers exists". It now lives
  in `<__bits/c_mbstate>` and both include it, because `<iosfwd>` must remain a
  header that pulls nothing in.
- `?` The decoder carries **no overlong / surrogate / out-of-range test after
  the loop**, and that is a measured result rather than an oversight: the
  lead-byte constraints subsume all three. Sweeping every byte sequence of
  length ≤ 4, control reaches that point 143,161,216 times and the three tests
  would reject exactly zero of them. The migration was checked the same way:
  the new decoder answers what the shipped one answered on all 4,311,810,305
  inputs of length ≤ 4. The consequence is that the lead-byte checks are
  load-bearing alone, so the phase mutates each of them individually.
- `!` That sweep is also what caught the one real defect in this work. `n` is
  how many bytes the *caller has*, not how many the character *uses* — and
  `mbstowcs` passes 4 on every call. A first draft examined `s[1]` before
  deciding a one-byte character was finished, so `"AB"` failed to convert.
  Four narrower tests missed it because every one of them passed `n` exactly
  equal to the character's length; the equivalence sweep disagreed with the
  shipped decoder on 1.6 billion inputs and named the first one.

## `<ctime>`

The header that could not exist while a twenty-byte structure held the name.

`time_t` in a BoxOS tree meant a packed calendar record — seconds, nanoseconds
and the broken-down fields — declared in `box/time.h` and used by the kernel's
RTC driver, the hardware deck and boxlib alike. [ctime.syn] requires `time_t` to
be an **arithmetic** type, so the two could not share a program, and the
collision was not theoretical: `<chrono>` had been dodging it since Ф30. Its own
comment said so, and `system_clock::to_time_t` returned `long long` — a recorded
departure from [time.clock.system]/3 whose stated cause was that the name was
taken. Ф41-e gave that structure the name the rest of the tree had been calling
it by for years — `clock_boxtime()`, `rtc_get_boxtime()`, *"Boxtime is the
preferred form"* — and the deviation retired with the rename. `to_time_t` and
`from_time_t` now work in `std::time_t`, and phase210 asserts it, so reverting
the rename stops the build rather than quietly restoring the old dodge.

- `+` **`clock()` is processor time, and until this commit there was no such
  thing in BoxOS to return.** The kernel carried a `total_cpu_time` field on
  every process that was zeroed at creation and never written again — a promise
  in a struct. It is accumulated now, per slice, **measured with the TSC**.

  The TSC is not an implementation detail here, it is the whole difference. The
  first version counted scheduler ticks, and at tick granularity whoever is
  current when the tick fires is credited the *whole* tick — so two processes
  alternating every tick are each credited 100% of the wall clock. Measured on a
  one-core boot (cxxtest and pid 1 ping-ponging every tick, both fully
  credited), that made `clock()` read *exactly* equal to wall time. A factor of
  two per participant is not a rounding error and no arithmetic over ticks
  removes it.
- `?` The value counts **completed slices plus the one in progress**, so two
  calls inside one slice differ by the work between them. Getting there took two
  wrong answers, both found by measurement. Deriving the in-flight part from the
  scheduler's dispatch stamp was wrong because that stamp is rewritten on every
  scheduler pass and survives a park — a 300 ms sleep reported 300 ms of
  processor time. Dropping the term was wrong in the other direction: with a
  spare core a strand runs 50 ms without one context switch, and on sixteen
  cores the answer never moved. The stamp that works is the one that exists
  only while the strand holds the core.
- `!` **A strand blocked in a timed park is still given the core, and `clock()`
  reports that faithfully.** Across a 300 ms `sleep_for` on a one-core boot the
  strand is credited ~260 ms of processor time. This is a BoxOS defect, not a
  property of `clock()`: the wait underneath a timed park is a poll rather than
  an event, so a sleeping strand keeps its share of the core. It is recorded
  here because a reader will otherwise conclude `clock()` is wall time — it is
  not; the same strand reads far below wall time when it competes with others,
  and reads near zero at process start while the machine's uptime is large.
  phase210 prints both numbers on every run so the day the park becomes an event
  is visible in the log. Fixing it belongs to the scheduler.
- `?` `clock()` is **per strand**, not per program. C defines it as the
  processor time used by "the program"; a BoxOS strand is its own schedulable
  entity with its own accounting, so a multi-strand program's strands each
  report their own. For a single-threaded program the two coincide.
- `~` **`localtime` IS `gmtime`.** There is no timezone database, and inventing
  an offset would be worse than saying so. `tm_isdst` is always 0, `%Z` renders
  `UTC` and `%z` renders `+0000`. The same shape as `<chrono>`'s documented
  absence of a leap-second table, and the two are consistent with each other.
- `+` **`gmtime`, `localtime`, `asctime` and `ctime` return storage that is per
  strand.** C makes those buffers process-wide, which in a program running
  strands over one address space is a data race the standard permits. Each of
  the four has its own, so a program may hold what `ctime` returned across a
  call to `asctime` — which C also allows a hosted library to break.
- `?` **`strftime` is not a second formatting engine.** `<chrono>`'s formatter
  already walks a `%`-string over the full specifier vocabulary, tested by every
  calendar phase from Ф30 on; `strftime` fills that engine's field struct from a
  `tm` and calls it. A date therefore prints identically through `std::format`
  and through `strftime`, and the conversions `<chrono>` documents as deviating
  (`%I`/`%r`/`%p` beyond twelve hours) deviate identically here. An unknown
  conversion is undefined in C: the engine throws, `strftime` catches, and 0 is
  returned — no exception crosses into a C caller.

  As of Ф43-d-3 there is a third caller of the same engine, `std::time_put`,
  and the `tm`-to-field conversion this entry describes is now written once and
  used by both. It had been about to be written twice: the facet's first draft
  carried its own copy, which is exactly how two spellings of "the ninth of
  March" start to disagree.
- `?` `asctime` reproduces C's format exactly, including the two details an
  implementation gets wrong by eye: the day is `%3d`, so it carries its own
  leading space and there is none between the month and it; and the year is
  `%d`, **not** `%4d`, so year 500 prints as `500` and the line is one character
  shorter. Field values that cannot fit their fixed column — where C leaves the
  behaviour undefined — return `nullptr` rather than emitting a non-digit.
- `?` The calendar arithmetic is `<chrono>`'s (`days_from_civil` /
  `civil_from_days` / `weekday_from_days`), not a fourth copy: the tree already
  had three civil-date conversions when this header was written. It was checked
  differentially against a hosted libc over 1,740,289 instants spanning
  1901–2100, with `mktime` and `gmtime` required to invert each other at every
  one of them.
- `~` `timespec_getres` is absent: [ctime.syn] in the C++23 baseline does not
  list it. `timespec_get` accepts `TIME_UTC` and returns 0 for any other base,
  which is the only base there is.

## `<cstdint>`

**Six macros [cstdint.syn] requires were absent until Ф42-b**, and the header
that noticed was `<cwchar>`: it needs `WCHAR_MIN` and `WCHAR_MAX`, and a caller
cannot derive the minimum from the maximum without already knowing that
`wchar_t` is signed here. `WINT_MIN`, `WINT_MAX`, `SIG_ATOMIC_MIN` and
`SIG_ATOMIC_MAX` belong to the same clause of C's 7.20.3 and were missing for
the same reason — the last two should have been caught when Ф41-a built
`<csignal>` and were not. libc++ defines all six; the omission was ours. They
are the compiler's own predefined values, as the rest of the header already is.
No other deviation.

## `<cstdio>`

The last C wrapper, and the only one the sentence §1.1 used to open with — "BoxOS
has no libc" — was ever really about. The others needed answers BoxOS already
had; this one needs a `FILE*`, and nothing underneath implemented one.

A FILE is a buffer, a position and a mode wrapped around a **Current**, exactly
as Ф36 built `basic_filebuf`. The three conventional streams are three
conventional tags: `stdout` and `stderr` are `"screen"`, `stdin` is
`"keyboard"`, and `fopen("report", "r")` opens `"file:report"`.

**The hard part was not the streams, it was the four names boxlib already owned
in the global namespace**, and each needed a different answer.

- `+` **`printf` and `getchar` are ONE function each, shared with boxlib rather
  than duplicated beside it.** `<cstdio>` declares them with exactly boxlib's
  signature and pulls them into `std` with a using-declaration, so `printf`
  inside a `using namespace std;` block names one entity. *Which*
  implementation runs is a link-time question: boxlib's two are `weak`, boxcxx
  defines strong ones, and `apps/Makefile` already ordered `libboxcxx.a` before
  `libbox.a` "so our runtime symbols always win". A C++ program therefore gets
  the full C conversion set; a C-only program — shell and the utilities, which
  cannot link boxcxx — keeps the smaller set it has always had. phase211 prints
  a line through the *unqualified* `printf` containing `%#x` and `%f`, neither
  of which boxlib's can convert, so the arrangement is proved on the real road
  rather than asserted.
- `!` **Declaring a second `printf` in `namespace std` was tried first and does
  not work.** [headers]/p permits it — it leaves unspecified whether a `<cxxx>`
  header's names also appear globally — but `cxxtest.cpp` alone contains 53
  `using namespace std;` blocks, and inside one an unqualified `printf` sees two
  functions with identical signatures. The build said so in twenty places. This
  is recorded because the idea is the obvious one and will occur to the next
  reader too.
- `~` **`fread` and `fwrite` are NOT injected into the global namespace.**
  boxlib's take a TagFS file id and a byte offset (`box/file.h`) — genuinely
  different functions wearing the same name — so the two cannot be merged the
  way `printf` was. They live in `std` only. Nothing is ambiguous, because the
  parameter lists do not match: a call shaped like C's `fread` cannot select
  boxlib's.
- `~` **`fopen` takes a NAME, not a path.** TagFS has no directories to walk, so
  `"file:report"` is the whole of it. A program that passes `"/tmp/x"` gets a
  file whose name contains slashes rather than a silent traversal — there is no
  working directory to be relative to and no notion of one to add later.
- `~` **`stderr` goes to the screen, not to the serial log**, and is unbuffered.
  The log is where a developer with a cable looks; a diagnostic that reaches
  only it is one nobody sees on a machine that has none. `std::cerr` (Ф36) and
  `assert` (Ф41-a) made the same call, and all three agree on purpose.
- `?` **EOF is the writer's, not the file's.** Current answers `CURRENT_CLOSED`
  when no more data will arrive, which for a file is exhaustion and for a stream
  is the writer letting go. `feof` reports that, and a live keyboard never sets
  it — it blocks, because a keyboard has no end.
- `?` The keyboard hands back a line **without** its newline, so the byte layer
  appends one, exactly as `<iostream>`'s console buffer does. Both must agree or
  a program mixing `std::cin` with `getchar()` would see two different line
  shapes.
- `?` `FOPEN_MAX` is 16 — the guarantee, not a ceiling. Open streams live on a
  linked list with no fixed table, so exceeding it is not an error here.
  `FILENAME_MAX` is 32, measured from `file_info_t::filename` rather than chosen.
- `?` **The printf engine does not generate its own digits for floating point.**
  `<charconv>` already produces correctly-rounded fixed, scientific, general and
  hex forms (Ф9A, MPFR-verified); printf's work here is the sign, the flags, the
  width and the padding around them. Likewise scanf hands spans to `from_chars`
  and only decides how much text belongs to the number.
- `?` Both engines were checked **differentially against a hosted libc before
  they ever booted**: printf over 35,817 format/value pairs and scanf over 441
  input/format pairs, both to zero differences. That sweep found seven real
  defects in this code — `+`/space applied to unsigned conversions; `#` on octal
  adding a zero the precision had already produced; `%#g` stripping the trailing
  zeros it exists to keep; a scanset returning EOF where C wants a matching
  failure; `inf`/`nan` unrecognised on input; a sticky end-of-input flag that
  lied after lookahead; and an out-of-range integer refusing the conversion
  instead of saturating.
- `?` scanf keeps **eight characters of pushback**, not one. Recognising
  `"infinity"` looks ahead that far, and a single slot silently swallowed seven
  of them. C promises a *stream* only one character of `ungetc`, and that is
  what is handed back when the call returns; the deeper lookahead lives inside
  one call.

**Stream orientation arrived in Ф42-b**, with `<cwchar>`. C says the first byte
or wide operation on a stream fixes which of the two worlds it belongs to, and
`fwide` reports it. Two consequences are worth stating because they surprise
people, and both are the rule rather than a limitation:

* `stdout` has carried `printf` since the program started, so it is
  byte-oriented and `fwprintf(stdout, …)` is refused. A stream is one world or
  the other for its whole life.
* The orientation is what makes `FILE::__unget` serve both the byte push-back
  and the wide one. They can never both be live, so `ungetwc` needed no new
  field and the struct's layout is unchanged.

The claim is made at the public byte entry points — `fgetc`, `fputc`, `ungetc`,
`fgets`, `fputs`, `puts`, `fread`, `fwrite` — and not inside the locked
primitives, because `<cwchar>`'s wide I/O calls those same primitives to move
its bytes. Putting it one layer lower was the first attempt and it made every
wide write to a wide-oriented stream refuse itself.

**The field-width parser saturates**, at a value high enough that no width a
program really writes is affected. That is an overflow guard and not a policy:
a width is written by the caller, nothing bounds its digits, and `v * 10` on a
plain `int` is signed overflow — undefined, not merely large. `<format>` used
to have a deliberate 65535 cap beside it and no longer does (§2), which leaves
this the only place in the library where a written width is quietly clamped
rather than honoured — and printf's `%*d` grammar has no way to report the
refusal, which is why it clamps. Ф42 found the hole while giving the wide
engine the same parser and having to explain why the two differed.

## `<csetjmp>`

Four instructions of register shuffling and one genuine problem: CET.

A `longjmp` travels UP the stack, so by the time it runs the shadow stack holds
an entry for every frame the jump is about to discard. Leaving them does not
fault at the jump — it faults later, the first time the resumed function
returns and its return address no longer matches the shadow one (#CP). So
`longjmp` pops them with `INCSSPQ` back to where `setjmp` stood, and then
**returns** to the saved address rather than jumping to it: at that point the
top shadow entry IS `setjmp`'s own return address, so a `RET` consumes both
stacks in step and lands exactly where `setjmp` would have returned. An
indirect `JMP` would leave the entry behind and, under IBT, demand an `ENDBR64`
at a target that has none — the unwinder's NOTRACK jump is right for a DWARF
landing pad and wrong here. `RDSSPQ` reads 0 when shadow stacks are off
(QEMU/TCG), so the same code takes the ordinary path there and the CET path on
real hardware.

- `jmp_buf` is nine words — six callee-saved registers, `rsp`, the resume
  address, and the shadow-stack pointer — and it is an **array type**, as
  [csetjmp.syn] requires, which is what lets `longjmp(env, 1)` be written
  without an ampersand.
- `setjmp` is a **macro**, also as required: the context saved must be the
  caller's, and a macro is what guarantees no extra frame stands between them.
- `longjmp(env, 0)` makes `setjmp` return 1, because 0 is how the first return
  is recognised.
- **[csetjmp.syn]/2's restriction is stated and not enforced**: the pair is
  undefined if replacing it with `catch` and `throw` would destroy an object
  with a non-trivial destructor. Nothing can enforce that. In a library with
  working exceptions there is almost never a reason to reach for this pair; it
  is here because ported C code reaches for it.

phase206's fourth check is the one that matters: it jumps out of five nested
frames and then RETURNS from the frame that caught the jump — the return that
would #CP if the shadow stack had been left one entry too deep.

## `<csignal>`

**Nothing in BoxOS ever raises a signal at your process.** There is no
asynchronous delivery here, by decision and not by omission: BoxOS already has
an event vocabulary — Touch, a tag-multicast event with a named subscriber,
delivered where the subscriber asked for it — and mapping the CPU's faults onto
handler callbacks would have built a second, older, worse one beside it. The
alternative was offered and declined when this header was planned.

That does not make the header a stub, and the distinction is the standard's,
not a convenience: [support.signal] never requires an implementation to raise a
signal on its own. It specifies `signal` (install), `raise` (deliver, on this
strand, now) and what the two default dispositions mean, and all of that is
real here.

- **Six signals** — `SIGINT` 2, `SIGILL` 4, `SIGABRT` 6, `SIGFPE` 8, `SIGSEGV`
  11, `SIGTERM` 15 — and no more. A `SIGKILL` or `SIGPIPE` would name a concept
  BoxOS does not have. The numbers follow the same de-facto convention
  `<__bits/errc>` uses for its values, so a number a ported program already
  wrote down means what it means elsewhere.
- **A default-disposition death exits with 128 + signal**, which is the status
  convention boxcxx already used before this header existed.
- **The disposition is not reset to `SIG_DFL` when a handler runs.** C leaves
  that to the implementation; a handler obliged to reinstall itself after every
  delivery races any other strand raising the same signal in the window.
- The table is one per process and every slot is atomic: signals are always
  delivered by the strand that raises them, but two strands may install and
  raise concurrently, and `raise` reads its slot exactly once so the decision
  and the call cannot come from different answers.

**`std::abort` is defined here**, because [support.start.term]/9 specifies it in
terms of `raise(SIGABRT)` — and building it is what exposed the defect this
header is really remembered for.

> **The library's fatal path used to run the program's teardown.**
> `boxcxx::Panic` — reached by an uncaught exception, a pure virtual call, a
> failed guard, a `new` with no handler — ended with `exit(134)`. The status was
> right (128 + SIGABRT); the road was not. `exit` runs `__cxa_finalize` and the
> `.fini_array`, so every static destructor and every `std::atexit` callback ran
> on the way out of a fatal error: destructors touching state an uncaught
> exception had just abandoned, teardown printing after the diagnostic, an
> `atexit` callback getting a turn [exception.terminate] never gives it. There
> was no `abort` to call instead until `<csignal>` existed. Phase203 pins it
> from outside, because no process can witness its own teardown: a child
> registers a destructor that exits 77, and 77 must appear when it returns from
> `main` and must NOT appear when it dies of a throw, a `raise` or a failed
> `assert`.

**One live edge is worth naming:** `abort()`'s own fallback — the termination
after `raise` returns — is reachable only when a `SIGABRT` handler returns or
the signal is ignored, because with the default disposition the `raise` itself
ends the process. That branch is covered by the assert-with-`SIG_IGN` child in
phase203; it was found by a mutation that changed the line and was not
observed, which is the only way an unreachable branch announces itself.

## `<cstdarg>`

The macros are the compiler's, because only the compiler knows where the
register save area is; a library that "implemented" them would be guessing at
the ABI it is compiled against. `std::va_list` is `::va_list` — the same type
the macros walk, which matters more here than usual: `<cstdio>`'s `vprintf`
family will take it by value, and on x86-64 SysV a `va_list` is an array of one
struct and therefore decays on the way in. No deviations.

## `<cwchar>`

- `✓` Closed in Ф43-f: **`WCHAR_WIDTH` was missing**, a freestanding entity of
  [cwchar.syn] that C23 added and neither reference library defines. It is
  `__WCHAR_WIDTH__`, not a literal 32: the width of `wchar_t` belongs to the
  target, and a number written here would be a second claim about it that
  nothing keeps in step with the first.

All of [cwchar.syn]: the wide strings, the restartable conversions, the seven
`wcsto*`, the wide character I/O, and the formatted families. `wchar_t` here is
a signed 32-bit `int` and `wint_t` an unsigned 32-bit one, so WEOF cannot
collide with a character — scalar values stop at U+10FFFF.

**One conversion machine, not two.** `mbrtowc` IS `mbrtoc32`: on this target
`wchar_t` and `char32_t` hold the same values, so a second state machine would
be a second thing to keep level with the first. What delegation must not lose
is that C gives every one of these functions its OWN internal state for a null
`ps` — `<cuchar>` keeps six separate ones for exactly that reason — so
`mbrtowc` passes its own when the caller supplies none, and shares only the
machine. `wcrtomb` and `c32rtomb` stand in the same relation.

**One formatting engine, not two.** Every conversion except the character and
string ones produces ASCII, so `fwprintf` rebuilds the directive narrow, hands
it to the engine `<cstdio>` already has, and widens the result. The
correctly-rounded floating-point path of Ф27 therefore arrived without being
ported. What the wide layer does itself is `%c`, `%lc`, `%s` and `%ls`, because
**their width and precision count CHARACTERS and the narrow engine counts
bytes** — `%8ls` on `L"жжж"` must pad to eight characters, not to eight bytes
of UTF-8. `%n` counts characters for the same reason.

**One tokenizer that could not be shared.** `fwscanf` gathers its own fields,
because two things the narrow scanner does are wrong at this width: white space
is `iswspace`, which is Unicode-wide (§2 `<cwctype>`), and a field width counts
characters. The VALUE conversion is still shared — a gathered token goes to
`wcstoll`/`wcstoull`/`wcstold`, whose `endptr` is authoritative about how much
of it was really a number.

**Where these functions live, and why not in boxlib.** Ф41-b put `strcoll`,
`strxfrm` and `strtok` in boxlib, under the split that gives it the raw string
primitives because C programs need them. That rule deliberately does not carry
over: measured, the whole of boxlib mentions `wchar_t` exactly zero times, and
no C program in this system uses a wide string. A symbol in boxlib that no C
caller ever names is weight in every C image for nothing.

**Two places this header is stronger than `<cstring>`, for a reason that is not
to its credit.** Nothing in the tree already owned these names — unlike
`printf`, `fread`, `fwrite` and `getchar`, which boxlib had claimed before
`<cstdio>` wanted them. So the const-correct overload pairs of [c.strings] reach
the **global** namespace too, where `::strchr` has to stay C-shaped and lose
const; and no renaming was needed anywhere, where Ф41 spent three separate
collisions on it.

**Deliberate answers, not stubs.**

| | |
|---|---|
| `wcscoll` | is `wcscmp`, and `wcsxfrm` is a copy: one locale, no collation table. C asks only that the order `wcscoll` imposes agree with comparing `wcsxfrm` results, and identity satisfies that. Measured divergence: macOS in `en_US.UTF-8` transforms `"abc"` into seven wide characters and returns −8 from `wcscoll(L"a", L"b")`. It has a real collation; BoxOS has one locale. |
| `wcstok` | needs no hidden cursor, and that is C's doing: the wide form takes an explicit `wchar_t **ptr`. Ф41-b's per-strand `strtok` cursor has no counterpart here and should not grow one. |
| `btowc` / `wctob` | only ASCII is one byte in UTF-8, so `btowc(0xD0)` — the first byte of `L'ж'` — is WEOF, and `wctob` answers EOF for everything above U+007F. |
| `swprintf` | reports a **negative** value when the buffer fills, rather than what it would have written. That is C's design and not `snprintf`'s; a caller sizing a buffer by asking first has to do it another way. |
| leading white space in `wcsto*` | is `iswspace`, so `wcstod(L"\u2003" L"42")` consumes the EM SPACE where the narrow `strtod` would stop at its first byte. Not an oversight in either direction — C defines the skip in terms of the classification of the character type it was given. |

**A defect measured in the reference, which boxcxx cannot have by
construction.** macOS's own `strtol` and `wcstol` disagree with each other on
`"0x"`, `"0x."`, `"0X"` and `"0xg"`, at base 0 and base 16 alike: the narrow one
consumes the `0` (which is right — the longest initial subsequence of the
expected form is `"0"`), the wide one consumes nothing. Here the wide function
IS the narrow one, so the two cannot drift.

**What the push-back can and cannot promise.** `ungetwc` accepts one character,
which is all C guarantees, and it goes in `FILE::__unget` — the same slot the
byte world uses. That sharing is safe for a reason rather than by luck: C
forbids mixing byte and wide operations on one stream, which is what
orientation MEANS, so the two push-backs can never both be live. No field was
added to `FILE` and its layout is unchanged. `fwscanf`'s deeper look-ahead
(eight characters, because `infinity` is eight long) lives in the call and only
the last character goes back to the stream — the one [fwscanf] requires to be
left unread. The narrow `vfscanf` hands its own back on exactly these terms.

**How it is known to be right.** Four differentials against the host's libc,
before any of it was booted: the strings and restartable conversions over
40 000 randomised iterations from an alphabet mixing ASCII with Cyrillic, CJK
and astral characters (**280 562 lines, 0 disagreements**); the seven `wcsto*`
over 65 inputs × 5 bases including a 2 996-digit run (**1 520 lines, 0**);
`swprintf` (**48 cases, 0**); `swscanf` (**63 cases, 0**). Thirteen mutations
were run. Ten were caught. Of the three that were not:

* one was a genuine test gap — `wcschr` that could no longer find the
  terminator passed clean, because no generated needle was ever `L'\0'`;
* one was a second genuine gap — the scanner's "push back what the converter
  declined" step had no input in the corpus that produced a tail at all;
* one is **equivalent, and proved so by pairing**: removing the `0x` back-off
  alone changes no answer, because `wcstoull`'s `endptr` returns the `x`
  regardless; removing both breaks eight cases. Exactly one of the two
  mechanisms is load-bearing, and the back-off is kept because its story is
  local.

Closing the first two gaps then found a real defect that no reading had: `%i`
on `"0888888888888"` resolves to octal, so eighteen characters became tail, the
eight-deep push-back dropped ten of them **silently**, and the next field read
the wrong number. The root cause was gathering generously and letting the
converter sort it out; the fix resolves the base before any digit is taken.

**Three more were found by asking what every fixed-size buffer would do when
the argument outgrew it**, and all three were silent:

* `%*d` with a NEGATIVE width lost its left-justification on every delegated
  conversion. A negative `*` width IS the `-` flag, and the flag was set in the
  parsed struct while the narrow directive was rebuilt from the flag ARRAY. If
  a directive is rebuilt, everything derived from an argument has to reach the
  form it is rebuilt from.
* `%s` of a 2 000-character multibyte string produced 511 characters and a
  return value that agreed with itself. It buffered; it no longer does, and
  decodes twice instead — a buffer has a size and a string does not.
* `%.500f` of 1e300 needs 812 characters and produced 511. `snprintf` reports
  the length it WOULD have written, so that is now the signal to reformat
  through a heap buffer. A precision is a number the caller picks.

None of the three could be caught by reading, and the first was caught by the
host differential rather than by review.

## `<cwctype>`

The eighteen functions of [cwctype.syn], answering for one wide character what
`<cctype>` answers for one byte — and answering it out of the Unicode Character
Database, which is the single decision worth recording about this header.

**Why it is not the ASCII answer.** `<cctype>` writes its classification out as
range tests, because the `"C"` locale's execution character set is ASCII. That
argument does not survive into the wide header, because Ф41-b already fixed the
encoding of this system's one locale as UTF-8 with `MB_CUR_MAX` 4, and Ф41-e-1
built the codec that turns those bytes into code points. A `wchar_t` here is a
Unicode scalar value. A system that decodes UTF-8 into real code points and
then reports that none of them is a letter would be contradicting itself, so
`iswalpha(L'ж')` is 1.

The two headers do not disagree, and the reason is worth stating: `0xD0`, the
first byte of `'ж'` in UTF-8, is not a character but a fragment of one, and
`isalpha(0xD0)` is right to answer 0. Each header is correct about the thing it
classifies.

**Two measured divergences from macOS libc in `en_US.UTF-8`,** both deliberate:

| | macOS | boxcxx | Why |
|---|---|---|---|
| `iswalpha(U+4E2D)` | 0 | **1** | `Lo` is `Alphabetic` in `DerivedCoreProperties`. A CJK ideograph is a letter; macOS classifies only what its own locale tables carry. |
| `iswprint(U+00AD)` | 1 | **0** | A SOFT HYPHEN is `Cf` — a format character with no glyph. Excluding it also forces `iswpunct(U+00AD)` to 0, since punct must imply print for the classification to hold together. |

**Three predicates stay ASCII on purpose, and both references agree.** C fixes
`iswdigit` and `iswxdigit` to the decimal- and hexadecimal-digit characters of
its own 5.2.1 rather than to a Unicode property, so `iswdigit(U+0660 ARABIC-INDIC
DIGIT ZERO)` is 0 — measured, in a UTF-8 locale, on the reference. `iswalnum` is
alpha-or-digit on top of that, which leaves U+0660 in no class but `print`:
not a digit, not alphanumeric, not punctuation. That is what a UTF-8 locale
answers.

**Which property backs which predicate** is recorded in
`tools/gen_unicode_wctype.py`, beside the code that acts on it, and the tables
it emits into `<__bits/unicode_wctype>` hold nothing but UCD facts. `iswgraph`,
`iswalnum`, `iswpunct`-implies-`iswprint` and the `iswctype` dispatch are
derived in the header from the tabled predicates rather than tabled themselves,
so they cannot drift from what they are defined in terms of.

**`towupper`/`towlower` are the SIMPLE case mappings** (UnicodeData.txt fields
12 and 13). They are single-code-point functions and cannot express the full
mappings of SpecialCasing.txt, where one character becomes several:
`towupper(U+00DF LATIN SMALL LETTER SHARP S)` stays U+00DF rather than becoming
`"SS"`. Every implementation draws the line in the same place.

**`wctype_t` and `wctrans_t` are `unsigned long` handles**, and an unknown
property name yields 0, which `iswctype` then answers 0 for — C's own wording
is that the returned value is usable only if it is nonzero. `wctype(nullptr)`
and `wctrans(nullptr)` yield 0 rather than dereferencing.

**How the tables are known to be right.** The generator verifies both encodings
against the raw UCD over all 1 114 112 code points and refuses to write on any
disagreement. That checks the tables but not the wiring, so the compiled header
was then dumped on the host and compared against an independent per-code-point
computation straight from the UCD text: 1 114 112 code points × 14 answers,
zero disagreements. Three mutations — a table wired to the wrong predicate,
`iswgraph` dropping its space exclusion, `MapCase` ignoring its stride — were
each caught, with a diagnostic naming the code point. `Phase212` then re-derives
the population count of every class on the target itself, because the host
proves nothing about a different compiler and a `wint_t` that is unsigned here
and signed there.

**One guard is defensive rather than load-bearing, and saying so is cheaper
than implying otherwise.** `<cwctype>` rejects anything above U+10FFFF before
consulting a table, which is what makes `towlower(WEOF) == WEOF` read as
intentional. Removing it would not change a single answer: the edge arrays are
balanced, so a code point past the last edge has even parity and is in no set,
and `MapCase` returns its argument when it falls past the last run. The guard
states the contract; the encoding already honoured it.

## `<debugging>`

- `~` `is_debugger_present()` always returns `false`, and that is the true
  answer rather than a placeholder: BoxOS has no debugger-attachment protocol,
  so there is nothing that could be attached. `breakpoint_if_debugging()` is
  therefore a no-op, which is exactly the property that makes it safe to leave
  in shipping code — the reason P2546R5 separates it from `breakpoint()`.
- `!` `breakpoint()` executes `INT3`. IDT vector 3 is wired (`isr.asm`,
  "Breakpoint") and a user-mode `#BP` takes the generic kill-the-faulting-
  process path in `idt.c`, so calling it with nothing attached **ends the
  program**. That is the standard's "otherwise the behavior is unspecified",
  and it is what an uncaught `SIGTRAP` does elsewhere; it is recorded here
  because the name does not suggest it.

## `<execution>`

- Complete as a header: `sequenced_policy`, `parallel_policy`,
  `parallel_unsequenced_policy`, `unsequenced_policy`, the four objects and
  `is_execution_policy`. New in Ф40; `__cpp_lib_execution` is 201902L
  (P1001R2, the step that added `unseq`).
- **What `par` runs on is the part worth reading.** A cabin keeps ONE brigade
  of strands, built the first time anything asks for parallelism, asleep on
  the kernel's address-park between jobs, and reused by every call after
  (`src/runtime/par_engine.cpp`). The two alternatives were rejected on
  purpose: hiring strands per call makes `par` slower than `seq` on anything
  short, which is the opposite of what the policy is for, and running
  everything sequentially — what libstdc++ ships when it has no TBB to hand —
  would make the feature-test macro name something that is not true. Measured
  on 12 app-cores: `par` used all twelve strands, `seq` used one, and a 400 k
  `transform_reduce` went from 60 ms to 32 ms.
- One brigade, one region at a time. A second strand asking for parallelism
  while a region runs waits for it rather than splitting the crew. A NESTED
  region — a parallel algorithm called from inside one — runs sequentially,
  which the standard permits (a policy is permission, not obligation) and
  which is the only shape in which the brigade cannot wait on itself.
- Without strands (no FSGSBASE, so `strand_spawn` refuses) the brigade has
  zero workers and every parallel call runs on its caller. Nothing degrades
  except the speed, and that is what a machine with one usable core is.
- `[algorithms.parallel.exceptions]`: an element access function that exits by
  throwing calls `terminate()`. That is the specification, not a shortcut, and
  the worker body catches everything and calls it rather than letting an
  exception walk out of a strand entry point.
- **The overload set is complete**, and `__cpp_lib_parallel_algorithm` is
  201603L: every `ExecutionPolicy` overload in [algorithms], [numeric.ops] and
  [specialized.algorithms] exists. What the macro does NOT say is that every
  one of them splits, and this document does:

  | | |
  |---|---|
  | **Element-wise** — the chunks are independent because the elements are | `for_each`, `for_each_n`, both `transform`s, `fill`, `fill_n`, `generate`, `generate_n`, `copy`, `copy_n`, `move`, `swap_ranges`, `replace`, `replace_if`, `replace_copy`, `replace_copy_if`, `reverse`, `reverse_copy`, `rotate_copy`, `adjacent_difference`, and the whole [specialized.algorithms] family |
  | **Per chunk, then combine** | `count`, `count_if` (one atomic add per chunk, not per element), `min_element`, `max_element`, `minmax_element` (a compare-and-exchange between chunk winners), `reduce` ×3, `transform_reduce` ×3 (a partial per chunk, folded afterwards) |
  | **First match** — and it short-circuits: a chunk past the current winner stops walking | `find`, `find_if`, `find_if_not`, `find_first_of`, `find_end`, `adjacent_find`, `search`, `search_n`, `mismatch`, `equal`, `lexicographical_compare`, `all_of`, `any_of`, `none_of`, `is_sorted`, `is_sorted_until`, `is_heap`, `is_heap_until`, `is_partitioned` |
  | **Two pass** — count per chunk, turn the counts into offsets, write | `copy_if`, `remove_copy`, `remove_copy_if`, `unique_copy`, `partition_copy`, `inclusive_scan`, `exclusive_scan`, `transform_inclusive_scan`, `transform_exclusive_scan` |
  | **Split then merge** — the sort is parallel, the merges are not | `sort`, `stable_sort` |
  | **`~` On the calling strand** | `rotate`, `remove`, `remove_if`, `unique`, `partition`, `stable_partition`, `inplace_merge`, `merge`, `includes`, `set_union`, `set_intersection`, `set_difference`, `set_symmetric_difference`, `nth_element`, `partial_sort`, `partial_sort_copy`, `shift_left`, `shift_right` |

  The last row is not laziness: each of those is a DIFFERENT algorithm in
  parallel rather than a chunked version of the same one — the in-place
  rearrangers move elements past each other, the set operations walk two ranges
  in lockstep, and `nth_element` and `partial_sort` are defined by an order the
  chunks cannot see. A chunked version that quietly serialised would be the
  claim this library does not make, so they say it here instead.
- `stable_sort` under a policy IS stable and `sort` is not, which is the same
  promise each makes without one. The two differ in exactly one place — which
  sort each chunk gets — because `inplace_merge` is stable and the runs are in
  order, so the chunk sort decides the answer. Sharing one implementation
  between them, which the first version of this did, quietly made
  `stable_sort(par, ...)` unstable: `std::sort` may reorder equal elements
  inside a chunk and no amount of stable merging puts them back. A test over
  `int`s cannot see that, which is why phase202 sorts a key with a payload.
- **The tie rules are where a parallel min/max goes wrong.**
  [alg.min.max] wants the FIRST smallest and, for `minmax_element`, the LAST
  largest — two different rules from one comparison. Getting the second by
  loosening the comparison to "greater or equal" makes both directions true for
  equal elements, and then whichever strand reaches the compare-and-exchange
  last wins: a race that one round of testing passes by luck. The tie rule
  belongs in the combine, not the comparison; phase202 runs those checks twenty
  times over for exactly that reason.
- The predicate algorithms short-circuit through a shared flag: a chunk that
  sees the answer already decided stops walking. The standard does not require
  it; without it every chunk would read a range whose answer is known.
- `accumulate` has no parallel overload and never will — that is the standard's
  design. `reduce` is the one that may reorder, which is why it requires an
  associative and commutative operation, and why this implementation folds
  per-chunk partials in whatever order they finish.

## `<flat_map>` and `<flat_set>`

- `~` `flat_set::iterator` **is** `KeyContainer::const_iterator` — for
  `flat_set<int>` that is literally `const int*`. The standard permits this
  (libstdc++ does the same; libc++ wraps it), and it means `flat_set` models
  `contiguous_range` and `ranges::data` hands back a `const Key*`. On bare metal
  that is worth having: the key block can go straight into a copy or a DMA
  descriptor.
- `~` `flat_map`'s iterator is a lockstep proxy over the two containers, so
  `iterator_traits<…>::iterator_category` is `input_iterator_tag`, `value_type` is
  `pair<K,T>`, and `pointer` is an arrow proxy. Both answers follow from the
  reference being a prvalue; libc++ reports `random_access_iterator_tag` for the
  same iterator, which its own reference type cannot support.
- `–` `insert_range` **copies** — it never moves out of the source range, even from
  a genuine rvalue range — and it does not accept tuple-like ranges such as
  `views::zip`. The first is what [flat.map.modifiers]/12 says
  (`for (const auto& e : rg)`); the second is the standard's own inconsistency,
  discussed in §4. The classic `insert(first, last)` **does** move.
- `~` `flat_set::emplace` carries `requires is_constructible_v<value_type, Args...>`,
  which [flat.set.defn] does not ask for. All three implementations reject
  `emplace(1,2,3)`; the real difference is that boxcxx and libstdc++ make the
  rejection SFINAE-detectable while libc++ reports the call as viable.
- `~` `flat_set::swap` is unconditionally `noexcept` with no `static_assert` gate on
  the container's swappability. [flat.set.defn] declares it unconditionally, so this
  is the synopsis followed exactly, accepting that a throwing container swap ends in
  `terminate` — the consequence [except.spec]/5 selects, not a diagnostic the
  library is free to invent. libstdc++ 15.2 adds such an assert and thereby rejects
  valid programs.

## `<format>`

- `✓` Closed in Ф46: **a format string chosen at run time had no way in.**
  `basic_format_string`'s constructor is `consteval`, which is the whole point of
  it — a spec the compiler cannot answer is a compile error rather than a throw,
  and an argument index past the end never reaches the engine. The hole that
  leaves is a string that does not exist until the program runs, and until Ф46
  the only route was `vformat` with a hand-spelled `make_format_args`: the caller
  builds the argument store themselves for the sake of one string that happens
  not to be a literal. P2918R2's `runtime_format` is the door, and it leads to the
  same engine — every spec below is also spelled as a literal in phase239, and the
  two must produce the same text.

  **What makes it safe is what it refuses.** The object holds a *view* of the
  caller's string, so its copy and assignment are deleted: a program that binds it
  to a name and formats with it later gets a diagnostic where that is written,
  rather than a read of a string that has died. The correct use pays nothing —
  `format(runtime_format(s), …)` is a prvalue that materialises straight into the
  parameter. The door is cut into `basic_format_string` itself, so `format_to`,
  `format_to_n`, `formatted_size` and the six locale-taking overloads of Ф43-e-1
  all take it without a second entry point.

  **The other half of the macro's value was already here, and this document's own
  source said otherwise.** `<__bits/version_format>` recorded that C++26's values
  "need P2905R2 and P2918R2, which this library does not have". P2905R2 is the
  paper that makes `make_format_args` take `Args&` rather than `Args&&`, so a
  temporary cannot be stored in an argument store that outlives it — and
  `make_format_args` here has always been declared `make_format_args(Ts &...vs)`.
  Measured rather than remembered: `make_format_args(i)` compiles,
  `make_format_args(42)` does not. So Ф46 was one paper, not two, and
  `__cpp_lib_format` is **202311L** (§1.3).

  ‼ Pinning that measurement took a second attempt, and the reason is worth
  keeping. `requires { make_format_args(42); }` written with no template
  parameter is a **hard error** on GCC 15.2 rather than a false
  requires-expression: deduction succeeds with `Ts = int`, and the failure is
  binding `int&` to an rvalue, which is reported outside the immediate context.
  Made dependent — `template <class T> concept … requires {
  make_format_args(declval<T>()); }` — it answers `false` and `true` correctly.
  Ф31 recorded that `requires{call(...)}` can report *present* when a thing is
  absent; this is the same trap from the other side.
- `✓` Closed in Ф31e-a: a `basic_string` or `basic_string_view` with non-default
  traits or allocator now has the *partial* specializations
  [format.formatter.spec]/2.2 asks for, and formats as its text. It used to match
  neither full specialization, fall through to the range formatter, and print
  `['a', 'b', 'c']` — with width padding the bracketed form, which made the wrong
  output look deliberate — while `{:?}` was rejected outright.
- `✓` Closed in Ф31e-a: `format("{}", volatile_lvalue)` is rejected, as it is by both
  reference libraries. `MapKind` applied `decay_t` before formattability was ever
  consulted, so the `volatile` was dropped, the argument was stored as a plain
  `int`, and the library's own `static_assert(FormattableWith<…>)` guard was
  bypassed — `formattable<volatile int, char>` said `false` the whole time.
- `✓` Closed in Ф43-e-2: **field width and precision were capped at 65535**, on
  the literal path as a compile-time error from the consteval check and on the
  dynamic path as a run-time throw. The reasoning had been that a hostile format
  string must not be able to ask for a two-gigabyte field — but the number was
  this library's own, no document mentions it, and the program that actually met
  it was the one laying out a wide report, not the attacker. It is gone from
  both paths. What bounds a field now is the memory the machine has: the sink
  grows until the allocator refuses and throws `bad_alloc`, the answer every
  other oversized request already got. The grammar still stops above `INT_MAX`,
  because a width is an `int`.

  The cap could never have been a budget anyway, and that is what settled it: a
  nine-character spec such as `"{::65535}"` applied to a large range multiplies
  the output whatever the per-field limit is, and both reference libraries
  produce the same volume byte for byte. A limit that stopped the honest case
  and not the hostile one was not paying for itself.

  **What "bounded by memory" does not cover, stated rather than glossed:**
  `formatted_size` and `format_to_n` must report the size the full field would
  have, so they still *iterate* it — `formatted_size("{:2000000000}", x)`
  allocates nothing and costs two billion steps. Time is not bounded here, only
  space, and a program that hands an untrusted format string to either of those
  should bound the string instead. libc++ behaves the same way; libstdc++ stops
  the literal spelling of it and not the dynamic one.
- `~` `enable_nonlocking_formatter_optimization` is provided. It is a **C++26**
  feature (P3107R5) — N4950 does not mention it — adopted early by decision. See
  §3 for the carve-outs; note also that libstdc++ 16.1 reports `true` for
  `sys_time` where boxcxx and libc++ 22 both report `false`.
- `~` P2510R3 (**C++26**) is implemented: the `0` flag is accepted for pointers,
  and is ignored when an explicit alignment is present.
- `~` `formatter<__int128>` and `formatter<unsigned __int128>` exist (both
  reference libraries have them too), but `__int128` is **rejected as a width or
  precision argument**: [format.string.std]/10 requires a *standard* signed or
  unsigned integer type, and the extended types are not standard integer types.
  The rejection happens in the consteval check, before any run-time throw.
- `✓` Closed in Ф43-e-2: **width was measured in code units**, where
  [format.string.std]/13 asks for the estimated width — extended grapheme
  clusters by UAX #29, of which the eighteen ranges the standard writes out
  count two columns. `format("{:4}", "é")` used to leave two spaces after a
  character one column wide, and a padded table of anything but ASCII came out
  ragged. The rules live in `<__bits/format_width>`, hand-written because a rule
  is not a fact; the three UCD properties they read
  (Grapheme_Cluster_Break, Extended_Pictographic, Indic_Conjunct_Break) live in
  `<__bits/unicode_grapheme>`, generated end to end. The rules were checked
  against **Unicode's own GraphemeBreakTest-17.0.0 — all 766 cases pass** — and
  then against both reference libraries over 40 000 random strings drawn from
  the alphabet where the rules bite: marks, jamo, conjuncts, emoji, regional
  indicators, controls and ill-formed bytes. §3 has the one corner where all
  three answers were not the same.

  Precision moved with it ([format.string.std]/14): it names the longest prefix
  whose estimated width fits, so a cut lands between clusters and can no longer
  split a UTF-8 sequence. Both pins phase227 had planted for this fell, and
  phase228 is where the rule is now tested.
- `✓` Closed in Ф43-e-1: **`L` was parsed and dropped, and there were no
  locale-taking overloads at all.** Both halves of that are gone. The six
  functions [format.functions] gives a `const locale&` — `format`, `format_to`,
  `format_to_n`, `formatted_size`, `vformat`, `vformat_to` — exist, the context
  carries the locale (by pointer: the pointee is the caller's argument and
  outlives the call, while a locale by value would put a refcounted member in
  every scratch context a range builds), and `basic_format_context::locale()`
  returns it rather than a default-constructed one. `L` then reads `numpunct`
  for the group separators, the radix character, and the two words a `bool` is
  spelt with.

  Measured on both reference implementations, which agree on every case:
  grouping applies to EVERY base (`{:Lb}` groups bits), the separators count
  toward the field width, and zero padding is NOT grouped — `{:012L}` of
  1234567 is `0001'234'567`. `L` is still explicitly rejected for strings and
  pointers, as both references reject it.

  The locale survives into the scratch context a range or tuple builds when a
  width forces it to measure itself first. That is not a detail: a mutation
  that dropped it survived the first version of the check, because without a
  width the elements go through the OUTER context and the plumbing is never
  exercised. The check that catches it now puts a width on the range.
- `~` The `set` and `map` range formatters expose `set_brackets` and
  `set_separator`, which the standard gives only to the sequence specialization.
  Both reference libraries reject those calls. A harmless extension, but generic
  code written strictly against the promised surface would not expect them.
- `✓` Closed in Ф42-g: **wide formatting exists**, and with it every name
  [format.syn] gives the wide half. See §1.2 for the shape of the change and
  for why it needed no second engine.
- `✓` Closed in Ф42-g, and it had been in NEITHER column: **`basic_format_context`
  did not exist at all.** [format.syn] names it as a class template and defines
  both `format_context` and `wformat_context` from it; boxcxx had a plain class
  called `format_context` and no template, and no line here said so. Found by
  reading the synopsis against the FILE rather than against the list of what is
  absent — the same way Ф42-f found the five `*streampos` aliases. It is now the
  template, with exactly one specialization defined (over the erased sink
  iterator, which is the only `Out` this library has); naming
  `basic_format_context<MyIterator, char>` is a compile error rather than a type
  that looks constructible and is not.
- `✓` Closed in Ф42-g, same reading, same result: **`basic_format_context::locale()`**
  ([format.context]) was missing. It costs the return of an empty class — `<ios>`
  has held one of those by value in every stream since it was written.
- `+` **A string whose character type is not the context's is REFUSED**, and this
  is where the two reference libraries part company. `format(L"{}", std::string("hi"))`
  is rejected here and by libstdc++ 16.1; **libc++ 22 prints `['h', 'i']`** —
  a string IS a range of characters, `formattable<char, wchar_t>` is true, and
  nothing in its range machinery stops it. With a string literal it goes further:
  `format(L"{}", "abc")` yields `['a', 'b', 'c', '\u{0}']`, the terminating NUL
  included. Measured: `format_kind<std::string>` is `sequence` in both, and
  `formattable<std::string, wchar_t>` is **0 in libstdc++, 1 in libc++**. boxcxx
  answers 0, through disabled partial specializations that beat the range
  catch-all by partial ordering — exactly the defect Ф31e-a closed on the narrow
  side, where an odd-traits string was being bracketed for the same reason.
  Bracketing is the worse failure of the two: it produces output that looks
  deliberate.
- `✓` Closed in Ф43-e-2 together with the narrow half: **both alphabets estimate
  the width**, through one engine. A `wchar_t` is a code point here, so the wide
  side needs no decoder — `NextScalar` hands the unit straight back — and the
  clustering and the two-column list are the same code. The entry this replaces
  said libc++ could not serve as an oracle for wide text; it can, and it was
  used as one.
- `~` A **character argument is measured too**, and that is where the two
  references part: `format(L"{:4}", L'你')` leaves two fills here and in
  libstdc++ 16.1, three in libc++, which counts a lone wide character as one
  column. A character is formatted as a string of one character and the standard
  gives it no separate rule.
- `+` **A localized number is measured the same way every other field is**, and
  here boxcxx parts company with BOTH references. They count the CODE POINTS of
  a localized number: measured, `format(loc, L"{:~<20L}", 1234567)` through a
  facet whose `thousands_sep` is U+FF0C pads to twenty code points in each of
  them, though those two separators occupy two columns apiece and the result is
  twenty-two columns wide. boxcxx pads to twenty columns. The reason is not
  pedantry — `width` cannot mean columns for a string and code points for a
  number inside one library, or a program printing a column of figures next to a
  column of names gets a ragged table and no reason for it. The body is still
  never materialized: one walk over the characters that are about to be written
  feeds the measurement, so the two cannot drift apart.
- `~` **The `'c'` presentation is the one place the two alphabets are SUPPOSED to
  disagree.** [tab:format.type.int] copies `static_cast<charT>(value)` and refuses
  what `charT` cannot hold, so `format("{:c}", 255)` throws while
  `format(L"{:c}", 255)` is U+00FF, and `format(L"{:c}", -7)` is `wchar_t(-7)`
  where the narrow half writes the single byte `0xF9`. Both halves match libc++ 22
  on both sides (measured before either was written).
- `~` **A byte above 0x7F in a narrow body widens byte-wise**, the same way
  `<ios>::widen` does. There is exactly one way to produce one — a `%Z`
  abbreviation handed to `chrono::local_time_format` is a `std::string` the
  program supplies — and `Phase219` pins it, because the alternative (decoding
  it as UTF-8, which on this system would arguably be more useful) would install
  a second widening rule beside `widen`'s. Two answers to one question is the
  defect Ф42-f found in `widen` itself.
- `?` For a type with no formatter, the intended `static_assert` message does fire
  — but four noisier errors precede it (a deleted constructor, a missing `parse`,
  and two consteval failures).

## `<fstream>`

New in Ф36. The backing is a `"file:"` Current, and the `s` a filebuf opens is
a TagFS **name** — there is no directory to walk and no separator to parse.
All nine rows of [filebuf.members] Table 122 are implemented, along with the
`noreplace` column P2467R1 added, and the suite pins each of them plus the
combinations the table leaves out.

- **The codecvt boundary (Ф42-f).** Until then a character moved to and from
  the file as `sizeof(CharT)` raw bytes, which was right for `char` and would
  have made a `wofstream` write **UTF-32** to a file every other program on
  this system reads as UTF-8. The facet plugs in here, and the switch between
  the two paths is the standard's own, `always_noconv()`. `Phase217` checks the
  bytes on the volume with boxlib's `fread` rather than with an `ifstream`,
  because a filebuf checked with a filebuf agrees with itself no matter how
  wrong it is.
- `~` **A relative seek fails on a wide file, and that is [filebuf.virtuals]
  speaking.** With `encoding() <= 0` a nonzero `off` is an *error*: `off`
  characters from here is not a number anyone can compute without reading
  them, and landing between two bytes of one character is worse than failing.
  `seekoff(0, …)` still works in all three directions, and `seekpos` with a
  `pos_type` this filebuf handed out works for any position — which is what
  `tellg()`/`seekg()` is for. **`seekpos` therefore does NOT route through
  `seekoff`**, which is how it was written at first and what the wide phase
  caught within one run: it inherited the very rule it exists to bypass, and
  `tellg`/`seekg` was unusable on every wide file while every other check
  passed.
- `?` **`tellg()` is a byte offset, and it is `codecvt::length()` that makes it
  one.** Characters × width is not a position when the width varies, so the
  filebuf keeps the external bytes its get area came from and asks the facet
  how many of them the characters up to `gptr()` consumed. That is the
  question `do_length` exists to answer.
- `?` **`sungetc()` past the start of the buffer returns a success marker, not
  the character.** [streambuf] makes it `return pbackfail(eof())`, and
  [filebuf.virtuals] promises of `pbackfail` only "some value other than
  `eof()`" on success; this one answers `not_eof(eof())`. The position has
  moved — the next `sgetc()` returns the character — but the value handed back
  says nothing about which. This entry exists because the wide phase's own
  first check asked for the character and failed against correct code.
- `+` **`putback` of a DIFFERENT character is refused when the facet
  converts.** For `char` the replacement is written through to the file,
  because putting it only in the buffer would be a lie about the file. A wide
  replacement need not encode to the same number of bytes as the character it
  displaces, so writing it through would shift everything after it. Refusing is
  the only honest answer left.
- `+` **A state-dependent facet cannot be opened over.** `encoding() < 0` means
  every position is a pair — an offset and the shift state to reach it — and
  every seek a replay. boxcxx has no such facet; `open()` refuses one rather
  than assume it will never appear, so the assumption `LogicalPos()` rests on
  is checked where it is made.
- `?` **A character split across a read is re-read, not carried.** The byte
  window grows until it holds a whole character or the file has no more to
  give, and whatever the conversion did not consume is simply left in the file
  for the next refill to seek back to. A filebuf over a pipe would have to
  carry those bytes forward; a Current is always seekable.
- `–` **No `filesystem::path` overloads** of the constructors or of `open()`,
  because there is no `<filesystem>` (§1.1). The `const char*` and
  `const string&` forms are complete.
- `~` **`truncate` is refused on a snapshotted file**, so `ios_base::out` on
  one fails to open rather than silently doing something else. A block that
  has not been copied since the snapshot was taken is still the snapshot's
  only copy, and a `CowSnapshot` records redirects rather than an extent list
  of its own — nothing in the kernel can tell "mine alone" from "shared with a
  frozen view". Freeing such a block would corrupt the snapshot. Refusing
  costs a rare failed open; guessing costs the snapshot.
- `~` **The truncate primitive shrinks only.** Growing a file by truncation is
  refused rather than served, because TagFS does not zero freshly allocated
  blocks and a grow would hand back whatever the allocator's previous tenant
  left there. No `<fstream>` operation needs it: files grow by being written.
- `?` **`binary` is accepted and changes nothing.** A Current is a byte channel
  and boxcxx performs no text translation, so binary and text are one road.
  The standard leaves the difference implementation-defined; this records
  which way it was settled.
- `?` **`showmanyc()` reports the bytes remaining in the file**, and `0` at the
  end rather than `-1`. `-1` would assert that no character can ever be read
  again, which of a file another writer may still append to is not knowable.
- `+` **`pbackfail()` with a character the file does not hold writes it
  through**, and fails when it cannot — in a read-only or appending mode.
  Putting it only in the get area would be a lie about the file. The standard
  permits either.
- C++26 `__cpp_lib_fstream_native_handle` (P1759R6) is claimed:
  `native_handle_type` is `Current*`, not an `int`, because BoxOS has no
  descriptor table to index into.

## `<functional>`

- `✓` Closed in Ф40, found by building `<stdfloat>`: **`std::hash` had no
  specialization for `long double`**, nor for any of the five extended
  floating-point types. [unord.hash]/2 asks for an enabled specialization for
  every arithmetic type, and only `float` and `double` had one — a
  `std::unordered_set<long double>` did not compile and nobody had tried. The
  long double specialization hashes **ten** bytes, not `sizeof`: x86_64 stores
  the type in sixteen, of which six are padding that no store writes, and
  hashing them would let one value hash two ways depending on what the stack
  held.

- `✓` Added in Ф34: **`std::copyable_function`** (P2548R6), all twelve
  cv/ref/noexcept specializations. It is the type `std::function` should have
  been: the qualifiers are part of the type, so a `copyable_function<void()
  const>` accepts only targets callable through a const reference — where
  `std::function`'s `const` operator() calls a mutable target anyway — and there
  is no `target()`/`target_type()`, which is what keeps the whole type free of
  RTTI. A heap-held target is deep-copied, never shared: sharing would make two
  functions one, and a non-const row may mutate.
- `✓` Closed in Ф31e-g-3: P2655R3's two `basic_common_reference` specializations
  for `reference_wrapper` were absent, so
  `common_reference_t<reference_wrapper<int>&, int&>` had **no type at all** —
  not a surprising type, none. `COND-RES` is ambiguous there (the wrapper
  converts to `int&` and `int&` converts to a wrapper), so bullet (1) of
  [meta.trans.other]/5 fails and `basic_common_reference` is what has to answer.
  The pair of specializations is asymmetric on purpose: exactly one side may be a
  wrapper, or the answer would not be unique. Closed
  `__cpp_lib_common_reference_wrapper` and, with it,
  `__cpp_lib_common_reference` (which `<type_traits>` owns for the same paper).
- `+` `function_ref` (P0792R14) implements **P3961R1**, which libstdc++ 16.1
  does not: constructing one specialization from another copies the source's
  thunk and bound entity instead of binding to the source object. The
  difference is a lifetime — the absorbed reference points at the original
  target and survives the source function_ref's death — and it is why the macro
  reads 202604.
- `~` `operator()` is not `constexpr`, exactly as [func.wrap.ref.inv] declares
  it. The constructors are, so a `function_ref` can be built in a constant
  expression; it cannot be called in one, because recovering a `T*` from the
  `void*` half of the bound entity is not a constant expression.
- `–` `std::nontype` / `nontype_t` do not exist. They were removed from C++26
  in favour of the `constant_wrapper` constructors, which are what is
  implemented here.

## `<generator>`

- `~` The generator's iterator declares the full legacy member set
  (`iterator_category`, `reference`, `pointer`) where [coro.generator.iterator]
  lists only `iterator_concept`, `value_type` and `difference_type`. The
  observable consequence is that `iterator_traits<generator<T>::iterator>` is
  **non-empty** here and empty in libstdc++ 16.1 (measured). `input_range` holds
  in both.

## `<inplace_vector>`

- `~` **Constant evaluation works for trivially-default-constructible,
  trivially-destructible element types only.** This is inherent, not a
  shortcut: the storage is `union { T elems_[N]; }` with no active member, and
  a union member whose lifetime has not begun cannot be written through in a
  constant expression. The compile-time path therefore value-initializes all N
  slots up front and then assigns; for a `T` that cannot be value-initialized
  there is nothing to value-initialize. libstdc++ 16 has the same limit and
  reaches for `__builtin_unreachable()` there; boxcxx calls a declared-but-not-
  defined, deliberately non-constexpr
  `__iv::NeedsTrivialTypeAtCompileTime()`, so the diagnostic names the reason.
  Every run-time path is fully general.
- `+` The size field is the narrowest unsigned type that can hold `N` without
  costing alignment padding, so `sizeof(inplace_vector<char, 8>)` is 9 rather
  than 16. This is not required by [inplace.vector]; it matters for a container
  whose whole purpose is to sit inside another object.
- `+` `iterator` is a plain `T*`. The standard leaves the type
  implementation-defined and asks only for a contiguous constexpr iterator;
  a wrapper would buy nothing but a longer name in diagnostics.
- `+` Insertion in the middle **appends and then rotates**, rather than
  shifting the tail right and filling the hole. The hole cannot be made
  exception-safe with a single size field: while it is open the live
  elements are not a prefix, so no count describes them, and a throw from
  `T` would leave the destructor about to run over slots that were never
  constructed. Appending keeps the count exact at every step, the rotate
  only ever move-assigns between live elements, and an argument that names
  an element of the same vector stays valid because nothing moves under it.
  The cost is about three moves per element where a shift costs one.

## `<iomanip>`

- `✓` **[iomanip.syn] is complete as of Ф43-d-3.** The six parameterized
  manipulators — `setw`, `setprecision`, `setfill`, `setbase`, `setiosflags`,
  `resetiosflags` — plus `quoted` have been here since the epic's early phases;
  Ф43-d-2 added `get_money`/`put_money` and Ф43-d-3 `get_time`/`put_time`, each
  on top of the facets it had just built. The header's own comment used to say
  all four were "out of scope" because those facets did not exist; that
  sentence is gone rather than left standing next to the functions it denies.
- `✓` The two monetary manipulators are the only things in this header that
  need a COMPLETE stream type and a facet lookup, so it includes `<istream>`
  for them where `setw` needs nothing but a member call. That is a real
  dependency and not tidiness: `operator>>` for `get_money` constructs a
  sentry, and a sentry cannot be built from a forward declaration.
- `~` `moneyT` is constrained by overload resolution rather than by a
  `static_assert`: `money_get` and `money_put` have exactly the `long double`
  and `basic_string` overloads and no generic one, so anything else fails to
  find a match. The diagnosis names the overloads instead of repeating the
  Mandates in prose.

## `<ios>`

- `✓` Closed in Ф42-f: **`widen` sign-extended and `narrow` truncated**, and
  neither could be seen while `char` was the only instantiation. `widen` was
  `static_cast<char_type>(c)` on a *signed* `char`, so `widen('\xD0')` produced
  the `wchar_t` **-48** — while `<ostream>`'s `WriteRun`, widening the same
  byte on its way into a wide stream, produced U+00D0. Two answers to one
  question, from two places that had never both been reachable. `narrow` was
  `static_cast<char>(c)`, so `narrow(L'ж', '?')` returned `0x36` — the digit
  `'6'`, a plausible character, from the function whose second parameter exists
  precisely to say *there is no such character*.
- `✓` Closed in Ф43-b: **`widen` and `narrow` answered for themselves.**
  [basic.ios.members] defines both as calls into `use_facet<ctype<charT>>` of
  the stream's own locale, and until Ф43-b they computed the answer inline.
  While there was no `ctype` to install that was a distinction without a
  difference; the moment Ф43-a made a locale a container it became one, and a
  program that installed its own `ctype` would have watched the stream ignore
  it in silence. The answers for the classic facet did not change — the
  reasoning below moved into `<__bits/locale_ctype>` beside the facet that now
  gives them. The same routing reached the whitespace question: the sentry's
  skip, the character-array extractor, `operator>>(istream&, string&)` and
  `std::ws` all ask `ctype<charT>::is(space, c)` now, where they used to call
  an `__ios::IsSpace` of their own — which is gone.
- `?` **The pair is not a round trip above ASCII, on purpose.** Only ASCII has a
  single-byte form in this system's encoding, and `<cwchar>`'s `wctob` already
  says so (`wctob(L'ж')` is `EOF`, measured in Ф42-b), so `narrow` answers with
  its default there. `widen` has no default parameter and must return
  something, so a byte widens to the code point with its own value.
  [locale.ctype] requires the round trip only for the basic character set.
  **`widen` is a per-character map and not a UTF-8 decoder**: inserting the two
  bytes of `"ж"` into a wide stream yields the two characters U+00D0 U+00B6,
  which is also what inserting them one at a time yields. A decoder here would
  make `os << "..."` and `os << '.' << '.' << ...` disagree, and would make
  `setw` count something other than what it pads.

## `<iosfwd>`

- `✓` Closed in Ф42-f: **the five position aliases of [iosfwd.syn] were in
  NEITHER column** — `streampos`, `wstreampos`, `u8streampos`, `u16streampos`
  and `u32streampos` were not in the tree, and were not recorded as absent
  either. The same shape of gap §1.1 found for `<ctime>`, found the same way:
  by reading the synopsis against the file rather than against the list.
- `?` **All five name the same type.** Each is defined as
  `fpos<char_traits<X>::state_type>` and every `char_traits` here reports
  `state_type = mbstate_t`, because a conversion state does not depend on which
  character type it is producing. They are spelled as `fpos<mbstate_t>` rather
  than through `char_traits` because `<iosfwd>` only forward-declares that
  template, and naming a member would make this header pull in what it exists
  not to pull in.

## `<iostream>`

New in Ф36. `cin`, `cout`, `cerr` and `clog` exist and are bound to the Current
spine: the three output objects to the `"screen"` Current, `cin` to the
`"keyboard"` one. They share the very handle `std::print` uses, so
`std::cout << a; std::print("{}", b);` emits `a` then `b` — one channel, one
order, one buffer underneath. The ties and buffering [iostream.objects]
specifies are all in place and pinned by the suite: `cin.tie() == &cout`,
`cerr.tie() == &cout`, `cerr` unit-buffered, `clog` neither.

- `~` **`std::cin` never reaches end of file.** The keyboard Current is a live
  console: it blocks for the next line and has no terminator, because a
  console has no end, and there is no Ctrl-D here to invent one. `eofbit` is
  therefore never set from input exhaustion, and `while (getline(cin, s))`
  does not terminate. This is the truth about the channel, not a defect in the
  stream — read a known number of lines, or read from a file.
- `~` **`cerr` and `clog` go to the screen, not to a diagnostic channel.**
  BoxOS has one — the serial `box::current::log` — and it keeps its own name.
  A `std::cerr` that only reached a serial cable would be invisible to anyone
  running on real hardware without one, and silently losing an error message
  is worse than not distinguishing it. `cout.rdbuf() == cerr.rdbuf()`, and the
  suite pins that too; the per-stream formatting state is still per-stream.
- `?` **The keyboard Current strips the line terminator; the stream buffer puts
  it back.** The channel is line-oriented and hands back one edited line
  without its newline. A character stream without line terminators is a
  different stream — `getline` would never find its delimiter and would splice
  every line the user ever typed into one — so `underflow()` appends the `\n`.
- `~` **`wcin`/`wcout`/`wcerr`/`wclog` exist since Ф42-f, and mixing them with
  the narrow four is well defined here — which C forbids.**
  [iostream.objects]/2 defers to the C standard, and C says a stream is byte-
  oriented or wide-oriented for its life; the reason is real there, because a
  wide write can leave a conversion suspended in a shift state that a byte
  write would walk over. It is not real here: the encoding is UTF-8, its
  codecvt reads and writes no state (§2 `<locale>`), both halves emit the same
  alphabet through the same handle, and neither keeps a buffer of its own — so
  `cout << "a"; wcout << L"б";` comes out in call order, exactly as
  `std::print` already interleaves with `std::cout`. On the input side `wcin`
  takes its bytes from `cin`'s own line buffer rather than opening the keyboard
  a second time, so the two share one cursor and a typed line cannot end up
  split between them.

  ‼ This is deliberately **not** what `<cstdio>` does, where orientation is
  enforced and `fwprintf(stdout, …)` refuses after `printf()` — a `FILE*` has
  one unget slot that byte and wide reads would have to share (§2 `<cstdio>`).
  These objects have no such slot to fight over.
- `?` **`sync_with_stdio` remains inert** and returns the previous value. There
  is no C stdio buffer to pair with; boxlib's own `printf` is a different
  subsystem that no stream here touches.
- The objects are constructed by a priority-102 constructor — after the TLS
  bootstrap at 101, ahead of every default-priority global in the program — so
  a user's own global constructor may write to `std::cout`. `ios_base::Init`,
  which was an honest no-op for the whole epic, now does the other half:
  its last destructor flushes the streams while they are still alive.

## `<istream>`

- `✓` Closed in Ф43-c-2: **every arithmetic extractor carried its own
  grammar.** [istream.formatted.arithmetic] defines each as a call into
  `num_get`, and moving the grammar there did more than relocate it.
  [facet.num.get.virtuals]/3's stage 2 states exactly which characters may be
  ACCUMULATED, and following that rule removed **five separate `sungetc`
  loops**: the old gatherer took anything number-shaped and then handed back
  whatever `from_chars` refused, while a grammar that never accumulates a
  character it cannot use has nothing to give back. That is also what lets
  extraction run through a single-pass iterator at all.
- `~` **A lone sign is CONSUMED now.** `is >> n` on `"-x"` used to leave the
  stream positioned at the `'-'`; it now leaves it at the `'x'`, fails, and
  stores zero. Stage 2 describes exactly this, and both reference
  implementations do it — the old putback was an undocumented deviation, and
  it is recorded here because it is the one visible behaviour change the move
  brought.
- `✓` Closed in Ф43-d-2: **stage 3 never checked where the separators
  stood.** `num_get` read thousands separators and discarded them without
  asking whether grouping() allowed them there, so `"12_34"` under a
  three-digit grouping parsed as `1234` with the stream still good.
  [facet.num.get.virtuals] stage 3 checks their positions and sets `failbit`
  while STILL storing the digits, which is what both reference implementations
  do and what makes the check observable rather than merely destructive. The
  rule itself now lives in one place, `__bits/field_engine`, because the
  monetary parser needed the same one and a second copy is a second chance to
  drift.
- `✓` **Whitespace is the `ctype` facet's answer** as of Ф43-b: the sentry's
  skip, the character-array extractor, `operator>>(istream&, string&)` and
  `std::ws` all ask `ctype<charT>::is(space, c)`, where they used to call an
  `__ios::IsSpace` of their own.
- What a program gains: `numpunct` reaches the parser. A comma decimal point,
  a thousands separator skipped as it is read, and `truename`/`falsename`
  under `boolalpha` all work on the way IN, which they could not before —
  there was no facet to ask.

## `<istream>` / `<ostream>` — the free inserters and extractors

- `~` **Whitespace is not the same question for the two character types, and
  Ф42-f made them answer differently.** `[istream.sentry]` asks
  `ctype<charT>::is(space, c)` and there is no ctype facet here. For `char` the
  answer is the six characters of the `"C"` locale, because a byte is where
  UTF-8 decoding has not happened yet — `0xA0` is half of U+00A0, not a space,
  and a narrow stream that treated it as one would split a character. For
  `wchar_t` it is **Unicode `White_Space`**, because by then the value IS a
  code point. Ф42-a settled that for `<cwctype>` and Ф42-b for `wcstod`, which
  must skip an EM SPACE before a number; a wide stream that then read U+3000 as
  part of a word would contradict both. Measured consequence, pinned by the
  suite: `wistringstream(L"a　b") >> w1 >> w2` gives `a` and `b`, while the same
  three bytes in a narrow stream are one word.
- `?` **`<ostream>`'s inserters exist in all three tiers of the synopsis, and
  `<istream>`'s in two.** Insertion has a `char` tier because a narrow literal
  offered to a wide stream can be widened; extraction has none, because the
  reverse would be narrowing and [istream.extractors] declares no `char&`
  overload for a stream of another character type.

## `<iterator>`

- `✓` Added in Ф34: **`basic_const_iterator`'s two conversion operators**
  (P2836R1). Without them a const iterator over a container's iterator could not
  be handed back to that container — `v.erase(ci)` did not compile, because
  `vector::const_iterator` is a different type and the only bridge, the
  underlying iterator's own conversion, was hidden behind `base()`. The
  constant-iterator requirement on the target is what stops the conversion from
  running the other way and undoing the const.
- `✓` Closed in Ф31e-g: `std::empty` and `std::data` had no `initializer_list`
  overloads. The generic ones cannot cover it — `initializer_list` has
  `begin`/`end`/`size` but no `empty()` and no `data()` member — so
  `std::empty({1, 2, 3})` failed while working on every container and array.
  They were the only two of the ten nonmember-access functions missing, and
  closed `__cpp_lib_nonmember_container_access`.
- `~` **`counted_iterator` has an `operator->` that the standard does not give
  it**, together with a forwarding `iterator_traits` specialization. The
  consequence is that `contiguous_iterator<counted_iterator<int*>>` **holds** in
  boxcxx, where the standard's `counted_iterator` can never be contiguous. A
  deliberate extension, not an oversight — but it means a concept check can answer
  differently here than elsewhere.
- `~` `iterator_traits` is specialized for `reverse_iterator<It>` and for
  `basic_const_iterator<I>`. The standard specifies neither specialization; they
  exist to supply the `iterator_concept` and `pointer` that those class bodies
  omit.
- `~` The `join_view`, `filter_view` and `transform_view` iterators declare
  `pointer` and `reference` members that their synopses do not list.
- `–` `pointer_traits` is not SFINAE-friendly: instantiating it for a type that is
  not pointer-like is a hard error rather than an empty specialization, and the
  error escapes even a `requires`-expression, so it cannot be detected.

## `<locale>`

- `✓` **A locale was a NAME and is now a CONTAINER.** Until Ф43 a locale here
  owned nothing: the facets were part of the image the way Ф37's Nameplate is,
  and `use_facet<F>(loc)` ignored `loc` entirely. The reason given was true —
  with no way to construct a second locale and no way to install a facet, a
  per-locale table would have been a table with one row and the row would have
  been the program — and Ф43 removed the premise rather than the conclusion. A
  program can now build `locale(loc, new my_facet)` and imbue a stream with it,
  which is the whole point of the mechanism and the one thing the old shape
  could not do at all. What it cost, measured: `locale` is one pointer instead
  of an empty class, so **`ios_base` and every `basic_streambuf` grew by 8
  bytes and stopped being trivially copyable** — each now holds a counted
  reference. A lookup is still two loads and no call, because the classic
  table is constant-initialized.
- `✓` **`locale::facet`'s destructor was not virtual, and now is.** The old
  entry recorded a real measurement — mark it `virtual` and the facet object
  moves from `.rodata` to `.data` and gains a `__cxa_atexit` registration whose
  destructor rewrites the vptr, so the facet stops answering during static
  teardown — and drew a conclusion wider than the measurement supported. Ф43
  measured the case the old entry never tried: a facet inside a **union cell**.
  A union does not destroy its active member, so the registered destructor runs
  and does nothing, and the facet answers at any instant just as before.
  `.rodata` did not come back with it — a virtual destructor makes the object
  writable-in-principle, and forcing the section anyway makes the assembler
  answer `setting incorrect section attributes for .rodata`, which is the
  protection keeping its name and losing its meaning. That is the price of the
  one thing a non-virtual destructor cannot do: end an object whose type it
  does not know. The standard's own mechanism — `delete this` from inside
  `facet`, where the protected destructor is reachable and virtual dispatch
  finds the real one — works for **any** facet a program writes, including one
  written to the letter with a protected destructor of its own.
- `✓` **`locale::id` carried no index, and now carries one.** The facets the
  image itself provides are numbered at compile time, which is what lets the
  classic table be an array finished before `main`; a facet a program writes
  takes its number from a counter the first time anyone asks. Two threads
  asking at once is settled by one compare-exchange, and the loser's number is
  dropped rather than reused — one empty column in a table that was always
  going to be sparse, against a lock on every lookup.
- `✓` **`facet(size_t refs)` accepted its argument and did not store it.** It
  stores it now, because [locale.facet]/2 makes it the answer to a question
  only the facet can answer: a locale handed a `facet*` has no other way to
  learn whether it is being given the object or merely shown it. `refs == 0`
  becomes a live count; anything else becomes a value no count can reach, so
  the whole question stays one comparison. A facet is 16 bytes rather than 8.
- `✓` **`use_facet<F>` for a facet the image did not carry was a COMPILE
  error.** It is `bad_cast` now, as [locale.convenience] says. The old answer
  was defensible while the set of facets was fixed at link time; a program can
  install one at run time now, so the question cannot be settled any earlier
  than it is asked. `has_facet<F>` still answers `false` rather than throwing.
- `~` **A locale name that is not this locale's is REFUSED.** `locale("C")`,
  `locale("POSIX")` and `locale("")` all name the one locale there is; any
  other name throws `runtime_error`. Until Ф43 the named constructor accepted
  anything and behaved as `"C"` — precisely the silent lie the rest of this
  document refuses to tell, sitting in the constructor whose entire job is to
  say which language you asked for. `locale::global` is real too: it replaces
  what `locale()` returns, so a stream constructed afterwards is imbued with
  the new one, and it calls `setlocale(LC_ALL, name)` for a named locale as
  [locale.statics] requires.
- `✓` **All six categories.**
  Ф43-b built `ctype_base`, `ctype<charT>`, the `ctype<char>` specialization
  with its table, `ctype<wchar_t>`, `ctype_byname` and the fourteen
  [classification] functions; Ф43-c added `numpunct`, `numpunct_byname`,
  `num_put` and `num_get`, and pointed every arithmetic inserter and extractor
  at the last two; Ф43-d-1 added `collate`, `messages` and their `_byname`
  forms, and with `collate` came `locale::operator()` — a locale can be passed
  as a comparator now, which it could not be while nothing knew how to order
  text; Ф43-d-2 added `money_base`, all four `moneypunct` instantiations,
  `moneypunct_byname`, `money_get` and `money_put`, and gave `<iomanip>` the
  `get_money`/`put_money` its own comment had been apologising for; Ф43-d-3
  finished the set with `time_base`, `time_get`, `time_put` and both `_byname`
  forms, and `<iomanip>` gained `get_time`/`put_time`. Nothing in
  [locale.category]'s Table 104 is missing now.

  **`time_put` contains no renderer.** `<chrono>` has had a complete
  `%`-conversion engine since Ф30, and `strftime` has been its second caller
  since Ф41; the facet is the third, through the one declaration in
  `__bits/time_render`. The indirection is forced, not stylistic:
  `__bits/chrono_format` includes `<format>`, `<format>` includes `<locale>`,
  so a `<locale>` leaf that included the engine would include itself. What the
  sharing buys is that `%U`, `%W`, `%V`, `%G` and `%g` come out of the ISO 8601
  week calendar `<chrono>` already had to get right, and that the `tm`-to-field
  conversion `strftime` carried is now used by both rather than copied — the
  facet's first draft did copy it, and that copy is what the review removed.

  **A conversion with no answer writes itself.** `%s` is a POSIX extension the
  C standard's `strftime` does not define and the engine does not implement;
  `put_time(&t, "%s")` produces `"%s"`. So does a conversion whose field cannot
  be rendered — `%b` with `tm_mon` of 99. That is what `strftime` does with a
  specifier it does not know, and it names the problem instead of printing a
  month that was invented.

  **`%Z` is `"UTC"` and `%z` is `"+0000"`.** Not a placeholder: `<ctime>` makes
  `localtime()` BE `gmtime()`, so this system's offset is known exactly and is
  zero. A facet answering `""` or refusing would be describing a system less
  certain of itself than this one is.

  **Four decisions where the two references part**, all measured before being
  chosen: `date_order()` is `mdy` (libstdc++ says `no_order`) because `%x` here
  IS month/day/year, and a facet that could not name the order of its own
  output would be refusing to read what it writes; `get_date` stores
  `tm_mday`/`tm_mon`/`tm_year` and does NOT derive `tm_wday`/`tm_yday`
  (libstdc++ derives them), because [locale.time.get.virtuals] names three
  fields and `mktime()` already exists to compute the rest — a facet filling
  neighbours makes it impossible to tell what was read from what was guessed;
  month and weekday names are matched WITHOUT regard to case (libstdc++ refuses
  `"march"`); and a name match must END where the parse stops, so `"Marbles"`
  is March with `"bles"` left in the stream while `"Marc"` fails — the `'c'`
  continued `"March"`, so it was consumed, and a single-pass iterator cannot
  give it back. Both references agree on that last one, failure included.

  **The `"C"` locale's negative sign is `"-"`, and that was a decision.** The
  two reference implementations disagree here — measured, not assumed:
  libstdc++ answers `""` and prints `put_money(-123456)` as `123456`, libc++
  answers `"-"` and prints `-123456`. The C++ standard specifies nothing about
  the `"C"` locale for `moneypunct`; the C standard DOES pin `lconv`, and pins
  it to `""`. boxcxx answers `"-"` in the facet and leaves `localeconv()` at
  `""`: two standards asking two different questions, one of which left its
  answer open. An amount whose sign disappears between `put_money` and the
  screen is not something this library ships, and under `""` `get_money` could
  not read a negative amount back at all — the round trip is a test now
  (phase225 (36)), not a hope.

  `put_money` on an infinity or a NaN sets `failbit` and writes nothing.
  Measured, because the first draft of this paragraph guessed and was wrong:
  libstdc++ writes an EMPTY string and leaves the stream good, libc++ writes
  `0` (and `-0` for negative infinity) and leaves the stream good. Both hand
  back a number that is not the one they were given, with no flag raised —
  which is the quiet kind of wrong this library exists not to ship. There is no
  amount of money that is infinity, and [ostream.formatted.reqmts]' `failbit`
  is the signal for a value that could not be generated. This one is boxcxx's
  own and is pinned by phase225 (80).

  Three smaller places where the two references differ and boxcxx had to
  choose, all recorded because a reader will otherwise assume the other one:
  an amount with fewer digits than `frac_digits()` prints `0.05`, not `.05`;
  `money_base::space` generates a real space, with any `internal` fill placed
  BESIDE it rather than instead of it (a fill character that is not a space
  would leave the required space missing); and `money_get` REQUIRES the
  fractional digits when the facet declares them, because the alternative
  silently reads `1234` as `12.34`.

  **Grouping exists as of Ф43-c, and had never been written before it.** The
  `"C"` locale groups nothing — its `grouping()` is empty — so there had been
  nothing to ask; a program can install a `numpunct` now and get separators,
  a different decimal point, and its own spellings of `true` and `false`. The
  grouping string is read RIGHT to left with its last element repeating, and a
  `0` or `CHAR_MAX` element stops grouping from there on. Separators count
  toward the field width, so padding is computed after grouping.

  **`ctype` answers two different questions under one name, and that is
  deliberate.** `ctype<char>` classifies BYTES the way `"C"` does, from a
  256-entry table derived at compile time from the rules `<cctype>` states.
  `ctype<wchar_t>` classifies CODE POINTS the way Unicode 17 does, from the
  tables Ф42-a generated. So byte `0xA0` is not whitespace (it is half of
  U+00A0) while the character U+00A0 is, and `toupper(L'ж')` is `L'Ж'` while
  `toupper('\xD0')` is itself. `digit` and `xdigit` stay ASCII in both, for
  the reason §2 `<cwctype>` gives.

  `ctype<char>`'s destructor honours the `del` flag of its constructor, which
  makes it the one facet here that owns something other than itself.

  **`collate` and `messages` answer with what looks like nothing, and is not.**
  Collation order in `"C"` IS code-unit order — [locale.collate] says the
  classic facet compares as if by `char_traits::compare` — so `do_transform`
  is the identity and a facet that reordered anything would be claiming a
  language this system does not have. `messages` answers `-1` to `open` and
  returns the caller's default from `get`, which is what [locale.messages.virtuals]
  defines for a catalog that could not be opened; there are no catalogs here.
  Both are the real answers rather than placeholders, and a program that has
  its own ordering or its own translations installs a facet and gets them.
- `–` **Of the `_byname` family, only `codecvt_byname` exists**, because it is
  the only one whose base facet exists. It validates the name and is otherwise
  its base, and it shares that base's `locale::id` — so installing one is found
  by `use_facet<codecvt<wchar_t, char, mbstate_t>>`, exactly as
  [locale.facet]/4 describes. A `codecvt_byname("ru_RU.KOI8-R")` throws rather
  than quietly producing UTF-8.
- `–` The two Annex D `codecvt` specializations deprecated in C++20 —
  `codecvt<char16_t, char, mbstate_t>` and `codecvt<char32_t, char, mbstate_t>` —
  are not provided. C++26 is already removing the neighbouring deprecated Unicode
  conversion machinery (P2871R3); building onto a surface the standard is walking
  away from is taking on a debt at the moment of writing. The **four that Table
  104 of [locale.category] requires are all here**: `codecvt<char, char>`,
  `codecvt<wchar_t, char>`, `codecvt<char16_t, char8_t>` and
  `codecvt<char32_t, char8_t>`, all over `mbstate_t`.
- `?` **`mbstate_t` is named in every `codecvt` signature and read in none.**
  That is a property of UTF-8, not a shortcut: the encoding has no shift state,
  and the protocol lets a converter say so by stopping AT an unfinished character
  instead of swallowing its bytes, so the caller offers the same bytes again with
  more behind them. libc++'s own `char8_t` facets answer identically (`partial`,
  `from_next` unmoved).
- `?` **`do_in` judges the source before it measures the destination.** When the
  destination is already full AND the next bytes are not a character, both
  `partial` and `error` are true and [locale.codecvt.virtuals] orders neither.
  boxcxx answers `error`: it is true whatever the destination holds, it leaves
  `from_next` in exactly the same place, and it does not ask the caller for room
  that cannot help. This is the **only** place the engine and libc++ disagree —
  36 lines out of 1 114 956 in the differential sweep, every one of them this
  shape.
- **Where the reference implementation is wrong.** libc++'s
  `codecvt<wchar_t, char, mbstate_t>` delegates to the platform's
  `mbsnrtowcs`/`wcsnrtombs`, and three of its answers are measurably wrong — each
  one contradicted by libc++'s **own** `char8_t` facets, which do not delegate:
  it returns `partial` for a byte that can never begin a character (`0xFF`, an
  overlong form, a surrogate — the platform's `mbrtowc` underneath correctly
  returns `-1`/`EILSEQ`, so the loss is libc++'s own); it returns `ok` from
  `unshift` where [locale.codecvt.virtuals] gives `noconv` the meaning "no
  termination sequence is needed for this state_type"; and it **loses an embedded
  NUL entirely** — `out(L"\0A")` into an eight-byte buffer produces zero bytes and
  reports `partial`, because `wcsnrtombs` reads the NUL as a terminator. The
  correct answer is `00 41` and `ok`, which is what its own
  `codecvt<char32_t, char8_t>` gives for the same input and what boxcxx gives.
- **How it is verified.** `<__bits/codecvt_engine>` is a leaf precisely so a host
  build can include the shipped file rather than a copy: one source, two builds —
  against the tree and against libc++'s `char8_t` facets — produced 1 114 956
  canonical lines each and agreed on all but the 36 above. That covers every code
  point in both directions at every destination size from 0 to 8, plus a
  23-string corpus of truncated, overlong, surrogate and out-of-range input.
  Twelve mutations of the engine were tried; ten changed the stream. Of the two
  that did not, one was proven equivalent by pairing (removing the lone-low-
  surrogate check changes nothing while `__utf8::Encode` still refuses
  surrogates — take that refusal away too and the pair diverges by 17 lines), and
  the other was found to have mutated a line that **did nothing**: an
  intermediate cast to `unsigned` that a modular conversion to `char32_t` already
  performed. That line is gone. What a host sweep cannot reach — this target's
  compiler, and the `wchar_t` instantiation that has no trustworthy oracle — is
  `Phase216`, which sweeps all 1 114 112 code points through the wide, the
  `char32_t` and the `char16_t` facets in 2.2 s of CPU and requires them to
  agree: 2 048 surrogates refused, 1 112 064 round-tripped.
- The named constructor still accepts any name and ignores it; `name()` always
  returns `"C"`.

## `<map>` / `<set>` / `<unordered_map>`

- `✓` Closed in Ф31e-f: **`map`, `multimap`, `set` and `multiset` had no
  relational operators at all.** [associative.map.syn] declares `==` and `<=>`
  and nothing else, because `<`, `>`, `<=` and `>=` are *synthesized* from
  `<=>` — so one missing operator took all four with it, and `m1 < m2` did not
  compile on a `map` while it did on a `vector`. The document had recorded only
  the missing `<=>`, not the four it implies. Comparison is lexicographic
  through *synth-three-way*, so a key with only `operator<` still yields
  `weak_ordering`. This closed `__cpp_lib_three_way_comparison`.
- `✓` Closed in Ф31e-f: `map::try_emplace` and `map::insert_or_assign` had two
  of their four overloads each — both `const_iterator` hint forms were absent,
  so a caller holding a position had nowhere to put it. The hint is advisory
  here ([associative.reqmts]): the tree finds the position itself, and a
  deliberately wrong hint still lands the element correctly. Closed
  `__cpp_lib_map_try_emplace`.
- `✓` Closed in Ф31e-f, and this one answered wrongly in silence:
  **`unordered_map::insert_or_assign` had ONE of its four overloads.** With only
  the `const key_type&` form, `m.insert_or_assign(std::move(k), v)` compiled,
  bound the rvalue to the const reference, and **copied the key** — which is the
  entire reason the rvalue overload exists. `try_emplace` was missing its two
  hint forms as well. Closed `__cpp_lib_unordered_map_try_emplace`.
- `✓` Closed in Ф32-a, and this one also answered in silence: **the four
  `unordered_*` containers had none of LWG 2713's three bucket-less
  allocator constructors** — `(first, last, alloc)`, `(init-list, alloc)`,
  `(from_range, r, alloc)` — which the four ordered containers have had all
  along. Two of the three simply failed to compile. The init-list one did not:
  with no such constructor, the braced list implicitly built a *whole temporary
  container* through `(init-list, buckets = 0, ...)`, which **default-constructs
  the allocator**, and that temporary then bound to `(const unordered_map&,
  const Alloc&)`. An extra full construction plus a copy, the first one taken
  from the wrong allocator — for a `pmr` container, from the default resource.
  Pinned with an allocator that has no default constructor, which makes the old
  path impossible to form.
- `✓` Closed in Ф32-a: **no associative container had a single
  `initializer_list` deduction guide**, and for the four `pair`-keyed ones that
  was not a redundancy. `value_type` is `pair<const Key, T>`, so no implicit
  guide can deduce `Key` and `T` from a braced list of `pair<int, int>`, and
  `std::map m{std::pair{1, 2}}` was a hard error — while `std::set s{1, 2, 3}`
  always worked, because `set`'s `value_type` *is* `Key`. `set` was not
  unaffected either: `std::set s({1, 2, 3}, alloc)` was **ambiguous**, one
  implicit guide deducing the allocator as the comparator. The standard's
  `NotAllocatorLike` split on the comparator slot is exactly what breaks that
  tie.
- `✓` Closed in Ф32-a: the bucket-less allocator *guides* of
  [unord.map.syn] / [unord.set.syn] were missing too — the standard has listed
  them all along, for constructors that did not exist here. A guide with no
  constructor behind it is two gaps, not one; the same shape as `subrange`'s
  range guides in Ф31e-e.
- `~` The move constructors of `map`, `set` and the `unordered_*` family are
  hard-coded `noexcept` even when the comparator or hasher has a throwing move,
  so such a move terminates instead of propagating. Both reference libraries
  condition it. Re-measured in Ф31e-d: the move is genuinely *viable* and
  genuinely `noexcept` — the `noexcept` is swallowed one level down in the tree
  and hash-table engines, so [dcl.fct.def.default]/3 never deletes it.
- `~` The iterator-pair constructors carry no input-iterator SFINAE guard.
  Diagnostics quality only: a wrong call fails inside the body rather than at
  the call site.
- `+` P2363R5 heterogeneous insertion is implemented across all four
  transparent-comparator containers: `operator[]`, `at`, `try_emplace` and
  `insert_or_assign` (both hint forms of each) on `map` and `unordered_map`,
  and `insert` (both forms) on `set` and `unordered_set`. The measurable
  property is that a hit constructs **nothing** — the suite counts key
  constructions to prove it, rather than asserting about it. `bucket(const K&)`
  on the four unordered containers already existed.
- `✓` Closed in Ф33, found while measuring for the above and none of it P2363's:
  **`unordered_set` had no `cbegin()` / `cend()` at all** — the name resolved to
  the bucket-local `cbegin(size_type)`, so `c.cbegin()` failed with "too few
  arguments" — and **no hint-insert overloads at all**, the only container in
  the library missing them. Separately, `set`, `multiset` and
  `unordered_multiset` had no `insert(const_iterator, value_type&&)`: an rvalue
  bound to the `const&` overload and was silently **copied** where
  [set.overview] and its siblings call for a move.

## `<mdspan>`

- `✓` Added in Ф35, whole: `extents` / `dextents` / `dims`, all five layout
  mapping policies (`layout_left`, `layout_right`, `layout_stride`,
  `layout_left_padded`, `layout_right_padded`), both accessors
  (`default_accessor`, `aligned_accessor`), `mdspan` with its eight deduction
  guides, and the complete `submdspan` family — `extent_slice`, `range_slice`,
  `full_extent`, `submdspan_mapping_result`, `canonical_slices`, `subextents`,
  `submdspan_mapping` for each of the five layouts, and `submdspan` itself.
  Three macros: `__cpp_lib_mdspan` 202406L (P0009R18 plus P2389R2's `dims`),
  `__cpp_lib_aligned_accessor` 202411L (P2897R7, whose other half
  `is_sufficiently_aligned` had been in `<memory>` since Ф32-e), and
  `__cpp_lib_submdspan` 202603L — claimed only because all five of the papers
  behind that value are here: P2630R4, P2642R6 (the padded layouts), P3355R1,
  P3663R3 and P3982R1.
- **The result is genuinely free.** `extents` stores only its dynamic extents;
  a fully static one is an EMPTY type, `mdspan<int, extents<int, 4, 6>>` is
  eight bytes — a pointer, nothing more — and
  `layout_right_padded<8>::mapping<extents<int, 5, 6>>` is two. The
  `[[no_unique_address]]` on the extents member is what makes that true and is
  marked as load-bearing at the definition; without it `array<index_type, 0>`
  still costs a byte, that byte stops `extents` from being empty, and the
  mdspan above doubles to sixteen. Measured against libstdc++ 16.1 for three
  representative specializations: identical footprints.
  (Two `[[no_unique_address]]` members do NOT collapse into one another when
  they contain a subobject of the same type — both padded-mapping members hold
  an `array<index_type, 0>` — which is why that mapping is two bytes and not
  one. libstdc++ lands on two for the same reason.)
- **The layout staircase is implemented, not approximated.** `submdspan` gives
  back the strongest layout the slicing allows — trimming whole rows off a
  row-major array yields `layout_right` again, trimming columns yields
  `layout_right_padded` carrying the ORIGINAL row pitch, and only a non-unit
  step falls back to `layout_stride`. Always answering `layout_stride` would
  pass every value test and still cost a multiply per index; the suite pins the
  chosen layout TYPE for ten representative slicings (phase185), and all of
  them were cross-checked against libstdc++ 16.1, which answers identically for
  each — same layout, same extents, same strides.
- **Compile-time slice bounds survive into the type.** A `range_slice` whose
  bounds are `constant_wrapper`s (Ф33) subtracts into a static span, and the
  result's extent is static: `submdspan(m, range_slice{cw<1zu>, cw<4zu>},
  full_extent)` has `static_extent(0) == 3`.
- `~` `std::copy` and `std::fill` over mdspans ([mdspan.copy]) are provided, but
  **`__cpp_lib_mdspan_copy` is not claimed**: the macro also names the
  `ExecutionPolicy` overloads, and there is no `<execution>` — the same reason
  `__cpp_lib_parallel_algorithm` is absent. The traversal order is unspecified
  by the standard; boxcxx walks in layout_right order, which keeps the writes
  to the destination contiguous.
- `~` `__cpp_lib_hardened_mdspan` is **not** claimed. `operator[]` does not
  check its indices; `at()` does, and throws `out_of_range`. That is the whole
  difference, and `at()` is the reason this header is not entirely
  freestanding.
- **A defect in libstdc++ 16.1, found by measuring:** it declares
  `layout_left_padded` and `layout_right_padded` **without the default template
  argument** `= dynamic_extent` that [mdspan.syn] specifies, so
  `std::layout_right_padded<>` — the spelling for a run-time pitch, which is
  the common case for a framebuffer — does not compile there. It does here.

## `<memory>`

- `✓` Closed in Ф43-f: **the whole of [ptrtag] was missing**, and with it
  `start_lifetime`. Both are freestanding entities of [memory.syn], so
  `__cpp_lib_freestanding_memory` could not be claimed without them.
  **No implementation ships [ptrtag]** — measured on libstdc++ 16.1 and libc++
  19.1.2, neither has `pointer_tag_pair`, `pointer_bits_available` or
  `max_pointer_bits_available` — so boxcxx is, as of Ф43-f, the only one of the
  three with C++26 pointer tagging. It lives in `<__bits/pointer_tag>`.
- `~` **`max_pointer_bits_available` is 12**, which is implementation-defined and
  is the only number in the facility boxcxx had to choose. On x86-64 every bit
  below an object's alignment is genuinely free and BoxOS puts nothing in them,
  so the limit is not a property of the pointer — it is the coarsest alignment
  the system can promise, which is one PAGE. A larger number would have been a
  promise about `alignas` on somebody's static object, which is not the
  system's to make; `from_overaligned` is how a caller who knows better says so.
- `+` **A `pointer_tag_pair` cannot be built during constant evaluation.** Tagging
  is arithmetic on a pointer's bits and the language has no constant-evaluation
  form of that — `reinterpret_cast` is not a constant expression and there is no
  other way to reach the low bits. The members are still declared `constexpr`
  exactly as the synopsis writes them, so the declaration matches and the
  failure is a compile error at the point of use rather than a silent
  difference; the default constructor and `tag()` really do work in a constant
  expression. Nothing to compare against: no other implementation has the type.

- `✓` Added in Ф38: **the whole of [allocator.uses.construction]** —
  `uses_allocator_construction_args` in all nine of its overloads,
  `make_obj_using_allocator` and `uninitialized_construct_using_allocator,`
  living in `<__bits/uses_allocator>` so `<memory_resource>`,
  `<__bits/flat_engine>` and `<scoped_allocator>` can all reach the one copy.
  `__cpp_lib_make_obj_using_allocator` is 201811L. **P0591R4 is from C++20;
  none of it was here.** The tree instead carried two partial hand-written
  copies of the rule (see `<memory_resource>` below and
  `<__bits/flat_engine>`), and the macro was not merely undefined — it was
  absent from `<version>`'s list of deliberately-absent macros AND from
  `tools/version_syn_owners.txt`, so no gate was looking for it. That third
  omission is why the first two could sit there unnoticed; the audit now
  covers it.
- **boxcxx implements two overloads libstdc++ 16.1 does not**, both measured
  rather than inferred: the *pair-like* form (P2165R4, C++23 —
  `uses_allocator_construction_args<pair<K,V>>(a, some_tuple_of_two)`) and the
  single-argument *pair-constructor* form (C++20). A probe over all nine
  shapes answers 7/9 against libstdc++ and 9/9 against libc++. On the
  pair-like path the two references disagree with the standard in opposite
  directions: libstdc++ does not compile the call, and libc++ compiles it and
  delivers the allocator to *neither* member, where the Effects clause
  delegates to the piecewise form and so reaches both. Phase194 pins reaching
  both.
- `✓` Added in Ф35: **`indirect` and `polymorphic`** ([mem.composite.types],
  P3019R11), with `hash<indirect<T, A>>` and the `pmr::indirect` /
  `pmr::polymorphic` aliases [memory.syn] pairs with them.
  `__cpp_lib_indirect` and `__cpp_lib_polymorphic` both carry 202502L. Both
  work in constant evaluation on this toolchain — including `polymorphic`'s
  clone, which dispatches through a vtable at compile time.
- `~` One documented deviation in those two, and it is in the safe direction:
  `indirect`'s `operator==` and its comparison against a bare `U` state the
  "the comparison is well-formed" half of their contract as a **Constraint**
  where [indirect.relops] states it as a **Mandates**. Unconstrained, they make
  `equality_comparable<indirect<NoEq>>` answer `true` and then fail inside the
  operator — the exact defect Ф34 removed from `pair`, `tuple`, `variant` and
  `expected`. libstdc++ constrains them for the same reason.

- `✓` Closed in Ф34, and it was a `constexpr` that had never been true: **all ten
  `uninitialized_*` algorithms were marked `constexpr` and none of them worked in
  a constant expression.** Each built its element with a bare placement-new,
  which is a constant expression only inside `std::construct_at`, so the first
  element of the first constant-evaluated call failed. They now construct through
  `construct_at`, and the suite calls every one of them from a `static_assert`
  (phase173). `__cpp_lib_raw_memory_algorithms` carries 202411L as a result.
- `✓` Closed in Ф34, one level down and the same defect: **`ranges::construct_at`
  was `constexpr` and unusable in a constant expression.** GCC accepts a
  placement-new in constant evaluation only inside a function it recognises by
  name as `std::construct_at`; `ranges::construct_at` is a call operator on a
  type in `std::ranges::__uninit`, which it does not. It now delegates, which is
  what makes the whole `ranges::uninitialized_*` family constexpr too.
- `~` One deviation follows from that, and it is the only one: in constant
  evaluation `uninitialized_default_construct[_n]` VALUE-initializes a
  non-trivially-default-constructible element where the standard says
  default-initialize. Default-initialization of a trivial member cannot be
  expressed in a constant expression without P2747R2's constexpr placement new,
  and GCC 15.2 reports `__cpp_constexpr 202211L`. No conforming program can
  observe the difference: reading an indeterminate value during constant
  evaluation is itself ill-formed, so the only programs that could tell the two
  apart are ones a conforming implementation must reject anyway. The runtime path
  is unchanged and still default-initializes.
- `✓` Closed in Ф34: **`shared_ptr<void>` did not compile at all** — `operator*`
  was declared `T&` and `operator[]` `element_type&`, and a requires-clause does
  not stop a member *declaration* from being instantiated with the class, so the
  most common type-erased owner in the language was a hard "forming reference to
  void". Both now go through `add_lvalue_reference_t`, which is `void` for
  `void`: a perfectly good return type for a member no program can call.
- `✓` Added in Ф34: `owner_hash` / `owner_equal`, and the `owner_hash()` /
  `owner_equal()` members behind them (P1901R2). `owner_less` made a shared_ptr
  usable as a `map` key; only these make it usable as an `unordered_map` key, and
  a `weak_ptr` can now be a lookup key without being locked first — locking it to
  look it up would resurrect the object the cache is deciding about.
- `✓` Closed in Ф31e-b-1: `owner_less<void>` had no `is_transparent` member, so
  its four heterogeneous call operators were unreachable through an associative
  container — `map<shared_ptr<T>, …, owner_less<>>::find(weak_ptr)` did not
  compile, which is the pairing P0074R0 exists for. Closed
  `__cpp_lib_transparent_operators`.
- `✓` Closed in Ф31e-d, and it was the sharpest debt left in the library:
  **`atomic<shared_ptr<T>>` and `atomic<weak_ptr<T>>` had no working `notify_one`
  / `notify_all` — both were empty bodies — and `wait()` was a bare
  `while (equivalent) ;`.** A strand waiting on one burned its core until the
  value changed, and a notification could not shorten that, in a library where
  every other atomic parks in the kernel. They now carry one control word — a
  lock bit, a parked bit and a mutation counter — so `wait()` snapshots the word
  before it re-reads the value and parks on the same substrate as `atomic<T>`,
  the two notifies bump the version pool and wake, and the strands contending for
  the pointer park rather than spinning a descheduled holder's quantum away. The
  counter is what makes the park safe: a store landing between a waiter's read
  and its park changes the word the park pre-checks, so the wakeup is declined
  rather than lost.
- `✓` Closed in Ф31e-d: the **single-`memory_order` `compare_exchange_weak` /
  `_strong`** were unusable on both, for opposite reasons. `atomic<shared_ptr>`
  declared them beside two-order forms whose second order was *also* defaulted,
  so every three-argument call was ambiguous; `atomic<weak_ptr>` never declared
  them, so the same call silently landed on the two-order overload with `seq_cst`
  as the failure order. `value_type` was missing from both. [util.smartptr.atomic
  .shared] defaults the single-order form and *only* that one, which is what
  makes the three-argument call unambiguous.
- `✓` Closed in Ф31e-d: Annex D's free `shared_ptr` atomic functions
  (`atomic_load`, `atomic_store`, `atomic_exchange`, the four
  `atomic_compare_exchange_*` and their `_explicit` forms, plus
  `atomic_is_lock_free`) were absent entirely. They are `[depr.util.smartptr
  .shared.atomic]` — deprecated in C++20, **removed in C++26** — but normative in
  the revision this library targets, so they are here, taking their exclusion
  from an address-hashed pool because they act on a `shared_ptr` the program owns
  rather than on an `atomic<>`. They are atomic only with respect to each other,
  never with respect to `atomic<shared_ptr>`. The C++26 phase deletes them.

- `✓` Closed in Ф31e-g-2, and it is the one in this section that was giving wrong
  answers rather than no answer: **`allocate_shared` never asked the allocator to
  construct anything.** [util.smartptr.shared.create]/7-8 draws a deliberate line
  — `make_shared` initialises via `::new (pv) U(…)`, `allocate_shared` via
  `allocator_traits<A2>::construct(a2, pv, …)`, and destroys the matching way —
  and boxcxx placement-new'd on both sides. For `std::allocator` that is
  indistinguishable, which is why it survived; for `polymorphic_allocator` it is
  the whole feature. `allocate_shared<pmr::vector<int>>(polymorphic_allocator{r},
  …)` handed back a vector that had quietly kept the **default** resource,
  because uses-allocator construction lives in the allocator's `construct` and
  nothing called it. Proved by a type whose constructor only the allocator can
  reach: it did not compile before, and does now.
- `✓` Closed in Ф31e-g-2: **`make_shared<const T>` did not compile.** The control
  block stored the object as `T`, so a `const` element type made
  `static_cast<void*>` of its address ill-formed — and `shared_ptr<const T>` from
  `make_shared` is ordinary code. Storage is `remove_cv_t<T>` now, in the object
  and the array block alike.
- `✓` Closed in Ф31e-g-2: `make_shared_for_overwrite<T>()` was literally
  `return make_shared<T>();` — it **value-initialised**, which is the one thing
  the factory exists not to do. Default-initialisation via `::new (pv) U` now,
  per /7's own for_overwrite bullet. Not observable from a runtime check without
  reading an indeterminate value, so the suite does not pretend to test it; it is
  a code-reading finding, and the allocator-routing counters next to it are what
  pin the `allocate_` side.
- `✓` Closed in Ф31e-g-2: the four missing array `allocate_shared` overloads
  (bounded, with and without a fill value) and all three
  `allocate_shared_for_overwrite` forms, closing `__cpp_lib_shared_ptr_arrays`
  and `__cpp_lib_smart_ptr_for_overwrite`.
- `✓` Closed in Ф31e-g-2: `allocation_result`, `allocator::allocate_at_least` and
  `allocator_traits::allocate_at_least` (`__cpp_lib_allocate_at_least`);
  `assume_aligned` (`__cpp_lib_assume_aligned`); `start_lifetime_as` and
  `start_lifetime_as_array` in all four cv forms (`__cpp_lib_start_lifetime_as`);
  and a constexpr converting constructor for `default_delete`, the single hole
  that had kept `__cpp_lib_constexpr_memory` undefined. There is no free
  `std::allocate_at_least` — C++23's [memory.syn] declares none, whatever
  `<version>`'s old note said.
- `✓` Closed in Ф31e-g-2 and in no tracker at all: **`std::align` did not
  exist.** [ptr.align] is C++11, it is the standard way to carve an aligned
  sub-buffer out of raw storage, and on a bare-metal target that is not a corner
  case. Its two guards are subtractions rather than `pad + size > space`, because
  that sum wraps on a buffer near the top of the address space.
- `–` The default `allocator<T>::allocate_at_least` returns exactly `n`.
  boxlib's `malloc_impl` does not report the bucket size it rounded up to, so
  there is no larger number to honestly return. [allocator.members] asks only for
  `count >= n`; libstdc++ and libc++ answer the same way.

- The `ranges::` half of [specialized.algorithms] — fourteen names that this
  header's `std::` half has had since Ф7 — arrived in Ф31e-e and is recorded
  under `<ranges>`, with what it does that the `std::` forms cannot.

## `<memory_resource>`

- `!` `✓` Closed in Ф38: **`polymorphic_allocator::construct` had a branch
  that silently dropped the allocator.** [mem.poly.allocator.mem]/1 is one
  member specified through `uses_allocator_construction_args`; boxcxx had six
  hand-written overloads instead, and the plain one ended in
  `else construct_at(p, args...);  // best effort`. That branch is reached
  when `uses_allocator_v<T, polymorphic_allocator>` is **true** and the type
  takes the allocator in neither position — a case [allocator.uses.construction]
  makes ill-formed. What it produced was a pmr element that did not share its
  container's resource, with no diagnostic: the same class of defect Ф31e-g-2
  found in `allocate_shared`, reached from a different direction. Proven by
  mutation rather than by reading: a type with `allocator_type` and only a
  `T(int)` constructor now fails to compile with one error naming the rule,
  and compiles silently with the old branch restored.
  The six overloads are now one, which also gained the two C++23 pair forms
  they never had (`pair<U,V>&` and `const pair<U,V>&&`) and the pair-like and
  single-argument forms.
- `?` `unsynchronized_pool_resource::options()` and `synchronized_pool_resource::options()`
  return the **effective** options, not the ones handed to the constructor.
  `largest_required_pool_block` is not a request this engine can honour — its
  size-class table stops at 2048 bytes — so echoing the caller's number back would
  describe pooling that does not happen. [mem.res.pool.mem] permits both.

## `<mutex>`

- `✓` Closed in Ф31e-d: `scoped_lock` had no `mutex_type`. [thread.lock.scoped]
  gives it one "only if `sizeof...(MutexTypes) == 1`", and boxcxx covered the
  one-mutex case inside the variadic primary with `if constexpr` — which locks
  correctly but cannot carry a conditional typedef. So generic code spelling
  `typename Lock::mutex_type`, the only reason the member is specified, compiled
  against `lock_guard` and `unique_lock` and not against `scoped_lock`. Supplied
  now from a conditional base rather than a second class body, which is what the
  standard's wording describes. Closed `__cpp_lib_scoped_lock`.

## `<optional>`

- `+` Since Ф33 the whole of `optional` is usable in constant evaluation.
  It was not: the one place that engages a disengaged optional went through a
  bare placement-new, and a placement-new expression is a constant expression
  only inside `std::construct_at` — so `emplace`, both converting
  constructors, all four assignments and the non-trivial copy and move
  constructors were run-time-only, which [optional] does not permit.
- `~` `hash<optional<T&>>` exists as a type but has no usable `operator()`,
  because `hash<remove_const_t<T&>>` is the undefined primary. libstdc++
  disables the specialization outright. Nothing can be hashed either way; the
  difference is only whether `is_default_constructible_v<hash<optional<int&>>>`
  answers true or false. This follows boxcxx's epic-wide pattern of poisoning
  the call operator rather than the class.

## `<ostream>`

- `✓` Closed in Ф43-c: **every arithmetic inserter rendered the number
  itself.** [ostream.inserters.arithmetic] defines each one as
  `use_facet<num_put<charT, ostreambuf_iterator<charT, traits>>>(getloc()).put(...)`,
  and until Ф43-c the rendering lived in `<ostream>` — which was defensible
  while no `num_put` existed to install, and became a silent lie the moment
  Ф43-a made a locale a container. The engine was not rewritten; it MOVED,
  into `num_put::do_put`, with the width/fill/adjustfield part factored into
  `<__bits/field_engine>` because the character and string inserters still
  need it and are *not* the locale's business ([ostream.inserters.character]
  never mentions a facet).

  Three things that had to survive the move, and each of which broke first:
  a facet has no stream to set a bit on, so a conversion that cannot fit its
  buffer signals through a private exception type that the inserter turns back
  into `failbit`; [ostream.inserters.arithmetic]/2's conversions are not a
  plain widening, since a signed type narrower than `long` goes through the
  UNSIGNED type of its width for `oct` and `hex` (so `hex << short(-1)` is
  `"ffff"`, not a 64-bit sign-extension); and the digits are widened THROUGH
  the ctype facet, which the first routing skipped whenever the stream's
  character type already matched the ASCII source — leaving a narrow stream
  consulting no `ctype` at all.

  The comment that used to sit on the widening path said it was "a cast and
  not a facet call ... exactly what a ctype facet's widen() would return,
  without a facet to consult". That was true, and it stopped being true the
  day a facet could be installed.

- `✓` Closed in Ф40, found by building `<syncstream>`: **`emit_on_flush`,
  `noemit_on_flush` and `flush_emit` had been no-ops.** They were honest about
  it — the comment above them said there was no buffer type in the tree to
  detect — but a manipulator that compiles and does nothing is the kind of
  thing generic code takes for a working one. All three now find a syncbuf
  through the allocator-blind base `<__bits/syncbuf_base>` gives it, using the
  RTTI the library already carries, and `flush_emit` does the whole of its
  clause: `os.flush()` first, then a sentry, then `emit()`, then `badbit` if
  that was refused. On a stream whose buffer is not a syncbuf all three stay
  exactly as inert as before, which Phase199 pins as well.
- `✓` Closed in Ф38, found by building `<spanstream>`: **a formatted inserter
  that could not write everything reported success anyway.**
  [ostream.formatted.reqmts] says a generation failure calls
  `setstate(badbit)`; the unformatted path (`basic_ostream::write`) had always
  checked `sputn`'s return, but every *formatted* inserter — integer, float,
  bool, pointer, char, string, and the fill a `width()` asks for — funnels
  through one shared width/fill engine, and that engine called `sputn` and
  `sputc` and looked at neither. The reason nobody had noticed is the reason it
  was worth finding: **no sink in this tree could refuse a write.** A
  `stringbuf` grows, a `filebuf` grows (Ф36), the screen Current always
  accepts. `basic_spanbuf` is the first that cannot — its buffer belongs to the
  caller and is a fixed size — so `ospanstream os(span); os << "too long";`
  dropped the tail and left `os.good()` true. The two write helpers now report
  refusal, the chain stops at the first one (there is no point filling a buffer
  that has said it is full), and the engine sets `badbit`. Pinned by three
  Phase193 checks — the string form, the number form and the padding — because
  they are three different call sites into the same engine; reverting the fix
  fails exactly those three and nothing else.
- `✓` Closed in Ф31e-a: the `wchar_t` / `char8_t` / `char16_t` / `char32_t`
  inserters are now deleted per [ostream.inserters.character], in both the
  character and the pointer form. Until then they were merely *absent*, which is
  not the same thing — the call bound to something else and compiled:
  `os << u8"hi"` and `os << L"hi"` reached `operator<<(const void*)` and printed
  the **pointer address**, `os << char8_t('x')` promoted to `operator<<(int)` and
  printed the **number**. Verified in generated assembly at the time
  (`_ZNSolsEPKv`, `_ZNSolsEi`); the deletions are pinned by cxxtest phase144.
- `✓` Closed in Ф42-e: **the synopsis's six `basic_ostream<wchar_t, traits>`
  deletions were missing, and the reason recorded for it was false twice over.**
  It read: "wide streams are a permanent exclusion and there is no
  `char_traits<wchar_t>`, so `basic_ostream<wchar_t, …>` can never be formed".
  `char_traits<wchar_t>` has existed all along in `<__bits/char_traits>`, and
  Ф42-d made a wide `basic_ostringstream` a working stream — so those overloads
  *were* candidates, and the silent wrong output the narrow deletions exist to
  stop was happening unwatched on the wide side: `wos << u8'x'` promoted to
  `int` and wrote **120**. All six are declared and deleted now, pinned by
  `Phase216` (29)–(38), controls included. This entry is the case for deriving a
  claim rather than carrying it: nothing about the code changed on the day the
  sentence became untrue, so nothing prompted anyone to reread it.
- `?` The extraction side is safe, but only by accident: no `istream` extractor
  binds a reference across distinct fundamental types, so `is >> char8_t_lvalue`
  fails to compile even though nothing deletes it either. The standard does not
  delete extractors, so there is nothing to add. Re-measured in Ф31e-d against
  libc++ 22, whose `<istream>` names `char8_t` nowhere while its `<ostream>` does
  — `<version>`'s own note had claimed [istream] required deletions, and it was
  corrected there. What keeps `__cpp_lib_char8_t` undefined is **not** anything
  in this header, and Ф31e-g-2 corrected the claim that it was `pmr::u8string`:
  [version.syn] names `<locale>` among the macro's owning headers because
  P0482R6 adds `codecvt<char16_t, char8_t, mbstate_t>`,
  `codecvt<char32_t, char8_t, mbstate_t>` and their `_byname` forms. **Both
  codecvt specializations exist as of Ф42-e**, so the reason this entry gave
  until then — "`<locale>` has no facets at all, and that exclusion is
  permanent" — is dead. Ф43-a closed the second of the two gaps that were left:
  `codecvt_byname` is in the tree, because a locale can be named now and a name
  that is not this locale's is refused. **`<filesystem>` is the only thing
  still blocking the macro**, and it is an absent header (§1.1).

## `<print>`

- `✓` Closed in Ф31e-g-3 (in `<ostream>`, which co-owns the macro): P2539R4's
  four `ostream`-taking overloads — `print`, `println`, `vprint_unicode`,
  `vprint_nonunicode` — did not exist, and that alone was what kept
  `__cpp_lib_print` undefined while the console forms had been complete since
  Ф9B-2. The exception rule is the part with teeth: [ostream.formatted.print]/6
  says `vformat`'s exception propagates **untouched** — no `badbit`, regardless of
  `os.exceptions()` — while a failure of the *insertion* sets `badbit`, so the
  formatting happens after the sentry and outside the guarded region.
- `–` There is no "terminal capable of displaying Unicode" branch: no stream type
  in this library can be one, so `vprint_unicode` and `vprint_nonunicode`
  necessarily coincide — the same coincidence the console forms document, reached
  from the other direction. `os.getloc()` is not passed to `vformat` either:
  `std::locale` can only ever be `"C"` here (§1.2), so the locale-taking overload
  would have exactly one possible answer.
- `~` `println()` with no arguments is provided and is **not** C++23 — it is
  P3142R0 (C++26), pre-existing. Its `ostream` counterpart, `println(ostream&)`,
  is deliberately not added: this library pins C++23.

- `–` There are no `FILE*` overloads of `print`, `println`, `vprint_unicode` or
  `vprint_nonunicode`. BoxOS has no `FILE` type anywhere for them to be declared
  against; the console forms simply omit the stream argument.
- `~` `vprint_unicode` forwards directly to `vprint_nonunicode` — there is no
  separate well-formed-UTF-8 transcoding step. The console replaces multi-byte
  UTF-8 with `?` (a kernel font limitation shared with `printf`), so non-ASCII
  output is lossy at the device, not in the formatter.
- `~` `println()` with no arguments exists. That is a C++26 addition, taken early
  and harmless.

## `<random>`

- `–` No `operator<<` / `operator>>` on engines or distributions (measured: the
  header contains neither operator at all). This entry used to say the reason
  was that [rand.req.eng]'s text serialization is specified in terms of
  "streams that BoxOS does not have". **That reason went stale in Ф30e**, when
  `<ostream>` and `<istream>` were built, and Ф36's sweep is what noticed —
  `<iostream>` exists now too. The honest reason is that nobody wrote the
  operators. Nothing blocks them.
- `?` The distributions are **not reproducible against another implementation**.
  The standard makes the engines deterministic and leaves the distributions
  implementation-defined; boxcxx picks its own algorithms (Marsaglia polar,
  Marsaglia–Tsang, Hörmann PTRS, BINV), so `normal_distribution` here and in
  libstdc++ agree in distribution, not in sequence. The engines *are* bit-exact.
- `~` `random_device::entropy()` reports 32.0 when RDRAND backs it and 0.0 when it
  falls back to a TSC-seeded splitmix. **Without RDRAND, `random_device` is not
  cryptographically strong**, and reports so.
- `~` The `Sseq` constructors and `seed` overloads take a concrete `seed_seq&`
  rather than [rand.req.eng]'s `template<class Sseq>`. boxcxx has exactly one
  seed-sequence type, and the concrete parameter is what keeps
  `engine(some_unsigned_lvalue)` from becoming ambiguous without the
  `is_convertible_v<Sseq, result_type>` exclusion the standard's form needs.
  A user-written seed sequence will not bind. Epic-wide across every engine,
  including `philox_engine`.
- `+` `philox_engine` (P2075R6) was verified differentially against libstdc++
  16.1 rather than by inspection: 300-output streams for `philox4x32` and
  `philox4x64` default-constructed and seeded, both `set_counter` and
  `seed_seq` paths, an all-ones counter wrap, and two off-menu instantiations
  (24-bit `n == 2` with 7 rounds, 40-bit `n == 4` with 3) — zero differences,
  plus both 10 000th values the standard states outright.

## `<ranges>`

- `✓` Closed in Ф31e-b-1: seven of the thirteen range-access CPOs were missing —
  `ranges::cbegin`, `cend`, `rbegin`, `rend`, `crbegin`, `crend`, `cdata`. They
  live in `<iterator>`, beside `make_const_iterator` and `make_reverse_iterator`,
  because `<__bits/ranges_core>` deliberately cannot include `<iterator>` (that
  direction is a real cycle). `constant_range` moved there with them. Note that
  `cbegin` *wraps*: per P2278R4 it yields a constant iterator even for a non-const
  range, which a plain `begin()` would not.
- `✓` Closed in Ф31e-b-2: `views::take_while` and `views::drop_while` did not
  exist — two core C++20 adaptors. `drop_while_view` caches its `begin()`, which
  [range.drop.while]/2 requires rather than suggests: without it `begin()` is O(n)
  on every call and the view stops meeting the amortized constant the range
  concept asks for.
- `✓` Closed in Ф31e-b-2: `ranges::is_permutation` did not exist, though the
  non-ranges `std::is_permutation` did.
- `✓` Closed in Ф31e-e: **the entire `ranges::` uninitialized-memory family was
  absent** — fourteen names, while the `std::` forms of all of them had been in
  `<memory>` since Ф7. They are not spelling variants of those: the range
  overloads return `borrowed_iterator_t`, so calling one on an expiring container
  yields `dangling` at compile time instead of an iterator into freed storage;
  `uninitialized_copy` and `uninitialized_move` take a sentinel for the
  **output** too, so a short destination is a bounded early return rather than a
  heap overrun, which the `std::` forms cannot express; and `uninitialized_move`
  moves through `ranges::iter_move`, so a proxy iterator's own ADL `iter_move`
  participates. They live in `<__bits/ranges_uninitialized>`, and `in_out_result`
  moved to `<__bits/ranges_core>` to get there without putting the whole of
  `<algorithm>` underneath every container that includes `<memory>`.
- `✓` Closed in Ф31e-e: `subrange` had **no range constructors** — only the two
  guides were recorded as missing, and a deduction guide with no constructor to
  deduce for is not a gap, it is two gaps. `subrange(v)`, the spelling every
  algorithm returning a `borrowed_subrange_t` hands back, did not compile. The
  iterator-pair constructors also took `I` exactly, where [range.subrange]
  requires *convertible-to-non-slicing*: `subrange<Base*>(derived_ptr,
  derived_ptr)` was accepted and would have strided by `sizeof(Base)` over an
  array of `Derived`. It is now rejected, while a qualification conversion
  (`int*` → `const int*`) still works.
- `✓` Closed in Ф31e-e: `std::get` did not reach `subrange`. [ranges.syn] hoists
  `ranges::get` into namespace `std`; structured bindings and an ADL-qualified
  `get(s)` found it either way, so only the spelling generic tuple-like code
  actually uses was broken.
- `✓` Closed in Ф31e-e: `range_rvalue_reference_t`, `range_common_reference_t`
  and the `iter_common_reference_t` they rest on did not exist, and
  P2387R3's `ranges::range_adaptor_closure` — the public hook a program outside
  this library needs to write an adaptor that composes with `|` — did not either.
- `~` **`zip_view` and its family are `tuple`-always where the standard is
  *tuple-or-pair*.** `views::zip(a, b)` yields `tuple<int&, int&>`; the standard
  specifies `pair<int&, int&>` at exactly two ranges. A deliberate Ф29d decision,
  recorded here for the first time in Ф31e-e — structured bindings and
  `get<N>` behave identically, but code that names the reference type does not
  port.
- `~` **`filter_view` accepts a predicate the standard rejects.** [range.filter]
  constrains it on `indirect_unary_predicate<Pred, iterator_t<V>>`, which
  requires `copy_constructible<Pred>`; boxcxx uses `predicate<Pred&,
  range_reference_t<V>>`, which does not. A move-only predicate is therefore
  accepted here and rejected by libc++ 22 (measured). More permissive, so no
  correct program breaks — but a program written against boxcxx may not port.
  `take_while_view` is *not* affected: it carries the standard's
  `indirect_unary_predicate<const Pred, …>` and rejects a move-only predicate,
  as libc++ does.
- `✓` Closed in Ф31e-e: `ranges::basic_istream_view` / `views::istream` /
  `istream_view` did not exist, and **the reason this entry used to give for that
  was wrong**: it said they are specified against `basic_istream`, "whose global
  objects BoxOS does not have". `views::istream` names no global — it binds
  whatever `basic_istream&` it is handed — and Ф30e had already put `<istream>`
  and `<sstream>` in the tree.
- `~` One residual on that: `<ranges>` includes only `<iosfwd>`, so a translation
  unit that includes `<ranges>` **alone** finds `istream_view<int>`'s constraint
  unsatisfied rather than the type. An edge from `<ranges>` to `<istream>` is a
  genuine cycle here, measured rather than assumed: `<istream>` → `<ostream>` →
  `<ios>`, and `ios_base` keeps its `iword`/`pword` and callback tables in
  `std::vector`, while `<vector>` → `<algorithm>` → `<__bits/ranges_algo>` →
  `<ranges>`. libc++ can afford the edge because its `ios_base` uses raw arrays
  for those three tables. Any program that can actually call `views::istream` has
  a stream, and so has `<istream>` or `<sstream>` included already.
- `?` `split_view` and `lazy_split_view` report `iterator_category ==
  input_iterator_tag`, which is what the standard itself specifies (their
  `operator*` yields a prvalue). The concept layer is unaffected —
  `forward_iterator` holds and both are `forward_range`, so algorithm dispatch and
  `range-for` behave as forward ranges. Only code that reads the legacy
  `iterator_category` sees "input".
- `~` `filter_view` is not const-iterable. So is the standard's — `filter_view`
  caches `begin()` and has no `begin() const` there either.
- `✓` Closed in Ф31e-b-2: `transform_view` was not const-iterable, so
  `range<const transform_view<…>>` was false and iterating one failed with
  "discards qualifiers". Its iterator and sentinel now take the `Const` parameter
  the standard's `iterator<Const>` implies, and `begin()`/`end()` have the
  conditional `const` overloads of [range.transform.view]. `filter_view` keeps its
  non-const shape deliberately — it caches `begin()`, and the standard gives it no
  `begin() const` either (the entry above).
- `✓` Closed in Ф31e-b-1: `ranges::ssize` existed as an overloaded function
  template rather than a customization-point object, so unlike its twelve siblings
  it could not be passed around as a value or protected from ADL hijacking.
- `✓` Closed in Ф31e-b-1: the free `cbegin`/`cend` of [iterator.range] carried no
  `noexcept`, where /7 and /9 mandate a conditional one. The other four of that
  family are not given one by the standard and still do not have one.

### P2846R6 — reserve_hint (Ф32-c)

- `✓` `ranges::reserve_hint`, `approximately_sized_range`, and `sized_range`
  rebased to refine it rather than `range`. Seventeen views carry the member —
  every one the paper touches — and `ranges::to` reserves off the hint instead
  of off `ranges::size`, which is the point: a range that can say "about this
  many" but never exactly how many could previously not be reserved for at all.
  `take_view`'s is the one unconstrained member (LEWG asked for it: `r | take(n)`
  is likely to yield n elements even when r offers no hint).
- `+` `ranges::reserve_hint` takes a forwarding reference, so
  `ranges::reserve_hint(v | views::take(3))` on a pipeline temporary is
  well-formed. **The older range-access CPOs in this library do not**: `Begin`,
  `End`, `Size`, `Data`, `Empty` and their c-/r- variants all take a plain `R&`,
  so `std::ranges::size(v | views::take(2))` — which both reference libraries
  accept — does not compile here. That is a pre-existing gap in [range.access],
  not something P2846R6 introduced, and it is recorded in §6 rather than fixed
  in this phase.

### P2542R8 — concat_view (Ф32-d)

- `✓` `concat_view` and `views::concat`. The position is a `variant` over the
  adapted iterators, because it genuinely is a sum type; `satisfy()` walks past
  however many empty ranges follow a boundary, which is the case that separates
  a working implementation from one that only looks right. The element type is
  the common reference of all the adapted ranges, gated by the `Concatable`
  chain that was already in the tree — it was built for `join_with` (LWG 4074)
  and this is its second consumer. `views::concat(r)` on a single range is
  `views::all(r)`, not a one-element `concat_view`.
- `~` The standard's wording indexes its parameter pack with C++26's `T...[I]`.
  boxcxx compiles at `-std=gnu++23`, where that is an extension, so every
  dispatch is an `if constexpr` recursion over `I` with `tuple_element_t` in
  place of pack indexing. Same semantics, no dependency on a C++26 language
  feature.

### P3138R5 / P3137R3 — cache_latest and to_input (Ф32-g)

- `✓` `views::cache_latest`. A `transform_view` recomputes on every dereference
  by design, so a `filter` above one calls the transform twice per surviving
  element — once for the predicate and once for the caller. This remembers the
  last value produced; the suite proves it by *counting* the calls (six instead
  of ten over six elements, same answer), which is the only way the difference is
  observable. A reference element is cached as a **pointer**, a prvalue as the
  value itself, so writing through `*it` reaches the original element rather than
  a copy of it. The cast to take that address is to an *lvalue*, not to
  `range_reference_t<V>`: the reference may legally be `T&&`
  (`views::as_rvalue`, `move_iterator`), and there is no address of an xvalue.
  The value branch constructs in place, so an element type that is constructible
  but not assignable still works.
- `✓` `views::to_input`. A range that could be walked twice, told not to be:
  the category drops to input and common-ness goes with it, so the adaptors above
  stop paying for a guarantee the consumer never collects. `size` and
  `reserve_hint` survive — only the *category* drops. Over a range that is
  already input-only and not common it is `views::all`, not a second wrapper.
- `+` Both views carry `reserve_hint`, which the draft gives them and
  libstdc++ 16 has not caught up with for `cache_latest_view`.
- `!` **Both iterators are move-only, and that exposed two adaptors in this
  library that had been quietly demanding more of an iterator than `[iterator.
  concept.input]` allows.** Neither defect was reachable before, because until
  these views there was no move-only iterator in the tree to reach them with.
  - `filter_view::iterator::operator++` handed the base iterator to `find_next`
    **by value from an lvalue**, which requires it to be *copyable*. An input
    iterator only has to be `movable`; copyable is a forward-iterator
    requirement. Fixed by moving.
  - `drop_while_view` held its cached `begin()` as a plain member with a default
    initializer, which requires the base iterator to be **default-constructible**
    — something `[range.drop.while]` never asks of `V`. The cache is only
    meaningful for a forward range, whose iterator is `semiregular` and therefore
    does supply one, so it is now present only in that case. This is the same
    conditional-member shape the tree already uses in `join`, `join_with`,
    `slide` and `split`; the helper moved from `<__bits/ranges_join>` up to
    `<__bits/ranges_views>` so this earlier file can reach it.

### P2714R1 / P2927R3 — callables by template argument, and looking inside an exception_ptr (Ф32-f)

- `✓` `bind_front<f>`, `bind_back<f>` and `not_fn<f>` name the callable as a
  template argument, so nothing of it is stored. boxcxx implements them by
  wrapping the NTTP in an EMPTY type and reusing the existing binders rather
  than duplicating them; the return type is unspecified by the standard either
  way, and an empty callable costs a byte rather than a pointer.
- `✓` `exception_ptr_cast`. Before it, the only way to look inside an
  `exception_ptr` was to rethrow into a `try` block — an unwind, two handlers,
  and impossible from a `noexcept` function. It asks the runtime the question
  the personality routine asks (`type_info::__do_catch`), so a base-class
  handler matches a derived exception and the returned pointer carries the
  base adjustment; a `type_info` comparison could do neither.
- `~` `std::exception` and `std::bad_exception` moved to
  `<__bits/exception_base>`. `<typeinfo>` derives `bad_cast` from
  `std::exception` and `<exception>` now needs `<typeinfo>`, which is a genuine
  cycle; the piece both sides need became its own leaf. Nothing else moved and
  both headers are unchanged from a user's point of view.
- `+` P3060R2's `views::indices(n)` is provided: `iota` from a zero of `n`'s own
  type, so `views::indices(v.size())` yields `size_t` indices and comparing one
  against another `size_t` needs no conversion. That type agreement is the
  entire content of the paper.

## `<regex>`

Provided as of Ф44 (§1.1 for why it stopped being excluded, and what the engine
is). Everything below was decided from [re.grammar] and the ECMA-262 text it
adopts rather than from either library, and every one of them was measured
three ways by `tools/cxx_regex_oracle.sh` — 40 000 generated cases per sweep,
plus curated lists for the five POSIX grammars.

- `+` **The engine refuses instead of hanging.** A pattern with no
  back-reference is regular and runs on a linear machine, where the time is set
  by the shape of the input. One with a back-reference is not regular and takes
  a bounded backtracker with an explicit step budget of `65536 + 4096·n`;
  exhausting it throws `error_complexity`, which is the name [re.err] has for
  exactly this. §3 has the measurements against both references.
- `?` **`error_type` is numbered from 1.** [re.err] fixes the names and leaves
  the values implementation-defined: libstdc++ numbers from 0, libc++ from 1.
  From 1 here, so that `if (e.code())` is not false for a real error. A sweep
  that compared the raw integers manufactured a disagreement on every throw
  until it was taught to compare names — the first thing this tool ever found.
- `?` **A back-reference to a group that did not participate matches the empty
  string.** ECMA-262's BackreferenceMatcher says "if r is undefined, return
  c(x)", so `x(a)?\1y` matches `"xy"`. **Both references call that a failure**
  — 471 of 40 000 generated cases are this one cell, and it is the single
  largest source of disagreement between us and them.
- `?` **A back-reference is valid if its number is at most the number of groups
  in the WHOLE pattern**, so forward references are legal and match the empty
  string until the group participates. ECMA-262's DecimalEscape is not modified
  by [re.grammar], and it counts the whole pattern. **Both references reject
  them** — another 2 136 cases, and none outside that cell.
- `?` **Captures inside a quantified body are reset on every iteration**, so
  `(?:(a)|b)*` against `"ab"` leaves group 1 unset. That is ECMA-262's
  RepeatMatcher; **libc++ does it, libstdc++ does not.**
- `?` **`\q` is legal.** [re.grammar]/3 defines IdentityEscape as
  "SourceCharacter but not c", which is wider than ECMA-262's own rule.
  libstdc++ agrees; **libc++ throws `error_escape`.**
- `?` **`[]` is an empty class** that matches nothing, and `[]]` is a class
  containing `]`. libstdc++ agrees; libc++ does not parse the second.
- `?` **The five POSIX grammars take the leftmost-LONGEST match**, so `a|ab`
  against `"ab"` is two characters under `extended` and one under `ECMAScript`.
  Both references do this; boxcxx did not until Ф44-d, which means its POSIX
  grammars had until then been another spelling of ECMAScript.
- `–` **No lookbehind.** `(?<=…)` is in no grammar the standard defines, and
  neither reference has it either — measured, not assumed.
- `~` **`a{4000000000}` throws `error_space`, not `bad_alloc`.** The repetition
  is expanded, so the bound is memory; [re.err] has a name for running out of
  it and this uses it.
- **Wide is the same program.** `wregex` and every `w` typedef of [re.syn] are
  the same templates instantiated at `wchar_t`; Ф44-e ran 40 000 generated ASCII
  cases through BOTH halves of all three columns and each column answers about
  a character the same way whatever width it was stored in. What is NOT the
  same is the classification: `regex_traits<wchar_t>` asks `ctype<wchar_t>`,
  which here is the Unicode database of Ф42-a, so `[[:alpha:]]` takes U+4E2D
  where macOS's wide ctype refuses it, and `[[:digit:]]` still refuses U+0660
  because C fixes it to ASCII. Those answers are ours by decision (§2
  `<cwctype>`), and `phase238` is where they are checked — a host stand cannot,
  because on the host that traits reads the host's tables.
- **[version.syn] gives `<regex>` no feature-test macro of its own.** It owns
  `__cpp_lib_nonmember_container_access`, which it defines.

## `<span>`

- `✓` Closed in Ф35, found while building `<mdspan>`: the **(iterator, sentinel)
  deduction guide dropped the static extent**. [span.deduct] puts the second
  argument through *maybe-static-ext*, so `span(p, integral_constant<size_t,
  4>{})` must deduce `span<int, 4>`; boxcxx hardcoded `dynamic_extent`, making
  the static-extent half of that guide unreachable. The two exposition-only
  entities [span.syn] owns — *integral-constant-like* and *maybe-static-ext* —
  now exist in this header, which is also where `<mdspan>` reads them from. As
  a side effect `span(p, cw<4zu>)` deduces a static extent too, since Ф33's
  `constant_wrapper` models the concept.

- `✓` Closed in Ф31e-g: the iterator constructors took **pointers**, not an
  iterator and a sentinel, and were **not** `explicit(extent != dynamic_extent)`.
  The second half was the one that mattered: a fixed-extent `span<int,3>` could
  be built *implicitly* from a pair, so `take({a, a + n})` compiled while
  asserting a size nothing checked. The range deduction guide was absent too, so
  `std::span(v)` over a vector could not deduce at all. **This document's claim
  that span had no `crbegin`/`crend` was wrong** — both were there. Closed
  `__cpp_lib_span`.

## `<spanstream>`

- Complete: `basic_spanbuf`, `basic_ispanstream`, `basic_ospanstream`,
  `basic_spanstream`, the four `swap` free functions and all eight typedefs.
  New in Ф38; `__cpp_lib_spanstream` is 202106L.
- `overflow`, `underflow` and `pbackfail` are **not** overridden, and that is
  the specification rather than an omission: [spanbuf.virtuals] says outright
  that with a fixed buffer none of them "can provide useful behavior". The
  inherited defaults refuse, which is what a caller wants — the alternative to
  refusing is writing past a buffer somebody else owns.
- `span()` answers two different questions depending on the mode: the WRITTEN
  prefix (`pbase()` to `pptr()`) when `out` is set, the whole buffer otherwise.
  An `ospanstream` reporting the whole buffer would be reporting uninitialised
  bytes as output.
- **boxcxx refuses a seek that libstdc++ 16.1 performs.**
  [spanbuf.virtuals] fails the positioning when "the next pointer is null and
  the new offset is nonzero" — which is exactly `pubseekoff(3, beg, out)` on a
  buffer opened `in` only. libstdc++ omits that test and evaluates
  `nullptr + 3`. Pinned by Phase193; removing the guard here fails exactly the
  two checks that name it.
- **boxcxx's `basic_ospanstream` constructor ORs `ios_base::out`; libstdc++
  16.1 ORs `ios_base::in`.** [ospanstream.cons] says `which | ios_base::out`,
  and it must: a stream with no put area fails its first insertion. The
  difference is invisible at the default argument, where `out` is already set,
  and total for anything else — `ospanstream(buf, ios_base::in)` writes
  normally here and is dead there. Measured against the normative text, not
  inferred; Phase193 pins the case, and reverting to libstdc++'s form fails
  exactly that one check.
- `pbump` takes an `int` ([streambuf.put.area]) while every offset in the
  header is an `off_type`. boxcxx steps it rather than narrowing, so a span
  larger than `INT_MAX` positions correctly; libstdc++ narrows. Unreachable at
  today's buffer sizes and free to get right.
- `setbuf(s, n)` with `n < 0` violates its precondition, so any behaviour
  conforms. boxcxx installs an empty span rather than converting the count to
  a ~2^64-element one, which is the only outcome that must not happen on a
  system without a memory-protection net under the streams.
- A moved-from `basic_spanbuf` is left unchanged. [spanbuf.cons] makes the
  moved-from state implementation-defined, and there is no owned resource to
  transfer — the caller still holds the span either way. Same choice
  libstdc++ documents.

## `<scoped_allocator>`

- Complete: `scoped_allocator_adaptor` with every typedef, every constructor,
  `inner_allocator`/`outer_allocator`, `allocate`/`deallocate`/`max_size`,
  `construct`/`destroy`, `select_on_container_copy_construction`, `rebind` and
  the free `operator==`. New in Ф38.
- It has no feature-test macro of its own; [version.syn] names it among the
  owners of `__cpp_lib_allocator_traits_is_always_equal`, which it now answers
  for. The map in `tools/version_syn_owners.txt` had listed it as an owner
  since that file was written, against a header that did not exist.
- The recursion is the whole implementation. *OUTERMOST* follows
  `.outer_allocator()` to the bottom of a stack of adaptors, so construction
  happens through the allocator that actually owns the storage; the object,
  meanwhile, is offered the **inner** allocator, which is what it should keep
  for whatever it allocates itself. Phase194 pins exactly that split, and
  swapping the two fails precisely the two checks that name it.
- With no inner allocators `inner_allocator_type` is
  `scoped_allocator_adaptor<OuterAlloc>` and `inner_allocator()` returns
  `*this` — there is no member at all in that case. The two cases are told
  apart by which overload of an internal `Get()` the `this` pointer matches,
  which is how one call site serves both.
- `select_on_container_copy_construction` is recursive rather than built on an
  index-tuple: the inner adaptor's own answer already applies
  `select_on_container_copy_construction` to each of its allocators, so
  asking it is asking all of them. Same result as libstdc++'s tie/index
  machinery, in four lines instead of forty.
- Deriving publicly from `OuterAlloc` is the standard's own synopsis, not an
  implementation liberty; the adaptor's own `allocate`/`deallocate` hide the
  base's.

## `<sstream>`

- `✓` Closed in Ф32-a: **P2495R3 is complete and its macro is claimed** — the
  first C++26 macro boxcxx defines (§1.3). The string_view-like constructors had
  been in the tree since Ф30e as a silent early superset, but collapsed: one
  constructor with two defaulted parameters instead of the standard's three.
  That cost two things. The `(t, allocator)` form did not exist at all — an
  allocator does not convert to `openmode`, so the call bound to nothing — and
  the three-argument form was `explicit`, which the synopsis does not make it,
  so `basic_stringbuf<char> b = {sv, mode, alloc}` was rejected although
  copy-list-initialization is well-formed there. Split into the standard's three
  on `basic_stringbuf` and on all three stream wrappers; the two-argument
  `(t, which)` form stays `explicit`, and the suite pins that it does.
- `✓` Closed in Ф40, found by building `<syncstream>`: **`void
  str(basic_string&&)` did not exist** on `basic_stringbuf` or on any of the
  three stream wrappers. It is the other half of P0408R7 — `str() &&` hands
  the characters out without a copy, the rvalue setter takes them back the
  same way — and only the getter had been built. It is not covered by the
  documented cross-allocator omission this header records elsewhere: the
  allocator is the same one. Found because a syncbuf whose target accepted
  part of a run must hand the remainder back to its own buffer, and copying
  it would have been a copy of everything not yet printed.
- `+` The constraint boxcxx uses is the standard's, in full:
  `is_convertible_v<const T&, basic_string_view<charT, traits>>` **and**
  `!is_convertible_v<const T&, const charT*>`. libstdc++ 16.1 writes only the
  first half (measured in its `<sstream>`), so boxcxx rejects a source the
  standard also means to keep out of these overloads and libstdc++ accepts.

## `<stacktrace>`

Added in Ф37, together with the OS capability it needs. The whole of
[stacktrace] is here: `stacktrace_entry`, `basic_stacktrace<Allocator>` with
all three `current` overloads and the full allocator-aware surface,
`pmr::stacktrace`, `to_string`, both `operator<<`, both formatters and both
`hash` specializations. `__cpp_lib_stacktrace` is 202011L.

- `~` **`description()` demangles, and the demangler's accuracy is measured
  rather than claimed.** The names in the image are mangled — demangling them
  at build time was measured and rejected, since it grows `cxxtest.elf`'s name
  blob from 3.67 MB to 9.71 MB against a 16 MB image — so `__cxa_demangle`
  (`include/cxxabi.h`, added in Ф37) does the work when someone asks.

  It is held to the reference implementation over the whole corpus the tree
  produces: **32 010 mangled names** from `cxxtest`, `brookexec` and
  `currentexec`, each compared byte for byte against `x86_64-elf-c++filt`
  (libiberty). The result, stated in full because a partial number would be
  the misleading one:

  | | |
  |---|---|
  | byte-identical to libiberty | **29 880 — 93.35%** |
  | refused (returns null; the caller prints the mangled name) | 1 197 — 3.74% |
  | demangled, but not identically | 933 — 2.91% |
  | ...of those, differing in the function NAME rather than in a nested template argument | 315 — 0.98% |

  The differences are concentrated in one place: template arguments of deeply
  nested generic lambdas, where a substitution has to be re-evaluated in a
  context the mangling only implies. A refusal is safe by construction — the
  caller still holds the mangled name and prints it — and the parser is
  fuzz-tested: 51 869 truncations, single-byte mutations and pure garbage
  under ASan, UBSan and LeakSanitizer produced no crash, no hang and no leak.

  371 names in that corpus the REFERENCE cannot demangle either — they use
  `Tk`, the C++20 constrained-template-parameter mangling libiberty does not
  implement — and they are excluded from the comparison rather than counted
  as wins.
- `~` **`source_file()` is always `""` and `source_line()` always `0`.**
  [stacktrace.entry.obs] specifies exactly this when the information is
  unavailable. It is unavailable for a measured reason rather than a chosen
  one: line numbers live in `.debug_line`, and the C++ images — the only ones
  that can call this header — must ship with `.debug_*` stripped to fit the
  image at all (with `-g`, `cxxtest.elf` grows about tenfold).
- `~` **The description ends where the CFI ends.** C sources build with
  `-fno-asynchronous-unwind-tables`, so a trace that reaches boxlib or `_start`
  stops there. That is the same boundary the exception unwinder has had since
  Ф4, not a new limit.
- `~` **`to_string(const basic_stacktrace&)` does not end in a newline.** Frames
  are `%4u# <entry>` joined by `\n`; libstdc++ writes a newline after every
  frame including the last, so `println("{}", trace)` there ends with a blank
  line. The whole shape is implementation-defined. An entry prints as
  `<description> [0x<pc>]`, with `<unknown>` standing in for a description the
  image cannot give, and an entry holding no address prints just `<unknown>`.
- `+` **`operator<<` is `os << to_string(x)`, exactly as
  [stacktrace.basic.nonmem] specifies it.** libstdc++ writes to the stream
  directly instead, which is observably different when the stream is in a
  state that makes the two differ.
- `~` **`native_handle_type` is `uintptr_t`** — the code address itself. BoxOS
  has no handle table to hand out an opaque token from, and the address is the
  number every other diagnostic in the system speaks in, the kernel's fault
  dump included.
- `+` A trace never allocates behind its allocator's back: the raw frame
  buffer the walk fills is taken from the same allocator the trace lives in
  and returned before `current` finishes, so a `pmr::stacktrace` on a
  monotonic resource touches only that resource. The suite pins it by counting
  the resource's allocations and frees.
- `+` The frames belonging to the library itself are dropped by **identity**,
  not by counting: `current`, its helper, the collector and `_Unwind_Backtrace`
  each name themselves, and the walk skips frames while it is still inside
  that set. A count would have been wrong the moment an inlining decision
  changed — it was, during development, because userspace builds without `-O`
  and an internal helper that was expected to vanish did not.

## `<stdbit.h>` and `<stdckdint.h>`

The two C23 headers C++26 adopts, added in Ф34. Both are thin: `<stdbit.h>` is
fourteen families of one-line wrappers over `<bit>`, `<stdckdint.h>` is three
functions over `__builtin_*_overflow`. Two things are worth recording.

- `!` **`stdc_bit_ceil` disagrees with libstdc++ 16.1, and the disagreement is
  measured.** The unrepresentable case returns 0 rather than being undefined —
  but "unrepresentable" means strictly *above* the top bit, and the top bit
  itself is a power of two that fits. libstdc++ tests `value & msb` and so
  returns 0 for `stdc_bit_ceil_uc(0x80)` and `stdc_bit_ceil_ui(0x80000000)`,
  where C23 7.18.15.3 asks for the value back. boxcxx tests `value > msb`. Both
  results were printed from a host binary, not read off a page.
- `~` Nothing in either header is `constexpr`, even though every `std::`
  function underneath is. [constexpr.functions] forbids declaring a standard
  library signature `constexpr` unless the standard says so, and
  [stdbit.h.syn] does not. A caller who wants the constant-expression form has
  `<bit>`.

## `<stdatomic.h>`

- `~` Provided in Ф34 as a pure using-declaration header over `<atomic>`:
  `_Atomic(T)` expands to `::std::atomic<T>`, and every name [atomics.syn]
  declares is re-exported into the global namespace. There is no second
  implementation — C code carrying atomics into BoxOS's C++ userspace gets the
  same objects the C++ side uses. The `ATOMIC_*_LOCK_FREE` and
  `ATOMIC_FLAG_INIT` macros need no re-export: `<atomic>` already defines them
  as macros, which have no namespace.

## `<stdfloat>`

- Complete: `float16_t`, `float32_t`, `float64_t`, `float128_t`,
  `bfloat16_t`, each declared under its own `__STDCPP_*_T__` macro. New in
  Ф40. There is no feature-test macro for this header — [version.syn] has
  none — so the compiler's five macros are the only test, which is the
  standard's design.
- **The header was the smallest part.** Because the compiler predefines those
  five macros, the five types were already standard floating-point types, and
  three things had been quietly wrong for as long as the toolchain has been in
  use: `is_floating_point` named three types instead of eight;
  `numeric_limits` had no specialization for any of the five (nor, it turned
  out, for `long double` in `std::hash`); and [cmath.syn]/2's "an overload for
  each cv-unqualified floating-point type" was five types short. Nothing had
  noticed because nothing in the tree had ever written one of the types down.
- `numeric_limits<bfloat16_t>::is_iec559` is **false**, and it is the only
  floating-point type here for which that holds. The compiler says so itself
  (`__BFLT16_IS_IEC_60559__` is 0): bfloat16 keeps binary32's eight exponent
  bits and drops sixteen significand bits, which is not one of the interchange
  formats [numeric.limits] sanctions.
- **float16_t and bfloat16_t have unordered conversion ranks**, and a call
  mixing them has no common type at all. [cmath.syn]/3 says such a program is
  ill-formed, and that is what happens here: `std::pow(float16_t, bfloat16_t)`
  finds no candidate. Neither type's value set contains the other's — 11
  significand bits against 8, 5 exponent bits against 8 — so this is the
  standard's own partial order showing through, not a gap.
- Equal ranks are broken by subrank, which means the STANDARD type wins:
  `std::pow(float32_t, float)` is `float`, not `float32_t`. `float128_t`
  outranks `long double` (every 80-bit value is a binary128 value), so a call
  mixing them is `float128_t`.
- **The four narrow types compute through the standard type whose format
  contains theirs**, which is exact rather than approximate: `float32_t` IS
  binary32 and `float64_t` IS binary64, so those are the same arithmetic under
  a different name; `float16_t` (11 bits) and `bfloat16_t` (8) widen into
  binary64, whose 53 bits are far past the 2p+2 that makes the double rounding
  provably harmless. `std::exp(float32_t(1))` is bit-for-bit `std::exp(1.0f)`,
  and the suite pins that.
- **float128_t gets real kernels** (`<__bits/cmath_quad>`), because the
  compiler lowers `__builtin_sqrtf128` and its neighbours to `sqrtf128`,
  `fmaf128`, `truncf128` and so on, none of which is in libgcc — they live in
  libquadmath, which a freestanding target does not have. That is the same
  reason libstdc++ 16.1 cannot compile `std::sqrt(std::float128_t)` on a host
  without it (measured), and libc++ 22.1.6 has no `<stdfloat>` at all. What
  libgcc does carry is correctly-rounded `+ - * /` and the comparisons, and
  everything below is built on those and on the bits:

  | | |
  |---|---|
  | `trunc` `floor` `ceil` `round` `rint` `nearbyint` | exact |
  | `fabs` `copysign`, all classification and comparison | exact |
  | `ldexp` `scalbn` `scalbln` `frexp` `modf` `logb` `ilogb` | exact |
  | `fmod` `remainder` `remquo` `nextafter` `fmin` `fmax` `fdim` `lerp` | exact |
  | `sqrt` | **correctly rounded** |
  | `fma` | **exact — one rounding** |
  | `hypot` | ≤ 1 ulp |
  | `cbrt` | ≤ 1 ulp (0 observed) |

  Verified against MPFR at 113 bits over 1.9 million values spanning the whole
  exponent range, including subnormals and engineered cancellations, on the
  development host — against the shipped source, not a copy. 160 of those
  vectors are baked into phase200 and re-checked on the target, where the
  underlying arithmetic comes from a different libgcc build.
- **`~` The transcendentals at float128_t are DELETED, not merely absent** —
  `exp`, `log`, `sin`, `cos`, `tan`, their inverses and hyperbolics, `pow`,
  `atan2`, `erf`, `erfc`, `lgamma`, `tgamma`. They need polynomial kernels
  carrying 113 bits, which is a phase of its own; computing them in 64 bits and
  returning a 113-bit type would be a lie that compiles. Deleting rather than
  omitting makes the call fail at the call site with a diagnostic naming the
  function, instead of somewhere inside a promotion template. Deviation from
  [cmath.syn]/2, recorded here, and the one place this library is knowingly
  short of what that paragraph asks.
- `std::format` accepts the four narrow types and **not** `float128_t`:
  writing 113 significand bits in decimal needs a converter `<charconv>` does
  not have yet, and claiming the formatter while narrowing to `long double`
  would print a number the caller never had. libstdc++ 16.1 refuses the same
  call for the same reason.
- `nexttoward` is deleted at every extended type. That one is **not** a
  deviation — [cmath.syn]/4 makes such a call ill-formed, and deleting the
  overload is how a library says so out loud.
- `<charconv>` is unchanged: [charconv.syn] declares `to_chars`/`from_chars`
  for `float`, `double` and `long double` by name, not "for each
  floating-point type", so the extended types are outside it by the standard's
  own wording rather than by omission.

## `<string>`

- `✓` Closed in Ф42-g: **the wide half of [string.conversions] was absent
  entirely** — no `to_wstring`, and not one of the eight `sto*(const wstring&)`.
  Nine narrow functions and nine wide ones now share ONE engine: the wide
  conversions narrow their argument one code unit at a time and hand it to the
  narrow functions, and `to_wstring` delegates to `to_string` and widens the
  result. A number has no character type, so every character a numeric literal
  can hold is ASCII, and the narrowing is one-to-one — which is also why `pos`
  maps straight back rather than needing its own arithmetic. `Phase219` checks
  all eighteen against their narrow twins over 601 values each, because a claim
  that they share an engine is testable rather than merely stated.
- `+` **`to_wstring` inherits P2587R3, and the reference has not implemented it
  at all.** `to_wstring(1e-9)` is `L"1e-09"` here; libc++ 22 answers
  `L"0.000000"`, and its `to_string` says the same — so it is consistently
  behind rather than wrong about the wide half only. Measured, and the reason
  this pair has no host oracle: the oracle is boxcxx's own narrow half.
- `–` **The wide `sto*` skip only the six "C" white-space characters**, not
  Unicode `White_Space`. [string.conversions] words the wide overloads "as if by
  `wcstol`", and boxcxx's own `wcstol` DOES skip a wide space (Ф42-b) — so
  `stoi(L"\u2003" L"42")` throws here where `wcstol` reads 42. Delegating to
  `wcstol` instead would have bought that one behaviour at the price of a second
  set of rules for overflow, invalid input and `pos`, which is the shape defects
  live in. `Phase219` pins both halves of the divergence, so it stays a decision
  rather than becoming a surprise.
- `✓` Closed in Ф34: **`s + sv` did not compile.** P2591R5's four
  `basic_string` + `basic_string_view` operators are provided;
  `type_identity_t` on the view parameter is what keeps them from hijacking
  `s + "literal"`, which must still pick the `const charT*` overload.
- `✓` Closed in Ф34: the **four `operator+` overloads whose RIGHT operand is the
  rvalue** were missing ([string.op.plus] lists twelve; boxcxx had eight). Every
  call still compiled by falling back to the `const&`/`const&` form, so nothing
  broke — what was lost was the point of the overloads: `"x" + std::move(s)` now
  prepends into the buffer that is about to be discarded instead of allocating a
  third string.
- `✓` Closed in Ф31e-g: `resize_and_overwrite` (P1072R10) did not exist. It is
  the one way to grow a string and fill the raw tail without first
  value-initializing characters the caller is about to overwrite. A callable
  reporting a length above the one it was given would make the string report
  characters nobody wrote; [string.capacity] leaves that undefined, and boxcxx
  throws `length_error` instead. Closed
  `__cpp_lib_string_resize_and_overwrite`.
- `✓` Closed in Ф31e-g: `std::erase_if(basic_string&, Pred)` was the single
  missing one of the thirteen uniform-erasure overloads, which is why
  `__cpp_lib_erase_if` stayed undefined while `erase_if` worked on every
  container. `basic_string_view` gained `crbegin`/`crend` in the same step,
  closing `__cpp_lib_string_view`.
- `✓` Closed in Ф39: **`basic_string` was not a literal type — no part of it
  worked in a constant expression** (P0980R1 unimplemented; `size()` itself was
  not `constexpr`). All of it is `constexpr` now, `__cpp_lib_constexpr_string` is
  claimed at 201907L, and `bitset::to_string()` folds with it. Three things had
  to be true, and none of them was the one the old note in `<version>` blamed
  (a "compiler-blessed constexpr allocator" — `std::allocator` and
  `construct_at` had been constant-evaluable since Ф31e):
  - The inline buffer is a **union member**, and a union member becomes active
    only when something writes it *through its name* ([class.union]/6). Every
    write in `<string>` went through the data pointer, which does not count.
    `BeginSso()` is the one line that does, and it fills the buffer rather than
    one element, because the value of a `constexpr` **variable** has to be
    complete.
  - `char_traits<char>::length` and `::compare` were bare `__builtin_strlen` /
    `__builtin_memcmp`, and **GCC folds neither over storage `std::allocator`
    handed out during constant evaluation**. That was not a string-only problem:
    a `basic_string_view` built over such storage was already not a constant
    expression here while it is one in both mainstream libraries.
  - `SelfOffset` (does this pointer alias my buffer?) and `char_traits::move`
    (which way do I walk?) both asked with `<` / `>=`, and a relational
    comparison of pointers into different objects is not a constant expression.
    Both now ask with `==` when constant-evaluated and keep the address
    comparison at run time.
- `~` **A `constexpr std::string` variable is limited to the inline buffer** — 15
  characters for `char`. A longer one owns an allocation, and an allocation
  cannot outlive constant evaluation. Measured: libstdc++ 16.1 and libc++ draw
  the line in exactly the same place. Transient long strings inside a constant
  expression are unrestricted.
- `~` `__cpp_lib_constexpr_string` is defined at **201907L**, P0980R1's value and
  the one C++23 carries. The working draft has since raised it to 202511L for a
  C++26 change this library does not implement; libstdc++ 16.1 and libc++ both
  report 201907L too.
- The union-activation question above is not academic: **Apple's libc++ 19.1.2
  answers it wrongly** — `shrink_to_fit()` from an allocated buffer back into the
  inline one is not constant-evaluable there ("assignment to member `__s` of
  union with active member `__l`"), and `std::erase(basic_string&, const U&)` is
  not declared `constexpr` at all though [string.erasure] says it is. Both are
  fixed in libc++ 22.1.6, which is this document's oracle, so neither is listed
  in §4. Recorded because it is the same trap, sprung on someone else.
- `✓` Closed in Ф31e-g-3: `pmr::basic_string`, `pmr::u8string` and
  `pmr::forward_list` did not exist. [string.syn] gives the string aliases an
  alias **template** first and spells the five concrete ones through it; boxcxx
  had the five and not the template, so a `pmr` string of any character type the
  standard did not enumerate could not be named, and `pmr::u8string` was missing
  with it.
- `~` All the `pmr::` aliases are declared in `<memory_resource>`, not in the
  header that owns the container as [string.syn] / [vector.syn] / … specify, so a
  translation unit that includes only `<string>` cannot see `pmr::string`. The
  aliases themselves are all present and correct.

- `+` P2587R3: `to_string` on a floating-point value now means
  `format("{}", v)` — the shortest decimal that reads back as the same value —
  rather than `sprintf("%f")`. The old rule printed `to_string(1e-9)` as
  `0.000000`, which does not round-trip, and `to_string(1e300)` as three
  hundred digits. The integer overloads are unchanged.

## `<string_view>`

- `✓` Closed in Ф38: **`enable_borrowed_range<basic_string_view<C,T>>` was
  declared in `<ranges>` instead of here**, so the answer depended on what else
  the translation unit happened to include. Measured before the move: with
  `<string_view>` alone `ranges::borrowed_range<string_view>` was **false**,
  and it became true only once `<ranges>`, `<algorithm>` or `<span>` came
  along. [string.view.synop] declares it in this header, and where a marker is
  declared is the whole of its meaning: a view that does not own its characters
  outlives an iterator into it, so `ranges::` algorithms may hand that iterator
  back rather than `ranges::dangling`. The primary template is in
  `<__bits/ranges_core>`, which this header already included for its own range
  constructor, so nothing new is pulled in to say it. `<ranges>` keeps
  `enable_view<basic_string_view>` — that one cannot be stated without
  `enable_view`, which is declared in `<ranges>` itself, and is unreachable
  without it. Found while building `<spanstream>`, whose range constructor is
  constrained on `borrowed_range` and would otherwise have rested on a
  transitive include.

### P1391R4 / P1989R2 — the two constructors, and a macro value that never existed (Ф32-i)

- `!` **`__cpp_lib_string_view` read 202106L, which is not a value this macro has
  ever carried in any revision.** N4950 says 201803L, and so does libstdc++,
  which knows only that and C++26's 202403L. It was a transcription error rather
  than a claim, and it had been pinned at the wrong number in the suite since the
  header was written, so the pin confirmed it rather than catching it. Corrected
  to 201803L. C++23 never bumped this macro, even in the cycle that added the two
  constructors below — so the value is right at 201803L either way, and fixing
  the value and fixing the header were genuinely separate jobs.
- `✓` `template<class It, class End> basic_string_view(It, End)` (P1391R4). Only
  a raw pointer pair was accepted before, which meant a contiguous range whose
  iterator is a **class type** could not build a view of itself at all —
  `string_view(v.begin(), v.end())` over a `vector<char>` did not compile. The
  `is_convertible_v<End, size_type>` exclusion in its constraints is what keeps
  `sv(p, 4)` on the `(pointer, count)` constructor instead of reading 4 as a
  sentinel; with pointer iterators `sized_sentinel_for` already excludes it, so
  the suite reaches that constraint with a purpose-built sentinel type that is
  *both* a valid sized sentinel and convertible to `size_type`, where the
  exclusion is the only thing choosing.
- `✓` `template<class R> explicit basic_string_view(R&&)` (P1989R2), explicit per
  P2499R0. Implicit was the original design and was pulled back before C++23
  shipped: it let any contiguous range of the character type decay to a view of
  itself at every call boundary, temporaries included, and those die at the
  semicolon. `basic_string` keeps its own conversion operator and stays implicit,
  which is what the `operator basic_string_view` exclusion in the constraints is
  for — the suite proves that one with a type whose operator deliberately returns
  a different length than its buffer.
- `✓` Both deduction guides from [string.view.synop]. Without them the two
  constructors are only reachable by naming the character type, which is most of
  the point of having them.
## `<syncstream>`

- Complete: `basic_syncbuf`, `basic_osyncstream`, the free `swap`, and the
  `syncbuf` / `osyncstream` typedefs — and `wsyncbuf` / `wosyncstream` since
  Ф42-f, which is where the wide half of every stream typedef arrived. New in Ф40; `__cpp_lib_syncbuf` is 201803L, and
  [version.syn] gives it two owners — `<syncstream>` and `<iosfwd>`, which
  declares the two class templates. It is the only macro in the tree whose
  owner is `<iosfwd>`.
- **The lock is per wrapped buffer and exact.** [syncstream.syncbuf.virtuals]
  permits `emit()` to touch the wrapped buffer "while holding a lock uniquely
  associated with `*wrapped`". libstdc++ draws that lock from a fixed pool
  indexed by a hash of the pointer, so two unrelated targets serialise against
  each other whenever they collide, and the pool's size caps how many targets
  can be independent. boxcxx allocates one entry per distinct wrapped buffer,
  refcounted by the syncbufs naming it and freed when the last one lets go
  (`src/runtime/syncstream.cpp`). Unbounded, and unique in the sense the
  clause uses the word.
- The associated output is a `basic_stringbuf`, composed rather than
  reimplemented: `overflow`/`xsputn` forward into it and the syncbuf keeps no
  get/put area of its own. Same choice libstdc++ makes, for the same reason —
  growing an allocator-aware character buffer correctly is already solved one
  header over.
- **boxcxx sets `badbit` where libstdc++ 16.1 sets `failbit`.**
  [syncstream.osyncstream.members] says `basic_osyncstream::emit()` "behaves as
  an unformatted output function. After constructing a sentry object, calls
  `_sb_.emit()`. If that call returns false, calls `setstate(badbit)`."
  libstdc++ constructs no sentry and sets `failbit`; libc++ 22.1.6 does what
  the standard says, and so does this. Phase199 asserts on `rdstate()` exactly,
  because "some error bit" would pass for both spellings and which bit it is
  is the whole difference.
- A partial transfer keeps exactly the untransferred remainder. `emit()`
  returns false and the accepted prefix is dropped from the associated output,
  so the next `emit()` neither reprints it nor loses the rest. The postcondition
  the standard states ("on success, the associated output is empty") says
  nothing about the failing case, and the two ways of getting it wrong —
  keeping everything, dropping everything — are a duplicated prefix and lost
  output respectively.
- What the type promises reaches exactly as far as the target: two syncbufs
  wrapping the same buffer serialise, a writer that bypasses them and writes to
  that buffer directly does not. That is true of every implementation, and on
  BoxOS it is worth saying plainly, because `std::cout` is one such direct
  writer — its buffer is the screen Current and it holds nothing back.

## `<tuple>`

- `!` `✓` Closed in Ф38, reported as a debt by the commit that found it and
  fixed in the next one: **the entire allocator-extended half of
  [tuple.cnstr] was missing** — all fourteen `allocator_arg_t` constructors,
  and the `uses_allocator<tuple<Types...>, Alloc>` specialization
  [tuple.syn] requires. Without the specialization the answer to "does a
  tuple want an allocator" was **false**, so uses-allocator construction
  stopped at the tuple and every element inside was built without one. A
  `vector<tuple<pmr::string, pmr::string>>` under a
  `scoped_allocator_adaptor` silently sent both strings to the global heap.
  Pinned by Phase195, which walks all fourteen with values taken from
  libstdc++ 16.1 and finishes with exactly that container.
- **The elements are constructed in place, and getting there is the one
  subtle part.** The obvious spelling —
  `value(make_obj_using_allocator<T>(a, args...))` — looks like guaranteed
  copy elision and is not: `TupleLeaf::value` carries
  `[[no_unique_address]]`, and elision into a potentially-overlapping
  subobject is not guaranteed. Measured rather than reasoned about: g++-16,
  clang and the cross compiler all three reject it for a non-movable
  element. So the argument list the rule produces is expanded straight into
  `value`'s initializer, which is what libstdc++ does and for the same
  reason. Phase195 builds a `tuple` of a type that is neither copyable nor
  movable; restoring the prvalue spelling fails to compile exactly there.
- The constraint on each allocator-extended constructor is its non-allocator
  twin's, which means it asks whether the element is constructible from the
  argument **alone** — a type constructible only *with* an allocator is
  therefore rejected. That reads backwards and is what the standard says;
  libstdc++ rejects it too, and Phase195 pins the rejection.
- `<tuple>` names one thing it cannot define: `__bits::UsesAllocArgs`, a
  forwarder to `uses_allocator_construction_args`, declared here and defined
  in `<__bits/uses_allocator>` which this header includes at its end. The
  rule is written in terms of `tuple`, so its file cannot come first; this
  is the single name that crosses back, and it restates nothing.
- `~` The two ordinary (non-allocator) `pair` constructors [tuple.cnstr]
  gained in C++23 — `pair<U1,U2>&` and `const pair<U1,U2>&&` — are still not
  declared by name; those calls reach the tuple-like constructor instead,
  which produces the same elements. All four exist in the allocator-extended
  set, because there they are what the fourteen are counted as.

- `✓` Closed in Ф31e-g-3: `apply`, `tuple_cat` and `make_from_tuple` were not
  constrained by the *tuple-like* concept, which C++23 applies to all three. They
  duck-typed on `tuple_size_v`/`get` instead, so they accepted any type carrying
  the protocol, and — the part that actually bites — a type carrying *neither*
  produced a hard error from inside the function body rather than a clean
  rejection, because the arity is computed there. **This document had recorded a
  different reason for the macro entirely** (that P2165R4's converting machinery
  did not compile); that machinery landed in `c5ba9fa` and works, and cxxtest
  phase137 had been carrying the correct reason all along. Closed
  `__cpp_lib_tuple_like`.
- `✓` Closed in Ф31e-g-3: P2517R1's conditional `noexcept` on `apply` — its
  exception specification is that of the `INVOKE` it performs. `apply` was
  unconditionally potentially-throwing, so a `noexcept` function could not call
  it over a `noexcept` callable without going through `noexcept(false)`.

## `<type_traits>`

- `✓` Closed in Ф31e-g-2: `is_layout_compatible` and
  `is_pointer_interconvertible_base_of` were here, but the two [meta.member]
  *functions* they are specified alongside — `is_corresponding_member` and
  `is_pointer_interconvertible_with_class` — were not, and each was half of a
  feature-test macro. They are functions rather than traits because the question
  is about a pointer-to-member **value** (which may be null), not about a type.
  Closed `__cpp_lib_is_layout_compatible` and
  `__cpp_lib_is_pointer_interconvertible`.
- `?` [meta.member]'s own example includes
  `is_pointer_interconvertible_with_class<C>(&C::b)` and
  `is_corresponding_member<C, C>(&C::a, &C::b)`, where `C` derives from `B`.
  **Neither compiles here — and neither compiles against libstdc++ 15 or clang,
  both measured.** With `S` fixed to `C` the parameter is `M C::*`, and template
  argument deduction does not perform the base-to-derived pointer-to-member
  conversion needed to match an `int B::*`; libstdc++'s declaration is the same
  shape as this one, and clang rejects the identical construct with "could not
  match 'C' against 'B'". The deduced forms, which are the surprising half the
  example exists to show, work and are pinned by cxxtest phase152.
- `✓` Closed in Ф31e-g: `aligned_storage_t` and `aligned_union_t` were the only
  members of the C++14 transformation-alias set absent — deprecated in C++23 and
  still required by it, which is precisely why nothing here had needed them. Note
  the default `Align`: [meta.trans.other] makes it the most stringent alignment
  any type of size ≤ `Len` could require, not `alignof(max_align_t)` flat, so
  `sizeof(aligned_storage_t<1>)` is 1 and not 16. Closed
  `__cpp_lib_transformation_trait_aliases`.
- `✓` Closed in Ф31e-a: `is_swappable_v` was false for **every** array type, which was
  not merely an inaccurate answer — it broke a working operation.
  `is_swappable_v<int[3]>` said false while `std::swap(int[3], int[3])` worked, and
  `std::swap(int[2][2], int[2][2])` did not compile at all. The mechanism:
  `<type_traits>` declares the generic `swap(T&, T&)` itself and detects
  swappability by unqualified lookup, so scalars and class types answered
  correctly; the *array* overload lived only in `<utility>`, and a built-in array
  has no associated namespace for ADL to reach, so the trait could never see it.
  The nested case then fell over because the array overload is constrained on
  `is_swappable` of its **element** type — element `int[2]` answered false. The
  fix declares the array overload beside the scalar one, where the trait looks.
- `+` `is_virtual_base_of` (P2985R0) is provided, gated on
  `__builtin_is_virtual_base_of`. There is no library fallback and there cannot
  be one: `is_base_of` is true either way, and the cast that would tell the two
  apart is ill-formed exactly when the answer is "virtual". The macro appears
  only when the builtin does.

## `<typeindex>`

- Complete: `type_index` with all five members and `hash<type_index>`. New in
  Ф38; the header used to be in §1.1.
- The order is **pointer order over mangled names**, because that is what
  `type_info::before` is here (see `<typeinfo>`: a BoxOS binary is one static
  image and vague linkage merges every duplicate typeinfo, so pointer identity
  *is* type identity). Comparing unrelated pointers with `<` is unspecified in
  the abstract machine and a total order on one flat address space; libstdc++
  makes the same trade whenever it may assume merged typeinfo names, and the
  alternative — `strcmp` per comparison — would cost a string walk at every
  tree node and buy nothing.
- What makes that order *safe* rather than merely conventional: `==` compares
  the `type_info` addresses and `before` compares their name addresses, so the
  two could in principle disagree — but only if two distinct `type_info`
  objects shared a name pointer, which cannot happen, since distinct types have
  distinct mangled names and a linker merges identical string contents, never
  differing ones. Phase192 pins the consequence directly rather than the
  argument: irreflexivity, asymmetry, transitivity and `<=>`/`==` agreement
  over every pair and every triple of a twelve-type zoo, then the `set`, `map`
  and `unordered_map` that would silently drop keys if any of it were false.
- `hash<type_index>` is `hash_code()`, which is FNV-1a over the mangled name.
  It is not required to agree with any other implementation's, and does not.

## `<utility>`

- `✓` Closed in Ф34: **`pair`'s comparisons were same-type only**, so
  `pair<int,double> == pair<long,float>` did not compile and neither did the
  `<=>`. [pairs.spec] has specified both as heterogeneous since C++11.
- `✓` Closed in Ф34 (P2944R3): the `operator==` of `pair`, `tuple`, `variant`
  and `expected` were **unconstrained**, so `equality_comparable<tuple<T>>`
  answered *true* for a `T` with no `operator==` and the failure arrived later,
  inside the operator, where no concept could see it. `optional` was already
  constrained. `expected`'s value-comparison overload also needed
  [expected.object.eq]'s "not a specialization of expected" constraint, without
  which the check re-enters itself — GCC reports that as "satisfaction of atomic
  constraint depends on itself", and the recursion is real, not a quirk.
- `✓` Added in Ф34: structured bindings for `integer_sequence`
  (`tuple_size`/`tuple_element`/`get`). It is deliberately NOT tuple-like:
  [tuple.like] stays the closed set, so `pair`'s tuple-like constructor still
  rejects it. `get` returns by value, and `tuple_element` does not add `const`
  for a const sequence — the elements are values, not members.
- `~` `in_range<char>(1)` compiles. [utility.intcmp]/5 makes the integer-comparison
  functions ill-formed for `char`, `bool` and the character types; both reference
  libraries diagnose it.
- `+` `constant_wrapper` / `cw` (P2781R9) are implemented **including LWG 4383**,
  which libstdc++ 16.1 does not have: there the pseudo-mutators are declared for
  every wrapper and fail inside a template argument's constant evaluation, which
  is not the immediate context — so `requires { ++std::cw<1>; }` is a hard error
  rather than `false`. Here the operators are constrained on the mutation being
  valid for a mutable copy, `++cw<1>` is `cw<2>`, and asking is a question. This
  is why the macro reads 202606 and not 202603.

## `<valarray>`

Provided as of Ф44-f. Until then it sat in §1.1 with "Excluded by plan" written
where the reason goes.

**An expression is a work order, and a crew carries it out.** `d = a + b * c`
computes nothing when it is written: the operators return small objects that
describe the arithmetic, and the pass over memory happens once, where the order
is written into an array. [valarray.syn]/3 permits exactly that, and both
reference libraries do it too. What neither of them does is hand the pass to
the strands `std::execution::par` runs on, which is what happens here once an
array is long enough to be worth a wake — twice the brigade's grain, 4096
elements. Below that the loop runs on the calling strand without asking `__par`
at all, so a program whose only valarray is six elements long never acquires a
crew of strands to add them up.

Fusing and splitting are the same decision made twice: three operators become
one region instead of three, and a region costs a wake and a barrier as well as
a pass over memory.

- `~` **`sum()` folds one partial per chunk, in chunk order.**
  [valarray.members] says the result is computed "in an unspecified order", so a
  parallel reduction is conforming here rather than merely faster — but for
  values that round, a parallel fold and a left-to-right fold are different
  doubles, and both references do the latter. Code that needs the sequential
  answer bit for bit should not ask a valarray for it. `min()` and `max()` split
  the same way and cannot differ, being order-independent.
- `~` **`apply()` may call its argument on several strands at once**, where both
  references call it once per element on one thread. [valarray.members] fixes
  neither the order nor the thread — only that each element is assigned the
  value of applying the function to the corresponding element. A function with
  state of its own is the caller's business.
- `?` **An overlapping self-assignment through a subset is unspecified, and here
  it additionally depends on where a chunk boundary fell.** `v[slice(0,3,2)] = v * 2.0`
  writes elements it has not read yet; the standard says nothing about the
  order, the references do it left to right, and a crew does it in pieces.
  Elementwise self-assignment — `v += v`, `v = v * 2.0` — is safe under any
  chunking, because an order reads element *i* to write element *i*, and that
  is checked on sixteen cores by `phase237`.
- `?` **`shift`, `cshift` and the four const subset subscripts materialise**
  rather than returning an order. Their declared return type is `valarray<T>`,
  and a permutation that stayed lazy could read an element the same statement
  had already written.
- `~` **`auto x = a + b;` gives you the order, not an array**, and it dangles if
  what it reads goes away. This is the standard's own hazard and both references
  have it; name the type and the order is carried out on the spot.
- `+` **The replacement types carry the whole const surface [valarray.syn]/3
  requires** — the unary operators, `sum`, `min`, `max`, `shift`, `cshift`, both
  `apply`s and the four subset subscripts, so `(a + b).sum()` and
  `(a * b)[mask]` compile. libc++ 22.1.6 does not manage this for floating-point
  elements (§4).
- `?` **The four subset proxies take a scalar for `=` and for nothing else.**
  `v[m] = 1.0` compiles and `v[m] += 1.0` does not, because
  [slice.arr.comp.assign] and its three siblings declare the compound
  assignments for `const valarray<T>&` alone. The asymmetry is the standard's;
  it is recorded here because it looks like an omission in this library until
  you check that no other library accepts it either.
- `?` **`slice`, `gslice`, and the proxies' index maps are borrowed, not owned**,
  except a mask's — which cannot be indexed without being counted first, so
  `v[mask]` allocates its position list once per subscript. Every proxy is
  transient by design, and the standard's model is the same.

## `<variant>`

- `?` `visit` is an O(N) constant-evaluated index match, not an O(1) function-pointer
  table — so a visit over *k* variants costs O(Nᵏ). The standard mandates no
  complexity here. It was chosen for constexpr cleanliness and small alternative
  counts; a variant with dozens of alternatives on a hot path would want the table.

## `<vector>`

- `✓` Closed in Ф31e-a: `vector<bool>::iterator` did not model
  `std::output_iterator<…, bool>`, so every algorithm constrained on an output
  iterator rejected the container. The precise cause: `indirectly_writable`
  requires `const_cast<const iter_reference_t<Out>&&>(*o) = t` to be valid, and the
  proxy had no const-qualified `operator=(bool)` — the one P2321R2 added for
  exactly this. `__cpp_lib_ranges_zip` was already defined, so the paper was being
  claimed with this piece of it missing; its other pieces (the views, and the
  const-qualified `pair` assignment and swap) were in place.
  (`vector<bool>` **is** the packed specialization — `size_t` word
  storage with a proxy reference — despite what the header's own banner said until
  this document was written.)
- `✓` Closed in Ф31e-a: `vector<bool>::swap` did not exchange allocators, so
  `propagate_on_container_swap` was ignored on this specialization while the
  primary template honoured it, leaving each vector holding words allocated by the
  other's allocator. Shown by differential probe, and re-run after the fix: with a
  POCS allocator that is not copy-assignable, `vector<int>::swap` and
  `vector<bool>::swap` now both fail to compile at the allocator assignment, where
  before only `vector<int>` did.
- `✓` Closed in Ф39: `vector` was not usable in a constant expression at all
  (P1004R2 unimplemented). All of it is `constexpr` now, `vector<bool>` and its
  proxy `reference` included, and `__cpp_lib_constexpr_vector` is claimed at
  201907L. The primary template needed nothing but the annotations — no union,
  no pointer question a constant expression refuses. `vector<bool>` needed one
  thing: a bit is set by reading a word back and OR-ing into it, and **the
  content of storage nobody has written is not usable in a constant
  expression**, so freshly allocated words are zeroed when constant-evaluated
  (and not at run time, where the read is fine and the fill would be waste).
  libstdc++ does the same, for the same reason.
- `~` A `constexpr std::vector` **variable** is impossible, at any size: its
  elements always come from an allocation, and no allocation outlives constant
  evaluation. `std::string` differs only because a short one has somewhere else
  to live. Both mainstream libraries agree; measured.
- `!` **[vector.cons]'s `initializer_list` constructor is split into two** —
  `vector(initializer_list<T>)` delegating to `vector(initializer_list<T>,
  const Allocator&)` — where the standard writes one signature with a defaulted
  allocator. The reason is a compiler crash, not a design preference:
  x86_64-elf-g++ 15.2.0 dies with "internal compiler error: in
  cxx_eval_indirect_ref, at cp/constexpr.cc:6169" on
  `vector<vector<vector<int>>> v{{{1, 2}, {3}}}` once the constructors are
  `constexpr`. Two levels of braces are fine, four crash as well, and the
  compiler builds the default argument as an allocator of the wrong element
  type and reaches through a cast to it. Measured to be unaffected by
  `[[no_unique_address]]`, by the try/catch, by how the member initializer
  spells the copy, and by the default argument's form; it goes away with a
  by-value parameter (which the standard does not allow) or with no default
  argument (which is what shipped). No conforming program can distinguish two
  constructors from one with a default argument — a constructor has no address
  to take — and the alternative is a crash in ordinary user code. Pinned by
  phase198's `Nested()`. Revisit when the toolchain moves.

# 3. Where boxcxx is stronger than the reference implementations

A conformance document that only listed shortfalls would be dishonest in the other
direction. These are places where boxcxx follows the standard and a reference
implementation does not.

**Everything in this section was re-measured on 2026-08-14** against the
toolchains installed then: **libstdc++ 16.1.0** (`__GLIBCXX__ 20260430`) and
**libc++ 22.1.6** (`_LIBCPP_VERSION 220106`). Where an older measurement has since
been fixed upstream, it is not claimed here — several were dropped for exactly
that reason, and §4 says which.

**The grapheme entry below is newer and its libc++ column is a different
build:** Ф43-e-2 measured on 2026-08-21 against libstdc++ 16.1.0 and the libc++
this machine now has, **19.1.2** (`_LIBCPP_VERSION 190102`). Nothing older is
re-claimed for 19, and nothing there is claimed for 22.

## Exception safety of the flat containers — the sharpest one

When an element operation throws part-way through an insertion, all three
libraries must leave a container that still satisfies its own invariant
([flat.set.overview]/6, [flat.map.overview]/5.1). Reproduced today:

| | `flat_set`, throw during the shift | `flat_map`, throw in a single-element insert |
|---|---|---|
| **libc++ 22** | leaves **duplicates** — `[10 20 20 30 …]`, 9 violations in 20 throw points | leaves **`keys=5, values=6`** — 12 violations in 24 throw points |
| **libstdc++ 16.1** | clears — 0 violations, but also discards data when nothing was touched | clears — same |
| **boxcxx** | untouched if the throw preceded any growth; invariant restored otherwise | same, for both containers |

The libc++ `flat_map` case is the serious one: because the two backing containers
desynchronize, every value after the insertion point shifts by one. The map keeps
working and answers lookups with the wrong values.

## `<format>`

- **`enable_nonlocking_formatter_optimization` carve-outs.** The blanket rule in
  [format.formatter.spec]/3 has exceptions written in *other* subclauses.
  [time.format]/8 gives `duration<Rep,Period>` the trait of its `Rep` — so
  `chrono::seconds` is **true** — and [format.tuple] makes the tuple case the
  conjunction of its elements — so `pair<int,int>` is **true**. libc++ 22 answers
  false to both. libstdc++ 16.1, which gained the trait after this work was done,
  answers exactly as boxcxx does on all three carve-outs including `thread::id` —
  an independent confirmation of the reading. The three implementations are not in
  full agreement, though: on `sys_time` libstdc++ 16.1 answers **true** while
  boxcxx and libc++ both answer false. That cell is recorded as a divergence, not
  claimed as a win.
- **`{:m}` on a range of pairs.** Table 115 says the element is formatted as if
  `m` were specified for its tuple type. boxcxx and libstdc++ give
  `{1: 2, 3: 4}`; libc++ gives `{(1, 2), (3, 4)}`.
- **`{:#.0f}` of `1e308`.** libstdc++ 16.1 still misplaces the decimal point
  (the output ends `…311833.6` — a digit after the point at precision 0).
  boxcxx matches C's `printf("%#.0f")` and the standard.
- **Precision on a floating-point-rep duration.** `{:.3}` of
  `duration<double>{1/3}`: boxcxx applies it (`0.333s`), libstdc++ 16.1 ignores it
  (`0.333333s`) — contradicting its own stream path, where
  `ostringstream.precision(3) <<` does produce `0.333s` — and libc++ truncates the
  formatted string instead.

## The flat containers

| Point | boxcxx | libstdc++ 16.1 | libc++ 22 |
|---|---|---|---|
| Four iterator-pair deduction guides for `flat_set`/`flat_multiset` | present | **absent** | present |
| `explicit` on the container constructor | present | **lost** (copy-initialization compiles) | present |
| Heterogeneous `insert(K&&)` must **not** exist on `flat_multiset` ([flat.multiset.defn] has no such member) | correctly absent | **leaks in** — `flat_multiset<string,less<>>::insert(string_view)` compiles and inserts | correctly absent |
| Heterogeneous `try_emplace` / `insert_or_assign` ([flat.map.modifiers]/21) | works | **does not compile** — the body spells `keys.insert(key_it, move(k))`, handing a `string_view` to `vector<string>::insert` | works |
| Construction from an already-sorted range must be linear ([flat.set.cons]/2), N = 1000 | **1 998** comparisons | **11 620** (N log N) | 3 005 |
| `erase_if` must call `pred(as_const(e))` ([flat.set.erasure]/2) | const | **mutable** | const |

## `<regex>` — the engine answers instead of hoping

Both references match with a backtracker, which is the `timeout` paradigm in
the world of strings: it hopes it will finish in time. Measured with a clock,
one process per case, in `tools/cxx_regex_oracle.sh --adversarial`:

| Pattern, subject `a`×n | libstdc++ 16.1 | libc++ 22.1.6 | boxcxx |
|---|---|---|---|
| `(a+)+b`, n=12 | 0.531 ms | 1.319 ms | **0.023 ms** |
| `(a+)+b`, n=18 | 33.4 ms | refuses, `error_complexity` | **0.013 ms** |
| `(a+)+b`, n=22 | 528 ms | refuses | **0.014 ms** |
| `(a+)+b`, n=26 | **8 777 ms** | refuses | **0.016 ms** |
| `(a\|a)*b`, n=26 | no answer in 10 s | refuses | **0.013 ms** |
| `(a*)*b`, n=18 | no answer in 10 s | refuses | **0.016 ms** |
| `(a\|ab)*c`, n=26 | 0.037 ms | 0.089 ms | 0.013 ms |

The answer to every question in that table is a trivial *no*. libstdc++ has no
ceiling at all — its column multiplies by sixteen for every four characters
added, which is the doubling-per-character an exponential search does — and
libc++ has a ceiling it reaches on inputs a person would type. The linear
machine is not faster at the same thing: it is answering questions the other
two cannot finish, and it does so because the time a match takes is set by the
shape of the input rather than by luck. The last row is the control — a shape
that blows up in nobody's engine, where all three are in the same tens of
microseconds and the difference is not the point.

‼ **The numbers in that table were re-measured for it rather than carried
here.** The figures this session started with, taken from Ф44-0's notes, said
5.0 s for the n=26 row and put libc++'s refusal at n=13. Today's run says
8.8 s, and libc++ answers at n=12 and refuses from n=18. A table of timings
copied from a previous session's notes is a table of that session's machine
load.

‼ **A prediction of mine that the measurement refuted**, recorded because it
shaped the design argument: I expected a backtracker to blow the fixed stack of
a cabin. Neither library dies on the stack — both survive a 200 000-character
subject. The damage is on the CLOCK, not on the stack. The argument for the
linear engine is unweakened (a cabin hung on eighteen characters is no better
than one that crashed), but it is a different argument than the one I started
with.

## Elsewhere

- **`span::crbegin` / `crend`.** [span.overview] declares them, with inline bodies.
  libc++ 22 does not provide them.
- **P2165R4.** `is_constructible_v<tuple<int,int>, array<int,2>>` is true in boxcxx
  and libstdc++; libc++ 22 fails to compile it.
- **`<complex>` Annex G directed special values.** G.6.2.1 specifies
  `acosh(±0 + iNaN)` as `NaN ± i(π/2)` — the imaginary part is *given*, and the
  adjacent "finite nonzero x + iNaN → NaN + iNaN" bullet explicitly excludes zero.
  **Both** libstdc++ 16.1 and libc++ 22 return `(NaN, NaN)`. boxcxx implements the
  carve-out.
- **Extended grapheme clusters, where each reference gets a different one wrong.**
  Ф43-e-2 gave `<format>` the estimated width of [format.string.std]/13. The
  rules pass all 766 cases of Unicode's own GraphemeBreakTest-17.0.0; they were
  then fuzzed against both references over 40 000 random strings, and the
  disagreements are these three, all reproduced today (libstdc++ 16.1.0,
  `__GLIBCXX__ 20260430`; libc++ 19.1.2, `_LIBCPP_VERSION 190102`):

  | case | boxcxx | libstdc++ | libc++ | rule |
  |---|---|---|---|---|
  | `"\r\n"` | **1** | 1 | 2 | GB3 — CR LF is one cluster |
  | `L"\uAC00\u0301"` (LV + a mark) | **2** | 3 | 2 | GB9 — × Extend, whatever precedes |
  | `"\u0600\U0001F1FA\U0001F1F8"` (Prepend + flag) | **1** | 2 | 2 | GB13 — `[^RI] (RI RI)* RI × RI` |

  The third is the interesting one, because **both** references break there and
  neither the conformance suite nor any spec text says they should. GB13 asks
  what precedes the break point: an Arabic number sign is `[^RI]`, the first
  regional indicator is the `RI`, and the pair holds. libc++'s state machine
  shows why it does not — the "active rule" it uses to remember an RI run is
  never armed when GB9b is what pulled the previous character in — and
  libstdc++ answers the same. Over 40 000 strings this was the ONLY case where
  boxcxx and an agreeing pair of references differed: 36 hits, every one of them
  a Prepend directly before two regional indicators.

- **Five entities of the freestanding subsets that NEITHER reference library
  has.** Ф43-f's audit named them and they had to be written before the macros
  could be claimed: `memccpy` and `memset_explicit` ([cstring.syn]),
  `memalignment` ([cstdlib.syn]), `WCHAR_WIDTH` ([cwchar.syn]) and the whole of
  **[ptrtag]** — `pointer_tag_pair`, `pointer_bits_available`,
  `max_pointer_bits_available` and the tuple interface — plus `start_lifetime`
  ([memory.syn]). Measured on libstdc++ 16.1.0 and libc++ 19.1.2: not one of
  them is present in either.

  libstdc++ defines seven `__cpp_lib_freestanding_*` macros and libc++ defines
  none; boxcxx defines all twenty-five, plus `__cpp_lib_ratio`. That is not a
  claim about being more complete in general — it is a claim about having run
  the audit, which is why the audit ships with it as
  `tools/cxx_freestanding_audit.sh` and refuses an overclaim.

- **Precision on an escaped string is monotone.** `format("{:.2?}", "你")` is
  `"` here and in libc++: the opening quote is one column, the ideograph would
  make three. libstdc++ prints `"你"` — four columns — while its own answer at
  precision **3** is `"你`, three. A longer prefix at a smaller precision is not
  a defensible reading of [format.string.std]/14 in either direction.

---

# 4. Defects found in the standard and in the reference implementations

## In N4950 itself

**[flat.multiset.defn] — both iterator-pair deduction guides are unusable as
published.** They are written

```
template<class InputIterator, class Compare = less<iter-value-type<InputIterator>>>
  flat_multiset(InputIterator, InputIterator, Compare = Compare())
    -> flat_multiset<iter-value-type<InputIterator>, iter-value-type<InputIterator>, Compare>;
```

so the second template argument — the *container* — is deduced as the key type and
the comparator lands in the container slot: instantiating the deduced type is a
hard error. The corresponding `flat_set` guides are written correctly with two
arguments. There is no LWG issue; the current working draft carries the corrected
form and libc++ implements the corrected form, so this is editorial. boxcxx
implements the corrected two-argument form.

**[flat.map.modifiers]/12 is internally inconsistent.** The constraint on
`insert_range` admits any range whose reference type is convertible to
`value_type`, which includes tuple-like ranges such as `views::zip(keys, values)`;
but the Effects clause reads `e.first` and `e.second`, which such a range does not
have.

boxcxx follows the letter, and **is alone in doing so**: measured today,
`flat_map(from_range, views::zip(k, v))` builds and runs on both libstdc++ 16.1 and
libc++ 22, and fails on boxcxx at `flat_map:746` with
"`tuple<int&, char&>` has no member named `first`". Both libraries chose to make
the motivating example work; boxcxx implements the clause as written.

This is recorded here rather than quietly "fixed", because twice during this epic a
proposal to change it *because the other library does it* was withdrawn after
reading the normative text. It is nevertheless a usability gap on our side, and the
honest summary is: the standard is inconsistent, and boxcxx picked the half that
loses a useful construction.

**[tuple.rel] as written makes `tuple<int>{1} == tuple<int>{1}` ambiguous** once
the C++23 tuple-like `operator==` is added, because the new overload is no worse
than the homogeneous one and the text carries no exclusion. libstdc++ suppresses
it with a tuple-specific guard wider than *different-from*; boxcxx does the same.
This was found by a failing test, not by reading.

## In libstdc++

Present in 16.1.0 (`__GLIBCXX__ 20260430`), all reproduced today: the six rows of
the flat-container table in §3, `{:#.0f}` of `1e308`, and the ignored precision on
floating-point-rep durations.

**One from Ф44-0, and it was found before boxcxx had a regex engine at all:**
**`^` inside a lookahead is true everywhere.** All six non-back-reference
disagreements of the tool's first sweep reduce to four shapes — `/a(?=^)/`
against `"ab"` matches and must not, `/a(?!^)/` does not match and must,
`/.{0,2}(?=^)/` reports length 2 where the assertion should force the greedy
quantifier back to 0, and `/ab(?=^)/` matches. libc++ is right in all four, so
on anchors inside lookaheads the oracle is libc++ and not libstdc++ — which is
the whole reason a differential stand is drawn before the code it will judge.

**Two more, from Ф43-e-2's grapheme work** (§3 has the table): a Hangul syllable
followed by a combining mark is segmented as **two** clusters, where GB9 joins any
Extend to whatever precedes it — `L"\uAC00\u0301"` measures 3 columns there and 2
everywhere else; and precision on an escaped string is **not monotone** —
`{:.2?}` of `"你"` yields a longer answer than `{:.3?}` does.

**Three more, from Ф45's zone work**, all adjudicated by `zdump` — tzcode's own
dumper, reading the very files tzcode's own compiler produced from the release
both sides were pinned to. Over 919 417 instants across all 598 zones, 3 066
answers differed from ours and 3 052 of them went this way:

- **A Zone record's `%s` is not re-expanded across the record boundary.**
  Pacific/Auckland changed from NZMT to NZST on 1946-01-01, and libstdc++ keeps
  saying `NZMT` for years afterwards. 122 instants in the Auckland family alone.
- **A wall-clock `UNTIL` is converted with the STANDARD offset instead of the
  one in force.** Europe/London's records meet at `1968 Oct 27`, which is
  1968-10-26T23:00Z because Britain was on BST that day; libstdc++ places it at
  00:00Z and reports the old record for the hour between. America/Asuncion's
  `2024 Oct 15` misses by the same hour, Africa/Tripoli's 2013 boundary by
  another. This is the same approximation our own generator makes and then
  corrects by snapping each boundary to the transition zic actually emitted.
- **Historical offsets that disagree with the compiled data outright** —
  Africa/Algiers in 1977 comes back as WET where the file, `zdump` and
  `zoneinfo` all say CET.

The local-time question inherits all three: of 800 872 wall-clock readings, the
8 172 where we differ are the same 262 zones, and CPython's PEP 495 machinery —
reading our compiled tree, so the data cannot be the difference — sides with us
in every one.

**Fixed since the epic measured them against 15.2 — no superiority is claimed:**
`{:<010p}` zero-padding despite an explicit align; `{:%F}` of `year{-43}`
disagreeing with its own `%Y`; `iterator_traits<flat_map::iterator>::value_type`
being `pair<const K,T>`; `%H` of `hours{300}` truncating to 8 bits; the absence of
`ranges::starts_with` / `ends_with` / `shift_left` / `shift_right`; and the
`<bitset>` string constructor not validating the whole string.

## In libc++ 22.1.6

The two flat-container invariant violations above (both reproduced today), the
`{:m}` tuple-element rule, the missing `span::crbegin`/`crend`, the
`enable_nonlocking_formatter_optimization` carve-outs, P2165R4, and — from the
escape-sequence work — three cases in `write_escaped.h` where a character is
treated as "previously escaped" (`U+0020`, a non-delimiter quote, and an
ill-formed run) and is then printed raw.

**Two from Ф44-a, both in the grammar:** `\q` is rejected with
`error_escape`, where [re.grammar]/3 makes IdentityEscape wider than
ECMA-262's and admits it; and `[]]` does not parse, where `[]` is an empty
class and the `]` that follows is an ordinary member. libstdc++ is right on
both. **And one in the engine:** `(a*)*b` is refused with `error_complexity` on an
eighteen-character subject — a real ceiling rather than a hang, which is the
better of the two failures, but the question it refuses has the trivial answer
*no*. Measured: it still answers `(a+)+b` at twelve characters and refuses from
eighteen on (§3 has the table).

**One from Ф44-f, and it is a whole clause of [valarray.syn] rather than a
value:** `shift` and `cshift` **on an expression do not compile for a
floating-point element type.** [valarray.syn]/3 permits a replacement type only
if "all the const member functions of valarray<T> other than begin and end are
also applicable" to it, and libc++'s `__shift_expr` computes its element
branchlessly —

```
(__expr_[(__i + __n_) & __m] & __m) | (value_type() & ~__m)
```

— which is arithmetic no `double` has. `valarray<double>::shift` itself is
fine; it is the replacement type that is not, so every floating-point valarray
expression in libc++ fails that paragraph. `(a + 1.0).shift(2)` is rejected at
compile time, which is why the stand's own case prints a placeholder in that
column (pinned in `tools/valarray_oracle_refdiff.txt`) rather than pretending
the columns agree.

**From Ф43-e-2, measured on 19.1.2** (`_LIBCPP_VERSION 190102`, the libc++ this
machine has; the rows above are from 22.1.6 and are not re-claimed for 19):
**GB3 is not applied** — a CR LF pair measures two columns where the rule makes
it one cluster — and a lone wide character is measured as one column rather than
by its own width.

## In both of them

**GB13 loses a regional-indicator pair to a Prepend.** `"\u0600"` followed by two
regional indicators is one cluster by the rule and two in each library; §3 has
the derivation and libc++'s state machine shows the mechanism. Found by fuzzing,
not by reading: it was the only disagreement left in 40 000 random strings after
everything else matched.

# 5. `box::` — the BoxOS-native surface

Everything above is about the ISO library. `include/box/cxx/` is the other half of
boxcxx: 33 headers that give BoxOS's own concepts a C++ face. They are not
replacements for standard facilities and they do not shadow them — they exist
because the kernel has ideas Unix does not, and a standard library has no words
for them.

| Area | Headers | What they add |
|---|---|---|
| I/O spine | `current.h`, `console.h`, `keyboard.h`, `line.h` | The BoxOS replacement for stdin/stdout/stderr: typed channels, colour, event-driven keys. |
| Events | `touch.h`, `reflex.h`, `system_touch.h` | Tag-multicast event subscription (REST / REACT / INTERRUPT) — the kernel wakes you; nothing polls. |
| Execution | `strand.h`, `executor.h`, `ferry.h`, `timing.h`, `timeouts.h` | In-cabin execution contexts, a cooperative executor, `co_await`-able async file I/O. |
| Memory | `heap.h`, `bay.h`, `bay_memory_resource.h`, `memtag.h`, `pku.h`, `hw.h` | Tagged allocation and accounting, cross-cabin shared memory, tagged-RAM introspection, protection keys, LAM/TME. |
| Storage | `tagfs.h` | Tag-addressed files: query by tags, RAII contexts and snapshots, anchor events. |
| Time | `clock.h` | Which reckoning of time the machine keeps: validate a zone name against the database, store it under `clock:zone`, announce the change. Reading it is `std::chrono::current_zone()`; only the writing half needed a name. |
| Messaging | `message.h`, `brook.h` | Process-to-process messages and ordered SPSC streams. |
| System | `process.h`, `child.h`, `system.h`, `manifest.h`, `cpu.h` | Process and cabin identity, child supervision, firmware/EFI introspection, the expert syscall builder. |
| Data | `flat_hash_map.h`, `flat_hash_set.h`, `math.h`, `error.h` | A robin-hood table, an angle-and-base maths layer, the native error model. |

Two of these are close enough to standard facilities that their differences matter.

## `box::flat_hash_map` / `box::flat_hash_set`

An open-addressed robin-hood table in one allocation, offered *alongside*
`std::unordered_map`, not as a replacement. Measured on BoxOS with
`box::heap::tag` at N = 2000: **1 block, 24 bytes per element**, against
`std::unordered_map`'s **2001 blocks, ~40 bytes per element** — a node map
allocates once per element, this allocates once.

Every difference below is deliberate, and each is verified in the tree:

- `–` No bucket interface, no `node_handle`, and therefore **no `merge`** (the
  standard defines `merge` in terms of `extract`).
- `–` No `multi` variants: robin-hood displacement and duplicate keys cannot both
  hold the table's invariant.
- `~` `value_type` is `pair<Key, T>`, not `pair<const Key, T>` — it follows
  `std::flat_map`, because a const key cannot be relocated. `*it` is a proxy, so
  `for (auto& [k, v] : m)` does not compile; use `auto` or `auto&&`. This is the
  same shape as the already-shipped `std::flat_map`.
- `~` Zero reference and iterator stability: any mutation may relocate.
- `~` `max_load_factor()` is the constant `0.875f`, not a setter.
- `~` `max_size()` is capped at 2³¹ slots — the spare control bit that makes probe
  distance overflow-proof costs one bit of the hash word.
- `~` A throw while relocating restores the invariant by clearing, following the
  `std::flat_set` doctrine already in the tree.
- `+` `m.fallible()` returns a facet whose allocating operations answer with
  `box::result` carrying the kernel's actual reason, instead of throwing.

The one thing to know before writing a loop: a hand-written erase-during-traversal
can see an element twice if a probe run crosses the seam of the array.
`box::erase_if` does not have this problem (its scan starts from an empty slot),
and bulk removal goes through it.

## `box::error` / `box::result`

BoxOS error codes are not `errno`. `box::error` carries the kernel's native code,
`box::result<T>` is the fallible return type, and neither maps onto
`std::error_code`'s POSIX categories. `<system_error>` exists and is conformant;
it is simply not what BoxOS itself speaks.

---

# 6. Open debts

These are known, recorded, and not yet closed. They are listed because a
conformance document that only lists finished work is an advertisement.

## The one that covers everything

**No run on physical hardware.** The entire C++ epic has been verified under
emulation only — QEMU and Bochs, BIOS and UEFI, 1 and 16 cores, `-cpu max`.
Every gate in this document means "green in that matrix". Several paths are
structurally dormant there and can only be exercised on real silicon:

| Path | Why it sleeps under emulation |
|---|---|
| CET shadow-stack `INCSSP` in the unwinder | TCG does not enforce IBT/SHSTK |
| PKU access faults | QEMU publishes `CR4.PKE` but does not trap |
| `WAITPKG` `UMONITOR`/`UMWAIT` | not exposed by TCG on this host |
| LAM tagged-pointer masking, TME | not present under TCG |
| x87 transcendentals at genuine 80-bit width | QEMU narrows them to host double; Bochs and real hardware do not |

## Library debts

Ф31e-a closed the six that answered wrongly in silence — the `<ostream>`
inserters, `is_swappable` over arrays, `get` on a const array rvalue, the
odd-traits `basic_string` formatter, `volatile` slipping past `formattable`, and
`vector<bool>`'s output-iterator and allocator-swap gaps. Ф31e-b-1 then closed
eight pieces of absent surface — `mismatch`'s four-iterator forms, `array<T,0>`'s
element and reverse members, `lerp`, three-argument `hypot`, the seven missing
range-access CPOs, `ssize` as a real CPO, `view_interface::cbegin`/`cend`,
`owner_less<void>`'s `is_transparent`, and `compare_three_way_result`'s default
argument — which between them made six feature-test macros honest. Each is marked
`✓` in §2 with what it used to do, and pinned by cxxtest phases 144 and 145.
Ф31e-b-2 then added `views::take_while`, `views::drop_while`,
`ranges::is_permutation` and a const-iterable `transform_view` (phase 146), and
Ф31e-c gave the whole header its [cmath.syn]/2 promoting overloads (phase 147).

Ф31e-d then took the concurrency corner (phase 148), which held the last debt in
this document marked as having real teeth: `atomic<shared_ptr>` and
`atomic<weak_ptr>` polled against empty notifies, and now park and wake on the
same substrate as every other atomic. It closed six feature-test macros with
them — the two smart-pointer atomics' own, `atomic_ref`'s missing
`difference_type` and compound assignments, the volatile half of the arithmetic
surface, six absent `atomic_flag` free functions, and `__cpp_lib_barrier`, which
had been withheld on a race that measurement had already disproved. Two claims in
this document were **wrong** and were corrected rather than closed: `<barrier>`'s
`[[nodiscard]]` is mandated by the synopsis, not an extension, and the volatile
gap was wider than the `atomic_float` entry said. `scoped_lock<Mutex>::mutex_type`
went with them, for a seventh.

Ф31e-e then closed the last of the missing `[ranges]` entities (phase 149): the
fourteen-name `ranges::` uninitialized-memory family, `subrange`'s range
constructors — which the previous entry had recorded as only two missing
deduction guides, when a guide with no constructor to deduce for is two gaps —
`range_rvalue_reference_t`, `range_common_reference_t`,
`range_adaptor_closure`, `std::get` over `subrange`, and `views::istream`. It did
**not** close `__cpp_lib_ranges`, and that is the honest outcome rather than a
shortfall: every entity exists, but the macro's C++23 value is contested (LWG
3931) and three of the four papers behind it are whole-clause relaxations this
library cannot claim by inspection. Two previously unrecorded deviations
surfaced while measuring: `zip`'s tuple-always shape, and `filter_view` accepting
a move-only predicate the standard rejects. Both are in §2.

Ф31e-f took the ordered associative containers (phase 150). They had no
relational operators at all — [associative.map.syn] declares only `==` and
`<=>`, with the other four synthesized from the latter, so one missing operator
took all four with it and this document had recorded only the one. With them,
`try_emplace` and `insert_or_assign` became 4-of-4 on both `map` and
`unordered_map`, where the latter's single `insert_or_assign` overload had been
**copying an rvalue key in silence**. Three more feature-test macros.

Ф31e-g-1 then took the small missing surface (phase 151): `span`'s iterator
constructors — which took pointers and, worse, were not `explicit` for a fixed
extent, so a `span<int,3>` could be built implicitly from a pair while
asserting a size nothing checked — and its range deduction guide;
`string_view`'s `crbegin`/`crend`; `resize_and_overwrite`;
`erase_if(basic_string&, Pred)`; the two `initializer_list` access functions;
`aligned_storage_t` / `aligned_union_t`; and the classic `sample` and
`shuffle`, the second of which was recorded nowhere. Seven more macros.

Ф31e-g-2 took lifetime and allocation (phase 152) and found the sharpest defect
of the whole sub-phase in code that had shipped and looked fine:
**`allocate_shared` never called the allocator's `construct` or `destroy`.**
[util.smartptr.shared.create]/7-8 draws the line deliberately — `make_shared`
placement-news, `allocate_shared` goes through `allocator_traits<A2>` — and
boxcxx placement-new'd on both sides, which is invisible for `std::allocator`
and is the entire feature for `polymorphic_allocator`: a `pmr` container built
that way silently keeps the *default* resource. `make_shared<const T>` did not
compile at all, and `make_shared_for_overwrite` was `return make_shared<T>();`
— value-initialising, the one thing the name promises not to do. With those:
`allocation_result` and both `allocate_at_least` entry points, `assume_aligned`,
`start_lifetime_as` / `start_lifetime_as_array`, the two [meta.member] functions
(`is_corresponding_member`, `is_pointer_interconvertible_with_class`), the four
missing array `allocate_shared` overloads, all three
`allocate_shared_for_overwrite` forms, a constexpr `default_delete` converting
constructor, and **`std::align`, which did not exist and was in no tracker** —
a C++11 function, and on a bare-metal target not a corner case. Eight more
macros. Two entries in this document were rewritten rather than closed:
`__cpp_lib_char8_t` is blocked by `<locale>`'s facet exclusion (P0482R6 adds
`codecvt<charN_t, char8_t, mbstate_t>`), not by `pmr::u8string` as recorded —
a reason that expired in its turn when Ф42-e built those two facets; what
blocks the macro now is in §2 `<locale>` — and
`__cpp_lib_is_implicit_lifetime` cannot be claimed at all — the trait needs
`__builtin_is_implicit_lifetime`, which this toolchain does not have, and no
library-only approximation can separate a user-provided destructor from a
member-induced non-trivial one.

Ф31e-g-3 took the five that needed a decision rather than a keystroke
(phase 153). Four of them were straightforward once decided; the first was not
what the backlog said it was. `<version>` recorded `__cpp_lib_tuple_like` as
blocked because P2165R4's converting machinery did not compile — measured, and
wrong: that machinery landed in `c5ba9fa` and works. The real gap was the
*other* half of the paper, and cxxtest phase137 had been carrying the correct
reason all along while the document carried the stale one: `apply`, `tuple_cat`
and `make_from_tuple` were still duck-typed on `tuple_size_v`/`get`, so they
accepted any type with the protocol and — worse — gave a hard error rather than
a clean rejection for a type with neither, because the arity was computed in the
function body. P2517R1's conditional `noexcept` on `apply` was missing with
them. Then: P2655R3's `basic_common_reference` for `reference_wrapper`, without
which `common_reference_t<reference_wrapper<int>&, int&>` has no type at all
(`COND-RES` is ambiguous — the wrapper converts to `int&` and `int&` converts to
a wrapper); P2404R3, which let a **move-only** type be compared with something
it converts from; P2539R4's four `ostream`-taking `print` overloads, the sole
reason `__cpp_lib_print` was undefined while the console forms had been complete
since Ф9B-2; and `pmr::basic_string`, which [string.syn] gives as an alias
*template* — boxcxx had the five concrete aliases spelled out and not the
template they are spelled through, so `pmr::u8string` was missing with it, and
`pmr::forward_list` too. Five macros. `stable_partition`'s `constexpr` came off
in the same step: C++23 does not ask for it (P2562R1 is C++26), and
`ranges::stable_partition` beside it already said so.

Ф32-a opened the C++26 uplift (phase 154). Its three pieces share one shape:
a promise written down and never wired up. LWG 2713's bucket-less allocator
constructors were missing from all four `unordered_*` containers while the
deduction guides for them had been standardised anyway — and the
`initializer_list` one of the three was not a compile error but a **silent
detour**, building a temporary container through a default-constructed allocator
and copying it. No associative container had an `initializer_list` deduction
guide at all, which the `const` in `pair<const Key, T>` turns from a redundancy
into `std::map m{std::pair{1, 2}}` not compiling, and which left
`std::set s({1, 2, 3}, alloc)` ambiguous besides. And P2495R3, in the tree since
Ф30e as an unannounced superset, was collapsed into one constructor where the
standard has three, which cost the `(t, allocator)` form entirely and made the
three-argument form `explicit`. Splitting it finished the paper, and its macro
is the first C++26 one this library claims (§1.3).

Ф39 made `basic_string` constexpr (phases 196 and 197), and what it had to fix
was not in `<string>`. The entry in `<version>` had blamed "the
compiler-blessed constexpr allocator path" for years; `std::allocator`,
`allocator_traits` and `construct_at` had all been constant-evaluable since
Ф31e, and measuring said so in a minute. Three real obstacles were underneath.
The inline buffer is a **union member**, and nothing in `<string>` ever wrote it
through its name, which is the only form that begins a union member's lifetime —
so every constant evaluation of a short string died on a buffer the compiler
considered uninitialized. `char_traits<char>::length` and `::compare` were bare
`__builtin_strlen` / `__builtin_memcmp`, which GCC folds over literals and
static arrays but **not over storage `std::allocator` handed out during constant
evaluation** — that one was already costing something visible before this phase:
a `basic_string_view` built over such storage was not a constant expression here
while it is one in both mainstream libraries. And two "does this pointer come
from my buffer" questions — `SelfOffset` and the direction choice inside
`char_traits::move` — were asked with `<` and `>=`, which is not a constant
expression between different objects; both now ask with `==` when
constant-evaluated and keep the address comparison at run time. `<bitset>`
needed no change at all: `__cpp_lib_constexpr_bitset` came back the moment its
blocker lifted, exactly as the note in its leaf had predicted.

The second commit of Ф39 did the same for `vector` (phase 198), and the header
itself needed nothing but the annotations — no union, no pointer question. Its
`<version>` entry had said "same reason" as `<string>`'s, and that was wrong in
both halves. `vector<bool>` was the one place with a real obstacle, and a
different one: a bit is set by reading a freshly allocated word back and
OR-ing into it, and the content of storage nobody has written is not usable in
a constant expression. Zeroing those words when constant-evaluated is the whole
of the fix, at no run-time cost.

What the commit also found is a **compiler crash**. With [vector.cons]'s single
`initializer_list` signature and its defaulted allocator, x86_64-elf-g++ 15.2.0
dies in `cxx_eval_indirect_ref` on three levels of nested braces, once the
constructors are `constexpr` — it builds the default argument as an allocator
of the wrong element type and reaches through a cast to it. Two levels are
fine; four crash too; libstdc++ 16.1 and libc++ 22.1.6 compile the same source.
Splitting that one signature into two constructors routes around it, and
phase198's `Nested()` is what keeps the shape tested.

What is left:

- `<ranges>`: **every entity `__cpp_lib_ranges` promises now exists** (Ф31e-e),
  and the macro is still not defined — for a reason that changed completely. Its
  C++23 value is not one number the field agrees on: LWG 3931 exists because
  *four* papers bumped this one macro, the C++23 working drafts reached 202302L,
  libc++ 22 reports 202211L under `-std=c++23`, and libstdc++ gates P2494R2 on
  `>= 202207L`. Of the four, only P2387R3 (`range_adaptor_closure`) is
  implemented and verified here; P2494R2, P2602R2 and P2609R3 are whole-clause
  requirements relaxations that cannot be established by inspection — the same
  bar that keeps `__cpp_lib_algorithm_iterator_requirements` undefined. Closing
  it is a verification task, not a missing-code one.
- `<cmath>`: `std::log10` is inexact on 1 of the 23 exact powers of ten. Measured
  on BoxOS; `box::log(x, 10)` returns 22 of 23 exactly by using `log10` directly.
- `<iterator>`: `incrementable_traits<common_iterator>` is not specialized. The
  non-standard `iterator_traits` specializations for `reverse_iterator` and
  `basic_const_iterator` (§2) are still load-bearing — they supply members the
  class bodies omit — so removing them is a design change, not a cleanup.
- Containers: the iterator-pair constructors carry no input-iterator SFINAE guard
  (diagnostics quality only), and the move constructors of `map`, `set` and the
  `unordered_*` family are hard-coded `noexcept` even when the comparator or hasher
  has a throwing move — so such a move terminates instead of propagating (§2).
  (`basic_string`'s unconditional `noexcept` is **not** a deviation:
  [string.cons] mandates it.)
- `box::heap::counters()` cannot see allocations of 8192 bytes or less — they come
  from the per-strand pool, and only the global-heap path increments the counters.
  **A test of the form "this does not allocate" is therefore not writable in
  BoxOS** for small blocks; such a claim must be pinned by reading the code and
  said out loud, which is what the diagnostic formatters do.
- `box::error`'s formatter allocates a `std::string`, unlike the five diagnostic
  formatters which render into a stack buffer.

## Substrate debts that surfaced through C++ work

- **`prefault_huge_locked` (`boxlib/src/memory.c`) issues a Manifest call while
  holding `heap_lock`.** The drain path inside can reach `malloc`, which wants the
  same lock. Pre-existing, not introduced by the C++ layer, and only reachable
  under contention; the real fix is an allocation-free drain path.
- One user-mode page fault was observed on a loaded 16-core BIOS run and has
  never reproduced in isolation. It is recorded rather than explained.
- `✓` **TagFS could not shorten a file, and nothing had noticed.** `tagfs_write`
  only ever grew one — `if (offset > file_size) file_size = offset` — so every
  shorter rewrite anywhere in BoxOS left the previous tail readable past the new
  content. This is not a C++ defect and predates the epic by years; what made it
  impossible to keep ignoring is that [filebuf.members] Table 122 defines plain
  `ios_base::out` as `"w"`, and `"w"` truncates. **Closed in Ф36** with a real
  primitive through all four layers: `tagfs_truncate_file` (frees the tail
  extents, trims the extent list, commits the new record before releasing a
  single block, unregisters the freed blocks from the dedup index and drops them
  from the read-ahead cache) → `STORAGE_OBJ_TRUNCATE` (guarded like
  `OBJ_DELETE`, and publishing a `TRUNCATED` Touch event with `op = 3` in the
  same 32-byte payload shape a write uses) → `file_truncate` → `current_resize`
  and the `CURRENT_TRUNCATE` open flag.
- **`tagfs_delete_file` does not unregister the blocks it frees from the dedup
  index.** Found while writing the truncate path above, which does unregister
  them. It is latent rather than live: inline dedup is disabled on the write
  path (`tagfs.c` says so, in a comment naming the block-reuse race that
  disabled it), so nothing currently consults a stale entry. Left alone rather
  than fixed in passing — it is one line in the most destructive path in the
  filesystem, and it deserves its own change with its own test, not a ride
  along a C++ header.
- **The file backing now accepts `CURRENT_READ | CURRENT_WRITE`.** Every other
  Current still takes exactly one role, and a Brook end explicitly rejects the
  combination — a producer and a consumer open different objects. A file is one
  object with one cursor, and `fstream(name, in|out)` needs exactly that; the
  spine's honesty rule ("never claim a capability the backing lacks") reads the
  other way too.

## Deliberately deferred to a future C++26 phase

This list named five things until Ф39 re-read it: `constexpr` `stable_sort` /
`stable_partition` / `inplace_merge` (P2562), `reserve_hint` (P2846),
`views::concat` (P2542), LWG 2713, and "the feature-test macro bumps to their
C++26 values". **Every one of them had shipped in Ф32** — the first three are
described in §3 by name, LWG 2713 in the Ф32-a paragraph above, and forty-six
macros now carry a C++26 value. The list had simply not been re-read since it
was written, which is the same failure mode §7 was written to prevent.

What is genuinely deferred is stated where it can be checked rather than
enumerated here: `<version>`'s own omission list names every undefined macro
with its reason, and the rule that governs the C++26 surface is that a macro is
defined only once its feature is complete. The open frontier beyond that is not
a debt — nothing promises it.

# 7. How the claims in this document were verified

Every statement here about **boxcxx** was checked against the tree as it stands,
not against notes from when the feature was written. The probe compiler is the
same cross compiler the library ships with:

```
x86_64-elf-g++ -std=gnu++23 -nostdinc++ -fexceptions -frtti -ffreestanding \
  -fno-builtin -I include/std -I include -I ../boxlib/include -I ../../include \
  -fsyntax-only <probe.cpp>
```

Every statement about **libstdc++** or **libc++** was re-measured on 2026-08-14
— and the one claim Ф36 added, that libstdc++ 16.1 defines
`__cpp_lib_fstream_native_handle` only at `-std=c++26` and at the same value
202306L, on 2026-08-16 —
against the toolchains installed on the build host — libstdc++ 16.1.0
(`__GLIBCXX__ 20260430`) and libc++ 22.1.6 (`_LIBCPP_VERSION 220106`) — by
compiling and running the same program against each. Claims that no longer
reproduce were removed, and §4 lists what was removed and why. Where a number
was measured against an older toolchain that is no longer installed, the version
is named at the claim.

Runtime behaviour of boxcxx cannot be observed by a syntax-only compile and is not
asserted here on that basis. It is pinned instead by the in-tree suite —
`src/userspace/apps/cxxtest.cpp` — which runs on BIOS and UEFI × 1 and 16 cores
with `-cpu max` before every commit. Its size is quoted in §"What the library is,
in numbers" and counted this way, so the figures are reproducible rather than
recalled: **phases** are the rows of the `kPhases` table `main` walks — phase 2,
which is proven by its translation unit linking at all rather than by anything
it does at run time, is an ordinary row of it as of Ф43 and no longer an
exception to the rule; **runtime checks** are `Check(`, `CheckText(`
and `CheckTextW(` call sites; **`static_assert`s** are occurrences of the keyword. Two of the three figures had drifted before Ф31e-d and were
re-measured there; the phase figure had drifted again by Ф32-a — the rule above
yields 170, not the 166 that incrementing the recorded 165 would have given — so
it is stated here as measured rather than as carried forward. The other two
reproduced exactly. The macro count is the one a translation unit actually sees:
`-dM -E` over a file containing only `#include <version>`, counting
`__cpp_lib_` defines — not a grep of the leaf files, which undercounts by one
because two macros are self-declared rather than defined in a leaf.

A claim about a *defect* is not written here from reading the new code either.
Each one in §2 marked `✓` was re-checked against the pre-fix headers — extracted
from git, put ahead of the current include path so only the header under test is
the old one — and had to fail to compile, or compile and give the wrong answer,
before the entry describing it was written. Ф31e-d ran twelve such probes and
Ф31e-e eleven. Two of the twenty-three came back the second way rather than the
first, which is the reason the method is worth its cost: `atomic<weak_ptr>`'s
three-argument `compare_exchange` compiled and silently used the wrong failure
order, and `subrange<Base*>` accepted a `Derived*` pair and would have strided by
the wrong element size.

## The trap that shaped this document

**A presence probe of the form `requires { some_call(...); }` reports success when
a *different* overload swallows the call, or when the constraint is satisfied and
the *body* is ill-formed.** Three claims flipped during this pass because of it,
one of them in a reference library:

- `std::mismatch(p, p, p, p)` compiles in boxcxx — the fourth `int*` binds to the
  `Pred` parameter of the three-iterator overload. The four-iterator overload does
  not exist.
- `std::get<0>(std::move(const_array))` compiles — it binds to the
  `const array&` overload and returns `const T&` instead of `const T&&`.
- `flat_map<string,int,less<>>::try_emplace(string_view, …)` satisfies its
  constraint on libstdc++ 16.1, so a `requires` probe called it supported; a real
  call fails inside `flat_map:537`.

**Presence is therefore asserted here only from a result type, a real
instantiation, or observed behaviour — never from "it compiles".**

## What is not verified

Physical hardware. Every result in this document comes from emulation (QEMU and
Bochs) or from host compilation. The paths listed in §6 that emulation cannot
exercise — CET shadow stacks, PKU faults, `UMWAIT`, LAM, TME, and genuine 80-bit
x87 transcendentals — are unverified in the strict sense, and are marked as such
rather than assumed to work.
