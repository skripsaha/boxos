
#include <stddef.h>
#include <stdint.h>

extern "C" {
void  *malloc(size_t);
void   free(void *);
size_t strlen(const char *);
void  *memcpy(void *, const void *, size_t);
int    memcmp(const void *, const void *, size_t);
}

namespace {


struct Block {
    Block *Next;
    size_t Used;
    size_t Cap;
    char   Data[1];
};

struct Arena {
    Block *Head = nullptr;
    bool   Bad  = false;

    void *Take(size_t n)
    {
        n = (n + 15u) & ~(size_t)15u;
        if (Head && Head->Cap - Head->Used >= n) {
            void *p = Head->Data + Head->Used;
            Head->Used += n;
            return p;
        }
        size_t cap   = n > 4096u ? n : 4096u;
        Block *block = (Block *)malloc(sizeof(Block) + cap);
        if (!block) {
            Bad = true;
            return nullptr;
        }
        block->Next = Head;
        block->Used = n;
        block->Cap  = cap;
        Head        = block;
        return block->Data;
    }

    void Release()
    {
        while (Head) {
            Block *next = Head->Next;
            free(Head);
            Head = next;
        }
    }
};


struct Str {
    const char *P = "";
    size_t      N = 0;

    bool Empty() const { return N == 0; }
};

struct Builder {
    Arena *A;
    char  *P   = nullptr;
    size_t N   = 0;
    size_t Cap = 0;

    explicit Builder(Arena *a) : A(a) {}

    void Reserve(size_t need)
    {
        if (N + need <= Cap) return;
        size_t cap = Cap ? Cap * 2 : 64;
        while (cap < N + need) cap *= 2;
        char *fresh = (char *)A->Take(cap);
        if (!fresh) return;
        if (N) memcpy(fresh, P, N);
        P   = fresh;
        Cap = cap;
    }

    void Add(const char *s, size_t n)
    {
        if (n == 0) return;
        Reserve(n);
        if (!P) return;
        memcpy(P + N, s, n);
        N += n;
    }

    void Add(const char *s) { Add(s, strlen(s)); }
    void Add(Str s) { Add(s.P, s.N); }
    void Add(char c) { Add(&c, 1); }

    Str Done() const
    {
        Str s;
        s.P = P ? P : "";
        s.N = N;
        return s;
    }
};

Str JoinArgs(Arena *a, Str name, Str args);

Str Join(Arena *a, Str x, Str y)
{
    if (x.Empty()) return y;
    if (y.Empty()) return x;
    Builder b(a);
    b.Add(x);
    b.Add(y);
    return b.Done();
}

Str JoinArgs(Arena *a, Str name, Str args)
{
    Builder b(a);
    b.Add(name);
    if (name.N && name.P[name.N - 1] == '<') b.Add(' ');
    b.Add(args);
    return b.Done();
}

Str Lit(const char *s)
{
    Str r;
    r.P = s;
    r.N = strlen(s);
    return r;
}

bool Same(Str s, const char *lit)
{
    size_t n = strlen(lit);
    return s.N == n && memcmp(s.P, lit, n) == 0;
}

Str Number(Arena *a, long long v)
{
    char  tmp[24];
    int   n   = 0;
    bool  neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ull : (unsigned long long)v;

    if (u == 0) tmp[n++] = '0';
    while (u) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    Builder b(a);
    if (neg) b.Add('-');
    while (n) b.Add(tmp[--n]);
    return b.Done();
}


struct Type {
    Str Pre;
    Str Post;

    bool Open = false;

    enum Ref { NoRef, LRef, RRef };
    Ref Reference = NoRef;

    const struct Type *Elems = nullptr;
    size_t             ElemCount = 0;
    bool               IsPack = false;
    bool               IsFunction = false;
    bool               IsCompound = false;

    bool Empty() const { return Pre.Empty() && Post.Empty(); }
};

Type Plain(Str s)
{
    Type t;
    t.Pre = s;
    return t;
}

Type PlainLit(const char *s) { return Plain(Lit(s)); }

Str Flatten(Arena *a, Type t)
{
    if (t.Post.Empty()) return t.Pre;
    return Join(a, t.Pre, t.Post);
}


Str BareName(Str s)
{
    size_t start = 0;
    int    depth = 0;
    for (size_t i = 0; i + 1 < s.N; i++) {
        if (s.P[i] == '<') depth++;
        else if (s.P[i] == '>') depth--;
        else if (depth == 0 && s.P[i] == ':' && s.P[i + 1] == ':') start = i + 2;
    }
    size_t end = start;
    while (end < s.N && s.P[end] != '<') end++;
    Str r;
    r.P = s.P + start;
    r.N = end - start;
    return r;
}


const size_t kMaxSubs      = 512;
const size_t kMaxTemplate  = 64;
const int    kMaxDepth     = 128;

struct Parser {
    Arena      *A;
    const char *P;
    const char *End;
    bool        Failed = false;
    int         Depth  = 0;

    struct Sub {
        Type        Value;
        const char *Begin = nullptr;
        const char *End   = nullptr;
        bool        HasParam = false;
    };
    Sub    Subs[kMaxSubs];
    size_t SubCount  = 0;
    size_t ParamUses = 0;

    Type   Templ[kMaxTemplate];
    size_t TemplCount = 0;
    bool   InTemplateArgs = false;
    bool   NameIsTemplate = false;
    bool   NameIsCtorDtor = false;
    int    TypeDepth      = 0;
    size_t PackIndex      = 0;
    size_t PackSize       = 0;
    bool   PackEmpty      = false;
    bool   InLambdaSig    = false;
    bool   PackOpen       = false;

    Parser(Arena *a, const char *p, size_t n) : A(a), P(p), End(p + n) {}

    bool Done() const { return P >= End; }
    char Peek() const { return P < End ? *P : '\0'; }
    char PeekAt(size_t i) const { return P + i < End ? P[i] : '\0'; }
    bool Eat(char c)
    {
        if (Peek() != c) return false;
        P++;
        return true;
    }
    void Fail() { Failed = true; }

    void AddSub(Type t)
    {
        if (SubCount < kMaxSubs) {
            Subs[SubCount].Value    = t;
            Subs[SubCount].Begin    = nullptr;
            Subs[SubCount].HasParam = false;
            SubCount++;
        }
    }

    void AddSubRange(Type t, const char *begin, size_t paramMark)
    {
        if (SubCount < kMaxSubs) {
            Subs[SubCount].Value    = t;
            Subs[SubCount].Begin    = begin;
            Subs[SubCount].End      = P;
            Subs[SubCount].HasParam = ParamUses != paramMark;
            SubCount++;
        }
    }

