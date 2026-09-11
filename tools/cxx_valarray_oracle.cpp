#include <cstdio>
#include <cstddef>
#include <cmath>

#ifdef BOXCXX_COLUMN
#  include <__bits/valarray_core>
#else
#  include <valarray>
#endif

#ifdef __clang__
#  pragma clang diagnostic ignored "-Wself-assign-overloaded"
#endif

using std::gslice;
using std::slice;
using std::valarray;

static void Pd(const char *id, const valarray<double> &v)
{
    printf("%-30s [", id);
    for (size_t i = 0; i < v.size(); ++i) printf("%s%.17g", i ? "," : "", v[i]);
    printf("]\n");
}
static void Pi(const char *id, const valarray<int> &v)
{
    printf("%-30s [", id);
    for (size_t i = 0; i < v.size(); ++i) printf("%s%d", i ? "," : "", v[i]);
    printf("]\n");
}
static void Pb(const char *id, const valarray<bool> &v)
{
    printf("%-30s [", id);
    for (size_t i = 0; i < v.size(); ++i) printf("%s%d", i ? "," : "", (int)v[i]);
    printf("]\n");
}
static void Ps(const char *id, double x) { printf("%-30s %.17g\n", id, x); }
static void Pn(const char *id, size_t n) { printf("%-30s %zu\n", id, n); }

static valarray<double> D6()
{
    return valarray<double>{1.5, -2.25, 3.125, -4.0625, 5.03125, -6.015625};
}
static valarray<int> I6() { return valarray<int>{7, -3, 12, 5, -9, 40}; }

