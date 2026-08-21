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
//   file PATH                    replay explicit `pattern<TAB>subject` cases
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
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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

// ── deterministic PRNG ─────────────────────────────────────────────────────
// Not std::mt19937: its stream is specified, but the distributions layered on
// top of it are not, and a case sequence that drifts between two libraries
// compares nothing. splitmix64 by hand cannot drift.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t Next()
    {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    unsigned Below(unsigned n) { return (unsigned)(Next() % n); }
};

// ── pattern generator ──────────────────────────────────────────────────────
// A recursive descent over the shapes of [re.grammar], bounded by depth so it
// terminates, and by fan so the result stays small enough to READ. A 400-char
// pattern proves a disagreement exists and explains nothing; the reduced cases
// live in the replay file instead.
struct Gen {
    Rng &r;
    int groups = 0;
    int fan;
    Gen(Rng &rng, int fanout) : r(rng), fan(fanout) {}

    std::string Atom(int depth)
    {
        unsigned pick = r.Below(depth > 0 ? 10u : 7u);
        switch (pick) {
        case 0: case 1: case 2: {
            static const char kAlpha[] = "abc";
            return std::string(1, kAlpha[r.Below(3)]);
        }
        case 3: return ".";
        case 4: {
            static const char *kClass[] = {"[ab]", "[^a]", "[a-c]", "[^b-c]", "[abc]"};
            return kClass[r.Below(5)];
        }
        case 5: {
            static const char *kEsc[] = {"\\d", "\\w", "\\s", "\\D", "\\W"};
            return kEsc[r.Below(5)];
        }
        case 6:
            if (groups > 0 && r.Below(2)) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%d", 1 + (int)r.Below((unsigned)groups));
                return buf;
            }
            return r.Below(2) ? "^" : "$";
        case 7: { ++groups; return "(" + Alt(depth - 1) + ")"; }
        case 8: return "(?:" + Alt(depth - 1) + ")";
        default: return r.Below(2) ? "(?=" + Alt(depth - 1) + ")" : "(?!" + Alt(depth - 1) + ")";
        }
    }

    std::string Quantified(int depth)
    {
        std::string a = Atom(depth);
        // A quantified anchor or lookahead is legal in the grammar and is a
        // separate argument between the two libraries; keeping it out of the
        // generator keeps this sweep about the ENGINE.
        if (a == "^" || a == "$" || a.compare(0, 3, "(?=") == 0 || a.compare(0, 3, "(?!") == 0)
            return a;
        unsigned q = r.Below(10);
        if (q >= 6) return a;
        static const char *kQ[] = {"*", "+", "?", "{1,2}", "{0,2}", "{2}"};
        std::string s = a + kQ[q];
        if (r.Below(4) == 0 && kQ[q][0] != '{') s += "?";   // lazy
        return s;
    }

    std::string Seq(int depth)
    {
        std::string s;
        unsigned n = 1 + r.Below((unsigned)fan);
        for (unsigned i = 0; i < n; ++i) s += Quantified(depth);
        return s;
    }

    std::string Alt(int depth)
    {
        std::string s = Seq(depth);
        unsigned n = r.Below((unsigned)fan);
        for (unsigned i = 0; i < n; ++i) s += "|" + Seq(depth);
        return s;
    }
};

// ── one case, rendered so two libraries produce byte-identical text ────────
static void Report(const char *id, const std::string &pattern, const std::string &subject)
{
    std::printf("%s |%s| |%s| -> ", id, pattern.c_str(), subject.c_str());
    try {
        std::regex re(pattern, std::regex_constants::ECMAScript);
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

static void GenCase(std::uint64_t seed, int depth, int fan)
{
    Rng r(seed);
    Gen g(r, fan);
    std::string pattern = g.Alt(depth);
    unsigned len = r.Below(9);
    std::string subject;
    for (unsigned i = 0; i < len; ++i) {
        static const char kIn[] = "aabbc 1";
        subject += kIn[r.Below(7)];
    }
    char id[16];
    std::snprintf(id, sizeof id, "%08llx", (unsigned long long)seed);
    Report(id, pattern, subject);
}

static int Replay(const char *path)
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
        Report(id, line.substr(0, tab), line.substr(tab + 1));
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
    if (argc > 2 && std::strcmp(argv[1], "file") == 0) {
        std::fprintf(stderr, "# regex oracle: %s, replay %s\n", kLib, argv[2]);
        return Replay(argv[2]);
    }
    int a = (argc > 1 && std::strcmp(argv[1], "gen") == 0) ? 1 : 0;
    std::uint64_t from = argc > a + 1 ? std::strtoull(argv[a + 1], nullptr, 0) : 1;
    std::uint64_t to   = argc > a + 2 ? std::strtoull(argv[a + 2], nullptr, 0) : 4000;
    int depth = argc > a + 3 ? std::atoi(argv[a + 3]) : 1;
    int fan   = argc > a + 4 ? std::atoi(argv[a + 4]) : 2;
    std::fprintf(stderr, "# regex oracle: %s, cases [%llu,%llu) depth=%d fan=%d\n", kLib,
                 (unsigned long long)from, (unsigned long long)to, depth, fan);
    for (std::uint64_t i = from; i < to; ++i) GenCase(i, depth, fan);
    return 0;
}
