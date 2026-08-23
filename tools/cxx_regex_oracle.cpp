// cxx_regex_oracle.cpp — the differential stand for Ф44's <regex>.
//
// boxcxx is written against the standard, but the standard's regex text is
// large enough that "I read it correctly" is a claim, not a fact. The two
// reference implementations are the cheapest way to turn it into a fact — as
// long as you know where they disagree with EACH OTHER, because those are
// precisely the cells where neither may be quoted as ground truth.
//
// This one translation unit is compiled twice, against libstdc++ and against
// libc++, and prints one deterministic line per case. The outputs diff
// directly. It exists BEFORE the engine, which is the whole point: the map of
// untrustworthy cells has to be drawn before there is any code tempted to
// trust them. A third column joins later, when the real __bits/regex_* leaves
// can be symlinked in beside it (the Ф43-e-2 stand's shape).
//
// Modes:
//   gen  FROM TO [DEPTH] [FAN]   generated sweep over case ids [FROM,TO)
//   parse FROM TO [DEPTH] [FAN]  the same cases, verdict only: accepted with
//                                how many groups, or which error
//   file PATH                    replay explicit `pattern<TAB>subject` cases
//   wgen FROM TO [DEPTH] [FAN]   the same generated cases run through BOTH
//                                halves — narrow and wide — in one process
//   wfile PATH                   replay UTF-8 `pattern<TAB>subject` cases as
//                                wide, for the characters no narrow half has
//   adv  PATTERN N               time ONE search of PATTERN against 'a'*N
//
// `adv` is one case per process on purpose. A tool that measures exponential
// blowup must not be able to hang on it: the shell puts a clock on the process
// and reports "hung" as a result, which is the answer, not a failure.
//
// Nothing here may call rand(), time() or anything else that differs between
// two runs: the two libraries must walk a byte-identical case sequence.
#include <regex>
#include <string>
#include <sstream>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "cxx_regex_oracle_gen.h"

static const char *kLib =
#ifdef _LIBCPP_VERSION
    "libc++";
#else
    "libstdc++";
#endif

// [re.err] fixes the NAMES of error_type and leaves the values to the
// implementation: libstdc++ numbers from 0, libc++ from 1. Printing the raw
// int manufactures a disagreement on every single throw — measured, on the
// first sweep this tool ever ran. Print the name.
static const char *ErrName(std::regex_constants::error_type e)
{
    using namespace std::regex_constants;
    if (e == error_collate) return "collate";
    if (e == error_ctype) return "ctype";
    if (e == error_escape) return "escape";
    if (e == error_backref) return "backref";
    if (e == error_brack) return "brack";
    if (e == error_paren) return "paren";
    if (e == error_brace) return "brace";
    if (e == error_badbrace) return "badbrace";
    if (e == error_range) return "range";
    if (e == error_space) return "space";
    if (e == error_badrepeat) return "badrepeat";
    if (e == error_complexity) return "complexity";
    if (e == error_stack) return "stack";
    return "other";
}

// ── one case, rendered so two libraries produce byte-identical text ────────
static void Report(const char *id, const std::string &pattern, const std::string &subject,
                   std::regex_constants::syntax_option_type gram
                       = std::regex_constants::ECMAScript)
{
    std::printf("%s |%s| |%s| -> ", id, pattern.c_str(), subject.c_str());
    try {
        std::regex re(pattern, gram);
        std::smatch m;
        if (!std::regex_search(subject, m, re)) {
            std::printf("nomatch\n");
            return;
        }
        std::printf("at=%d len=%d groups=%d", (int)m.position(0), (int)m.length(0),
                    (int)m.size() - 1);
        for (std::size_t i = 1; i < m.size(); ++i) {
            if (m[i].matched)
                std::printf(" g%zu=[%d,%d]'%s'", i, (int)m.position(i), (int)m.length(i),
                            m[i].str().c_str());
            else
                std::printf(" g%zu=-", i);
        }
        std::printf("\n");
    } catch (const std::regex_error &e) {
        std::printf("throw %s\n", ErrName(e.code()));
    }
}

