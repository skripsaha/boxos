// cxx_regex_stand.cpp — boxcxx's own regex leaves, on the host compiler.
//
// The third column of the differential stand. It includes the REAL
// <__bits/regex_syntax>, <__bits/regex_program> and <__bits/regex_parse> —
// reached through symlinks the driver drops in a private include directory, so
// that boxcxx's <string> and <vector> do not shadow the host's — and walks the
// identical case sequence the two reference columns walk.
//
// This is the whole reason the parser was written as a template over its traits
// and told nothing about locales: with no locale in it, it builds anywhere.
// The traits below mirror std::regex_traits in <regex> member for member, so a
// verdict that differs from a reference is the PARSER differing, not the traits.
#include <cctype>
#include <chrono>
#include <cwctype>
#include <cstdint>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <locale>
#include <string>
#include <vector>

#include <__bits/regex_exec>
#include <__bits/regex_parse>
#include <__bits/regex_program>
#include <__bits/regex_syntax>

#include "cxx_regex_oracle_gen.h"

static const char *ErrName(std::regex_constants::error_type e)
{
    using namespace std::regex_constants;
    switch (e) {
    case error_collate:    return "collate";
    case error_ctype:      return "ctype";
    case error_escape:     return "escape";
    case error_backref:    return "backref";
    case error_brack:      return "brack";
    case error_paren:      return "paren";
    case error_brace:      return "brace";
    case error_badbrace:   return "badbrace";
    case error_range:      return "range";
    case error_space:      return "space";
    case error_badrepeat:  return "badrepeat";
    case error_complexity: return "complexity";
    case error_stack:      return "stack";
    }
    return "other";
}

// A copy of boxcxx's std::regex_traits<charT>, over the HOST's locale rather
// than over BoxOS's. Every table here is the same table.
//
// ‼ It is a template for Ф44-e, and what that does and does not prove is worth
// being exact about. Instantiated for wchar_t it proves the ENGINE is generic
// over the character type — that the parser, the two machines and the class
// representation answer the same way when the same characters arrive in a
// wider box. It does NOT prove anything about boxcxx's own wide ctype: this
// traits asks the host's <cwctype>, and BoxOS's answers come from the Unicode
// database Ф42-a built, which deliberately differs from macOS's (iswalpha of
// U+4E2D is 1 there and 0 here). The questions that go through those tables
// are answered in QEMU by phase238, not on this stand.
template <class charT>
class HostTraitsT {
public:
    using char_type = charT;
    using string_type = std::basic_string<charT>;
    using char_class_type = unsigned int;

    static constexpr char_class_type kUnderscore = 1u << 16;

    static std::size_t length(const charT *p)
    {
        std::size_t n = 0;
        while (p[n]) ++n;
        return n;
    }

    charT translate(charT c) const { return c; }
    charT translate_nocase(charT c) const { return Lower(c); }

    template <class It> string_type transform(It first, It last) const
    {
        return string_type(first, last);
    }

    template <class It> string_type transform_primary(It first, It last) const
    {
        string_type s(first, last);
        for (auto &c : s) c = Lower(c);
        return s;
    }

    template <class It> string_type lookup_collatename(It first, It last) const
    {
        const string_type name(first, last);
        if (name.size() == 1) return name;
        static const struct { const char *n; char v; } kNames[] = {
            {"NUL", '\0'}, {"alert", '\a'}, {"backspace", '\b'}, {"tab", '\t'},
            {"newline", '\n'}, {"vertical-tab", '\v'}, {"form-feed", '\f'},
            {"carriage-return", '\r'}, {"space", ' '}, {"period", '.'},
            {"hyphen", '-'}, {"comma", ','}, {"underscore", '_'},
        };
        for (const auto &e : kNames)
            if (Same(name, e.n)) return string_type(1, (charT)e.v);
        return string_type();
    }

    template <class It>
    char_class_type lookup_classname(It first, It last, bool icase = false) const
    {
        const string_type name(first, last);
        static const struct { const char *n; char_class_type v; } kNames[] = {
            {"alnum", kAlnum}, {"alpha", kAlpha}, {"blank", kBlank},
            {"cntrl", kCntrl}, {"digit", kDigit}, {"graph", kGraph},
            {"lower", kLower}, {"print", kPrint}, {"punct", kPunct},
            {"space", kSpace}, {"upper", kUpper}, {"xdigit", kXdigit},
            {"d", kDigit}, {"s", kSpace}, {"w", kAlnum | kUnderscore},
        };
        for (const auto &e : kNames) {
            if (!Same(name, e.n)) continue;
            char_class_type m = e.v;
            if (icase && (m & (kUpper | kLower)))
                m = (m & ~(char_class_type)(kUpper | kLower)) | kAlpha;
            return m;
        }
        return 0;
    }

