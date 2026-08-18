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
| Standard headers provided | **86** — 82 of C++23 (23 absent, §1) plus four of C++26: `<inplace_vector>`, `<debugging>`, `<stdbit.h>`, `<stdckdint.h>` |
| Internal implementation leaves (`include/std/__bits/`) | 123 |
| Header source | ~87 000 lines |
| Feature-test macros defined | 206 — 160 at their C++23 value, 46 carrying a later one (measured against libstdc++ 16.1 at `-std=c++23`) |
| BoxOS-native headers (`include/box/cxx/`) | 32 (§5) |
| In-tree conformance suite | `src/userspace/apps/cxxtest.cpp` — 213 phases (194 of them the numbered `PhaseN` series), 5 430 runtime checks, 1 816 `static_assert`s |
| Gate run on every commit | BIOS and UEFI × 1 and 16 cores, `-cpu max` |

The four counted rows drifted three times before the rule was written down, so
here it is: headers and leaves are the *regular files* in `include/std` and
`include/std/__bits` (`ls -1 include/std` returns one more than the header
count, because `__bits` is an entry too — the rule used to say plain `ls -1`,
and Ф38 re-derived every row and found that one off by exactly that
directory); macros are `#define __cpp_lib_` lines from `-dM -E` on a
translation unit containing only `#include <version>`; phases are `Phase*();`
call sites in `main`; checks are occurrences of `Check(` in `cxxtest.cpp` and
`static_assert`s are occurrences of the bare token `static_assert` (which is 21
more than `static_assert(`, the difference being the times the keyword is
named in a comment — the two rows were never counted the same way, and saying
so is cheaper than renumbering both). `tools/cxx_ftm_audit.sh` re-derives the
macro count and checks it against [version.syn] on every run.

The phase count has drifted twice, in both directions, so it is now stated
with the rule that produces it: `Phase*();` call sites in `main`, of which
there are exactly as many as there are phase definitions. That is **213**. The
166 recorded at Ф33 was a different count -- the numbered `PhaseN` series
alone, leaving out `Phase4a`, `Phase7b`, `Phase9a2`, `PhaseCurrent` and the
other suffixed ones -- so both numbers are given above and neither can drift
without the other contradicting it.

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
| a huge `{:70000}` field | throws `format_error` | Field width is capped at 65535 on purpose (§3). |

---

# 1. What is absent entirely

## 1.1 Headers that do not exist (23)

82 of the C++23 headers are provided and 23 are absent, which accounts for the
whole C++23 header list apart from the deprecated `<codecvt>`. Four C++26
headers are provided on top of that — `<inplace_vector>` (§2), `<debugging>`
(§2), `<stdbit.h>` and `<stdckdint.h>` (§2) — so the tree holds 86 standard
headers in all.

### C library wrappers — 17

`<cassert>` `<cctype>` `<cerrno>` `<cfenv>` `<cfloat>` `<cinttypes>`
`<climits>` `<clocale>` `<csetjmp>` `<csignal>` `<cstdarg>` `<cstdio>`
`<cstdlib>` `<cstring>` `<cuchar>` `<cwchar>` `<cwctype>`

BoxOS has no libc. Userspace links `boxlib`, the native BoxOS library, whose
vocabulary is Manifests, Crates, Touch and TagFS rather than POSIX. A `<cstdio>`
would have to invent a `FILE*` that nothing below it implements.

`<stdatomic.h>` used to be listed here as absent. It is **provided** as of Ф34:
it is a pure using-declaration header over `<atomic>`, so `_Atomic(T)` means
`std::atomic<T>` and every name [atomics.syn] declares is reachable from the
global namespace — there is no second implementation to keep in step. Writing
it is what surfaced the missing `atomic_int_least*_t` / `atomic_int_fast*_t`
aliases, which [atomics.syn] has listed since C++11 and `<atomic>` never had.
The two C23 headers C++26 adopts, `<stdbit.h>` and `<stdckdint.h>`, are
provided for the same reason and on the same terms (§2).