static void Big()
{
    const size_t N = 200003;
    valarray<double> a(N), b(N), c(N);
    for (size_t i = 0; i < N; ++i) {
        a[i] = double((long)(i % 1024) - 512);
        b[i] = double((long)((i * 7) % 256) - 128);
        c[i] = double(1 << (i % 8));
    }
    valarray<double> r = a + b * c - a / 2.0;
    Pn("big/size", r.size());
    Ps("big/fused.sum", r.sum());
    Ps("big/fused.min", r.min());
    Ps("big/fused.max", r.max());
    Ps("big/fused.first", r[0]);
    Ps("big/fused.mid", r[N / 2]);
    Ps("big/fused.last", r[N - 1]);

    valarray<double> t = a;
    t += b;
    t *= 2.0;
    Ps("big/compound.sum", t.sum());

    valarray<double> sl = r[slice(1, N / 3, 3)];
    Pn("big/slice.size", sl.size());
    Ps("big/slice.sum", sl.sum());

    valarray<size_t> sz{100, 200}, st{7, 1};
    valarray<double> gs = r[gslice(3, sz, st)];
    Pn("big/gslice.size", gs.size());
    Ps("big/gslice.sum", gs.sum());

    valarray<bool> m = a > 0.0;
    valarray<double> ms = r[m];
    Pn("big/mask.size", ms.size());
    Ps("big/mask.sum", ms.sum());

    valarray<size_t> ix(N / 4);
    for (size_t i = 0; i < ix.size(); ++i) ix[i] = (i * 3) % N;
    valarray<double> id = r[ix];
    Pn("big/indirect.size", id.size());
    Ps("big/indirect.sum", id.sum());

    valarray<double> sh = r.shift((int)(N / 3));
    Ps("big/shift.sum", sh.sum());
    valarray<double> cs = r.cshift(-(int)(N / 3));
    Ps("big/cshift.sum", cs.sum());
    Ps("big/cshift.first", cs[0]);

    valarray<double> ap = a.apply([](double x) { return x < 0 ? -x : x; });
    Ps("big/apply.sum", ap.sum());

    valarray<double> w = a;
    w[slice(0, N / 2, 2)] = 3.0;
    Ps("big/slice-write.sum", w.sum());
    valarray<double> w2 = a;
    w2[m] = -1.0;
    Ps("big/mask-write.sum", w2.sum());
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'b') {
        Big();
        return 0;
    }
    { valarray<double> v;            Pn("cons/default.size", v.size()); Pd("cons/default", v); }
    { valarray<double> v(4);         Pd("cons/n", v); }
    { valarray<double> v(2.5, 3);    Pd("cons/val-n", v); }
    { const double a[] = {1, 2, 3};  valarray<double> v(a, 3); Pd("cons/ptr-n", v); }
    { valarray<double> v{1, 2, 3};   Pd("cons/init-list", v); }
    { valarray<double> s = D6(), v(s); Pd("cons/copy", v); }
    { valarray<double> s = D6(), v(static_cast<valarray<double> &&>(s));
      Pd("cons/move", v); Pn("cons/move.src-size", s.size()); }
    { valarray<double> v(0);         Pn("cons/zero.size", v.size()); Pd("cons/zero", v); }

    { valarray<double> v(9.0, 6); v = D6();       Pd("assign/valarray", v); }
    { valarray<double> v = D6();  v = 7.25;       Pd("assign/scalar", v); }
    { valarray<double> v = D6();  v = {8, 9};     Pd("assign/init-list", v); }
    { valarray<double> v = D6();  v = v;          Pd("assign/self", v); }
    { valarray<double> v = D6();  v = v[slice(0, 6, 1)]; Pd("assign/self-slice", v); }
    { valarray<double> v = D6();  v += v;         Pd("assign/self-add", v); }
    { valarray<double> v(1.0, 2);  v = D6();      Pd("assign/grow", v); }
    { valarray<double> v = D6();   v = valarray<double>{1, 2}; Pd("assign/shrink", v); }

    { const valarray<double> v = D6(); Ps("access/const-index", v[4]); }
    { valarray<double> v = D6(); v[4] = 99.5;     Pd("access/index-write", v); }
    { const valarray<double> v = D6();
      valarray<double> r = v[slice(1, 3, 2)];     Pd("sub/slice-const", r); }
    { valarray<double> v = D6();
      valarray<double> r = v[slice(0, 3, 2)];     Pd("sub/slice-nonconst", r); }
    { valarray<double> v = D6(); v[slice(0, 3, 2)] = 0.0; Pd("sub/slice-assign-scalar", v); }
    { valarray<double> v = D6(); v[slice(0, 3, 2)] = valarray<double>{10, 20, 30};
      Pd("sub/slice-assign-va", v); }
    { valarray<double> v = D6(); v[slice(1, 3, 2)] *= valarray<double>{2, 2, 2};
      Pd("sub/slice-compound", v); }
    { const valarray<double> v = D6();
      valarray<double> r = v[slice(2, 0, 1)];     Pd("sub/slice-empty", r); }
    { const valarray<double> v = D6();
      valarray<double> r = v[slice(2, 3, 0)];     Pd("sub/slice-stride0", r); }

    { const valarray<double> v = D6();
      valarray<size_t> sz{2, 3}, st{3, 1};
      valarray<double> r = v[gslice(0, sz, st)];  Pd("sub/gslice-2d", r); }
    { const valarray<double> v = D6();
      valarray<size_t> sz{3}, st{2};
      valarray<double> r = v[gslice(0, sz, st)];  Pd("sub/gslice-1d", r); }
    { const valarray<double> v = D6();
      valarray<size_t> sz, st;
      valarray<double> r = v[gslice(1, sz, st)];  Pd("sub/gslice-empty", r); }
    { valarray<double> v = D6();
      valarray<size_t> sz{2, 2}, st{3, 1};
      v[gslice(0, sz, st)] = 0.5;                 Pd("sub/gslice-assign", v); }

    { const valarray<double> v = D6();
      valarray<bool> m{true, false, true, false, true, false};
      valarray<double> r = v[m];                  Pd("sub/mask", r); }
    { valarray<double> v = D6();
      valarray<bool> m{false, true, false, true, false, true};
      v[m] = -1.0;                                Pd("sub/mask-assign", v); }
    { const valarray<double> v = D6();
      valarray<bool> m(false, 6);
      valarray<double> r = v[m];                  Pd("sub/mask-none", r); }

    { const valarray<double> v = D6();
      valarray<size_t> ix{5, 0, 3};
      valarray<double> r = v[ix];                 Pd("sub/indirect", r); }
    { valarray<double> v = D6();
      valarray<size_t> ix{5, 0, 3};
      v[ix] = valarray<double>{100, 200, 300};    Pd("sub/indirect-assign", v); }
    { const valarray<double> v = D6();
      valarray<size_t> ix;
      valarray<double> r = v[ix];                 Pd("sub/indirect-empty", r); }

    { valarray<double> r = +D6(); Pd("unary/plus", r); }
    { valarray<double> r = -D6(); Pd("unary/minus", r); }
    { valarray<int> r = ~I6();    Pi("unary/bitnot", r); }
    { valarray<bool> r = !I6();   Pb("unary/not", r); }