    bool isctype(charT c, char_class_type f) const
    {
        if constexpr (sizeof(charT) == 1) {
            const unsigned char u = (unsigned char)c;
            if ((f & kAlpha) && std::isalpha(u)) return true;
            if ((f & kDigit) && std::isdigit(u)) return true;
            if ((f & kSpace) && std::isspace(u)) return true;
            if ((f & kUpper) && std::isupper(u)) return true;
            if ((f & kLower) && std::islower(u)) return true;
            if ((f & kPunct) && std::ispunct(u)) return true;
            if ((f & kPrint) && std::isprint(u)) return true;
            if ((f & kCntrl) && std::iscntrl(u)) return true;
            if ((f & kXdigit) && std::isxdigit(u)) return true;
            if ((f & kBlank) && (u == ' ' || u == '\t')) return true;
        } else {
            const std::wint_t u = (std::wint_t)c;
            if ((f & kAlpha) && std::iswalpha(u)) return true;
            if ((f & kDigit) && std::iswdigit(u)) return true;
            if ((f & kSpace) && std::iswspace(u)) return true;
            if ((f & kUpper) && std::iswupper(u)) return true;
            if ((f & kLower) && std::iswlower(u)) return true;
            if ((f & kPunct) && std::iswpunct(u)) return true;
            if ((f & kPrint) && std::iswprint(u)) return true;
            if ((f & kCntrl) && std::iswcntrl(u)) return true;
            if ((f & kXdigit) && std::iswxdigit(u)) return true;
            if ((f & kBlank) && (u == L' ' || u == L'\t')) return true;
        }
        if ((f & kUnderscore) && c == (charT)'_') return true;
        return false;
    }

    int value(charT ch, int radix) const
    {
        const char n = Narrow(ch);
        int v;
        if (n >= '0' && n <= '9') v = n - '0';
        else if (n >= 'a' && n <= 'f') v = n - 'a' + 10;
        else if (n >= 'A' && n <= 'F') v = n - 'A' + 10;
        else return -1;
        return v < radix ? v : -1;
    }

private:
    static charT Lower(charT c)
    {
        if constexpr (sizeof(charT) == 1) return (charT)std::tolower((unsigned char)c);
        else return (charT)std::towlower((std::wint_t)c);
    }
    static char Narrow(charT c)
    {
        return (unsigned long)c < 0x80u ? (char)c : '\0';
    }
    bool Same(const string_type &s, const char *name) const
    {
        std::size_t i = 0;
        for (; name[i]; ++i) {
            if (i >= s.size() || Narrow(s[i]) != name[i]) return false;
        }
        return i == s.size();
    }

    static constexpr char_class_type kSpace = 1u << 0, kPrint = 1u << 1,
                                     kCntrl = 1u << 2, kUpper = 1u << 3,
                                     kLower = 1u << 4, kAlpha = 1u << 5,
                                     kDigit = 1u << 6, kPunct = 1u << 7,
                                     kXdigit = 1u << 8, kBlank = 1u << 9;
    static constexpr char_class_type kAlnum = kAlpha | kDigit;
    static constexpr char_class_type kGraph = kAlnum | kPunct;
};

using HostTraits = HostTraitsT<char>;
using HostTraitsW = HostTraitsT<wchar_t>;

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

static void ParseOne(const char *id, const std::string &pattern,
                     std::regex_constants::syntax_option_type f)
{
    std::printf("%s |%s| -> ", id, pattern.c_str());
    try {
        HostTraits t;
        boxcxx::re::Compiler<HostTraits> c(t, f);
        auto prog = c.Run(pattern.data(), pattern.data() + pattern.size());
        std::printf("ok marks=%u\n", prog.groups);
    } catch (const std::regex_error &e) {
        std::printf("throw %s\n", ErrName(e.code()));
    }
}