    Type Resolve(const Sub &s)
    {
        if (!s.HasParam || !s.Begin) return s.Value;

        const char  *savedP    = P;
        const char  *savedEnd  = End;
        const size_t savedSubs = SubCount;

        P   = s.Begin;
        End = s.End;
        Type again;
        bool got = ParseType(&again);

        P        = savedP;
        End      = savedEnd;
        SubCount = savedSubs;
        return got ? again : s.Value;
    }

    bool ParseNumber(long long *out)
    {
        bool neg = false;
        if (Peek() == 'n') {
            neg = true;
            P++;
        }
        if (Peek() < '0' || Peek() > '9') return false;
        long long v = 0;
        while (Peek() >= '0' && Peek() <= '9') {
            v = v * 10 + (*P - '0');
            P++;
        }
        *out = neg ? -v : v;
        return true;
    }

    bool ParseSourceName(Str *out)
    {
        long long len = 0;
        if (!ParseNumber(&len) || len <= 0) return false;
        if ((size_t)(End - P) < (size_t)len) return false;

        const char *start = P;
        P += len;

        if (len >= 12 && memcmp(start, "_GLOBAL__N_", 11) == 0) {
            *out = Lit("(anonymous namespace)");
            return true;
        }
        Str s;
        s.P  = start;
        s.N  = (size_t)len;
        *out = s;
        return true;
    }

