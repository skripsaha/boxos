/*
 * cxxtest — BoxOS C++ support verification app.
 *
 * Cumulative per-phase checks; the stress matrix greps the final
 * "[CXX] ALL PASS" marker. Per-phase PASS lines are informational.
 *
 *   phase0 — pure language core (no runtime): constexpr, templates,
 *            virtual dispatch, lambdas. Proves toolchain → ELF loader →
 *            ring 3 under CET.
 *   phase1 — boxcxx runtime: .init_array ctors (ordered), function-local
 *            static guards, the full operator new/delete set (scalar /
 *            array / aligned / nothrow / placement), initializer_list,
 *            new_handler, <exception> hierarchy key functions, static
 *            dtors via __cxa_atexit→__cxa_finalize at exit().
 */

#include "box/print.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <exception>
#include <initializer_list>
#include <typeinfo>
#include <unwind.h>

namespace {

int g_failures = 0;

void Check(bool ok, const char *what)
{
    if (!ok) {
        printf("[CXX] FAIL %s\n", what);
        g_failures++;
    }
}

// ── phase0 fixtures ────────────────────────────────────────────────────

struct Point {
    int x;
    int y;
    constexpr int Sum() const { return x + y; }
};

template <typename T, unsigned long N>
constexpr T ArraySum(const T (&arr)[N])
{
    T s{};
    for (auto v : arr) s += v;
    return s;
}

struct Shape {
    virtual int Sides() const { return 0; }
};

struct Quad : Shape {
    int Sides() const override { return 4; }
};

struct Tri final : Shape {
    int Sides() const override { return 3; }
};

int VirtualDispatch(const Shape &s)
{
    return s.Sides();
}

void Phase0()
{
    constexpr Point kOrigin{40, 2};
    static_assert(kOrigin.Sum() == 42, "constexpr member call");

    const int vals[] = {10, 20, 12};
    Check(ArraySum(vals) == 42, "phase0 template-sum");

    Quad quad;
    Tri tri;
    Check(VirtualDispatch(quad) + VirtualDispatch(tri) == 7,
          "phase0 virtual-dispatch");

    const int total = 42;
    const auto scale = [total](int k) { return total * k; };
    Check(scale(2) == 84, "phase0 lambda");

    printf("[CXX] PASS phase0: lang-core\n");
}

// ── phase1 fixtures ────────────────────────────────────────────────────

int g_ctor_calls = 0;
int g_ctor_order[2] = {0, 0};

struct GlobalProbe {
    int id;
    explicit GlobalProbe(int i) : id(i)
    {
        if (g_ctor_calls < 2) g_ctor_order[g_ctor_calls] = i;
        g_ctor_calls++;
    }
    ~GlobalProbe()
    {
        // Runs after main via __cxa_finalize — order must be reverse (2, 1).
        printf("[CXX] info: static dtor %d\n", id);
    }
};

GlobalProbe g_probe_first{1};
GlobalProbe g_probe_second{2};

int g_static_evals = 0;

int ComputeOnce()
{
    g_static_evals++;
    return 42;
}

int LocalStatic()
{
    static int value = ComputeOnce();   // __cxa_guard_* protocol
    return value;
}

struct alignas(64) WideBlock {
    uint64_t lanes[8];
};

int SumList(std::initializer_list<int> il)
{
    int s = 0;
    for (int v : il) s += v;
    return s;
}

bool g_handler_ran = false;

void ProbeNewHandler()
{
    g_handler_ran = true;
    std::set_new_handler(nullptr);   // second pass returns nullptr → nothrow path
}

void Phase1()
{
    // .init_array ran exactly once, in declaration order
    Check(g_ctor_calls == 2, "phase1 init_array count");
    Check(g_ctor_order[0] == 1 && g_ctor_order[1] == 2,
          "phase1 init_array order");

    // function-local static: single evaluation across calls
    Check(LocalStatic() == 42 && LocalStatic() == 42 && g_static_evals == 1,
          "phase1 local-static guard");

    // scalar new/delete
    int *one = new int(42);
    Check(one != nullptr && *one == 42, "phase1 scalar new");
    delete one;

    // array new/delete
    int *many = new int[100];
    for (int i = 0; i < 100; i++) many[i] = i;
    Check(many[99] == 99, "phase1 array new");
    delete[] many;

    // aligned new/delete (alignas(64) → align_val_t form)
    WideBlock *wide = new WideBlock();
    Check((reinterpret_cast<uintptr_t>(wide) & 63u) == 0,
          "phase1 aligned new (64)");
    wide->lanes[7] = 0xB0CE5ULL;
    Check(wide->lanes[7] == 0xB0CE5ULL, "phase1 aligned store");
    delete wide;

    // nothrow new: success path + exhaustion path returns nullptr
    int *soft = new (std::nothrow) int(7);
    Check(soft != nullptr && *soft == 7, "phase1 nothrow new");
    delete soft;

    void *huge = operator new(SIZE_MAX / 2, std::nothrow);
    Check(huge == nullptr, "phase1 nothrow exhaustion → nullptr");

    // new_handler is consulted before giving up
    std::new_handler prev = std::set_new_handler(ProbeNewHandler);
    void *huge2 = operator new(SIZE_MAX / 2, std::nothrow);
    Check(huge2 == nullptr && g_handler_ran, "phase1 new_handler invoked");
    std::set_new_handler(prev);

    // placement new + explicit destructor call
    alignas(Point) unsigned char slab[sizeof(Point)];
    Point *placed = new (static_cast<void *>(slab)) Point{21, 21};
    Check(placed->Sum() == 42, "phase1 placement new");
    placed->~Point();

    // initializer_list
    Check(SumList({2, 8, 32}) == 42, "phase1 initializer_list");

    // <exception> hierarchy: virtual what() through the base
    std::bad_alloc oom;
    const std::exception &base = oom;
    const char *msg = base.what();
    Check(msg != nullptr && msg[0] == 's', "phase1 exception::what virtual");

    printf("[CXX] PASS phase1: runtime\n");
}

// ── phase3 fixtures: TLS (local-exec, FS base via boxcxx tls_init) ─────

thread_local int g_tls_counter = 1000;        // .tdata — template copy check
thread_local uint64_t g_tls_zeroed;           // .tbss  — zero-fill check
thread_local int *g_tls_self = &g_tls_counter; // address-of through fs:0

int g_tls_dynamic_evals = 0;

int ComputeTlsValue()
{
    g_tls_dynamic_evals++;
    return 42;
}

struct TlsProbe {
    int value;
    TlsProbe() : value(ComputeTlsValue()) {}
    ~TlsProbe()
    {
        // Runs at exit BEFORE static dtors (__cxa_thread_atexit ordering).
        printf("[CXX] info: tls dtor (must precede static dtors)\n");
    }
};
thread_local TlsProbe g_tls_probe;

void Phase3()
{
    Check(g_tls_counter == 1000, "phase3 tdata template value");
    g_tls_counter++;
    g_tls_counter++;
    Check(g_tls_counter == 1002, "phase3 tls increment");

    Check(g_tls_zeroed == 0, "phase3 tbss zero-fill");
    g_tls_zeroed = 0xB0CE5ULL;
    Check(g_tls_zeroed == 0xB0CE5ULL, "phase3 tbss store");

    // address-of thread_local goes through fs:0 self-pointer
    Check(g_tls_self == &g_tls_counter, "phase3 fs:0 self-pointer");
    Check((reinterpret_cast<uintptr_t>(&g_tls_zeroed) & 7u) == 0,
          "phase3 tls alignment");

    // dynamic-init thread_local: lazy wrapper, single evaluation,
    // destructor registered via __cxa_thread_atexit
    Check(g_tls_probe.value == 42 && g_tls_dynamic_evals == 1,
          "phase3 dynamic thread_local init");
    Check(g_tls_probe.value == 42 && g_tls_dynamic_evals == 1,
          "phase3 dynamic thread_local single eval");

    printf("[CXX] PASS phase3: TLS (fs-base/tdata/tbss/thread_atexit)\n");
}

// ── phase4a fixtures: DWARF unwinder backtrace ─────────────────────────

struct BtState {
    int frames;
    bool saw_leaf_region;
};

[[gnu::noinline]] int BtLeaf();

_Unwind_Reason_Code BtTrace(_Unwind_Context *ctx, void *arg)
{
    BtState *st = static_cast<BtState *>(arg);
    st->frames++;
    // One of the traced frames must be BtLeaf itself (the first frame is
    // _Unwind_Backtrace's own — same as libgcc).
    uintptr_t region = _Unwind_GetRegionStart(ctx);
    uintptr_t ip     = _Unwind_GetIP(ctx);
    if (region == reinterpret_cast<uintptr_t>(&BtLeaf) && ip > region)
        st->saw_leaf_region = true;
    return st->frames > 64 ? _URC_NORMAL_STOP : _URC_NO_REASON;
}

[[gnu::noinline]] int BtLeaf()
{
    BtState st{0, false};
    _Unwind_Backtrace(BtTrace, &st);
    Check(st.saw_leaf_region, "phase4a region-start sanity");
    return st.frames;
}

[[gnu::noinline]] int BtMid(int bias)
{
    int frames = BtLeaf() + bias;
    __asm__ volatile("" ::: "memory");   // keep a real (non-sibling) call
    return frames;
}

[[gnu::noinline]] void Phase4a()
{
    int frames = BtMid(0);
    // Expected chain: BtLeaf → BtMid → Phase4a → main (+crt = no CFI, stops).
    Check(frames >= 4, "phase4a backtrace depth");
    printf("[CXX] PASS phase4a: DWARF unwinder backtrace (%d frames)\n",
           frames);
}

// ── phase4b fixtures: exceptions end-to-end ────────────────────────────

int g_unwind_dtors = 0;

struct UnwindProbe {
    ~UnwindProbe() { g_unwind_dtors++; }
};

struct EhBase {
    virtual int Code() const { return 1; }
    virtual ~EhBase() = default;
};
struct EhDerived : EhBase {
    int Code() const override { return 42; }
};

[[gnu::noinline]] void ThrowInt(int v)
{
    UnwindProbe local;            // destroyed during unwind
    throw v;
}

[[gnu::noinline]] int CatchInt()
{
    try {
        UnwindProbe outer;        // destroyed during unwind too
        ThrowInt(42);
    } catch (int v) {
        return v;
    }
    return -1;
}

[[gnu::noinline]] int CatchByBase()
{
    try {
        throw EhDerived{};
    } catch (const EhBase &b) {   // derived → base catch (upcast walk)
        return b.Code();
    }
}

[[gnu::noinline]] int CatchEllipsisRethrow()
{
    int stage = 0;
    try {
        try {
            throw 4.25;           // double
        } catch (...) {
            stage = 1;
            throw;                // rethrow
        }
    } catch (double d) {
        return stage * 100 + (d == 4.25 ? 42 : 0);
    } catch (...) {
        return -2;
    }
}

[[gnu::noinline]] int CatchPointer()
{
    static int target = 7;
    try {
        throw &target;            // int*
    } catch (const int *p) {      // cv-superset pointer catch
        return *p;
    }
}

[[gnu::noinline]] int NestedDepthThrow(int depth)
{
    UnwindProbe local;
    if (depth == 0) throw 1234;
    return NestedDepthThrow(depth - 1) + 1;
}

void Phase4b()
{
    g_unwind_dtors = 0;
    Check(CatchInt() == 42, "phase4b throw/catch int");
    Check(g_unwind_dtors == 2, "phase4b dtors during unwind");

    Check(CatchByBase() == 42, "phase4b catch by base ref");
    Check(CatchEllipsisRethrow() == 142, "phase4b catch(...) + rethrow");
    Check(CatchPointer() == 7, "phase4b pointer catch");

    g_unwind_dtors = 0;
    try {
        NestedDepthThrow(20);
        Check(false, "phase4b deep throw reached");
    } catch (int v) {
        Check(v == 1234, "phase4b deep-frame throw value");
    }
    Check(g_unwind_dtors == 21, "phase4b deep unwind dtor count");

    // bad_alloc from operator new + uncaught_exceptions bookkeeping
    bool caught_bad_alloc = false;
    try {
        void *p = operator new(SIZE_MAX / 2);
        (void)p;
    } catch (const std::bad_alloc &e) {
        caught_bad_alloc = (e.what() != nullptr);
    }
    Check(caught_bad_alloc, "phase4b operator new throws bad_alloc");
    Check(std::uncaught_exceptions() == 0, "phase4b uncaught counter rests");

    printf("[CXX] PASS phase4b: exceptions (catch/base/ptr/rethrow/cleanup)\n");
}

// ── phase5 fixtures: RTTI (__dynamic_cast / typeid) ────────────────────

struct SiBase { virtual ~SiBase() = default; long v = 7; };
struct SiDerived : SiBase { long w = 8; };        // __si_class_type_info
struct SiSibling : SiBase { long u = 9; };

struct RttiBase  { virtual ~RttiBase() = default; int b = 1; };
struct RttiOther { virtual ~RttiOther() = default; int o = 2; };
struct RttiMulti : RttiBase, RttiOther { int m = 3; };   // __vmi, MI

struct DiaTop { virtual ~DiaTop() = default; int t = 1; };
struct DiaL : virtual DiaTop { int l = 2; };
struct DiaR : virtual DiaTop { int r = 3; };
struct DiaBottom : DiaL, DiaR { int d = 4; };     // shared virtual base

struct PrivBase { virtual ~PrivBase() = default; int p = 5; };
struct PrivDerived : private PrivBase {
    static PrivBase *Expose(PrivDerived *d) { return d; }
    int q = 6;
};

// Repeated non-virtual base: two distinct SiBase subobjects. The right
// arm is private — discriminates both subobject ADDRESS identity and
// access checking in the runtime walkers.
struct RepL : SiBase { int rl = 1; };
struct RepR : SiBase { int rr = 2; };
struct RepBoth : RepL, private RepR {
    static SiBase *ExposeRight(RepBoth *b) { return static_cast<RepR *>(b); }
    int rb = 3;
};

[[gnu::noinline]] RttiBase *NullRttiBase() { return nullptr; }

void Phase5()
{
    // si downcast: success + sibling failure
    SiDerived sd;
    SiBase *sb = &sd;
    Check(dynamic_cast<SiDerived *>(sb) == &sd, "phase5 si downcast");
    Check(dynamic_cast<SiSibling *>(sb) == nullptr,
          "phase5 sibling downcast → null");

    // MI downcast from the second base (non-zero this-adjustment back)
    RttiMulti mi;
    RttiOther *ro = &mi;
    Check(static_cast<void *>(ro) != static_cast<void *>(&mi),
          "phase5 second-base offset sanity");
    Check(dynamic_cast<RttiMulti *>(ro) == &mi, "phase5 mi downcast adjusts");

    // cross-cast across sibling bases
    RttiBase *rb = &mi;
    Check(dynamic_cast<RttiOther *>(rb) == static_cast<RttiOther *>(&mi),
          "phase5 cross-cast");

    // wrong dynamic type → null
    RttiBase solo;
    RttiBase *psolo = &solo;
    Check(dynamic_cast<RttiMulti *>(psolo) == nullptr,
          "phase5 wrong dynamic type → null");

    // dynamic_cast<void*> → most-derived object (offset-to-top path)
    Check(dynamic_cast<void *>(ro) == static_cast<void *>(&mi),
          "phase5 cast to void*");

    // virtual diamond: shared base → most-derived and → each arm
    DiaBottom dia;
    DiaTop *dt = &dia;
    Check(dynamic_cast<DiaBottom *>(dt) == &dia,
          "phase5 virtual-base downcast");
    Check(dynamic_cast<DiaL *>(dt) == static_cast<DiaL *>(&dia),
          "phase5 vbase → left arm");
    Check(dynamic_cast<DiaR *>(dt) == static_cast<DiaR *>(&dia),
          "phase5 vbase → right arm");

    // private base: both clauses must refuse
    PrivDerived priv;
    PrivBase *pb = PrivDerived::Expose(&priv);
    Check(dynamic_cast<PrivDerived *>(pb) == nullptr,
          "phase5 private base → null");

    // repeated base: public left arm casts back; private right arm refuses
    RepBoth rep;
    SiBase *left  = static_cast<RepL *>(&rep);
    SiBase *right = RepBoth::ExposeRight(&rep);
    Check(left != right, "phase5 repeated-base distinct subobjects");
    Check(dynamic_cast<RepBoth *>(left) == &rep,
          "phase5 repeated-base public arm downcast");
    Check(dynamic_cast<RepBoth *>(right) == nullptr,
          "phase5 repeated-base private arm → null");
    Check(dynamic_cast<RepL *>(right) == nullptr,
          "phase5 cross-cast from private arm → null");

    // failed reference cast → bad_cast
    bool caught_bad_cast = false;
    try {
        RttiMulti &bad = dynamic_cast<RttiMulti &>(*psolo);
        (void)bad;
    } catch (const std::bad_cast &e) {
        caught_bad_cast = (e.what() != nullptr);
    }
    Check(caught_bad_cast, "phase5 ref-cast failure → bad_cast");

    // typeid on a null polymorphic glvalue → bad_typeid
    bool caught_bad_typeid = false;
    try {
        (void)typeid(*NullRttiBase());
    } catch (const std::bad_typeid &e) {
        caught_bad_typeid = (e.what() != nullptr);
    }
    Check(caught_bad_typeid, "phase5 typeid(*null) → bad_typeid");

    // typeid identity / names / hashing
    Check(typeid(*sb) == typeid(SiDerived), "phase5 typeid dynamic identity");
    Check(typeid(*psolo) != typeid(RttiMulti), "phase5 typeid inequality");
    Check(typeid(int).name()[0] == 'i', "phase5 typeid(int) mangled name");
    Check(typeid(RttiMulti).hash_code() == typeid(*rb).hash_code(),
          "phase5 hash_code agrees with dynamic type");

    printf("[CXX] PASS phase5: RTTI (dynamic_cast si/mi/diamond/access + typeid)\n");
}

} // namespace

// cxxtest_traits.cpp — phase 2 header torture (compile-time); links iff green.
int CxxTraitsTortureCompiled();

int main()
{
    Phase0();
    Phase1();
    Phase3();
    Phase4a();
    Phase4b();
    Phase5();

    if (CxxTraitsTortureCompiled() == 1) {
        printf("[CXX] PASS phase2: freestanding headers (compile-time torture)\n");
    } else {
        printf("[CXX] FAIL phase2\n");
        g_failures++;
    }

    if (g_failures == 0) {
        printf("[CXX] ALL PASS\n");
        return 0;
    }
    printf("[CXX] TOTAL FAILURES: %d\n", g_failures);
    return 1;
}