Two of the C wrappers **are** provided — `<cstddef>` and `<cstdint>` — because
they are pure type and macro headers with no runtime behind them. Independently,
the compiler's own freestanding C headers remain available and are used by the
library itself: `<stdint.h>`, `<stddef.h>`, `<stdarg.h>`, `<limits.h>`,
`<float.h>` come from GCC, not from a libc, and resolve normally.

### Excluded by decision — 3

| Header | Why |
|---|---|
| `<filesystem>` | There is no hierarchical path namespace to model. TagFS is tag-addressed: a file is found by the tags it carries, not by where it sits. This is also why `<fstream>`'s `filesystem::path` overloads are absent (§2 `<fstream>`). |
| `<regex>` | Excluded by plan. |
| `<valarray>` | Excluded by plan. |

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

### C++23 features not implemented — 3

| Header | Status |
|---|---|
| `<syncstream>` | Not implemented. The blocker this entry used to name — that its contract is written against `<iostream>` — went away in Ф36; what is left is the work itself (`basic_syncbuf`, `basic_osyncstream`, and the emit-on-destruction contract), which nothing has done. |
| `<execution>` | Not implemented; the parallel overloads of the algorithms are absent with it. |
| `<stdfloat>` | Not implemented — no extended floating-point types. |

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

- **Wide characters.** There are no wide streams, no `wformat_context`, and no
  wide-character facets. `wchar_t` itself works as a type; nothing in the
  library is instantiated for it. Concretely, since Ф36 gave the narrow
  streams a home: `<iostream>` declares `cin`/`cout`/`cerr`/`clog` and not
  `wcin`/`wcout`/`wcerr`/`wclog`, and `<fstream>` and `<iosfwd>` carry
  `filebuf`/`ifstream`/`ofstream`/`fstream` and not their `w` counterparts.
  Every class template involved is still generic in `CharT`, so the exclusion
  is a decision about what is shipped and tested, not a structural block.
- **Locales beyond `"C"`.** `<locale>` exists as the minimum the stream
  machinery needs. The `L` format specifier is accepted and ignored.
- **Time zones and leap seconds.** `<chrono>` has no `tzdb`, no `time_zone`, no
  `zoned_time`, and no leap-second table.

## 1.3 Feature-test macros

boxcxx defines **201** `__cpp_lib_*` macros. Two properties were verified across
the whole set, not sampled:

- **Every C++23 macro carries its N4950 value**, and none is defined at a later
  revision's value. The exceptions are the macros of *implemented C++26
  features*, which carry their C++26 value and are listed at the end of this
  section; there are forty-six so far, measured rather than counted by hand: every
  macro whose value here exceeds what libstdc++ 16.1 reports at `-std=c++23`,
  plus every macro it does not define there at all.
- **Every one is visible both from `<version>` and from every header
  [version.syn] names as an owner**, as [support.limits.general] requires —
  checked over the full cross-product of 206 macros × 86 headers by
  `tools/cxx_ftm_audit.sh`, against a transcription of [version.syn]'s ownership
  lists kept beside it in `tools/version_syn_owners.txt`.

  This entry used to claim the same thing without the tool, on the strength of
  one hand-check. **By Ф32-i it was false**: `__cpp_lib_nonmember_container_access`
  was missing from seven of the twelve headers that own it — `<forward_list>`,
  `<list>`, `<map>`, `<set>`, `<string>`, `<unordered_map>`, `<unordered_set>` —
  because it lives in the `<iterator>` leaf and those seven include only the
  container one. A claim nothing re-checks has a shelf life, so it is now
  re-checked; the macros are declared by name at the top of each owning header
  (`BOXCXX_OWNS_<stem>`) and each `__bits/version_*` leaf defines only what the
  including header declared.

- **The converse does not hold, and cannot.** 161 of the 201 macros are also
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

**14 of the macros [version.syn] names are not defined**, and `<version>` lists
every one by name with its specific reason — that list, not this section, is the authoritative
backlog. The governing rule is that a macro is defined only when the feature
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