    Str ParseOperatorName();
    bool ParseUnqualifiedName(Str *out, bool *isCtorDtor, Str enclosing);
    bool ParseName(Type *out);
    bool ParseNestedName(Type *out);
    bool ParseUnscopedName(Type *out);
    bool ParseLocalName(Type *out);
    bool ParseType(Type *out);
    bool ParseBuiltinType(Type *out);
    bool ParseFunctionType(Type *out);
    bool ParseTemplateArgs(Str *out, bool nameLevel = false);
    bool ParseTemplateArg(Type *out);
    bool ParseExpression(Type *out);
    bool ParseExprPrimary(Type *out);
    bool ParseSubstitution(Type *out, bool *wasAbbrev = nullptr);
    bool ParseTemplateParam(Type *out);
    bool ParseCallOffset();
    Str  ParseCvQualifiers(bool *hadCv, bool *hadRef, bool *hadRRef);
    bool ParseBareFunctionType(Str *out, bool skipFirst, Type *firstType);
    bool ParseUnnamedTypeName(Str *out);
};

Str Parser::ParseOperatorName()
{
    struct Entry {
        char        Code[3];
        const char *Text;
    };
    static const Entry kOps[] = {
        {"nw", "operator new"},      {"na", "operator new[]"},
        {"dl", "operator delete"},   {"da", "operator delete[]"},
        {"ps", "operator+"},         {"ng", "operator-"},
        {"ad", "operator&"},         {"de", "operator*"},
        {"co", "operator~"},         {"pl", "operator+"},
        {"mi", "operator-"},         {"ml", "operator*"},
        {"dv", "operator/"},         {"rm", "operator%"},
        {"an", "operator&"},         {"or", "operator|"},
        {"eo", "operator^"},         {"aS", "operator="},
        {"pL", "operator+="},        {"mI", "operator-="},
        {"mL", "operator*="},        {"dV", "operator/="},
        {"rM", "operator%="},        {"aN", "operator&="},
        {"oR", "operator|="},        {"eO", "operator^="},
        {"ls", "operator<<"},        {"rs", "operator>>"},
        {"lS", "operator<<="},       {"rS", "operator>>="},
        {"eq", "operator=="},        {"ne", "operator!="},
        {"lt", "operator<"},         {"gt", "operator>"},
        {"le", "operator<="},        {"ge", "operator>="},
        {"ss", "operator<=>"},       {"nt", "operator!"},
        {"aa", "operator&&"},        {"oo", "operator||"},
        {"pp", "operator++"},        {"mm", "operator--"},
        {"cm", "operator,"},         {"pm", "operator->*"},
        {"pt", "operator->"},        {"cl", "operator()"},
        {"ix", "operator[]"},        {"qu", "operator?"},
        {"aw", "operator co_await"}, {"li", "operator\"\""},
    };

    if (End - P < 2) return Str();

    if (P[0] == 'c' && P[1] == 'v') {
        P += 2;
        Type t;
        bool saved = InTemplateArgs;
        InTemplateArgs = false;
        bool ok = ParseType(&t);
        InTemplateArgs = saved;
        if (!ok) return Str();
        Builder b(A);
        b.Add("operator ");
        b.Add(Flatten(A, t));
        return b.Done();
    }
    if (P[0] == 'l' && P[1] == 'i') {
        P += 2;
        Str name;
        if (!ParseSourceName(&name)) return Str();
        Builder b(A);
        b.Add("operator\"\" ");
        b.Add(name);
        return b.Done();
    }
    if (P[0] == 'v') {
        P += 1;
        if (Peek() < '0' || Peek() > '9') return Str();
        P++;
        Str name;
        if (!ParseSourceName(&name)) return Str();
        return name;
    }

    for (const Entry &e : kOps) {
        if (P[0] == e.Code[0] && P[1] == e.Code[1]) {
            P += 2;
            return Lit(e.Text);
        }
    }
    return Str();
}

bool Parser::ParseUnnamedTypeName(Str *out)
{
    if (Peek() != 'U') return false;

    if (PeekAt(1) == 't') {
        P += 2;
        long long n = -1;
        if (Peek() != '_') {
            if (!ParseNumber(&n)) return false;
        }
        if (!Eat('_')) return false;
        Builder b(A);
        b.Add("{unnamed type#");
        b.Add(Number(A, n + 2));
        b.Add('}');
        *out = b.Done();
        return true;
    }

    if (PeekAt(1) != 'l') return false;
    P += 2;

    const bool   savedLambda = InLambdaSig;
    InLambdaSig = true;
    Builder sig(A);
    sig.Add("{lambda(");
    if (Peek() == 'v') {
        P++;
    } else {
        bool first = true;
        while (!Done() && Peek() != 'E') {
            Type t;
            if (!ParseType(&t)) { InLambdaSig = savedLambda; return false; }
            if (!first) sig.Add(", ");
            first = false;
            sig.Add(Flatten(A, t));
        }
    }
    if (!Eat('E')) { InLambdaSig = savedLambda; return false; }
    sig.Add(")#");

    long long n = -1;
    if (Peek() != '_') {
        if (!ParseNumber(&n)) return false;
    }
    if (!Eat('_')) return false;
    sig.Add(Number(A, n + 2));
    sig.Add('}');
    InLambdaSig = savedLambda;
    *out = sig.Done();
    return true;
}

bool Parser::ParseUnqualifiedName(Str *out, bool *isCtorDtor, Str enclosing)
{
    *isCtorDtor = false;

    char c = Peek();
    if (c >= '0' && c <= '9') return ParseSourceName(out);

    if (c == 'C') {
        char k = PeekAt(1);
        if (k == 'I') {
            P += 2;
            if (Peek() < '1' || Peek() > '5') return false;
            P++;
            Type base;
            if (!ParseType(&base)) return false;
            *out        = BareName(Flatten(A, base));
            *isCtorDtor = true;
            return true;
        } else {
            if (k < '1' || k > '5') return false;
            P += 2;
        }
        *out        = BareName(enclosing);
        *isCtorDtor = true;
        return true;
    }
    if (c == 'D' && PeekAt(1) >= '0' && PeekAt(1) <= '5') {
        P += 2;
        Builder b(A);
        b.Add('~');
        b.Add(BareName(enclosing));
        *out        = b.Done();
        *isCtorDtor = true;
        return true;
    }
    if (c == 'U') return ParseUnnamedTypeName(out);
    if (c == 'L') {
        P++;
        return ParseUnqualifiedName(out, isCtorDtor, enclosing);
    }
    if (c == 'D' && PeekAt(1) == 'C') {
        P += 2;
        Builder b(A);
        b.Add('[');
        bool first = true;
        while (Peek() != 'E' && !Done()) {
            Str n;
            if (!ParseSourceName(&n)) return false;
            if (!first) b.Add(", ");
            first = false;
            b.Add(n);
        }
        if (!Eat('E')) return false;
        b.Add(']');
        *out = b.Done();
        return true;
    }

    Str op = ParseOperatorName();
    if (op.Empty()) return false;
    *out = op;
    return true;
}

bool Parser::ParseSubstitution(Type *out, bool *wasAbbrev)
{
    if (wasAbbrev) *wasAbbrev = false;
    if (!Eat('S')) return false;

    struct Abbrev {
        char        Code;
        const char *Text;
    };
    static const Abbrev kStd[] = {
        {'t', "std"},
        {'a', "std::allocator"},
        {'b', "std::basic_string"},
        {'s', "std::string"},
        {'i', "std::istream"},
        {'o', "std::ostream"},
        {'d', "std::iostream"},
    };
    for (const Abbrev &a : kStd) {
        if (Peek() == a.Code) {
            P++;
            if (wasAbbrev) *wasAbbrev = true;
            if (a.Code == 's') *out = PlainLit("std::basic_string<char, std::char_traits<char>, std::allocator<char> >");
            else if (a.Code == 'i') *out = PlainLit("std::basic_istream<char, std::char_traits<char> >");
            else if (a.Code == 'o') *out = PlainLit("std::basic_ostream<char, std::char_traits<char> >");
            else if (a.Code == 'd') *out = PlainLit("std::basic_iostream<char, std::char_traits<char> >");
            else *out = PlainLit(a.Text);
            return true;
        }
    }

    size_t index = 0;
    if (Peek() != '_') {
        size_t v = 0;
        while (!Done()) {
            char c = Peek();
            if (c >= '0' && c <= '9')      v = v * 36 + (size_t)(c - '0');
            else if (c >= 'A' && c <= 'Z') v = v * 36 + (size_t)(c - 'A' + 10);
            else break;
            P++;
        }
        index = v + 1;
    }
    if (!Eat('_')) return false;
    if (index >= SubCount) return false;
    *out = Resolve(Subs[index]);
    return true;
}

bool Parser::ParseTemplateParam(Type *out)
{
    if (!Eat('T')) return false;
    ParamUses++;

    size_t index = 0;
    if (Peek() != '_') {
        long long n = 0;
        if (!ParseNumber(&n) || n < 0) return false;
        index = (size_t)n + 1;
    }
    if (!Eat('_')) return false;

    if (InLambdaSig) {
        PackOpen = true;
        Builder b(A);
        b.Add("auto:");
        b.Add(Number(A, (long long)index + 1));
        *out = Plain(b.Done());
        return true;
    }
    if (index < TemplCount) {
        Type t = Templ[index];
        if (t.IsPack) {
            if (t.ElemCount > PackSize) PackSize = t.ElemCount;
            if (PackIndex < t.ElemCount) {
                *out = t.Elems[PackIndex];
                return true;
            }
            if (t.ElemCount == 0) {
                PackEmpty = true;
                *out = Type();
                return true;
            }
        }
        *out = t;
        return true;
    }
    PackOpen = true;
    Builder b(A);
    b.Add("auto:");
    b.Add(Number(A, (long long)index + 1));
    *out = Plain(b.Done());
    return true;
}

Type Wrap(Arena *a, Type t, const char *decl)
{
    Type r;

    if (t.Open) {
        Builder pre(a);
        pre.Add(t.Pre);
        pre.Add(decl);
        r.Pre  = pre.Done();
        r.Post = t.Post;
        r.Open = true;
        return r;
    }

    if (t.Pre.N && t.Pre.P[t.Pre.N - 1] == ' ') t.Pre.N--;
    if (!t.Post.Empty()) {
        Builder pre(a);
        pre.Add(t.Pre);
        pre.Add(" (");
        pre.Add(decl);
        r.Pre = pre.Done();

        Builder post(a);
        post.Add(')');
        post.Add(t.Post);
        r.Post = post.Done();
        r.Open = true;
        return r;
    }
    Builder pre(a);
    pre.Add(t.Pre);
    pre.Add(decl);
    r.Pre = pre.Done();
    return r;
}

Str Parser::ParseCvQualifiers(bool *hadCv, bool *hadRef, bool *hadRRef)
{
    Builder b(A);
    *hadCv = *hadRef = *hadRRef = false;

    bool restrict_ = false, volatile_ = false, const_ = false;
    for (;;) {
        if (Peek() == 'r') { restrict_ = true; P++; continue; }
        if (Peek() == 'V') { volatile_ = true; P++; continue; }
        if (Peek() == 'K') { const_ = true; P++; continue; }
        break;
    }
    if (const_)    { b.Add(" const"); *hadCv = true; }
    if (volatile_) { b.Add(" volatile"); *hadCv = true; }
    if (restrict_) { b.Add(" restrict"); *hadCv = true; }

    if (Peek() == 'R') { P++; *hadRef = true; }
    else if (Peek() == 'O') { P++; *hadRRef = true; }

    return b.Done();
}

bool Parser::ParseBuiltinType(Type *out)
{
    struct Entry {
        char        Code;
        const char *Text;
    };
    static const Entry kBuiltins[] = {
        {'v', "void"},           {'w', "wchar_t"},
        {'b', "bool"},           {'c', "char"},
        {'a', "signed char"},    {'h', "unsigned char"},
        {'s', "short"},          {'t', "unsigned short"},
        {'i', "int"},            {'j', "unsigned int"},
        {'l', "long"},           {'m', "unsigned long"},
        {'x', "long long"},      {'y', "unsigned long long"},
        {'n', "__int128"},       {'o', "unsigned __int128"},
        {'f', "float"},          {'d', "double"},
        {'e', "long double"},    {'g', "__float128"},
        {'z', "..."},
    };

    char c = Peek();
    for (const Entry &e : kBuiltins) {
        if (c == e.Code) {
            P++;
            *out = PlainLit(e.Text);
            return true;
        }
    }

    if (c == 'D') {
        char k = PeekAt(1);
        switch (k) {
        case 'a': P += 2; *out = PlainLit("auto"); return true;
        case 'c': P += 2; *out = PlainLit("decltype(auto)"); return true;
        case 'n': P += 2; *out = PlainLit("decltype(nullptr)"); return true;
        case 'i': P += 2; *out = PlainLit("char32_t"); return true;
        case 's': P += 2; *out = PlainLit("char16_t"); return true;
        case 'u': P += 2; *out = PlainLit("char8_t"); return true;
        case 'h': P += 2; *out = PlainLit("_Float16"); return true;
        case 'F': {
            P += 2;
            long long bits = 0;
            if (!ParseNumber(&bits)) return false;
            if (Peek() == 'x') P++;
            Builder b(A);
            b.Add("_Float");
            b.Add(Number(A, bits));
            *out = Plain(b.Done());
            return true;
        }
        default: break;
        }
    }
    return false;
}

bool Parser::ParseBareFunctionType(Str *out, bool skipFirst, Type *firstType)
{
    Builder b(A);
    b.Add('(');

    bool first = true;
    bool wroteAny = false;
    while (!Done() && Peek() != 'E' && Peek() != '.') {
        if ((Peek() == 'R' || Peek() == 'O') && PeekAt(1) == 'E') break;
        Type t;
        if (!ParseType(&t)) return false;
        if (first && skipFirst) {
            if (firstType) *firstType = t;
            first = false;
            continue;
        }
        first = false;
        if (!wroteAny && Same(Flatten(A, t), "void") &&
            (Done() || Peek() == 'E' || Peek() == '.' ||
             ((Peek() == 'R' || Peek() == 'O') && PeekAt(1) == 'E')))
            break;
        if (Flatten(A, t).Empty()) continue;
        if (wroteAny) b.Add(", ");
        wroteAny = true;
        b.Add(Flatten(A, t));
    }
    b.Add(')');
    *out = b.Done();
    return true;
}

bool Parser::ParseFunctionType(Type *out)
{
    if (!Eat('F')) return false;
    if (Peek() == 'Y') P++;

    Type ret;
    if (!ParseType(&ret)) return false;

    Str params;
    if (!ParseBareFunctionType(&params, false, nullptr)) return false;

    Builder post(A);
    post.Add(params);
    if (Peek() == 'R') { P++; post.Add(" &"); }
    else if (Peek() == 'O') { P++; post.Add(" &&"); }
    if (!Eat('E')) return false;

    out->Pre        = Join(A, Flatten(A, ret), Lit(" "));
    out->Post       = post.Done();
    out->IsFunction = true;
    return true;
}

bool Parser::ParseType(Type *out)
{
    if (Failed || Done()) return false;
    if (++Depth > kMaxDepth) { Fail(); return false; }
    TypeDepth++;
    struct DepthGuard {
        int *D;
        int *T;
        ~DepthGuard() { (*D)--; (*T)--; }
    } guard{&Depth, &TypeDepth};

    const char  *typeStart = P;
    const size_t paramMark = ParamUses;
    char         c         = Peek();

    if (c == 'r' || c == 'V' || c == 'K') {
        bool cv = false, ref = false, rref = false;
        Str  quals = ParseCvQualifiers(&cv, &ref, &rref);
        Type inner;
        if (!ParseType(&inner)) return false;
        Type r;
        if (inner.IsFunction && !inner.Open) {
            r.Pre  = inner.Pre;
            r.Post = Join(A, inner.Post, quals);
        } else {
            r.Pre  = Join(A, inner.Pre, quals);
            r.Post = inner.Post;
        }
        r.Open       = inner.Open;
        r.Reference  = inner.Reference;
        r.IsFunction = inner.IsFunction;
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }

    switch (c) {
    case 'P': {
        P++;
        Type inner;
        if (!ParseType(&inner)) return false;
        Type r = Wrap(A, inner, "*");
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'R': {
        P++;
        Type inner;
        if (!ParseType(&inner)) return false;
        Type r = inner.Reference != Type::NoRef ? inner : Wrap(A, inner, "&");
        r.Reference = Type::LRef;
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'O': {
        P++;
        Type inner;
        if (!ParseType(&inner)) return false;
        Type r = inner.Reference != Type::NoRef ? inner : Wrap(A, inner, "&&");
        if (inner.Reference == Type::NoRef) r.Reference = Type::RRef;
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'C': {
        P++;
        Type inner;
        if (!ParseType(&inner)) return false;
        Type r;
        r.Pre  = Join(A, inner.Pre, Lit(" complex"));
        r.Post = inner.Post;
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'G': {
        P++;
        Type inner;
        if (!ParseType(&inner)) return false;
        Type r;
        r.Pre  = Join(A, inner.Pre, Lit(" imaginary"));
        r.Post = inner.Post;
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'A': {
        P++;
        Builder dim(A);
        dim.Add(" [");
        if (Peek() != '_') {
            long long n = 0;
            if (ParseNumber(&n)) {
                dim.Add(Number(A, n));
            } else {
                Type e;
                if (!ParseExpression(&e)) return false;
                dim.Add(Flatten(A, e));
            }
        }
        dim.Add(']');
        if (!Eat('_')) return false;
        Type elem;
        if (!ParseType(&elem)) return false;
        Type r;
        r.Pre  = elem.Pre;
        r.Post = Join(A, dim.Done(), elem.Post);
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'M': {
        P++;
        Type cls;
        if (!ParseType(&cls)) return false;
        Type member;
        if (!ParseType(&member)) return false;
        Builder decl(A);
        decl.Add(Flatten(A, cls));
        decl.Add("::*");
        Type r = Wrap(A, member, decl.Done().P ? decl.Done().P : "::*");
        {
            Builder pre(A);
            Builder post(A);
            if (!member.Post.Empty()) {
                Str base = member.Pre;
                if (base.N && base.P[base.N - 1] == ' ') base.N--;
                pre.Add(base);
                pre.Add(" (");
                pre.Add(Flatten(A, cls));
                pre.Add("::*");
                post.Add(')');
                post.Add(member.Post);
            } else {
                Str base = member.Pre;
                if (base.N && base.P[base.N - 1] == ' ') base.N--;
                pre.Add(base);
                pre.Add(' ');
                pre.Add(Flatten(A, cls));
                pre.Add("::*");
            }
            r.Pre  = pre.Done();
            r.Post = post.Done();
        }
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'F': {
        Type r;
        if (!ParseFunctionType(&r)) return false;
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'T': {
        Type r;
        if (!ParseTemplateParam(&r)) return false;
        if (Peek() == 'I') {
            Str args;
            if (!ParseTemplateArgs(&args)) return false;
            Type t = Plain(Join(A, Flatten(A, r), args));
            AddSubRange(t, typeStart, paramMark);
            *out = t;
            return true;
        }
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    case 'S': {
        if (PeekAt(1) == 't') {
            Type n;
            if (!ParseName(&n)) return false;
            AddSubRange(n, typeStart, paramMark);
            *out = n;
            return true;
        }
        Type r;
        if (!ParseSubstitution(&r)) return false;
        if (Peek() == 'I') {
            Str args;
            if (!ParseTemplateArgs(&args)) return false;
            Type t = Plain(Join(A, Flatten(A, r), args));
            AddSubRange(t, typeStart, paramMark);
            *out = t;
            return true;
        }
        *out = r;
        return true;
    }
    case 'D': {
        if (PeekAt(1) == 'p') {
            P += 2;
            const char  *start    = P;
            const size_t subMark  = SubCount;
            const size_t savedIdx = PackIndex;
            const size_t savedLen = PackSize;

            PackIndex = 0;
            PackSize  = 0;
            PackEmpty = false;
            PackOpen  = false;
            Type firstElem;
            if (!ParseType(&firstElem)) { PackIndex = savedIdx; PackSize = savedLen; return false; }
            size_t total = PackSize;

            if (PackEmpty) {
                PackIndex = savedIdx;
                PackSize  = savedLen;
                PackEmpty = false;
                *out = Type();
                return true;
            }
            if (total == 0 && PackOpen) {
                PackIndex = savedIdx;
                PackSize  = savedLen;
                Builder b(A);
                b.Add('(');
                b.Add(Flatten(A, firstElem));
                b.Add(")...");
                *out = Plain(b.Done());
                return true;
            }
            if (total <= 1) {
                PackIndex = savedIdx;
                PackSize  = savedLen;
                *out = firstElem;
                return true;
            }

            Builder b(A);
            b.Add(Flatten(A, firstElem));
            for (size_t k = 1; k < total; k++) {
                P        = start;
                SubCount = subMark;
                PackIndex = k;
                Type elem;
                if (!ParseType(&elem)) { PackIndex = savedIdx; PackSize = savedLen; return false; }
                b.Add(", ");
                b.Add(Flatten(A, elem));
            }
            PackIndex = savedIdx;
            PackSize  = savedLen;
            *out = Plain(b.Done());
            return true;
        }
        if (PeekAt(1) == 'o' || PeekAt(1) == 'O') {
            bool computed = PeekAt(1) == 'O';
            P += 2;
            Str condition;
            if (computed) {
                Type e;
                if (!ParseExpression(&e)) return false;
                if (!Eat('E')) return false;
                condition = Flatten(A, e);
            }
            Type fn;
            if (!ParseFunctionType(&fn)) return false;
            Builder post(A);
            post.Add(fn.Post);
            post.Add(" noexcept");
            if (!condition.Empty()) {
                post.Add('(');
                post.Add(condition);
                post.Add(')');
            }
            Type r;
            r.Pre  = fn.Pre;
            r.Post = post.Done();
            AddSubRange(r, typeStart, paramMark);
            *out = r;
            return true;
        }
        if (PeekAt(1) == 't' || PeekAt(1) == 'T') {
            P += 2;
            Type e;
            if (!ParseExpression(&e)) return false;
            if (!Eat('E')) return false;
            Builder b(A);
            b.Add("decltype (");
            b.Add(Flatten(A, e));
            b.Add(')');
            Type r = Plain(b.Done());
            AddSubRange(r, typeStart, paramMark);
            *out = r;
            return true;
        }
        Type r;
        if (ParseBuiltinType(&r)) {
            *out = r;
            return true;
        }
        return false;
    }
    case 'U': {
        P++;
        Str name;
        if (!ParseSourceName(&name)) return false;
        if (Peek() == 'I') {
            Str args;
            if (!ParseTemplateArgs(&args)) return false;
            name = JoinArgs(A, name, args);
        }
        Type inner;
        if (!ParseType(&inner)) return false;
        Builder b(A);
        b.Add(inner.Pre);
        b.Add(' ');
        b.Add(name);
        Type r;
        r.Pre  = b.Done();
        r.Post = inner.Post;
        AddSubRange(r, typeStart, paramMark);
        *out = r;
        return true;
    }
    default:
        break;
    }

    Type builtin;
    if (ParseBuiltinType(&builtin)) {
        *out = builtin;
        return true;
    }

    Type name;
    if (!ParseName(&name)) return false;
    AddSubRange(name, typeStart, paramMark);
    *out = name;
    return true;
}

bool Parser::ParseTemplateArg(Type *out)
{
    if (Peek() == 'X') {
        P++;
        Type e;
        if (!ParseExpression(&e)) return false;
        if (!Eat('E')) return false;
        *out = e;
        return true;
    }
    if (Peek() == 'J') {
        P++;
        const size_t kMaxPack = 64;
        Type  *elems = (Type *)A->Take(sizeof(Type) * kMaxPack);
        size_t count = 0;
        Builder b(A);
        bool first = true;
        while (!Done() && Peek() != 'E') {
            Type t;
            if (!ParseTemplateArg(&t)) return false;
            if (count < kMaxPack) elems[count++] = t;
            if (!first) b.Add(", ");
            first = false;
            b.Add(Flatten(A, t));
        }
        if (!Eat('E')) return false;
        Type r = Plain(b.Done());
        r.Elems     = elems;
        r.ElemCount = count;
        r.IsPack    = true;
        *out = r;
        return true;
    }
    if (Peek() == 'L') return ParseExprPrimary(out);
    return ParseType(out);
}

bool Parser::ParseTemplateArgs(Str *out, bool nameLevel)
{
    if (!Eat('I')) return false;

    if (nameLevel && TypeDepth == 0) TemplCount = 0;

    bool saved = InTemplateArgs;
    InTemplateArgs = true;

    Builder b(A);
    b.Add('<');
    bool   first        = true;
    bool   lastWasAngle = false;
    size_t base         = TemplCount;

    while (!Done() && Peek() != 'E') {
        Type t;
        if (!ParseTemplateArg(&t)) { InTemplateArgs = saved; return false; }
        if (nameLevel && TypeDepth == 0 && TemplCount < kMaxTemplate) Templ[TemplCount++] = t;
        Str text = Flatten(A, t);
        if (text.Empty()) {
            lastWasAngle = false;
            continue;
        }
        if (!first) b.Add(", ");
        first = false;
        b.Add(text);
        lastWasAngle = text.N && text.P[text.N - 1] == '>';
    }
    if (!Eat('E')) { InTemplateArgs = saved; return false; }
    (void)base;

    if (lastWasAngle) b.Add(' ');
    b.Add('>');

    InTemplateArgs = saved;
    *out = b.Done();
    return true;
}

bool Parser::ParseLocalName(Type *out)
{
    if (!Eat('Z')) return false;

    Type   savedTempl[kMaxTemplate];
    size_t savedCount = TemplCount;
    for (size_t i = 0; i < TemplCount; i++) savedTempl[i] = Templ[i];
    const int savedTypeDepth = TypeDepth;
    TypeDepth = 0;

    Type enclosing;
    if (!ParseName(&enclosing)) { TypeDepth = savedTypeDepth; return false; }

    const bool enclosingHasReturn = NameIsTemplate && !NameIsCtorDtor;
    Str params;
    if (Peek() != 'E') {
        Type ret;
        if (!ParseBareFunctionType(&params, enclosingHasReturn, &ret)) {
            TypeDepth = savedTypeDepth;
            return false;
        }
    }
    TypeDepth  = savedTypeDepth;
    TemplCount = savedCount;
    for (size_t i = 0; i < savedCount; i++) Templ[i] = savedTempl[i];
    if (!Eat('E')) return false;

    Str trailing;
    Builder b(A);
    b.Add(enclosing.Pre);
    b.Add(params);
    b.Add(enclosing.Post);
    b.Add("::");

    if (Peek() == 's') {
        P++;
        b.Add("string literal");
    } else if (Peek() == 'd') {
        P += 1;
        long long n = 0;
        if (Peek() != '_') ParseNumber(&n);
        if (!Eat('_')) return false;
        Type inner;
        if (!ParseName(&inner)) return false;
        b.Add(Flatten(A, inner));
    } else {
        Type inner;
        if (!ParseName(&inner)) return false;
        b.Add(inner.Pre);
        trailing = inner.Post;
    }

    if (Peek() == '_') {
        const char *save = P;
        P++;
        if (Peek() == '_') {
            P++;
            long long n = 0;
            if (!ParseNumber(&n) || !Eat('_')) P = save;
        } else {
            long long n = 0;
            if (!ParseNumber(&n)) P = save;
        }
    }

    Type r;
    r.Pre  = b.Done();
    r.Post = trailing;
    *out = r;
    return true;
}

bool Parser::ParseNestedName(Type *out)
{
    if (!Eat('N')) return false;
    Eat('H');

    bool cv = false, ref = false, rref = false;
    Str  quals = ParseCvQualifiers(&cv, &ref, &rref);

    Str  path;
    Str  last;
    bool any = false;
    bool pending = false;

    while (!Done() && Peek() != 'E') {
        if (Failed) return false;
        if (pending) { AddSub(Plain(path)); pending = false; }

        Type component;
        Str  text;

        char c = Peek();
        if (c == 'S') {
            bool abbrev = false;
            if (!ParseSubstitution(&component, &abbrev)) return false;
            text = Flatten(A, component);
            path = any ? Join(A, Join(A, path, Lit("::")), text) : text;
            last = text;
            any  = true;
            if (Peek() == 'I') {
                Str args;
                if (!ParseTemplateArgs(&args, true)) return false;
                path = JoinArgs(A, path, args);
                if (TypeDepth == 0) NameIsTemplate = true;
            } else {
                NameIsTemplate = false;
            }
            (void)abbrev;
            pending = false;
            continue;
        }
        if (c == 'T') {
            if (!ParseTemplateParam(&component)) return false;
            text = Flatten(A, component);
            path = any ? Join(A, Join(A, path, Lit("::")), text) : text;
            last = text;
            any  = true;
            pending = true;
            NameIsTemplate = false;
            NameIsCtorDtor = false;
            continue;
        }
        if (c == 'D' && PeekAt(1) == 't') {
            Type d;
            if (!ParseType(&d)) return false;
            text = Flatten(A, d);
            path = any ? Join(A, Join(A, path, Lit("::")), text) : text;
            last = text;
            any  = true;
            continue;
        }
        if (c == 'M') {
            P++;
            continue;
        }
        if (c == 'F') {
            P++;
            bool ctor = false;
            Str  fname;
            if (!ParseUnqualifiedName(&fname, &ctor, last)) return false;
            Builder fb(A);
            fb.Add(fname);
            fb.Add("[friend]");
            text = fb.Done();
            path = any ? Join(A, Join(A, path, Lit("::")), text) : text;
            last = text;
            any  = true;
            if (Peek() == 'I') {
                AddSub(Plain(path));
                Str args;
                if (!ParseTemplateArgs(&args, true)) return false;
                path = JoinArgs(A, path, args);
                NameIsTemplate = true;
            } else {
                NameIsTemplate = false;
            }
            NameIsCtorDtor = false;
            pending = true;
            continue;
        }

        bool isCtorDtor = false;
        Str  name;
        if (!ParseUnqualifiedName(&name, &isCtorDtor, last)) return false;
        text = name;
        path = any ? Join(A, Join(A, path, Lit("::")), text) : text;
        last = text;
        any  = true;

        if (Peek() == 'I') {
            AddSub(Plain(path));
            Str args;
            if (!ParseTemplateArgs(&args, true)) return false;
            path = JoinArgs(A, path, args);
            last = JoinArgs(A, text, args);
            NameIsTemplate = true;
        } else {
            NameIsTemplate = false;
        }
        NameIsCtorDtor = isCtorDtor;
        pending = true;
    }
    if (!Eat('E')) return false;

    Type r;
    r.Pre = path;
    if (cv || ref || rref) {
        Builder b(A);
        b.Add(quals);
        if (ref)  b.Add(" &");
        if (rref) b.Add(" &&");
        r.Post = b.Done();
    }
    *out = r;
    return true;
}

bool Parser::ParseUnscopedName(Type *out)
{
    bool isStd = false;
    if (Peek() == 'S' && PeekAt(1) == 't') {
        P += 2;
        isStd = true;
    }

    bool isCtorDtor = false;
    Str  name;
    if (!ParseUnqualifiedName(&name, &isCtorDtor, Str())) return false;

    if (isStd) name = Join(A, Lit("std::"), name);

    if (Peek() == 'I') {
        AddSub(Plain(name));
        Str args;
        if (!ParseTemplateArgs(&args, true)) return false;
        name = JoinArgs(A, name, args);
        NameIsTemplate = true;
    } else {
        NameIsTemplate = false;
    }
    NameIsCtorDtor = isCtorDtor;
    *out = Plain(name);
    return true;
}

bool Parser::ParseName(Type *out)
{
    if (Failed || Done()) return false;

    char c = Peek();
    if (c == 'N') return ParseNestedName(out);
    if (c == 'Z') return ParseLocalName(out);

    if (c == 'S' && PeekAt(1) != 't') {
        Type sub;
        if (!ParseSubstitution(&sub)) return false;
        if (Peek() == 'I') {
            Str args;
            if (!ParseTemplateArgs(&args)) return false;
            Type r = Plain(Join(A, Flatten(A, sub), args));
            AddSub(r);
            *out = r;
            return true;
        }
        *out = sub;
        return true;
    }

    return ParseUnscopedName(out);
}

bool Parser::ParseExprPrimary(Type *out)
{
    if (!Eat('L')) return false;

    if (Peek() == '_' && PeekAt(1) == 'Z') {
        P += 2;
        Type inner;
        if (!ParseName(&inner)) return false;
        if (!Eat('E')) return false;
        *out = inner;
        return true;
    }

    Type t;
    if (!ParseType(&t)) return false;
    Str typeText = Flatten(A, t);

    if (Same(typeText, "bool")) {
        long long v = 0;
        if (!ParseNumber(&v)) return false;
        if (!Eat('E')) return false;
        *out = PlainLit(v ? "true" : "false");
        return true;
    }
    if (Same(typeText, "std::nullptr_t")) {
        if (Peek() == '0') P++;
        if (!Eat('E')) return false;
        *out = PlainLit("nullptr");
        return true;
    }

    Builder digits(A);
    while (!Done() && Peek() != 'E') {
        digits.Add(Peek());
        P++;
    }
    if (!Eat('E')) return false;
    Str raw = digits.Done();

    Builder value(A);
    for (size_t i = 0; i < raw.N; i++) {
        if (i == 0 && raw.P[i] == 'n') value.Add('-');
        else value.Add(raw.P[i]);
    }
    Str v = value.Done();

    if (Same(typeText, "int")) {
        *out = Plain(v);
        return true;
    }
    if (Same(typeText, "unsigned int")) {
        *out = Plain(Join(A, v, Lit("u")));
        return true;
    }
    if (Same(typeText, "long")) {
        *out = Plain(Join(A, v, Lit("l")));
        return true;
    }
    if (Same(typeText, "unsigned long")) {
        *out = Plain(Join(A, v, Lit("ul")));
        return true;
    }
    if (Same(typeText, "long long")) {
        *out = Plain(Join(A, v, Lit("ll")));
        return true;
    }
    if (Same(typeText, "unsigned long long")) {
        *out = Plain(Join(A, v, Lit("ull")));
        return true;
    }

    Builder b(A);
    b.Add('(');
    b.Add(typeText);
    b.Add(')');
    b.Add(v);
    *out = Plain(b.Done());
    return true;
}

bool Parser::ParseExpression(Type *out)
{
    if (Failed || Done()) return false;
    if (++Depth > kMaxDepth) { Fail(); return false; }
    struct DepthGuard {
        int *D;
        ~DepthGuard() { (*D)--; }
    } guard{&Depth};

    if (Peek() == 'L') return ParseExprPrimary(out);
    if (Peek() == 'T') return ParseTemplateParam(out);

    struct Binary {
        char        Code[3];
        const char *Text;
    };
    static const Binary kBinary[] = {
        {"pl", "+"},  {"mi", "-"},  {"ml", "*"},  {"dv", "/"},
        {"rm", "%"},  {"an", "&"},  {"or", "|"},  {"eo", "^"},
        {"ls", "<<"}, {"rs", ">>"}, {"lt", "<"},  {"gt", ">"},
        {"le", "<="}, {"ge", ">="}, {"eq", "=="}, {"ne", "!="},
        {"aa", "&&"}, {"oo", "||"}, {"cm", ", "}, {"ss", "<=>"},
    };
    static const Binary kUnary[] = {
        {"nt", "!"}, {"ng", "-"}, {"ps", "+"}, {"co", "~"},
        {"de", "*"}, {"ad", "&"},
    };

    if (End - P >= 2) {
        for (const Binary &b : kBinary) {
            if (P[0] == b.Code[0] && P[1] == b.Code[1]) {
                P += 2;
                Type lhs, rhs;
                if (!ParseExpression(&lhs)) return false;
                if (!ParseExpression(&rhs)) return false;
                Builder t(A);
                if (lhs.IsCompound) t.Add('(');
                t.Add(Flatten(A, lhs));
                if (lhs.IsCompound) t.Add(')');
                t.Add(b.Text);
                if (rhs.IsCompound) t.Add('(');
                t.Add(Flatten(A, rhs));
                if (rhs.IsCompound) t.Add(')');
                Type r = Plain(t.Done());
                r.IsCompound = true;
                *out = r;
                return true;
            }
        }
        for (const Binary &u : kUnary) {
            if (P[0] == u.Code[0] && P[1] == u.Code[1]) {
                P += 2;
                Type operand;
                if (!ParseExpression(&operand)) return false;
                Builder t(A);
                t.Add(u.Text);
                t.Add('(');
                t.Add(Flatten(A, operand));
                t.Add(')');
                Type r = Plain(t.Done());
                r.IsCompound = true;
                *out = r;
                return true;
            }
        }

        if (P[0] == 's' && (P[1] == 'z' || P[1] == 'Z')) {
            bool pack = P[1] == 'Z';
            P += 2;
            Type operand;
            if (!ParseExpression(&operand) && !ParseType(&operand)) return false;
            Builder t(A);
            t.Add(pack ? "sizeof...(" : "sizeof (");
            t.Add(Flatten(A, operand));
            t.Add(')');
            *out = Plain(t.Done());
            return true;
        }
        if (P[0] == 's' && P[1] == 't') {
            P += 2;
            Type t;
            if (!ParseType(&t)) return false;
            Builder b(A);
            b.Add("sizeof (");
            b.Add(Flatten(A, t));
            b.Add(')');
            *out = Plain(b.Done());
            return true;
        }
        if (P[0] == 'a' && P[1] == 't') {
            P += 2;
            Type t;
            if (!ParseType(&t)) return false;
            Builder b(A);
            b.Add("alignof (");
            b.Add(Flatten(A, t));
            b.Add(')');
            *out = Plain(b.Done());
            return true;
        }
        if (P[0] == 'f' && (P[1] == 'p' || P[1] == 'L')) {
            P += 2;
            long long n = 0;
            if (Peek() != '_') ParseNumber(&n);
            if (Peek() == 'p') {
                P++;
                if (Peek() != '_') ParseNumber(&n);
            }
            if (!Eat('_')) return false;
            Builder b(A);
            b.Add("{parm#");
            b.Add(Number(A, n + 1));
            b.Add('}');
            *out = Plain(b.Done());
            return true;
        }
        if (P[0] == 'd' && (P[1] == 't' || P[1] == 'n')) {
            P += 2;
            Type obj;
            if (!ParseExpression(&obj)) return false;
            Str name;
            bool ctor = false;
            if (!ParseUnqualifiedName(&name, &ctor, Str())) return false;
            Builder b(A);
            b.Add(Flatten(A, obj));
            b.Add('.');
            b.Add(name);
            *out = Plain(b.Done());
            return true;
        }
        if (P[0] == 's' && P[1] == 'r') {
            P += 2;
            Type scope;
            if (!ParseType(&scope)) return false;
            Str name;
            bool ctor = false;
            if (!ParseUnqualifiedName(&name, &ctor, Str())) return false;
            Builder b(A);
            b.Add(Flatten(A, scope));
            b.Add("::");
            b.Add(name);
            *out = Plain(b.Done());
            return true;
        }
        if (P[0] == 'c' && P[1] == 'l') {
            P += 2;
            Type callee;
            if (!ParseExpression(&callee)) return false;
            Builder b(A);
            b.Add(Flatten(A, callee));
            b.Add('(');
            bool first = true;
            while (!Done() && Peek() != 'E') {
                Type arg;
                if (!ParseExpression(&arg)) return false;
                if (!first) b.Add(", ");
                first = false;
                b.Add(Flatten(A, arg));
            }
            if (!Eat('E')) return false;
            b.Add(')');
            *out = Plain(b.Done());
            return true;
        }
        if (P[0] == 't' && P[1] == 'l') {
            P += 2;
            Type t;
            if (!ParseType(&t)) return false;
            Builder b(A);
            b.Add(Flatten(A, t));
            b.Add('{');
            bool first = true;
            while (!Done() && Peek() != 'E') {
                Type e;
                if (!ParseExpression(&e)) return false;
                if (!first) b.Add(", ");
                first = false;
                b.Add(Flatten(A, e));
            }
            if (!Eat('E')) return false;
            b.Add('}');
            *out = Plain(b.Done());
            return true;
        }
    }

    return ParseType(out);
}

bool Parser::ParseCallOffset()
{
    if (Peek() == 'h') {
        P++;
        long long n = 0;
        if (!ParseNumber(&n)) return false;
        return Eat('_');
    }
    if (Peek() == 'v') {
        P++;
        long long a = 0, b = 0;
        if (!ParseNumber(&a) || !Eat('_')) return false;
        if (!ParseNumber(&b) || !Eat('_')) return false;
        return true;
    }
    return false;
}

}


extern "C" char *__cxa_demangle(const char *mangled, char *buf, size_t *n, int *status)
{
    enum { kOk = 0, kMemory = -1, kInvalid = -2, kArgs = -3 };

    auto finish = [&](int code) -> char * {
        if (status) *status = code;
        return nullptr;
    };

    if (!mangled) return finish(kArgs);
    if (buf && !n) return finish(kArgs);

    size_t len = strlen(mangled);

    if (len < 2 || mangled[0] != '_' || mangled[1] != 'Z') return finish(kInvalid);

    size_t body = len;

    Arena arena;
    Str   result;
    bool  ok = false;

    for (;;) {
        const char *p   = mangled + 2;
        size_t      rem = body - 2;

        const char *tlsKind = nullptr;
        if (rem >= 2 && p[0] == 'T' && (p[1] == 'W' || p[1] == 'H')) {
            tlsKind = p[1] == 'W' ? "TLS wrapper function for "
                                  : "TLS init function for ";
            p += 2;
            rem -= 2;
        }

        bool        isThunk    = false;
        bool        isCovariant = false;
        const char *thunkKind  = "virtual thunk to ";
        if (rem >= 2 && p[0] == 'T' && (p[1] == 'h' || p[1] == 'v' || p[1] == 'c')) {
            isThunk     = true;
            isCovariant = p[1] == 'c';
            if (p[1] == 'h') thunkKind = "non-virtual thunk to ";
            if (p[1] == 'c') thunkKind = "covariant return thunk to ";
            p += 1;
            rem -= 1;
        }

        Parser parser(&arena, p, rem);

        if (tlsKind) {
            Type target;
            if (parser.ParseName(&target)) {
                Builder b(&arena);
                b.Add(tlsKind);
                b.Add(Flatten(&arena, target));
                result = b.Done();
                ok     = parser.Done() && !parser.Failed;
            }
        } else if (isThunk) {
            if (parser.ParseCallOffset() && (!isCovariant || parser.ParseCallOffset())) {
                Type target;
                if (parser.ParseName(&target)) {
                    Str params;
                    if (!parser.Done())
                        parser.ParseBareFunctionType(&params, true, nullptr);
                    Builder b(&arena);
                    b.Add(thunkKind);
                    b.Add(Flatten(&arena, target));
                    b.Add(params);
                    result = b.Done();
                    ok     = parser.Done() && !parser.Failed;
                }
            }
        } else {
            Type name;
            if (parser.ParseName(&name)) {
                Builder b(&arena);
                b.Add(name.Pre);

                bool bodyOk = true;
                if (!parser.Done() && !parser.Failed) {
                    Str  params;
                    Type ret;
                    const bool hasReturn = parser.NameIsTemplate && !parser.NameIsCtorDtor;
                    bodyOk = parser.ParseBareFunctionType(&params, hasReturn, &ret);
                    if (bodyOk) {
                        if (hasReturn) {
                            Builder full(&arena);
                            if (!ret.Post.Empty()) {
                                full.Add(ret.Pre);
                                full.Add(b.Done());
                                full.Add(params);
                                full.Add(ret.Post);
                                full.Add(name.Post);
                                result = full.Done();
                                ok     = parser.Done() && !parser.Failed;
                                goto emitted;
                            }
                            full.Add(ret.Pre);
                            full.Add(' ');
                            full.Add(b.Done());
                            b = full;
                        }
                        b.Add(params);
                        b.Add(name.Post);
                    }
                } else {
                    b.Add(name.Post);
                }
                result = b.Done();
                ok     = bodyOk && parser.Done() && !parser.Failed;
            emitted:;
            }
        }

        if (ok) break;
        size_t dot = 0;
        for (size_t i = 2; i < body; i++)
            if (mangled[i] == '.') dot = i;
        if (dot == 0) break;
        body = dot;
        arena.Release();
        result = Str();
    }

    if (!ok || arena.Bad || result.Empty()) {
        arena.Release();
        return finish(arena.Bad ? kMemory : kInvalid);
    }

    size_t need = result.N + (len - body) + 16;
    char  *dst;
    if (buf && n && *n >= need) {
        dst = buf;
    } else {
        dst = (char *)malloc(need);
        if (!dst) {
            arena.Release();
            return finish(kMemory);
        }
        if (n) *n = need;
    }

    memcpy(dst, result.P, result.N);
    size_t at = result.N;
    if (body < len) {
        memcpy(dst + at, " [clone ", 8);
        at += 8;
        memcpy(dst + at, mangled + body, len - body);
        at += len - body;
        dst[at++] = ']';
    }
    dst[at] = '\0';

    arena.Release();
    if (status) *status = kOk;
    return dst;
}