#define BINOP_D(tag, op)                                                      \
    { valarray<double> r = D6() op D6();       Pd("bin/" tag "/vv", r); }     \
    { valarray<double> r = D6() op 2.5;        Pd("bin/" tag "/vs", r); }     \
    { valarray<double> r = 2.5 op D6();        Pd("bin/" tag "/sv", r); }
    BINOP_D("add", +) BINOP_D("sub", -) BINOP_D("mul", *) BINOP_D("div", /)
#undef BINOP_D
#define BINOP_I(tag, op)                                                      \
    { valarray<int> r = I6() op I6();          Pi("bin/" tag "/vv", r); }     \
    { valarray<int> r = I6() op 3;             Pi("bin/" tag "/vs", r); }     \
    { valarray<int> r = 3 op I6();             Pi("bin/" tag "/sv", r); }
    BINOP_I("mod", %) BINOP_I("and", &) BINOP_I("or", |) BINOP_I("xor", ^)
#undef BINOP_I
    { valarray<int> r = I6() << valarray<int>(2, 6); Pi("bin/shl/vv", r); }
    { valarray<int> r = I6() << 2;                   Pi("bin/shl/vs", r); }
    { valarray<int> r = 256 >> valarray<int>{1, 2, 3, 4, 5, 6}; Pi("bin/shr/sv", r); }
    { valarray<double> a = D6(), b = D6() * 2.0, c = D6() + 1.0;
      valarray<double> r = a + b * c - a / 2.0;      Pd("bin/fused-4", r); }

#define CMP(tag, op)                                                          \
    { valarray<bool> r = D6() op D6();         Pb("cmp/" tag "/vv", r); }     \
    { valarray<bool> r = D6() op 0.0;          Pb("cmp/" tag "/vs", r); }     \
    { valarray<bool> r = 0.0 op D6();          Pb("cmp/" tag "/sv", r); }
    CMP("eq", ==) CMP("ne", !=) CMP("lt", <) CMP("gt", >) CMP("le", <=) CMP("ge", >=)
#undef CMP
    { valarray<bool> a{true, false, true}, b{true, true, false};
      valarray<bool> r1 = a && b, r2 = a || b;
      Pb("cmp/logand", r1); Pb("cmp/logor", r2); }

#define CASSIGN_D(tag, op)                                                    \
    { valarray<double> v = D6(); v op D6();    Pd("cas/" tag "/v", v); }      \
    { valarray<double> v = D6(); v op 2.5;     Pd("cas/" tag "/s", v); }
    CASSIGN_D("add", +=) CASSIGN_D("sub", -=) CASSIGN_D("mul", *=) CASSIGN_D("div", /=)
#undef CASSIGN_D
#define CASSIGN_I(tag, op)                                                    \
    { valarray<int> v = I6(); v op I6();       Pi("cas/" tag "/v", v); }      \
    { valarray<int> v = I6(); v op 3;          Pi("cas/" tag "/s", v); }
    CASSIGN_I("mod", %=) CASSIGN_I("and", &=) CASSIGN_I("or", |=) CASSIGN_I("xor", ^=)