// Until the machine exists, the only way to see whether a quantifier produced
// the SHAPE it should is to read the program out. `a{0,2}` must hold two Char
// instructions, not three — a distinction no accept/reject sweep can make.
static const char *OpName(boxcxx::re::Op op)
{
    using boxcxx::re::Op;
    switch (op) {
    case Op::Char: return "Char"; case Op::Any: return "Any";
    case Op::AnyAll: return "AnyAll"; case Op::Class: return "Class";
    case Op::Split: return "Split"; case Op::Jmp: return "Jmp";
    case Op::Save: return "Save"; case Op::Reset: return "Reset";
    case Op::Mark: return "Mark"; case Op::Check: return "Check";
    case Op::Bol: return "Bol"; case Op::Eol: return "Eol";
    case Op::WordB: return "WordB"; case Op::NotWordB: return "NotWordB";
    case Op::Backref: return "Backref"; case Op::Look: return "Look";
    case Op::LookEnd: return "LookEnd"; case Op::Accept: return "Accept";
    }
    return "?";
}

// The match verdict, printed in the oracle's format so the three columns diff
// directly. match_results does not exist yet; the machine fills a slot vector
// and this reads it, which is exactly what match_results will do.
static void MatchOne(const char *id, const std::string &pattern, const std::string &subject,
                     std::regex_constants::syntax_option_type f)
{
    std::printf("%s |%s| |%s| -> ", id, pattern.c_str(), subject.c_str());
    HostTraits t;
    boxcxx::re::Program<char, HostTraits::char_class_type> prog;
    try {
        boxcxx::re::Compiler<HostTraits> c(t, f);
        prog = c.Run(pattern.data(), pattern.data() + pattern.size());
    } catch (const std::regex_error &e) {
        std::printf("throw %s\n", ErrName(e.code()));
        return;
    }
    const char *first = subject.data();
    const char *last = first + subject.size();
    std::vector<std::ptrdiff_t> slots;
    bool hit;
    try {
        hit = boxcxx::re::Execute<HostTraits>(prog, t, std::regex_constants::match_default,
                                              first, last, first, false, false, slots);
    } catch (const std::regex_error &e) {
        std::printf("throw %s\n", ErrName(e.code()));
        return;
    }
    if (!hit) { std::printf("nomatch\n"); return; }
    std::printf("at=%d len=%d groups=%u", (int)slots[0], (int)(slots[1] - slots[0]), prog.groups);
    for (unsigned g = 1; g <= prog.groups; ++g) {
        const std::ptrdiff_t b = slots[2 * g], e = slots[2 * g + 1];
        if (b < 0 || e < 0) std::printf(" g%u=-", g);
        else std::printf(" g%u=[%d,%d]'%s'", g, (int)b, (int)(e - b),
                         subject.substr((std::size_t)b, (std::size_t)(e - b)).c_str());
    }
    std::printf("\n");
}

// ── the wide half, on the same leaves ───────────────────────────────────
// One verdict rendered exactly as the oracle renders it, so the three columns
// diff without anything having to know which is which.
template <class charT>
static std::string VerdictT(const std::basic_string<charT> &pattern,
                            const std::basic_string<charT> &subject,
                            std::regex_constants::syntax_option_type f)
{
    using Traits = HostTraitsT<charT>;
    Traits t;
    boxcxx::re::Program<charT, typename Traits::char_class_type> prog;
    try {
        boxcxx::re::Compiler<Traits> c(t, f);
        prog = c.Run(pattern.data(), pattern.data() + pattern.size());
    } catch (const std::regex_error &e) {
        return std::string("throw ") + ErrName(e.code());
    }
    const charT *first = subject.data();
    const charT *last = first + subject.size();
    std::vector<std::ptrdiff_t> slots;
    bool hit;
    try {
        hit = boxcxx::re::Execute<Traits>(prog, t, std::regex_constants::match_default, first,
                                          last, first, false, false, slots);
    } catch (const std::regex_error &e) {
        return std::string("throw ") + ErrName(e.code());
    }
    if (!hit) return "nomatch";
    std::ostringstream out;
    out << "at=" << (int)slots[0] << " len=" << (int)(slots[1] - slots[0])
        << " groups=" << prog.groups;
    for (unsigned g = 1; g <= prog.groups; ++g) {
        const std::ptrdiff_t b = slots[2 * g], e = slots[2 * g + 1];
        if (b < 0 || e < 0) { out << " g" << g << "=-"; continue; }
        const std::basic_string<charT> got = subject.substr((std::size_t)b, (std::size_t)(e - b));
        std::string text;
        if constexpr (sizeof(charT) == 1) text.assign((const char *)got.data(), got.size());
        else text = ToUtf8(std::wstring(got.begin(), got.end()));
        out << " g" << g << "=[" << (int)b << "," << (int)(e - b) << "]'" << text << "'";
    }
    return out.str();
}