Of those, the largest single group is the freestanding-subset markers
(`__cpp_lib_freestanding_*`): they do not name a feature at all but an
assertion that a header meets the standard's freestanding subset, which is a
separate audit against [compliance] and has not been done. `__cpp_lib_ratio`
and `__cpp_lib_out_ptr`'s C++26 value are held back for the same reason. The
rest belong to features whose owning header does not exist (`execution`,
`filesystem`, `spanstream`, `syncbuf`) or to
whole-clause requirements relaxations (`__cpp_lib_ranges`,
`__cpp_lib_algorithm_iterator_requirements`). The in-tree suite pins the absences
as well as the values, so a macro cannot quietly appear — and when one is closed,
the guard fires and forces the pin to be flipped in the same commit.

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

## `<charconv>`

- `~` `__int128` and `unsigned __int128` are deliberately unsupported. The standard
  requires the standard integer types and `char`; adding the extended ones would
  drag in the `make_unsigned<__int128>` question for no gain. `long double` **is**
  supported in both directions, at genuine 80-bit width.
- `~` `from_chars` does not skip whitespace, does not accept `+`, and does not
  accept a `0x` prefix — strictly per [charconv]. The `strtod`-style preamble lives
  in `stof`/`stod` instead.

## `<chrono>`

- `–` No time zones: `tzdb`, `time_zone`, `zoned_time` and `leap_second` do not
  exist, and there is no leap-second table.
- `~` `utc_clock`, `tai_clock`, `gps_clock`, `file_clock` and `local_t` exist as
  **type surface only** — `rep`, `period`, `duration`, `time_point` — with no
  `now()`. Calling `utc_clock::now()` is a compile error, which is the honest
  answer without a leap-second database. `tai` and `gps` are exact fixed offsets
  and need no table.
- `!` **`utc_time` is `sys_time` with the same count.** Without a leap-second table
  UTC differs from true UTC by the number of inserted leap seconds, and `%S` will
  never show the 60th second. The deviation is localized: for every specifier,
  `utc(c)` renders identically to `sys(c)`.
- `~` `clock_cast` is identity-only. A cross-clock cast (`system_clock` →
  `utc_clock`) does not compile — correct in the absence of
  `clock_time_conversion`, and better than silently converting wrongly.
- `~` `to_time_t` / `from_time_t` work in `long long` epoch seconds. There is no
  `std::time_t` and no `<ctime>`; the name `time_t` already belongs to a 20-byte
  BoxOS structure in `box/time.h`.
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

## `<cmath>`

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
  [c.math.abs] puts those three in `<cstdlib>`, which BoxOS does not have (§1.1);
  they are provided here rather than left to the promotion rule, because promoting
  would turn `std::abs(-3)` from the `int` `3` every program expects into `3.0`.
  Unsigned arguments remain ill-formed, which is what [c.math.abs]/3 asks for.
- `~` `nan("payload")` ignores the payload string and returns a plain quiet NaN.
- `?` `std::log10` is not exact on one of the 23 exactly-representable powers of
  ten (measured on BoxOS). `box::log(x, 10)` returns 22 of the 23 exactly by
  calling `log10` directly rather than dividing logarithms.

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
- `+` **Field width is capped at 65535.** On the literal path this is a
  compile-time error ("width exceeds the field limit") caught by the consteval
  format-string check; on the dynamic path (`{:{}}`) it throws at run time. Neither
  reference library bounds the dynamic path. Deliberate: on bare metal an
  attacker-supplied format string asking for a two-gigabyte field is a denial of
  service.
- `?` The cap is **per field, not a total budget**. A nine-character spec such as
  `"{::65535}"` applied to a large range still multiplies the output, and both
  reference libraries produce the same volume byte for byte. A total budget was
  considered and rejected: it would break a legitimate `format()` of a large
  container. The threat closed here is the hostile *format string*; the size of the
  argument is chosen by the program.
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
- `~` Width is measured in **code units**. There is no grapheme clustering and no
  East-Asian width estimation; for the ASCII console this is exact.