#undef CASSIGN_I
    { valarray<int> cnt{0, 1, 2, 3, 4, 5};
      valarray<int> v = I6(); v <<= cnt; Pi("cas/shl/v", v); }
    { valarray<int> v = I6(); v <<= 2;   Pi("cas/shl/s", v); }
    { valarray<int> cnt{0, 1, 2, 3, 4, 5};
      valarray<int> v = I6(); v >>= cnt; Pi("cas/shr/v", v); }
    { valarray<int> v = I6(); v >>= 1;   Pi("cas/shr/s", v); }

    { valarray<double> v = D6(); Pn("mem/size", v.size()); }
    { valarray<double> v = D6(); Ps("mem/sum", v.sum()); }
    { valarray<double> v{4.0}; Ps("mem/sum-one", v.sum()); }
    { valarray<double> v = D6(); Ps("mem/min", v.min()); Ps("mem/max", v.max()); }
    { valarray<double> r = D6().shift(2);   Pd("mem/shift+2", r); }
    { valarray<double> r = D6().shift(-2);  Pd("mem/shift-2", r); }
    { valarray<double> r = D6().shift(0);   Pd("mem/shift0", r); }
    { valarray<double> r = D6().shift(99);  Pd("mem/shift-past", r); }
    { valarray<double> r = D6().shift(-99); Pd("mem/shift-past-neg", r); }
    { valarray<double> r = D6().cshift(2);  Pd("mem/cshift+2", r); }
    { valarray<double> r = D6().cshift(-2); Pd("mem/cshift-2", r); }
    { valarray<double> r = D6().cshift(8);   Pd("mem/cshift-wrap", r); }
    { valarray<double> r = D6().cshift(-8);  Pd("mem/cshift-wrap-neg", r); }
    { valarray<double> r = D6().cshift(6);   Pd("mem/cshift-full", r); }
    { valarray<double> e; valarray<double> r = e.cshift(3); Pd("mem/cshift-empty", r); }
    { valarray<double> r = D6().apply([](double x) { return x * x; });
      Pd("mem/apply", r); }
    { valarray<double> v = D6(); v.resize(3);      Pd("mem/resize-shrink", v); }
    { valarray<double> v = D6(); v.resize(8, 1.25); Pd("mem/resize-grow", v); }
    { valarray<double> v = D6(); v.resize(6, 0.0);  Pd("mem/resize-same", v); }
    { valarray<double> a = D6(), b(1.0, 2); a.swap(b);
      Pd("mem/swap-a", a); Pd("mem/swap-b", b); }

    {
        valarray<double> u{0.25, 0.5, 0.75};
        Pd("tr/abs",   abs(D6()));
        Pd("tr/acos",  acos(u));
        Pd("tr/asin",  asin(u));
        Pd("tr/atan",  atan(u));
        Pd("tr/cos",   cos(u));
        Pd("tr/cosh",  cosh(u));
        Pd("tr/exp",   exp(u));
        Pd("tr/log",   log(u));
        Pd("tr/log10", log10(u));
        Pd("tr/sin",   sin(u));
        Pd("tr/sinh",  sinh(u));
        Pd("tr/sqrt",  sqrt(u));
        Pd("tr/tan",   tan(u));
        Pd("tr/tanh",  tanh(u));
        valarray<double> w{1.5, 2.5, 3.5};
        Pd("tr/atan2/vv", atan2(u, w));
        Pd("tr/atan2/vs", atan2(u, 2.0));
        Pd("tr/atan2/sv", atan2(2.0, u));
        Pd("tr/pow/vv",   pow(w, u));
        Pd("tr/pow/vs",   pow(w, 2.0));
        Pd("tr/pow/sv",   pow(2.0, w));
        Pd("tr/of-expr", sqrt(D6() * D6()));
    }

    {
        valarray<double> v = D6();
        double s = 0;
        for (double x : v) s += x;
        Ps("range/for-sum", s);
        Pn("range/distance", (size_t)(end(v) - begin(v)));
        const valarray<double> c = D6();
        Ps("range/const-first", *begin(c));
    }

    {
        valarray<double> v = D6();
        valarray<bool> m{true, true, false, false, true, true};
        v[m] += valarray<double>(100.0, 4);
        Pd("chain/mask-compound", v);
    }
    {
        valarray<double> v = D6();
        valarray<size_t> ix{0, 2, 4};
        v[ix] *= valarray<double>(3.0, 3);
        Pd("chain/indirect-compound", v);
    }
    {
        valarray<double> v = D6();
        valarray<size_t> sz{2, 2}, st{3, 1};
        v[gslice(1, sz, st)] += valarray<double>{1, 2, 3, 4};
        Pd("chain/gslice-compound", v);
    }
    { valarray<double> r = -(D6() + D6());        Pd("ord/unary-minus", r); }
    { valarray<double> r = +(D6() * D6());        Pd("ord/unary-plus", r); }
    { valarray<int> r = ~(I6() + I6());           Pi("ord/unary-flip", r); }
    { valarray<bool> r = !(I6() - I6());          Pb("ord/unary-not", r); }
    { valarray<double> r = (D6() + D6()) * 2.5;   Pd("ord/scalar-right", r); }
    { valarray<double> r = 2.5 * (D6() + D6());   Pd("ord/scalar-left", r); }
    { valarray<double> r = (D6() * 2.0) - (D6() / 4.0); Pd("ord/order-order", r); }
    { valarray<bool> r = (D6() + 1.0) < (D6() * 2.0);   Pb("ord/order-cmp", r); }
    { valarray<double> r = sqrt(abs(D6() - D6() * 3.0)); Pd("ord/nested-fn", r); }
    { valarray<double> r = pow(D6() * D6(), 0.5);        Pd("ord/pow-order", r); }
    { valarray<double> r = atan2(D6() + 1.0, D6() - 1.0); Pd("ord/atan2-order", r); }
    { valarray<double> r = abs(-D6());            Pd("ord/abs-of-unary", r); }
    { valarray<double> v = D6(); v += D6() * 2.0; Pd("ord/compound-order", v); }
    { valarray<double> v = D6(); v[slice(0, 3, 2)] = valarray<double>{1, 2, 3} * 10.0;
      Pd("ord/into-slice", v); }
    { valarray<double> v = D6();
      valarray<bool> m{true, false, true, false, true, false};
      v[m] += valarray<double>{1, 2, 3} * 100.0;  Pd("ord/into-mask", v); }
    {
        valarray<short> a{3, -4, 5}, b{7, 9, -2};
        valarray<short> r = a * b;
        printf("%-30s [%d,%d,%d]\n", "ord/no-promotion", (int)r[0], (int)r[1], (int)r[2]);
        static_assert(sizeof(decltype(a * b)) > 0, "");
    }
    {
        Ps("req/sum", (D6() + D6()).sum());
        Ps("req/min", (D6() * 2.0).min());
        Ps("req/max", (D6() * 2.0).max());
        Pn("req/size", (D6() - D6()).size());
        Ps("req/index", (D6() + D6())[3]);
#if defined(_LIBCPP_VERSION)
        printf("%-30s %s\n", "req/shift", "<libc++: does not compile>");
        printf("%-30s %s\n", "req/cshift", "<libc++: does not compile>");
#else
        Pd("req/shift", (D6() + 1.0).shift(2));
        Pd("req/cshift", (D6() + 1.0).cshift(-2));
#endif
        Pd("req/apply", (D6() * 2.0).apply([](double x) { return x + 0.5; }));
        Pd("req/unary-minus", -(D6() + 1.0));
        Pb("req/unary-not", !(D6() - D6()));
        Pd("req/sub-slice", (D6() * 2.0)[slice(1, 3, 2)]);
        valarray<size_t> sz{2, 2}, st{3, 1};
        Pd("req/sub-gslice", (D6() * 2.0)[gslice(0, sz, st)]);
        valarray<bool> m{true, false, true, false, true, false};
        Pd("req/sub-mask", (D6() * 2.0)[m]);
        valarray<size_t> ix{4, 1, 0};
        Pd("req/sub-indirect", (D6() * 2.0)[ix]);
    }

    {
        valarray<double> v = D6(), w(0.0, 6);
        w[slice(0, 3, 1)] = v[slice(3, 3, 1)];
        Pd("ord/proxy-to-proxy", w);
    }
    {
        valarray<double> v = D6(), w(0.0, 6);
        valarray<size_t> ix{4, 2, 0};
        w[ix] = v[ix];
        Pd("ord/indirect-to-indirect", w);
    }
    {
        valarray<double> v = D6();
        v[slice(0, 3, 1)] = v[slice(3, 3, 1)];
        Pd("chain/slice-from-self", v);
    }
    return 0;
}