// ── the wide half ───────────────────────────────────────────────────────
// ‼ For an ASCII case the narrow half IS the oracle for the wide one, and that
// is a stronger check than any cross-library diff: a library that answers
// differently about the same characters depending on the width of the type it
// stored them in has a defect, and it needs no second implementation to say
// so. The shape is Ф42-g's, where <format>'s narrow half was the oracle for
// its wide half over 20 286 spec/argument pairs.
//
// Text crosses the boundary as UTF-8 both ways, so the two verdicts are
// byte-comparable strings and the case file stays a file anyone can read.
// Decoding is done here rather than through a locale on purpose: mbstowcs
// would make the answer depend on the host's LC_CTYPE, and this tool exists to
// remove that kind of dependence, not to acquire one.
// One verdict, rendered identically whatever the character type, so that the
// two halves produce the same text for the same answer or the diff means
// something. Positions and lengths are in CHARACTERS of the subject's own
// type, which is what makes them comparable for an ASCII case and what makes
// them the right answer for a wide one.
template <class C>
static std::string Verdict(const std::basic_string<C> &pattern,
                           const std::basic_string<C> &subject,
                           std::regex_constants::syntax_option_type gram)
{
    std::ostringstream out;
    try {
        std::basic_regex<C> re(pattern, gram);
        std::match_results<typename std::basic_string<C>::const_iterator> m;
        if (!std::regex_search(subject, m, re)) return "nomatch";
        out << "at=" << (int)m.position(0) << " len=" << (int)m.length(0)
            << " groups=" << (int)m.size() - 1;
        for (std::size_t i = 1; i < m.size(); ++i) {
            if (!m[i].matched) { out << " g" << i << "=-"; continue; }
            std::basic_string<C> g = m[i].str();
            std::string text;
            if constexpr (sizeof(C) == 1) text.assign((const char *)g.data(), g.size());
            else text = ToUtf8(std::wstring(g.begin(), g.end()));
            out << " g" << i << "=[" << (int)m.position(i) << "," << (int)m.length(i)
                << "]'" << text << "'";
        }
    } catch (const std::regex_error &e) {
        return std::string("throw ") + ErrName(e.code());
    }
    return out.str();
}

// Both halves of one case, on one line. A column disagreeing with ITSELF is
// the finding; the three columns are still diffed as usual, because a library
// can also be consistently wrong.
static void BothHalves(const char *id, const std::string &pattern, const std::string &subject,
                       std::regex_constants::syntax_option_type gram)
{
    const std::string n = Verdict<char>(pattern, subject, gram);
    const std::string w = Verdict<wchar_t>(ToWide(pattern), ToWide(subject), gram);
    std::printf("%s |%s| |%s| -> N{%s} W{%s}%s\n", id, pattern.c_str(), subject.c_str(),
                n.c_str(), w.c_str(), n == w ? "" : "  <<WIDTH-DIFF");
}

// A case whose characters no narrow half can hold. Here the two reference
// columns are the oracle again, and only for the questions that do not go
// through a locale table: literals, ranges, `.`, quantifiers, back-references
// and the anchors. Anything that asks what KIND of character U+4E2D is would
// be comparing macOS's wide ctype against ours, which is Ф42-a's measured
// divergence and not a regex question — those cases live in the QEMU phase.
static void WideOnly(const char *id, const std::string &pattern, const std::string &subject,
                     std::regex_constants::syntax_option_type gram)
{
    const std::string w = Verdict<wchar_t>(ToWide(pattern), ToWide(subject), gram);
    std::printf("%s |%s| |%s| -> %s\n", id, pattern.c_str(), subject.c_str(), w.c_str());
}

static void GenCase(std::uint64_t seed, int depth, int fan)
{
    const Case c = MakeCase(seed, depth, fan);
    Report(c.id, c.pattern, c.subject);
}

// Parse-only verdict. Until the machine exists there is nothing to match with,
// but whether a pattern is ACCEPTED, and with how many groups, is already a
// complete differential question — and it is the one Ф44-a has to answer.
static std::regex_constants::syntax_option_type GrammarOf(const char *name)
{
    using namespace std::regex_constants;
    if (!name || !*name || std::strcmp(name, "ECMAScript") == 0) return ECMAScript;
    if (std::strcmp(name, "basic") == 0) return basic;
    if (std::strcmp(name, "extended") == 0) return extended;
    if (std::strcmp(name, "awk") == 0) return awk;
    if (std::strcmp(name, "grep") == 0) return grep;
    if (std::strcmp(name, "egrep") == 0) return egrep;
    std::fprintf(stderr, "unknown grammar %s\n", name);
    std::exit(2);
}

static void ParseCase(std::uint64_t seed, int depth, int fan,
                      std::regex_constants::syntax_option_type gram)
{
    const Case c = MakeCase(seed, depth, fan);
    std::printf("%s |%s| -> ", c.id, c.pattern.c_str());
    try {
        std::regex re(c.pattern, gram);
        std::printf("ok marks=%u\n", (unsigned)re.mark_count());
    } catch (const std::regex_error &e) {
        std::printf("throw %s\n", ErrName(e.code()));
    }
}