static void BothHalves(const char *id, const std::string &pattern, const std::string &subject,
                       std::regex_constants::syntax_option_type f)
{
    const std::string n = VerdictT<char>(pattern, subject, f);
    const std::string w = VerdictT<wchar_t>(ToWide(pattern), ToWide(subject), f);
    std::printf("%s |%s| |%s| -> N{%s} W{%s}%s\n", id, pattern.c_str(), subject.c_str(),
                n.c_str(), w.c_str(), n == w ? "" : "  <<WIDTH-DIFF");
}

static void WideOnly(const char *id, const std::string &pattern, const std::string &subject,
                     std::regex_constants::syntax_option_type f)
{
    const std::string w = VerdictT<wchar_t>(ToWide(pattern), ToWide(subject), f);
    std::printf("%s |%s| |%s| -> %s\n", id, pattern.c_str(), subject.c_str(), w.c_str());
}

// One timed search, so the blowup shapes are measured in THREE columns and not
// in two with the third asserted. Same shape as the oracle's `adv`: one case
// per process, because a tool that measures a hang must not be able to hang.
static int Adversarial(const char *pattern, int n)
{
    const std::string subject((std::size_t)n, 'a');
    HostTraits t;
    boxcxx::re::Program<char, HostTraits::char_class_type> prog;
    try {
        boxcxx::re::Compiler<HostTraits> c(t, std::regex_constants::ECMAScript);
        prog = c.Run(pattern, pattern + std::strlen(pattern));
    } catch (const std::regex_error &e) {
        std::printf("%-10s n=%-4d %-9s REFUSED %s\n", pattern, n, "boxcxx", ErrName(e.code()));
        return 0;
    }
    const char *first = subject.data();
    const char *last = first + subject.size();
    std::vector<std::ptrdiff_t> slots;
    const auto t0 = std::chrono::steady_clock::now();
    bool hit = false;
    try {
        hit = boxcxx::re::Execute<HostTraits>(prog, t, std::regex_constants::match_default,
                                              first, last, first, false, false, slots);
    } catch (const std::regex_error &e) {
        std::printf("%-10s n=%-4d %-9s REFUSED %s\n", pattern, n, "boxcxx", ErrName(e.code()));
        return 0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("%-10s n=%-4d %-9s answered match=%d  %10.3f ms\n", pattern, n, "boxcxx",
                (int)hit, std::chrono::duration<double, std::milli>(t1 - t0).count());
    return 0;
}

static int Dump(const char *pattern, std::regex_constants::syntax_option_type f)
{
    HostTraits t;
    boxcxx::re::Compiler<HostTraits> c(t, f);
    try {
        auto prog = c.Run(pattern, pattern + std::strlen(pattern));
        std::printf("/%s/  groups=%u classes=%zu backrefs=%d looks=%d\n", pattern,
                    prog.groups, prog.classes.size(), (int)prog.backrefs, (int)prog.looks);
        for (std::size_t i = 0; i < prog.code.size(); ++i) {
            const auto &in = prog.code[i];
            std::printf("  %3zu  %-9s", i, OpName(in.op));
            switch (in.op) {
            case boxcxx::re::Op::Char:
                std::printf(" '%c'", in.ch >= 32 && in.ch < 127 ? in.ch : '?'); break;
            case boxcxx::re::Op::Split: std::printf(" %u, %u", in.x, in.y); break;
            case boxcxx::re::Op::Jmp:   std::printf(" %u", in.x); break;
            case boxcxx::re::Op::Look:
                std::printf(" body=%u cont=%u%s", in.x, in.y, in.neg ? " NEG" : ""); break;
            case boxcxx::re::Op::Save:  std::printf(" slot %u", in.x); break;
            case boxcxx::re::Op::Reset: std::printf(" groups %u..%u", in.x, in.y); break;
            case boxcxx::re::Op::Mark:  std::printf(" slot %u", in.x); break;
            case boxcxx::re::Op::Check: std::printf(" slot %u -> %u", in.x, in.y); break;
            case boxcxx::re::Op::Class: std::printf(" #%u", in.x); break;
            case boxcxx::re::Op::Backref: std::printf(" \\%u", in.x); break;
            default: break;
            }
            std::printf("\n");
        }
    } catch (const std::regex_error &e) {
        std::printf("/%s/  throw %s\n", pattern, ErrName(e.code()));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 3 && std::strcmp(argv[1], "adv") == 0)
        return Adversarial(argv[2], std::atoi(argv[3]));
    if (argc > 2 && std::strcmp(argv[1], "dump") == 0)
        return Dump(argv[2], GrammarOf(argc > 3 ? argv[3] : nullptr));

    if (argc > 1 && std::strcmp(argv[1], "match") == 0) {
        const std::uint64_t from = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1;
        const std::uint64_t to   = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : 4000;
        const int depth = argc > 4 ? std::atoi(argv[4]) : 1;
        const int fan   = argc > 5 ? std::atoi(argv[5]) : 2;
        const auto g = GrammarOf(argc > 6 ? argv[6] : nullptr);
        std::fprintf(stderr, "# regex stand: boxcxx match, cases [%llu,%llu)\n",
                     (unsigned long long)from, (unsigned long long)to);
        for (std::uint64_t i = from; i < to; ++i) {
            const Case c = MakeCase(i, depth, fan);
            MatchOne(c.id, c.pattern, c.subject, g);
        }
        return 0;
    }

    if (argc > 1 && std::strcmp(argv[1], "wmatch") == 0) {
        const std::uint64_t from = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1;
        const std::uint64_t to   = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : 4000;
        const int depth = argc > 4 ? std::atoi(argv[4]) : 1;
        const int fan   = argc > 5 ? std::atoi(argv[5]) : 2;
        const auto g = GrammarOf(argc > 6 ? argv[6] : nullptr);
        std::fprintf(stderr, "# regex stand: boxcxx both halves, cases [%llu,%llu)\n",
                     (unsigned long long)from, (unsigned long long)to);
        for (std::uint64_t i = from; i < to; ++i) {
            const Case c = MakeCase(i, depth, fan);
            BothHalves(c.id, c.pattern, c.subject, g);
        }
        return 0;
    }

    if (argc > 2 && std::strcmp(argv[1], "wmfile") == 0) {
        const auto g = GrammarOf(argc > 3 ? argv[3] : nullptr);
        std::FILE *in = std::fopen(argv[2], "r");
        if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
        char line[8192];
        int n = 0;
        while (std::fgets(line, sizeof line, in)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            const std::string::size_type tab = s.find('\t');
            if (tab == std::string::npos) continue;
            char id[16];
            std::snprintf(id, sizeof id, "case%04d", ++n);
            WideOnly(id, s.substr(0, tab), s.substr(tab + 1), g);
        }
        std::fclose(in);
        return 0;
    }

    if (argc > 2 && std::strcmp(argv[1], "mfile") == 0) {
        const auto g = GrammarOf(argc > 3 ? argv[3] : nullptr);
        std::FILE *in = std::fopen(argv[2], "r");
        if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
        char line[8192];
        int n = 0;
        while (std::fgets(line, sizeof line, in)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            const std::string::size_type tab = s.find('\t');
            char id[16];
            std::snprintf(id, sizeof id, "case%04d", ++n);
            MatchOne(id, tab == std::string::npos ? s : s.substr(0, tab),
                     tab == std::string::npos ? std::string() : s.substr(tab + 1), g);
        }
        std::fclose(in);
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "file") == 0) {
        const auto f = GrammarOf(argc > 3 ? argv[3] : nullptr);
        std::FILE *in = std::fopen(argv[2], "r");
        if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
        char line[8192];
        int n = 0;
        while (std::fgets(line, sizeof line, in)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            const std::string::size_type tab = s.find('\t');
            char id[16];
            std::snprintf(id, sizeof id, "case%04d", ++n);
            ParseOne(id, tab == std::string::npos ? s : s.substr(0, tab), f);
        }
        std::fclose(in);
        return 0;
    }

    const std::uint64_t from = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1;
    const std::uint64_t to   = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : 4000;
    const int depth = argc > 4 ? std::atoi(argv[4]) : 1;
    const int fan   = argc > 5 ? std::atoi(argv[5]) : 2;
    const auto f = GrammarOf(argc > 6 ? argv[6] : nullptr);
    std::fprintf(stderr, "# regex stand: boxcxx, cases [%llu,%llu) depth=%d fan=%d\n",
                 (unsigned long long)from, (unsigned long long)to, depth, fan);
    for (std::uint64_t i = from; i < to; ++i) {
        const Case c = MakeCase(i, depth, fan);
        ParseOne(c.id, c.pattern, f);
    }
    return 0;
}
