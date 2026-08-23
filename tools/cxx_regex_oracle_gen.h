// cxx_regex_oracle_gen.h — the case sequence, shared.
//
// The differential stand only means anything if every column walks the SAME
// cases, so the generator lives here rather than in either of them. Nothing in
// it may call rand(), time() or anything else that differs between two runs.
#ifndef BOXCXX_REGEX_ORACLE_GEN_H
#define BOXCXX_REGEX_ORACLE_GEN_H

#include <string>
#include <cstdint>
#include <cstdio>

// ── UTF-8 <-> wide, by hand ────────────────────────────────────────────────
// Shared because both the oracle and the stand have to agree byte for byte on
// what a case file says. Done here rather than through mbstowcs on purpose:
// a locale-driven conversion would make the answer depend on the host's
// LC_CTYPE, and this stand exists to remove that kind of dependence.
inline std::wstring ToWide(const std::string &s)
{
    std::wstring w;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        unsigned long cp;
        int extra;
        if (c < 0x80)      { cp = c;         extra = 0; }
        else if (c < 0xE0) { cp = c & 0x1Fu; extra = 1; }
        else if (c < 0xF0) { cp = c & 0x0Fu; extra = 2; }
        else               { cp = c & 0x07u; extra = 3; }
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i)
            cp = (cp << 6) | ((unsigned char)s[i] & 0x3Fu);
        w.push_back((wchar_t)cp);
    }
    return w;
}

inline std::string ToUtf8(const std::wstring &w)
{
    std::string s;
    for (wchar_t wc : w) {
        const unsigned long cp = (unsigned long)wc;
        if (cp < 0x80) s.push_back((char)cp);
        else if (cp < 0x800) {
            s.push_back((char)(0xC0 | (cp >> 6)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back((char)(0xE0 | (cp >> 12)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (cp >> 18)));
            s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return s;
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


// One case: the pattern and the subject the seed produces.
struct Case { std::string pattern, subject; char id[16]; };

inline Case MakeCase(std::uint64_t seed, int depth, int fan)
{
    Case c;
    Rng r(seed);
    Gen g(r, fan);
    c.pattern = g.Alt(depth);
    unsigned len = r.Below(9);
    for (unsigned i = 0; i < len; ++i) {
        static const char kIn[] = "aabbc 1";
        c.subject += kIn[r.Below(7)];
    }
    std::snprintf(c.id, sizeof c.id, "%08llx", (unsigned long long)seed);
    return c;
}

#endif // BOXCXX_REGEX_ORACLE_GEN_H