// Parse verdict only, for a curated list. The POSIX grammars cannot be swept
// with a generator that emits ECMAScript shapes — the patterns mean different
// things there, and the noise buries the signal — so they are checked against
// lists written by hand.
static int ReplayParse(const char *path, std::regex_constants::syntax_option_type gram)
{
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::string::size_type tab = line.find('\t');
        const std::string pat = tab == std::string::npos ? line : line.substr(0, tab);
        char id[16];
        std::snprintf(id, sizeof id, "case%04d", ++n);
        std::printf("%s |%s| -> ", id, pat.c_str());
        try {
            std::regex re(pat, gram);
            std::printf("ok marks=%u\n", (unsigned)re.mark_count());
        } catch (const std::regex_error &e) {
            std::printf("throw %s\n", ErrName(e.code()));
        }
    }
    return 0;
}

static int Replay(const char *path, std::regex_constants::syntax_option_type gram)
{
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::string::size_type tab = line.find('\t');
        if (tab == std::string::npos) continue;
        char id[16];
        std::snprintf(id, sizeof id, "case%04d", ++n);
        Report(id, line.substr(0, tab), line.substr(tab + 1), gram);
    }
    return 0;
}

static int ReplayWide(const char *path, std::regex_constants::syntax_option_type gram)
{
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::string::size_type tab = line.find('\t');
        if (tab == std::string::npos) continue;
        char id[16];
        std::snprintf(id, sizeof id, "case%04d", ++n);
        WideOnly(id, line.substr(0, tab), line.substr(tab + 1), gram);
    }
    return 0;
}

// One timed search. Printed by the caller's clock as well, so a process the
// shell had to kill is still reported — by its absence of a line here.
static int Adversarial(const char *pattern, int n)
{
    std::string subject((std::size_t)n, 'a');
    // Nothing is printed before the search returns. A process the shell has to
    // kill would otherwise leave half a line behind and the killer's report
    // would land on the end of it.
    try {
        std::regex re(pattern);
        auto t0 = std::chrono::steady_clock::now();
        bool m = std::regex_search(subject, re);
        auto t1 = std::chrono::steady_clock::now();
        std::printf("%-10s n=%-4d %-9s answered match=%d  %10.3f ms\n", pattern, n, kLib,
                    (int)m, std::chrono::duration<double, std::milli>(t1 - t0).count());
    } catch (const std::regex_error &e) {
        std::printf("%-10s n=%-4d %-9s REFUSED %s\n", pattern, n, kLib, ErrName(e.code()));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 3 && std::strcmp(argv[1], "adv") == 0)
        return Adversarial(argv[2], std::atoi(argv[3]));
    if (argc > 2 && std::strcmp(argv[1], "pfile") == 0) {
        const auto g = GrammarOf(argc > 3 ? argv[3] : nullptr);
        std::fprintf(stderr, "# regex oracle: %s, parse %s\n", kLib, argv[2]);
        return ReplayParse(argv[2], g);
    }
    if (argc > 2 && std::strcmp(argv[1], "file") == 0) {
        const auto g = GrammarOf(argc > 3 ? argv[3] : nullptr);
        std::fprintf(stderr, "# regex oracle: %s, replay %s\n", kLib, argv[2]);
        return Replay(argv[2], g);
    }
    if (argc > 2 && std::strcmp(argv[1], "wfile") == 0) {
        const auto g = GrammarOf(argc > 3 ? argv[3] : nullptr);
        std::fprintf(stderr, "# regex oracle: %s, wide replay %s\n", kLib, argv[2]);
        return ReplayWide(argv[2], g);
    }
    const bool bothHalves = argc > 1 && std::strcmp(argv[1], "wgen") == 0;
    const bool parseOnly = argc > 1 && std::strcmp(argv[1], "parse") == 0;
    int a = (argc > 1 && (std::strcmp(argv[1], "gen") == 0 || parseOnly || bothHalves)) ? 1 : 0;
    std::uint64_t from = argc > a + 1 ? std::strtoull(argv[a + 1], nullptr, 0) : 1;
    std::uint64_t to   = argc > a + 2 ? std::strtoull(argv[a + 2], nullptr, 0) : 4000;
    int depth = argc > a + 3 ? std::atoi(argv[a + 3]) : 1;
    int fan   = argc > a + 4 ? std::atoi(argv[a + 4]) : 2;
    const auto gram = GrammarOf(argc > a + 5 ? argv[a + 5] : nullptr);
    std::fprintf(stderr, "# regex oracle: %s, cases [%llu,%llu) depth=%d fan=%d\n", kLib,
                 (unsigned long long)from, (unsigned long long)to, depth, fan);
    for (std::uint64_t i = from; i < to; ++i) {
        if (parseOnly) ParseCase(i, depth, fan, gram);
        else if (bothHalves) {
            const Case c = MakeCase(i, depth, fan);
            BothHalves(c.id, c.pattern, c.subject, gram);
        } else GenCase(i, depth, fan);
    }
    return 0;
}