- `~` `L` (locale) is parsed and ignored for arithmetic types, and explicitly
  rejected for strings and pointers. There is only the `"C"` locale.
- `~` The `set` and `map` range formatters expose `set_brackets` and
  `set_separator`, which the standard gives only to the sequence specialization.
  Both reference libraries reject those calls. A harmless extension, but generic
  code written strictly against the promised surface would not expect them.
- `–` No wide formatting: `wformat_context`, `wformat_args` and
  `format(wstring_view, …)` do not exist.
- `?` For a type with no formatter, the intended `static_assert` message does fire
  — but four noisier errors precede it (a deleted constructor, a missing `parse`,
  and two consteval failures).

## `<fstream>`

New in Ф36. The backing is a `"file:"` Current, and the `s` a filebuf opens is
a TagFS **name** — there is no directory to walk and no separator to parse.
All nine rows of [filebuf.members] Table 122 are implemented, along with the
`noreplace` column P2467R1 added, and the suite pins each of them plus the
combinations the table leaves out.

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
- `–` No `wcin`/`wcout`/`wcerr`/`wclog` (§1.2).
- `?` **`sync_with_stdio` remains inert** and returns the previous value. There
  is no C stdio buffer to pair with; boxlib's own `printf` is a different
  subsystem that no stream here touches.
- The objects are constructed by a priority-102 constructor — after the TLS
  bootstrap at 101, ahead of every default-priority global in the program — so
  a user's own global constructor may write to `std::cout`. `ios_base::Init`,
  which was an honest no-op for the whole epic, now does the other half:
  its last destructor flushes the streams while they are still alive.

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

- `–` Only the `"C"` locale exists, and it has no facets: there is no facet base,
  no `use_facet` / `has_facet`, and no `ctype` / `num_get` / `num_put` / … The
  named constructor accepts any name and ignores it; `name()` always returns
  `"C"`. The header exists to satisfy the stream machinery's references to it.

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
- `~` The synopsis's six `basic_ostream<wchar_t, traits>` deletions are **not**
  mirrored. Wide streams are a permanent exclusion (§1.2) and there is no
  `char_traits<wchar_t>`, so `basic_ostream<wchar_t, …>` can never be formed and
  those overloads could never be candidates. Declared for completeness they would
  be unreachable text.
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
  `codecvt<char32_t, char8_t, mbstate_t>` and their `_byname` forms, and
  boxcxx's `<locale>` has no facets at all (§1.2). That exclusion is permanent,
  so this macro is too.

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

## `<string>`

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
toolchains installed today: **libstdc++ 16.1.0** (`__GLIBCXX__ 20260430`) and
**libc++ 22.1.6** (`_LIBCPP_VERSION 220106`). Where an older measurement has since
been fixed upstream, it is not claimed here — several were dropped for exactly
that reason, and §4 says which.

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
- **Bounded field width.** boxcxx caps a format field at 65535 on the literal path
  (a compile-time error, verified) and on the dynamic path (a run-time throw,
  pinned by the in-tree suite). Neither reference library bounds the dynamic path;
  libc++ bounds nothing. On bare metal a hostile format string asking for a
  two-gigabyte field is a denial of service, so this is deliberate strictness —
  recorded in §2 under `<format>` as a `+`.

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

# 5. `box::` — the BoxOS-native surface

Everything above is about the ISO library. `include/box/cxx/` is the other half of
boxcxx: 32 headers that give BoxOS's own concepts a C++ face. They are not
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
`codecvt<charN_t, char8_t, mbstate_t>`), not by `pmr::u8string` as recorded, and
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
recalled: **phases** are the `Phase*()` entry points `main` invokes, plus phase 2,
which is proven by its translation unit linking at all rather than by a call;
**runtime checks** are `Check(` call sites; **`static_assert`s** are occurrences
of the keyword. Two of the three figures had drifted before Ф31e-d and were
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
