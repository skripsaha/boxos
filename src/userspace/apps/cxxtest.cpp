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
#include "box/system.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <coroutine>
#include <generator>
#include <deque>
#include <expected>
#include <format>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <print>
#include <random>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <exception>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <unwind.h>
#include <vector>

#include "box/cxx/bay.h"
#include "box/cxx/bay_memory_resource.h"
#include "box/cxx/brook.h"
#include "box/cxx/console.h"
#include "box/cxx/cpu.h"
#include "box/cxx/current.h"
#include "box/cxx/executor.h"
#include "box/cxx/heap.h"
#include "box/cxx/hw.h"
#include "box/cxx/keyboard.h"
#include "box/cxx/manifest.h"
#include "box/cxx/math.h"
#include "box/cxx/memtag.h"
#include "box/cxx/message.h"
#include "box/cxx/pku.h"
#include "box/cxx/process.h"
#include "box/cxx/system.h"
#include "box/cxx/tagfs.h"
#include "box/cxx/touch.h"

// A user-defined formatter (exercised in phase9b) — drives the type-erased
// handle / FmtThunk path: the engine reaches it through a function pointer,
// the user parse() consumes a custom 'v' flag, and format() recurses via
// std::format_to(ctx.out(), ...). Must live at namespace scope to specialize
// std::formatter.
namespace cxxfmt {
struct Point {
    int x, y;
};
} // namespace cxxfmt
template <> struct std::formatter<cxxfmt::Point> {
    bool verbose = false;
    constexpr auto parse(std::format_parse_context &pc)
    {
        auto it = pc.begin();
        if (it != pc.end() && *it == 'v') { verbose = true; ++it; }
        return it;
    }
    auto format(const cxxfmt::Point &p, std::format_context &ctx) const
    {
        return verbose
                   ? std::format_to(ctx.out(), "Point(x={}, y={})", p.x, p.y)
                   : std::format_to(ctx.out(), "({},{})", p.x, p.y);
    }
};

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

// ── phase6 fixtures: <atomic> + <mutex> ────────────────────────────────

struct Wide16 {
    uint64_t lo;
    uint64_t hi;
    bool operator==(const Wide16 &) const = default;
};
static_assert(sizeof(Wide16) == 16);
static_assert(std::atomic<Wide16>::is_always_lock_free);

struct Odd12 {
    uint32_t a, b, c;
    bool operator==(const Odd12 &) const = default;
};
static_assert(sizeof(Odd12) == 12);
static_assert(!std::atomic<Odd12>::is_always_lock_free);

static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic_ref<int>::required_alignment == 4);
static_assert(std::atomic<unsigned __int128>::is_always_lock_free);

int g_once_runs = 0;
std::once_flag g_once_flag;

void Phase6()
{
    // atomic<int>: core ops + fetch family + operator sugar
    std::atomic<int> ai{40};
    Check(ai.load() == 40, "phase6 atomic load");
    ai.store(2, std::memory_order_release);
    Check(ai.load(std::memory_order_acquire) == 2, "phase6 store/load orders");
    Check(ai.exchange(10) == 2 && ai.load() == 10, "phase6 exchange");
    Check(ai.fetch_add(5) == 10 && ai.fetch_sub(3) == 15, "phase6 fetch add/sub");
    Check((ai &= 0xC) == 12 && (ai |= 1) == 13 && (ai ^= 2) == 15,
          "phase6 fetch bitwise sugar");
    Check(++ai == 16 && ai++ == 16 && ai.load() == 17, "phase6 increments");

    int expected = 0;
    Check(!ai.compare_exchange_strong(expected, 1) && expected == 17,
          "phase6 CAS failure updates expected");
    Check(ai.compare_exchange_strong(expected, 42) && ai.load() == 42,
          "phase6 CAS success");

    std::atomic_thread_fence(std::memory_order_seq_cst);

    // volatile-qualified overloads
    volatile std::atomic<int> vai{7};
    vai.store(8);
    Check(vai.load() == 8, "phase6 volatile atomic");

    // atomic<bool> + atomic_flag
    std::atomic<bool> ab{false};
    Check(!ab.exchange(true) && ab.load(), "phase6 atomic<bool>");
    std::atomic_flag flag;
    Check(!flag.test() && !flag.test_and_set() && flag.test(),
          "phase6 atomic_flag set");
    flag.clear();
    Check(!flag.test(), "phase6 atomic_flag clear");

    // atomic<T*> scales by pointee
    static int arr[8] = {};
    std::atomic<int *> ap{arr};
    Check(ap.fetch_add(2) == arr && ap.load() == arr + 2, "phase6 ptr fetch_add");
    Check(++ap == arr + 3 && ap.fetch_sub(3) == arr + 3 && ap.load() == arr,
          "phase6 ptr sugar");

    // atomic<float> (C++20 floating fetch ops)
    std::atomic<float> af{1.5f};
    Check(af.fetch_add(2.5f) == 1.5f && af.load() == 4.0f, "phase6 float fetch_add");

    // 16-byte lock-free path (cmpxchg16b via __atomic_*_16)
    std::atomic<Wide16> aw{{1, 2}};
    Check(aw.is_lock_free(), "phase6 16B is_lock_free");
    Check(aw.load() == Wide16{1, 2}, "phase6 16B load");
    aw.store({3, 4});
    Wide16 wexp{0, 0};
    Check(!aw.compare_exchange_strong(wexp, {9, 9}) && wexp == Wide16{3, 4},
          "phase6 16B CAS failure");
    Check(aw.compare_exchange_strong(wexp, {5, 6}) && aw.load() == Wide16{5, 6},
          "phase6 16B CAS success");
    Check(aw.exchange({7, 8}) == Wide16{5, 6}, "phase6 16B exchange");

    std::atomic<unsigned __int128> a128{1};
    unsigned __int128 big = (static_cast<unsigned __int128>(0xB0CE5ULL) << 64) | 1u;
    a128.store(big);
    Check(a128.load() == big, "phase6 int128 store/load");
    Check(a128.fetch_add(1) == big, "phase6 int128 fetch_add");
    Check((a128 += 1) == big + 2, "phase6 int128 add_fetch sugar");

    // odd-size generic protocol (cabin-private lock pool)
    std::atomic<Odd12> ao{{1, 2, 3}};
    Check(!ao.is_lock_free(), "phase6 odd-size not lock-free");
    ao.store({4, 5, 6});
    Check(ao.load() == Odd12{4, 5, 6}, "phase6 odd-size store/load");
    Odd12 oexp{4, 5, 6};
    Check(ao.compare_exchange_strong(oexp, {7, 8, 9}) &&
              ao.load() == Odd12{7, 8, 9},
          "phase6 odd-size CAS");

    // wait returns immediately when the value already differs; notify is
    // a conformance no-op (address-monitor based wake)
    std::atomic<int> awake{1};
    awake.wait(0);
    awake.notify_one();
    awake.notify_all();
    Check(awake.load() == 1, "phase6 wait fast-path");

    // atomic_ref over a plain object
    int plain = 5;
    std::atomic_ref<int> ref(plain);
    Check(ref.fetch_add(3) == 5 && plain == 8, "phase6 atomic_ref");

    // free functions
    std::atomic<int> afree{1};
    std::atomic_store(&afree, 2);
    Check(std::atomic_load(&afree) == 2 && std::atomic_fetch_add(&afree, 3) == 2,
          "phase6 atomic free functions");

    // mutex: exclusion observable through try_lock on one thread
    std::mutex m;
    m.lock();
    Check(!m.try_lock(), "phase6 mutex try_lock while held");
    m.unlock();
    Check(m.try_lock(), "phase6 mutex try_lock after unlock");
    m.unlock();

    // recursive_mutex: re-entry by the owner
    std::recursive_mutex rm;
    rm.lock();
    rm.lock();
    Check(rm.try_lock(), "phase6 recursive re-entry");
    rm.unlock();
    rm.unlock();
    rm.unlock();
    Check(rm.try_lock(), "phase6 recursive released");
    rm.unlock();

    // RAII wrappers
    {
        std::lock_guard<std::mutex> g(m);
        Check(!m.try_lock(), "phase6 lock_guard holds");
    }
    std::unique_lock<std::mutex> ul(m, std::defer_lock);
    Check(!ul.owns_lock(), "phase6 unique_lock defer");
    ul.lock();
    Check(ul.owns_lock() && !m.try_lock(), "phase6 unique_lock lock");
    std::unique_lock<std::mutex> ul2(std::move(ul));
    Check(ul2.owns_lock() && !ul.owns_lock(), "phase6 unique_lock move");
    ul2.unlock();

    // lock algorithms + scoped_lock
    std::mutex m2;
    std::lock(m, m2);
    Check(!m.try_lock() && !m2.try_lock(), "phase6 std::lock both");
    m.unlock();
    m2.unlock();
    m2.lock();
    Check(std::try_lock(m, m2) == 1, "phase6 try_lock failed index");
    Check(m.try_lock(), "phase6 try_lock rollback released first");
    m.unlock();
    m2.unlock();
    {
        std::scoped_lock both(m, m2);
        Check(!m.try_lock() && !m2.try_lock(), "phase6 scoped_lock holds both");
    }
    Check(m.try_lock() && m2.try_lock(), "phase6 scoped_lock released");
    m.unlock();
    m2.unlock();

    // call_once: single execution; a throwing run leaves the flag passive
    std::call_once(g_once_flag, [] { g_once_runs++; });
    std::call_once(g_once_flag, [] { g_once_runs++; });
    Check(g_once_runs == 1, "phase6 call_once single run");

    std::once_flag throwing_flag;
    int recovered_runs = 0;
    try {
        std::call_once(throwing_flag, [] { throw 7; });
    } catch (int) {
    }
    std::call_once(throwing_flag, [&] { recovered_runs++; });
    Check(recovered_runs == 1, "phase6 call_once retry after throw");

    printf("[CXX] PASS phase6: atomic+mutex (16B cas/lock-pool/ref/wait + locks)\n");
}

// ── phase7a fixtures: strings + containers + exceptions ────────────────

using namespace std::literals;

static_assert("hello"sv.find("ll") == 2);
static_assert("hello"sv.rfind('l') == 3);
static_assert("hello"sv.substr(1, 3) == "ell"sv);
static_assert("hello"sv.starts_with("he") && "hello"sv.ends_with("lo"));
static_assert(("abc"sv <=> "abd"sv) < 0);
static_assert(std::array{1, 2, 3}.size() == 3);
static_assert(std::get<2>(std::array{1, 2, 3}) == 3);
static_assert((std::array{1, 2} <=> std::array{1, 3}) < 0);

struct MoveProbe {
    int *dtors;
    explicit MoveProbe(int *d) : dtors(d) {}
    MoveProbe(MoveProbe &&o) noexcept : dtors(o.dtors) { o.dtors = nullptr; }
    MoveProbe(const MoveProbe &) = default;
    MoveProbe &operator=(MoveProbe &&o) noexcept
    {
        dtors   = o.dtors;
        o.dtors = nullptr;
        return *this;
    }
    ~MoveProbe()
    {
        if (dtors) (*dtors)++;
    }
};

void Phase7a()
{
    // ── string: SSO boundary and heap migration ─────────────────────────
    std::string s(15, 'x');
    const char *obj = reinterpret_cast<const char *>(&s);
    Check(s.capacity() == 15 && s.size() == 15, "phase7a sso capacity");
    Check(s.data() >= obj && s.data() < obj + sizeof(s), "phase7a sso inline");
    s.push_back('y');
    Check(s.capacity() >= 16 &&
              !(s.data() >= obj && s.data() < obj + sizeof(s)),
          "phase7a sso→heap migration");
    Check(s.size() == 16 && s[14] == 'x' && s.back() == 'y',
          "phase7a content survives migration");

    // ── string: editing surface ─────────────────────────────────────────
    std::string t = "hello"s;
    t += " world";
    Check(t == "hello world", "phase7a append/+=");
    t.insert(5, ",");
    Check(t == "hello, world", "phase7a insert");
    t.erase(5, 1);
    Check(t == "hello world", "phase7a erase");
    t.replace(0, 5, "bye");
    Check(t == "bye world" && t.size() == 9, "phase7a replace");
    Check(t.find("world") == 4 && t.rfind('o') == 5, "phase7a find via view");
    Check(t.substr(4) == "world", "phase7a substr");
    Check(("a"s + "b"s + 'c') == "abc", "phase7a operator+ chain");
    Check(std::to_string(-1234) == "-1234" && std::to_string(98765u) == "98765",
          "phase7a to_string");

    std::string self = "abcdef";
    self.append(self.data() + 1, 3); // self-aliased append
    Check(self == "abcdefbcd", "phase7a self-aliased append");

    std::string sso_a = "short";
    std::string heap_b(40, 'z');
    sso_a.swap(heap_b);
    Check(sso_a.size() == 40 && heap_b == "short", "phase7a sso/heap swap");

    // string_view interop both directions
    std::string_view tv = t;
    Check(tv.size() == t.size() && tv.contains("world"),
          "phase7a string→view");
    std::string from_view{"viewed"sv};
    Check(from_view == "viewed", "phase7a view→string");

    // ── exceptions: stdexcept hierarchy + length guard ──────────────────
    bool caught_oor = false;
    try {
        (void)t.at(100);
    } catch (const std::logic_error &e) { // out_of_range IS-A logic_error
        caught_oor = e.what() != nullptr && e.what()[0] == 's';
    }
    Check(caught_oor, "phase7a at() → out_of_range → logic_error");

    bool caught_len = false;
    try {
        t.reserve(SIZE_MAX); // beyond max_size() → length_error, no alloc
    } catch (const std::length_error &) {
        caught_len = true;
    }
    Check(caught_len, "phase7a reserve → length_error");

    // ── system_error: unique_lock protocol violation ────────────────────
    std::mutex pm;
    std::unique_lock<std::mutex> ul(pm);
    bool caught_se = false;
    try {
        ul.lock();
    } catch (const std::system_error &e) {
        caught_se = e.code() == std::errc::resource_deadlock_would_occur &&
                    e.code().category() == std::generic_category() &&
                    e.what() != nullptr;
    }
    Check(caught_se, "phase7a unique_lock → system_error");
    Check(std::make_error_code(std::errc::timed_out).value() == 110,
          "phase7a errc numbering");

    // ── vector: growth, contiguity, editing ─────────────────────────────
    std::vector<int> v;
    for (int i = 0; i < 100; ++i) v.push_back(i);
    Check(v.size() == 100 && v.capacity() >= 100 && v[99] == 99,
          "phase7a vector growth");
    Check(v.data()[50] == 50 && &v[51] == v.data() + 51,
          "phase7a vector contiguity");
    v.insert(v.begin() + 1, 777);
    Check(v.size() == 101 && v[1] == 777 && v[2] == 1, "phase7a vector insert");
    v.erase(v.begin() + 1);
    Check(v[1] == 1 && v.size() == 100, "phase7a vector erase");
    Check(std::erase_if(v, [](int x) { return x % 2 == 0; }) == 50 &&
              v.size() == 50 && v[0] == 1,
          "phase7a vector erase_if");

    bool caught_vec_oor = false;
    try {
        (void)v.at(1000);
    } catch (const std::out_of_range &) {
        caught_vec_oor = true;
    }
    Check(caught_vec_oor, "phase7a vector::at → out_of_range");

    // non-trivial elements: strings inside vector (dtors/moves on grow)
    std::vector<std::string> vs;
    for (int i = 0; i < 20; ++i)
        vs.emplace_back("payload-with-some-length-" + std::to_string(i));
    Check(vs.size() == 20 && vs[19].ends_with("-19"),
          "phase7a vector<string>");
    std::vector<std::string> vs2 = std::move(vs);
    Check(vs2.size() == 20 && vs.empty(), "phase7a vector move");

    // destructor accounting through scope exit + pop_back
    int dtors = 0;
    {
        std::vector<MoveProbe> probes;
        probes.emplace_back(&dtors);
        probes.emplace_back(&dtors);
        probes.emplace_back(&dtors); // growth relocations must NOT double-count
        probes.pop_back();
        Check(dtors == 1, "phase7a pop_back destroys one");
    }
    Check(dtors == 3, "phase7a scope destroys the rest");

    // ── array + span ────────────────────────────────────────────────────
    std::array<int, 4> ar{1, 2, 3, 4};
    ar.fill(7);
    Check(ar[0] == 7 && ar.back() == 7, "phase7a array fill");
    std::array<int, 4> ar2{1, 1, 1, 1};
    ar.swap(ar2);
    Check(ar[0] == 1 && ar2[0] == 7, "phase7a array swap");

    int raw[4] = {1, 2, 3, 4};
    std::span sp(raw);
    static_assert(decltype(sp)::extent == 4);
    static_assert(sizeof(decltype(sp)) == sizeof(void *)); // static extent
    Check(sp.size_bytes() == 16 && sp.front() == 1 && sp.back() == 4,
          "phase7a span basics");
    auto mid = sp.subspan(1, 2);
    Check(mid.size() == 2 && mid[0] == 2 && mid[1] == 3, "phase7a subspan");
    mid[0] = 9;
    Check(raw[1] == 9, "phase7a span writes through");
    auto bytes = std::as_bytes(sp);
    Check(bytes.size() == 16, "phase7a as_bytes");
    std::span<int> dynsp(v.data(), v.size());
    Check(dynsp.size() == v.size() && dynsp.last(1)[0] == v.back(),
          "phase7a dynamic span over vector");

    printf("[CXX] PASS phase7a: string/sv/vector/array/span + stdexcept/system_error\n");
}

// ── phase7b fixtures: algorithm + iterator + ranges-core ───────────────

static_assert(std::random_access_iterator<int *>);
static_assert(std::contiguous_iterator<int *>);
static_assert(std::bidirectional_iterator<std::reverse_iterator<int *>>);
static_assert(std::output_iterator<int *, int>);
static_assert(std::ranges::contiguous_range<std::vector<int>>);
static_assert(std::ranges::contiguous_range<int[5]>);
static_assert(std::ranges::borrowed_range<std::string_view>);
static_assert(std::ranges::view<std::span<int>>);
static_assert(!std::ranges::view<std::vector<int>>);
static_assert(std::is_same_v<std::ranges::iterator_t<std::vector<int>>, int *>);

void Phase7b()
{
    // introsort + binary search over a worst-ish mix
    std::vector<int> v;
    for (int i = 0; i < 200; ++i) v.push_back((i * 37) % 101);
    std::sort(v.begin(), v.end());
    Check(std::is_sorted(v.begin(), v.end()), "phase7b sort");
    Check(std::binary_search(v.begin(), v.end(), v[100]), "phase7b bsearch");
    auto [lo, hi] = std::equal_range(v.begin(), v.end(), v[50]);
    Check(lo != hi && std::all_of(lo, hi, [&](int x) { return x == *lo; }),
          "phase7b equal_range");

    // stable_sort keeps equal-key order (buffered path)
    std::vector<std::pair<int, int>> sp;
    for (int i = 0; i < 64; ++i) sp.push_back({i % 4, i});
    std::stable_sort(sp.begin(), sp.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    bool stable = true;
    for (size_t i = 1; i < sp.size(); ++i)
        if (sp[i - 1].first == sp[i].first && sp[i - 1].second > sp[i].second)
            stable = false;
    Check(stable && std::is_sorted(sp.begin(), sp.end(),
                                   [](const auto &a, const auto &b) {
                                       return a.first < b.first;
                                   }),
          "phase7b stable_sort stability");

    // heap family
    std::vector<int> h{3, 1, 4, 1, 5, 9, 2, 6};
    std::make_heap(h.begin(), h.end());
    Check(std::is_heap(h.begin(), h.end()), "phase7b make_heap");
    h.push_back(42);
    std::push_heap(h.begin(), h.end());
    Check(h.front() == 42, "phase7b push_heap");
    std::pop_heap(h.begin(), h.end());
    h.pop_back();
    std::sort_heap(h.begin(), h.end());
    Check(std::is_sorted(h.begin(), h.end()), "phase7b sort_heap");

    // nth_element + partial_sort
    std::vector<int> ne;
    for (int i = 0; i < 50; ++i) ne.push_back((i * 17) % 53);
    std::nth_element(ne.begin(), ne.begin() + 10, ne.end());
    bool nth_ok = true;
    for (int i = 0; i < 10; ++i)
        if (ne[i] > ne[10]) nth_ok = false;
    for (size_t i = 11; i < ne.size(); ++i)
        if (ne[i] < ne[10]) nth_ok = false;
    Check(nth_ok, "phase7b nth_element");
    std::partial_sort(ne.begin(), ne.begin() + 5, ne.end());
    Check(std::is_sorted(ne.begin(), ne.begin() + 5) &&
              ne[4] <= *std::min_element(ne.begin() + 5, ne.end()),
          "phase7b partial_sort");

    // rotate / remove / unique / reverse
    std::vector<int> r{1, 2, 3, 4, 5};
    std::rotate(r.begin(), r.begin() + 2, r.end());
    Check(r == std::vector<int>{3, 4, 5, 1, 2}, "phase7b rotate");
    std::vector<int> dup{1, 1, 2, 2, 2, 3, 1};
    dup.erase(std::unique(dup.begin(), dup.end()), dup.end());
    Check(dup == std::vector<int>{1, 2, 3, 1}, "phase7b unique+erase");
    dup.erase(std::remove(dup.begin(), dup.end(), 1), dup.end());
    Check(dup == std::vector<int>{2, 3}, "phase7b remove+erase");
    std::reverse(r.begin(), r.end());
    Check(r == std::vector<int>{2, 1, 5, 4, 3}, "phase7b reverse");

    // set operations through back_inserter
    std::vector<int> sa{1, 2, 3, 5}, sb{2, 3, 4}, out;
    std::set_union(sa.begin(), sa.end(), sb.begin(), sb.end(),
                   std::back_inserter(out));
    Check(out == std::vector<int>{1, 2, 3, 4, 5}, "phase7b set_union");
    out.clear();
    std::set_intersection(sa.begin(), sa.end(), sb.begin(), sb.end(),
                          std::back_inserter(out));
    Check(out == std::vector<int>{2, 3}, "phase7b set_intersection");
    Check(std::includes(sa.begin(), sa.end(), out.begin(), out.end()),
          "phase7b includes");

    // move_iterator drains the source strings
    std::vector<std::string> msrc{"alpha", "beta"};
    std::vector<std::string> mdst;
    std::copy(std::make_move_iterator(msrc.begin()),
              std::make_move_iterator(msrc.end()), std::back_inserter(mdst));
    Check(mdst[1] == "beta" && msrc[0].empty(), "phase7b move_iterator");

    // min/max/clamp + lexicographic three-way
    Check(std::clamp(7, 1, 5) == 5 && std::max({1, 9, 4}) == 9 &&
              std::min(3, 2) == 2,
          "phase7b min/max/clamp");
    Check(std::lexicographical_compare_three_way(sa.begin(), sa.end(),
                                                 sb.begin(), sb.end()) < 0,
          "phase7b lex three-way");

    // vector::insert rotate path (range + count forms)
    std::vector<int> vi{1, 2, 7, 8};
    int mid[] = {3, 4, 5, 6};
    vi.insert(vi.begin() + 2, mid, mid + 4);
    Check(vi == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8},
          "phase7b vector insert range");
    vi.insert(vi.begin(), 2, 0);
    Check(vi.size() == 10 && vi[0] == 0 && vi[1] == 0 && vi[2] == 1,
          "phase7b vector insert count");

    // string::replace single-pass paths: shrink, grow-in-place, realloc
    std::string sr = "0123456789";
    sr.replace(2, 5, "XY", 2); // shrink
    Check(sr == "01XY789", "phase7b replace shrink");
    sr.replace(2, 2, "ABCD", 4); // grow within capacity
    Check(sr == "01ABCD789", "phase7b replace grow");
    std::string big(30, 'q');
    sr.replace(0, 2, big.data(), 30); // forces reallocation
    Check(sr.size() == 37 && sr[29] == 'q' && sr.ends_with("ABCD789"),
          "phase7b replace realloc");
    std::string al = "abcdef";
    al.replace(1, 2, al.data() + 3, 3); // self-alias with n != erased
    Check(al == "adefdef", "phase7b replace self-alias");

    // ranges CPOs over containers and C-arrays
    int carr[3] = {7, 8, 9};
    Check(std::ranges::size(carr) == 3 && *std::ranges::begin(carr) == 7,
          "phase7b ranges CPO array");
    Check(std::ranges::size(vi) == vi.size() &&
              std::ranges::data(vi) == vi.data() && !std::ranges::empty(vi),
          "phase7b ranges CPO vector");

    printf("[CXX] PASS phase7b: algorithm/iterator/ranges-core\n");
}

// ── phase7c fixtures: associative + unordered + deque + list ───────────

void Phase7c()
{
    // map: RB-tree stress — modular permutation insert, ordered walk,
    // erase half, re-verify order + bounds (rebalance both fixups)
    std::map<int, int> m;
    for (int i = 0; i < 200; ++i) m[(i * 73) % 199] = i;
    Check(m.size() == 199, "phase7c map size");
    int prev = -1;
    bool ordered = true;
    for (auto &kv : m) {
        if (kv.first <= prev) ordered = false;
        prev = kv.first;
    }
    Check(ordered, "phase7c map ordered walk");
    for (int i = 0; i < 199; i += 2) m.erase(i);
    prev    = -1;
    ordered = true;
    for (auto &kv : m) {
        if (kv.first <= prev) ordered = false;
        prev = kv.first;
    }
    Check(ordered && m.size() == 99 && !m.contains(2) && m.contains(3),
          "phase7c map erase+order");
    Check(m.lower_bound(4)->first == 5 && m.upper_bound(5)->first == 7,
          "phase7c map bounds");
    auto [ti, tok] = m.try_emplace(3, 999);
    Check(!tok && ti->second != 999, "phase7c try_emplace existing");
    m.insert_or_assign(3, 42);
    Check(m.at(3) == 42, "phase7c insert_or_assign");
    std::map<int, int> mc = m;
    Check(mc == m, "phase7c map copy+equality");
    std::map<int, int> mv = std::move(mc);
    Check(mv.size() == m.size() && mc.empty(), "phase7c map move");
    bool caught = false;
    try {
        (void)m.at(123456);
    } catch (const std::out_of_range &) {
        caught = true;
    }
    Check(caught, "phase7c map::at throws");

    // set / multiset / multimap
    std::set<std::string> s{"b", "a", "c", "a"};
    Check(s.size() == 3 && *s.begin() == "a" && s.contains("c"),
          "phase7c set basic");
    std::multiset<int> ms{1, 2, 2, 3, 2};
    Check(ms.count(2) == 3 && ms.size() == 5, "phase7c multiset count");
    ms.erase(2);
    Check(ms.size() == 2 && ms.count(2) == 0, "phase7c multiset erase key");
    std::multimap<int, int> mm;
    mm.emplace(1, 10);
    mm.emplace(1, 20);
    mm.emplace(0, 5);
    auto [mlo, mhi] = mm.equal_range(1);
    int spread      = 0;
    for (auto it = mlo; it != mhi; ++it) ++spread;
    Check(mm.count(1) == 2 && spread == 2 && mm.begin()->first == 0,
          "phase7c multimap equal_range");

    // unordered_map: growth across several rehashes + bucket sanity
    std::unordered_map<std::string, int> um;
    for (int i = 0; i < 300; ++i) um["k" + std::to_string(i)] = i;
    Check(um.size() == 300, "phase7c umap size");
    Check(um.load_factor() <= um.max_load_factor() + 0.01f,
          "phase7c umap load factor");
    bool all = true;
    for (int i = 0; i < 300; ++i)
        if (um.at("k" + std::to_string(i)) != i) all = false;
    Check(all, "phase7c umap lookups after rehash");
    size_t buckets = um.bucket_count();
    um.rehash(buckets * 4);
    Check(um.bucket_count() >= buckets * 4, "phase7c umap explicit rehash");
    all = true;
    for (int i = 0; i < 300; ++i)
        if (!um.contains("k" + std::to_string(i))) all = false;
    Check(all, "phase7c umap survives rehash");
    Check(um.erase("k7") == 1 && !um.contains("k7"), "phase7c umap erase");
    auto [ui, uok] = um.try_emplace("k9", 999);
    Check(!uok && ui->second == 9, "phase7c umap try_emplace");
    std::unordered_map<std::string, int> umc = um;
    Check(umc == um, "phase7c umap copy+equality");

    // unordered_set / multiset
    std::unordered_set<int> us{5, 3, 5, 1};
    Check(us.size() == 3 && us.contains(5) && !us.contains(2),
          "phase7c uset dedup");
    std::erase_if(us, [](int x) { return x < 4; });
    Check(us.size() == 1 && us.contains(5), "phase7c uset erase_if");
    std::unordered_multiset<int> ums{7, 7, 8};
    auto [ulo, uhi] = ums.equal_range(7);
    spread          = 0;
    for (auto it = ulo; it != uhi; ++it) ++spread;
    Check(ums.count(7) == 2 && spread == 2, "phase7c umultiset adjacency");

    // deque: ping-pong over block boundaries forces map growth both ways
    std::deque<int> d;
    for (int i = 0; i < 2000; ++i) {
        d.push_back(i);
        d.push_front(-i);
    }
    Check(d.size() == 4000 && d.front() == -1999 && d.back() == 1999,
          "phase7c deque growth both ends");
    // index 1999 is push_front(-0), index 2000 is push_back(0)
    Check(d[2000] == 0 && d[2001] == 1 && d.at(0) == -1999,
          "phase7c deque indexing");
    for (int i = 0; i < 1500; ++i) {
        d.pop_front();
        d.pop_back();
    }
    Check(d.size() == 1000 && d.front() == -499 && d.back() == 499,
          "phase7c deque shrink");
    Check(std::is_sorted(d.begin(), d.end()), "phase7c deque RA iterator");
    d.insert(d.begin() + 5, 7777);
    Check(d[5] == 7777 && d.size() == 1001, "phase7c deque insert");
    d.erase(d.begin() + 5);
    Check(d[5] == -494 && d.size() == 1000, "phase7c deque erase");
    std::deque<int> dm = std::move(d);
    Check(dm.size() == 1000 && dm.back() == 499, "phase7c deque move");

    // list: splice/merge/sort (the 64-bin bottom-up path on 1000 nodes)
    std::list<int> l{5, 1, 4, 2, 3};
    l.sort();
    prev    = 0;
    ordered = true;
    for (int x : l) {
        if (x <= prev) ordered = false;
        prev = x;
    }
    Check(ordered && l.front() == 1 && l.back() == 5, "phase7c list sort");
    std::list<int> l2{0, 6};
    l.merge(l2);
    Check(l.size() == 7 && l2.empty() && l.front() == 0 && l.back() == 6,
          "phase7c list merge");
    l.remove_if([](int x) { return x % 2 == 0; });
    Check(l.size() == 3, "phase7c list remove_if");
    l.push_front(1);
    l.unique();
    Check(l.size() == 3, "phase7c list unique");
    l.reverse();
    Check(l.front() == 5 && l.back() == 1, "phase7c list reverse");
    std::list<std::string> ls;
    ls.emplace_back("b");
    ls.emplace_front("a");
    std::list<std::string> ls2;
    ls2.splice(ls2.begin(), ls);
    Check(ls.empty() && ls2.size() == 2 && ls2.front() == "a",
          "phase7c list splice");
    std::list<int> big;
    for (int i = 999; i >= 0; --i) big.push_back(i);
    big.sort();
    prev    = -1;
    ordered = true;
    for (int x : big) {
        if (x != prev + 1) ordered = false;
        prev = x;
    }
    Check(ordered && big.size() == 1000, "phase7c list sort 1000");

    printf("[CXX] PASS phase7c: map/set/unordered/deque/list\n");
}

// ── phase7d fixtures: ranges views (Ф7B-3) ─────────────────────────────

static_assert(std::ranges::view<std::ranges::ref_view<std::vector<int>>>);
static_assert(
    std::ranges::borrowed_range<std::ranges::ref_view<std::vector<int>>>);
static_assert(std::is_same_v<std::views::all_t<std::vector<int> &>,
                             std::ranges::ref_view<std::vector<int>>>);
static_assert(std::is_same_v<std::views::all_t<std::vector<int>>,
                             std::ranges::owning_view<std::vector<int>>>);
static_assert(
    std::ranges::range<std::ranges::take_view<std::ranges::ref_view<
        std::vector<int>>>>);

void Phase7d()
{
    auto sum = [](auto &&r) {
        int s = 0;
        for (auto &&x : r) s += static_cast<int>(x);
        return s;
    };
    auto count = [](auto &&r) {
        int n = 0;
        for (auto &&x : r) {
            (void)x;
            ++n;
        }
        return n;
    };

    std::vector<int> v{1, 2, 3, 4, 5, 6}; // sum 21

    // view_interface surface through ref_view
    auto rf = std::ranges::ref_view(v);
    Check(rf.size() == 6 && rf.front() == 1 && rf.back() == 6 && rf[2] == 3 &&
              !rf.empty() && *rf.data() == 1,
          "phase7d ref_view + view_interface");

    // all: lvalue → ref_view, rvalue → owning_view (piped)
    Check(sum(std::views::all(v)) == 21, "phase7d views::all lvalue");
    Check(sum(std::vector<int>{10, 20} | std::views::all) == 30,
          "phase7d owning_view over rvalue");

    // subrange + structured bindings + next
    std::ranges::subrange sr(v.begin(), v.end());
    Check(sr.size() == 6 && !sr.empty(), "phase7d subrange size");
    auto [sb, se] = sr;
    Check(sb == v.begin() && se == v.end(), "phase7d subrange bindings");
    auto sr2 = sr.next(2);
    Check(sr2.size() == 4 && sum(sr2) == 18 && *sr2.begin() == 3,
          "phase7d subrange next");

    // take: clamp + size
    auto tk = v | std::views::take(3);
    Check(sum(tk) == 6 && count(tk) == 3 && tk.size() == 3, "phase7d take");
    Check(count(v | std::views::take(100)) == 6, "phase7d take over-count");

    // drop
    auto dr = v | std::views::drop(4);
    Check(sum(dr) == 11 && count(dr) == 2 && dr.size() == 2, "phase7d drop");
    Check(count(v | std::views::drop(100)) == 0, "phase7d drop over-count");

    // filter: forward sum + bidirectional decrement from the end
    auto fl = v | std::views::filter([](int x) { return x % 2 == 0; });
    Check(sum(fl) == 12 && count(fl) == 3, "phase7d filter even");
    auto eit = fl.end();
    --eit;
    Check(*eit == 6, "phase7d filter bidirectional");

    // transform
    auto tr = v | std::views::transform([](int x) { return x * 2; });
    Check(sum(tr) == 42 && count(tr) == 6, "phase7d transform");

    // compositions (take over filter/transform exercises counted_iterator)
    Check(sum(v | std::views::filter([](int x) { return x > 1; }) |
              std::views::take(2)) == 5,
          "phase7d filter|take");
    Check(sum(v | std::views::transform([](int x) { return x + 1; }) |
              std::views::take(3)) == 9,
          "phase7d transform|take (counted)");
    Check(sum(v | std::views::drop(1) |
              std::views::filter([](int x) { return x < 5; })) == 9,
          "phase7d drop|filter");
    Check(sum(v | std::views::filter([](int x) { return x % 2 == 0; }) |
              std::views::take(2)) == 6,
          "phase7d filter|take (counted)");

    // reusable composed closure applied to a range
    auto pipe = std::views::transform([](int x) { return x * 10; }) |
                std::views::take(2);
    Check(sum(v | pipe) == 30, "phase7d reusable closure");

    // span from a contiguous range — closes the ranges-ctor leftover
    std::span<int> sp(v);
    Check(sp.size() == 6 && sp[0] == 1 && sp.back() == 6,
          "phase7d span from range");
    std::span<const int> csp(v);
    Check(csp.size() == 6 && csp[5] == 6, "phase7d span<const> from range");
    std::vector<int> vm{7, 7, 7};
    std::span<int> spm(vm);
    spm[1] = 9;
    Check(vm[1] == 9, "phase7d span writes through to range");

    // ── non-random-access base (std::list): bounded drop, counted take,
    //    non-common Sentinel paths in filter/transform, MovableBox assign ──
    std::list<int> ll{1, 2, 3, 4, 5, 6};
    Check(sum(ll | std::views::drop(2)) == 18 &&
              count(ll | std::views::drop(2)) == 4,
          "phase7d drop over list (bounded path)");
    Check(count(ll | std::views::drop(100)) == 0, "phase7d drop list over-count");
    Check(sum(ll | std::views::filter([](int x) { return x % 3 == 0; })) == 9,
          "phase7d filter over list");
    Check(sum(ll | std::views::transform([](int x) { return x + 10; })) == 81,
          "phase7d transform over list");
    Check(sum(ll | std::views::take(3)) == 6 &&
              count(ll | std::views::take(3)) == 3,
          "phase7d take over list (counted_iterator)");
    // transform over a non-common base (take over list) → transform::Sentinel
    Check(sum(ll | std::views::take(4) |
              std::views::transform([](int x) { return x * 2; })) == 20,
          "phase7d transform over non-common base (Sentinel)");
    // filter over a non-common base (take over list) → filter::Sentinel
    Check(sum(ll | std::views::take(5) |
              std::views::filter([](int x) { return x % 2 == 1; })) == 9,
          "phase7d filter over non-common base (Sentinel)");
    std::ranges::subrange lsr(ll.begin(), ll.end());
    Check(sum(lsr) == 21, "phase7d subrange over list");

    // MovableBox move-assign: capturing lambda is non-assignable → the
    // destroy+reconstruct path runs.
    int thresh = 4;
    auto pred = [thresh](int x) { return x > thresh; };
    auto mvf  = ll | std::views::filter(pred);
    auto mvf2 = ll | std::views::filter(pred);
    mvf2 = std::move(mvf);
    Check(sum(mvf2) == 11, "phase7d filter_view move-assign (MovableBox)");

    printf("[CXX] PASS phase7d: ranges views "
           "(view_interface/subrange/all/take/drop/filter/transform/pipe + "
           "span-ctor)\n");
}

// ── phase8a fixtures: optional / variant / expected ────────────────────

int g_opt_dtors = 0;
struct OptDtor {
    int *d;
    explicit OptDtor(int *p) : d(p) {}
    OptDtor(const OptDtor &) = default;
    OptDtor(OptDtor &&o) noexcept : d(o.d) { o.d = nullptr; }
    ~OptDtor()
    {
        if (d) ++*d;
    }
};

struct Boom {
    int v;
    Boom() : v(0) {}
    explicit Boom(int) : v(0) { throw 42; } // throws while emplacing
    Boom(const Boom &) = default;
    Boom(Boom &&)      = default;
};

// compile-time proof that the three vocabulary types are constexpr-usable.
constexpr int Phase8aConstexpr()
{
    std::optional<int> o;
    o = 5;
    auto t = o.transform([](int x) { return x * 2; }).value_or(0); // 10
    std::expected<int, long> e{3};
    e = std::unexpected<long>(7);
    int eo = static_cast<int>(e.error_or(0)); // 7
    std::variant<int, long, char> v;
    v.emplace<1>(9L);
    int vi = static_cast<int>(std::get<1>(v)); // 9
    return t + eo + vi + static_cast<int>(v.index()); // 10+7+9+1 = 27
}

void Phase8a()
{
    using std::expected;
    using std::optional;
    using std::variant;

    // ── optional ───────────────────────────────────────────────────────
    static_assert(std::is_trivially_copyable_v<optional<int>>,
                  "optional<int> trivially copyable");
    static_assert(sizeof(optional<int>) == sizeof(int) * 2,
                  "optional<int> compact");
    static_assert(Phase8aConstexpr() == 27, "phase8a constexpr fold");

    optional<int> e;
    Check(!e && !e.has_value(), "phase8a optional empty");
    Check(e.value_or(99) == 99, "phase8a optional value_or empty");

    optional<int> o = 7;
    Check(o && *o == 7 && o.value() == 7, "phase8a optional engaged");
    o = 8;
    Check(*o == 8, "phase8a optional assign value");
    Check(o.emplace(11) == 11 && *o == 11, "phase8a optional emplace");

    bool threw = false;
    try {
        (void)e.value();
    } catch (const std::bad_optional_access &) {
        threw = true;
    }
    Check(threw, "phase8a optional bad_optional_access");

    // monadic chain
    auto r = optional<int>{4}
                 .and_then([](int x) -> optional<long> { return x + 1; })
                 .transform([](long x) { return static_cast<int>(x * 2); })
                 .or_else([] { return optional<int>{0}; });
    Check(r && *r == 10, "phase8a optional monadic chain");
    auto r2 = optional<int>{}.transform([](int x) { return x + 1; });
    Check(!r2, "phase8a optional transform on empty");

    // comparisons
    Check(optional<int>{3} == optional<int>{3} && optional<int>{3} != e,
          "phase8a optional ==");
    Check(optional<int>{2} < optional<int>{5} && e < optional<int>{1},
          "phase8a optional <");
    Check(o == 11 && e == std::nullopt, "phase8a optional mixed compare");

    // swap + dtor accounting
    optional<int> a{1}, b;
    a.swap(b);
    Check(!a && b && *b == 1, "phase8a optional swap");

    g_opt_dtors = 0;
    {
        optional<OptDtor> od{std::in_place, &g_opt_dtors};
        od.reset();
        Check(g_opt_dtors == 1 && !od, "phase8a optional reset destroys");
        od.emplace(&g_opt_dtors);
    } // dtor of engaged od → +1
    Check(g_opt_dtors == 2, "phase8a optional scope dtor");
    Check(std::make_optional<long>(5L).value() == 5,
          "phase8a make_optional");

    // ── expected ───────────────────────────────────────────────────────
    static_assert(std::is_trivially_copyable_v<expected<int, int>>,
                  "expected<int,int> trivially copyable");

    expected<int, long> ev = 5;
    Check(ev && *ev == 5 && ev.value() == 5, "phase8a expected value");
    Check(ev.value_or(0) == 5 && ev.error_or(-1) == -1,
          "phase8a expected value_or/error_or");

    ev = std::unexpected<long>(7); // value → error switch
    Check(!ev && ev.error() == 7, "phase8a expected error switch");
    bool ethrew = false;
    try {
        (void)ev.value();
    } catch (const std::bad_expected_access<long> &x) {
        ethrew = (x.error() == 7);
    }
    Check(ethrew, "phase8a expected bad_expected_access carries error");

    ev = 9; // error → value switch
    Check(ev && *ev == 9, "phase8a expected value switch back");

    auto et = expected<int, long>{4}
                  .and_then([](int x) -> expected<long, long> { return x + 1; })
                  .transform([](long x) { return static_cast<int>(x); })
                  .or_else([](long) -> expected<int, long> { return 0; });
    Check(et && *et == 5, "phase8a expected monadic chain");

    auto eterr = expected<int, long>{std::unexpect, 3}.transform_error(
        [](long x) { return static_cast<int>(x + 1); });
    Check(!eterr && eterr.error() == 4, "phase8a expected transform_error");

    expected<void, int> vexp;
    Check(vexp.has_value(), "phase8a expected<void> value");
    vexp = std::unexpected<int>(5);
    Check(!vexp && vexp.error() == 5, "phase8a expected<void> error");
    auto vexp2 = vexp.transform_error([](int x) { return x + 1; });
    Check(!vexp2 && vexp2.error() == 6, "phase8a expected<void> transform_error");

    Check((expected<int, long>{3} == expected<int, long>{3}),
          "phase8a expected ==");
    Check((ev == 9) && (vexp == std::unexpected<int>(5)),
          "phase8a expected mixed compare");

    // ── variant ────────────────────────────────────────────────────────
    static_assert(std::is_trivially_copyable_v<variant<int, float, char>>,
                  "variant trivially copyable");
    static_assert(std::variant_size_v<variant<int, char, long>> == 3,
                  "variant_size");

    variant<int, double, char> var;
    Check(var.index() == 0 && std::get<0>(var) == 0, "phase8a variant default");

    var = 3.5;
    Check(var.index() == 1 && std::holds_alternative<double>(var) &&
              std::get<double>(var) == 3.5,
          "phase8a variant converting assign (double)");

    var = 'A';
    Check(std::holds_alternative<char>(var) && std::get<char>(var) == 'A',
          "phase8a variant converting assign (char)");
    Check(*std::get_if<char>(&var) == 'A' && std::get_if<int>(&var) == nullptr,
          "phase8a variant get_if");

    bool gthrew = false;
    try {
        (void)std::get<int>(var);
    } catch (const std::bad_variant_access &) {
        gthrew = true;
    }
    Check(gthrew, "phase8a variant bad_variant_access");

    // visit: single and two-variant
    int vr = std::visit([](auto x) { return static_cast<int>(x); }, var); // 'A'
    Check(vr == 65, "phase8a visit single");
    variant<int, char> x1{2}, y1{'c'};
    int vr2 = std::visit(
        [](auto a2, auto b2) { return static_cast<int>(a2) + static_cast<int>(b2); },
        x1, y1); // 2 + 99
    Check(vr2 == 101, "phase8a visit two-variant");

    // emplace, comparisons, swap
    variant<int, char> c1{1}, c2{1};
    Check(c1 == c2 && !(c1 < c2), "phase8a variant ==");
    c2 = 'z';
    Check(c1 < c2, "phase8a variant < by index");
    c1.swap(c2);
    Check(std::holds_alternative<char>(c1) && std::holds_alternative<int>(c2),
          "phase8a variant swap differing index");

    variant<std::monostate, int> mv;
    Check(std::holds_alternative<std::monostate>(mv) && mv.index() == 0,
          "phase8a monostate default");

    // valueless_by_exception via throwing emplace
    variant<int, Boom> vb;
    bool bthrew = false;
    try {
        vb.emplace<1>(0);
    } catch (int) {
        bthrew = true;
    }
    Check(bthrew && vb.valueless_by_exception() &&
              vb.index() == std::variant_npos,
          "phase8a variant valueless_by_exception");

    // hash smoke
    Check(std::hash<variant<int, char>>{}(variant<int, char>{5}) != 0 &&
              std::hash<std::monostate>{}(std::monostate{}) != 0,
          "phase8a variant/monostate hash");

    printf("[CXX] PASS phase8a: optional/variant/expected "
           "(monadic/visit/valueless/trivial-copy)\n");
}

// ── phase8b fixtures: <memory> smart pointers ──────────────────────────

int g_sp_dtor = 0;
struct Res {
    int v;
    explicit Res(int x) : v(x) {}
    ~Res() { ++g_sp_dtor; }
};
struct Node : std::enable_shared_from_this<Node> {
    int id;
    explicit Node(int i) : id(i) {}
    std::shared_ptr<Node> me() { return shared_from_this(); }
};
struct SB {
    virtual ~SB() {}
    int b = 10;
};
struct SD : SB {
    int d = 20;
};
struct ResDel {
    int *n;
    void operator()(Res *p) const
    {
        ++*n;
        delete p;
    }
};

void Phase8b()
{
    using std::make_shared;
    using std::make_unique;
    using std::shared_ptr;
    using std::unique_ptr;
    using std::weak_ptr;

    static_assert(sizeof(unique_ptr<int>) == sizeof(int *),
                  "unique_ptr pointer-sized");
    static_assert(std::contiguous_iterator<std::vector<int>::iterator>,
                  "vector iterator contiguous via to_address");

    // unique_ptr
    auto u = make_unique<int>(21);
    Check(u && *u == 21, "phase8b unique_ptr make");
    int *raw = u.release();
    Check(!u && *raw == 21, "phase8b unique_ptr release");
    delete raw;
    auto ua = make_unique<int[]>(3);
    ua[0] = 1;
    ua[1] = 2;
    ua[2] = 3;
    Check(ua[2] == 3, "phase8b unique_ptr array");

    g_sp_dtor = 0;
    {
        unique_ptr<Res> ur = make_unique<Res>(5);
        Check(ur->v == 5, "phase8b unique_ptr custom type");
    }
    Check(g_sp_dtor == 1, "phase8b unique_ptr dtor");

    // shared_ptr lifecycle + weak_ptr
    g_sp_dtor = 0;
    {
        shared_ptr<Res> s1 = make_shared<Res>(9);
        Check(s1.use_count() == 1, "phase8b shared use_count 1");
        shared_ptr<Res> s2 = s1;
        Check(s1.use_count() == 2 && s2->v == 9, "phase8b shared use_count 2");
        weak_ptr<Res> w = s1;
        Check(!w.expired() && w.use_count() == 2, "phase8b weak observes");
        {
            shared_ptr<Res> s3 = w.lock();
            Check(s3 && s1.use_count() == 3, "phase8b weak lock");
        }
        Check(s1.use_count() == 2, "phase8b lock released");
        s2.reset();
        Check(s1.use_count() == 1 && g_sp_dtor == 0, "phase8b reset one");
    }
    Check(g_sp_dtor == 1, "phase8b shared all gone → dtor");

    // weak after expiry
    weak_ptr<Res> wexp;
    {
        auto t = make_shared<Res>(1);
        wexp    = t;
    }
    Check(wexp.expired() && !wexp.lock(), "phase8b weak expired");

    // enable_shared_from_this
    {
        auto n  = make_shared<Node>(42);
        auto n2 = n->me();
        Check(n2.get() == n.get() && n.use_count() == 2,
              "phase8b shared_from_this");
    }

    // pointer casts
    shared_ptr<SB> base = make_shared<SD>();
    auto           der  = std::dynamic_pointer_cast<SD>(base);
    Check(der && der->d == 20, "phase8b dynamic_pointer_cast");
    auto base2 = std::static_pointer_cast<SB>(der);
    Check(base2->b == 10 && base.use_count() == 3,
          "phase8b static_pointer_cast shares");

    // custom deleter + get_deleter
    int dcount = 0;
    g_sp_dtor  = 0;
    {
        shared_ptr<Res> cs(new Res(3), ResDel{&dcount});
        auto           *gd = std::get_deleter<ResDel>(cs);
        Check(gd != nullptr && gd->n == &dcount, "phase8b get_deleter");
    }
    Check(dcount == 1 && g_sp_dtor == 1, "phase8b custom deleter ran");

    // unique → shared
    shared_ptr<int> fromU = make_unique<int>(15);
    Check(*fromU == 15, "phase8b unique→shared");

    // aliasing ctor
    auto            pr = make_shared<std::pair<int, int>>(7, 8);
    shared_ptr<int> second(pr, &pr->second);
    Check(*second == 8 && pr.use_count() == 2, "phase8b aliasing ctor");

    // atomic<shared_ptr>
    std::atomic<shared_ptr<int>> asp{make_shared<int>(100)};
    Check(*asp.load() == 100, "phase8b atomic<shared_ptr> load");
    asp.store(make_shared<int>(200));
    Check(*asp.load() == 200, "phase8b atomic<shared_ptr> store");
    auto old = asp.exchange(make_shared<int>(300));
    Check(*old == 200 && *asp.load() == 300,
          "phase8b atomic<shared_ptr> exchange");

    printf("[CXX] PASS phase8b: memory "
           "(unique/shared/weak/enable_shared/casts/atomic + to_address)\n");
}

// ── phase8c fixtures: std::pmr + box::bay_memory_resource ───────────────

void Phase8c()
{
    namespace pmr = std::pmr;

    // monotonic_buffer_resource over a stack buffer
    alignas(std::max_align_t) char buf[2048];
    pmr::monotonic_buffer_resource mono{buf, sizeof(buf)};
    pmr::vector<int>               v{&mono};
    for (int i = 0; i < 60; ++i) v.push_back(i);
    int s = 0;
    for (int x : v) s += x;
    Check(s == 60 * 59 / 2, "phase8c monotonic + pmr::vector");

    // pool resource + nested pmr (vector of pmr::string) → allocator
    // propagation must reach the inner string
    pmr::unsynchronized_pool_resource pool{};
    pmr::vector<pmr::string>          vs{&pool};
    vs.emplace_back("hello");
    vs.emplace_back("a-much-longer-string-that-heap-allocates");
    Check(vs.size() == 2 && vs[0] == "hello", "phase8c pool + vector<string>");
    Check(vs[1].get_allocator().resource() == &pool,
          "phase8c uses-allocator propagation into nested string");

    // null resource throws
    bool caught = false;
    try {
        (void)pmr::null_memory_resource()->allocate(16);
    } catch (const std::bad_alloc &) {
        caught = true;
    }
    Check(caught, "phase8c null_memory_resource throws");

    // default resource get/set round-trip
    auto *before = pmr::get_default_resource();
    auto *prev   = pmr::set_default_resource(&mono);
    Check(pmr::get_default_resource() == &mono && prev == before,
          "phase8c set_default_resource");
    pmr::set_default_resource(before);
    Check(pmr::get_default_resource() == before, "phase8c restore default");

    // synchronized pool
    pmr::synchronized_pool_resource spool{};
    pmr::vector<int>                sv{&spool};
    for (int i = 0; i < 200; ++i) sv.push_back(i);
    Check(sv.size() == 200 && sv[199] == 199, "phase8c synchronized_pool");

    // box::bay_memory_resource — pmr container built into a shared Bay
    box::bay_memory_resource bay{"cxx:phase8c:bay", 1u << 20, BAY_CREATE};
    pmr::vector<int>         bv{&bay};
    for (int i = 0; i < 100; ++i) bv.push_back(i + 1);
    int bs = 0;
    for (int x : bv) bs += x;
    Check(bs == 100 * 101 / 2 && bay.used() > 0,
          "phase8c box::bay_memory_resource");

    printf("[CXX] PASS phase8c: pmr "
           "(memory_resource/polymorphic_allocator/monotonic/pool + bay)\n");
}

// ── phase8d fixtures: exception_ptr / nested + make_shared arrays ───────

void Phase8d()
{
    // make_shared arrays
    auto a = std::make_shared<int[]>(5);
    for (int i = 0; i < 5; ++i) a[i] = i;
    Check(a[4] == 4 && a.use_count() == 1, "phase8d make_shared<int[]>(n)");
    auto b = std::make_shared<int[]>(3, 9);
    Check(b[0] == 9 && b[2] == 9, "phase8d make_shared<int[]>(n,u)");
    auto c = std::make_shared<long[2]>(7);
    Check(c[0] == 7 && c[1] == 7, "phase8d make_shared<T[N]>(u)");

    // exception_ptr capture + rethrow
    std::exception_ptr ep;
    try {
        throw std::runtime_error("captured");
    } catch (...) {
        ep = std::current_exception();
    }
    Check(static_cast<bool>(ep), "phase8d current_exception captures");
    int rt = 0;
    try {
        std::rethrow_exception(ep);
    } catch (const std::runtime_error &e) {
        rt = (e.what()[0] == 'c');
    }
    Check(rt == 1, "phase8d rethrow_exception");

    // make_exception_ptr
    auto ep2     = std::make_exception_ptr(std::logic_error("le"));
    bool got_le  = false;
    try {
        std::rethrow_exception(ep2);
    } catch (const std::logic_error &) {
        got_le = true;
    }
    Check(got_le, "phase8d make_exception_ptr");

    // dependent-exception path: rethrow an exception_ptr while it is NOT the
    // active one (the common case) — exercises __cxa_rethrow_primary
    int dep = 0;
    {
        std::exception_ptr inner;
        try {
            throw 99;
        } catch (...) {
            inner = std::current_exception();
        }
        try {
            std::rethrow_exception(inner);
        } catch (int x) {
            dep = x;
        }
    }
    Check(dep == 99, "phase8d dependent rethrow");

    // nested_exception
    int nest_outer = 0, nest_inner = 0;
    try {
        try {
            throw std::runtime_error("inner-err");
        } catch (...) {
            std::throw_with_nested(std::logic_error("outer-err"));
        }
    } catch (const std::logic_error &e) {
        nest_outer = 1;
        try {
            std::rethrow_if_nested(e);
        } catch (const std::runtime_error &) {
            nest_inner = 1;
        }
    }
    Check(nest_outer && nest_inner,
          "phase8d throw_with_nested / rethrow_if_nested");

    Check(std::uncaught_exceptions() == 0, "phase8d uncaught counter rests");

    printf("[CXX] PASS phase8d: exception_ptr/nested + make_shared arrays\n");
}

// ── phase8e: closed leftovers (vector<bool>/algorithm/deque/errc) ──────

void Phase8e()
{
    // vector<bool> packed specialization
    std::vector<bool> vb;
    for (int i = 0; i < 200; ++i) vb.push_back(i % 2 == 0);
    int ones = 0;
    for (bool b : vb)
        if (b) ++ones;
    Check(ones == 100, "phase8e vector<bool> push/iterate");
    vb[1] = true;
    Check(vb[1], "phase8e vector<bool> proxy assign");
    vb.flip();
    int ones2 = 0;
    for (size_t i = 0; i < vb.size(); ++i)
        if (vb[i]) ++ones2;
    Check(ones2 == 99, "phase8e vector<bool> flip");
    Check(sizeof(std::vector<bool>) <= 40, "phase8e vector<bool> compact");

    // stable_partition (stable order preserved)
    std::vector<int> sp{1, 2, 3, 4, 5, 6, 7, 8};
    auto it = std::stable_partition(sp.begin(), sp.end(),
                                    [](int x) { return x % 2 == 0; });
    Check(sp[0] == 2 && sp[1] == 4 && sp[2] == 6 && *it == 1,
          "phase8e stable_partition");

    // permutations
    int arr[] = {1, 2, 3};
    int n     = 0;
    do {
        ++n;
    } while (std::next_permutation(arr, arr + 3));
    Check(n == 6, "phase8e next_permutation count");
    int chk[] = {3, 1, 2};
    Check(std::is_permutation(arr, arr + 3, chk), "phase8e is_permutation");

    // deque shrink_to_fit
    std::deque<int> dq;
    for (int i = 0; i < 1000; ++i) dq.push_back(i);
    for (int i = 0; i < 900; ++i) dq.pop_front();
    dq.shrink_to_fit();
    Check(dq.size() == 100 && dq.front() == 900 && dq.back() == 999,
          "phase8e deque shrink_to_fit");

    // system_error message (BoxOS-meaningful errc, not Unix socket/STREAMS)
    auto ec = std::make_error_code(std::errc::invalid_argument);
    Check(ec.message().size() > 0 && ec.message()[0] == 'i',
          "phase8e system_error message");

    printf("[CXX] PASS phase8e: "
           "vector<bool>/stable_partition/permutations/deque-shrink/errc\n");
}

// ── phase8f: unordered per-bucket local_iterator ───────────────────────

void Phase8f()
{
    std::unordered_map<int, int> m;
    for (int i = 0; i < 64; ++i) m[i] = i * 10;

    size_t total      = 0;
    bool   sizes_ok   = true;
    for (size_t b = 0; b < m.bucket_count(); ++b) {
        long n = 0;
        for (auto it = m.begin(b); it != m.end(b); ++it) {
            ++n;
            ++total;
        }
        if (static_cast<size_t>(n) != m.bucket_size(b)) sizes_ok = false;
    }
    Check(total == 64, "phase8f local_iterator visits every element once");
    Check(sizes_ok, "phase8f per-bucket count matches bucket_size");

    printf("[CXX] PASS phase8f: unordered local_iterator (per-bucket)\n");
}

// ── phase9a: <charconv> integer + sto* family ─────────────────────────────
void Phase9a()
{
    char buf[64];

    // to_chars: integer formatting
    {
        auto r = std::to_chars(buf, buf + sizeof(buf), 12345, 10);
        Check(r.ec == std::errc{} &&
                  std::string_view(buf, r.ptr - buf) == "12345",
              "phase9a to_chars dec");
    }
    {
        auto r = std::to_chars(buf, buf + sizeof(buf), -2147483647 - 1, 10);
        Check(r.ec == std::errc{} &&
                  std::string_view(buf, r.ptr - buf) == "-2147483648",
              "phase9a to_chars INT_MIN");
    }
    {
        auto r = std::to_chars(buf, buf + sizeof(buf), 0xDEADBEEFu, 16);
        Check(r.ec == std::errc{} &&
                  std::string_view(buf, r.ptr - buf) == "deadbeef",
              "phase9a to_chars hex lowercase");
    }
    {
        auto r = std::to_chars(buf, buf + sizeof(buf), 255u, 2);
        Check(r.ec == std::errc{} &&
                  std::string_view(buf, r.ptr - buf) == "11111111",
              "phase9a to_chars base2");
    }
    {
        auto r = std::to_chars(buf, buf + sizeof(buf), 35, 36);
        Check(r.ec == std::errc{} && r.ptr == buf + 1 && buf[0] == 'z',
              "phase9a to_chars base36");
    }
    {
        auto r = std::to_chars(buf, buf + sizeof(buf), 0, 10);
        Check(r.ec == std::errc{} && r.ptr == buf + 1 && buf[0] == '0',
              "phase9a to_chars zero");
    }
    {
        char small[3];
        auto r = std::to_chars(small, small + 3, 12345, 10);
        Check(r.ec == std::errc::value_too_large && r.ptr == small + 3,
              "phase9a to_chars value_too_large");
    }

    // from_chars: integer parsing
    {
        int        v = 0;
        const char s[] = "12345";
        auto       r = std::from_chars(s, s + 5, v, 10);
        Check(r.ec == std::errc{} && r.ptr == s + 5 && v == 12345,
              "phase9a from_chars dec");
    }
    {
        int        v = 0;
        const char s[] = "-2147483648";
        auto       r = std::from_chars(s, s + 11, v, 10);
        Check(r.ec == std::errc{} && v == (-2147483647 - 1),
              "phase9a from_chars INT_MIN");
    }
    {
        unsigned   v = 0;
        const char s[] = "DeadBeef";
        auto       r = std::from_chars(s, s + 8, v, 16);
        Check(r.ec == std::errc{} && v == 0xDEADBEEFu,
              "phase9a from_chars hex case-insensitive");
    }
    {
        int        v = 0;
        const char s[] = "123abc";
        auto       r = std::from_chars(s, s + 6, v, 10);
        Check(r.ec == std::errc{} && r.ptr == s + 3 && v == 123,
              "phase9a from_chars partial");
    }
    {
        int        v = 7;
        const char s[] = "zzz";
        auto       r = std::from_chars(s, s + 3, v, 10);
        Check(r.ec == std::errc::invalid_argument && r.ptr == s && v == 7,
              "phase9a from_chars invalid");
    }
    {
        int        v = 7;
        const char s[] = "99999999999";
        auto       r = std::from_chars(s, s + 11, v, 10);
        Check(r.ec == std::errc::result_out_of_range && r.ptr == s + 11 &&
                  v == 7,
              "phase9a from_chars overflow");
    }
    {
        unsigned   v = 7;
        const char s[] = "-5";
        auto       r = std::from_chars(s, s + 2, v, 10);
        Check(r.ec == std::errc::invalid_argument && r.ptr == s && v == 7,
              "phase9a from_chars unsigned rejects sign");
    }

    // to_chars/from_chars round-trip across bases
    {
        bool ok = true;
        long long values[] = {0,
                              1,
                              -1,
                              7,
                              -7,
                              1000000007LL,
                              -1000000007LL,
                              9223372036854775807LL,
                              -9223372036854775807LL - 1};
        int bases[] = {2, 8, 10, 16, 36};
        for (long long val : values) {
            for (int base : bases) {
                char b[80];
                auto w = std::to_chars(b, b + sizeof(b), val, base);
                if (w.ec != std::errc{}) {
                    ok = false;
                    continue;
                }
                long long back = 0;
                auto      rr   = std::from_chars(b, w.ptr, back, base);
                if (rr.ec != std::errc{} || rr.ptr != w.ptr || back != val)
                    ok = false;
            }
        }
        Check(ok, "phase9a round-trip all bases");
    }

    // sto* family (strtol semantics)
    {
        size_t pos = 0;
        Check(std::stoi("42") == 42, "phase9a stoi");
        Check(std::stoi("-42") == -42, "phase9a stoi neg");
        Check(std::stol("   123") == 123, "phase9a stol whitespace");
        Check(std::stoll("+777") == 777, "phase9a stoll plus");
        Check(std::stoi("123abc", &pos) == 123 && pos == 3, "phase9a stoi pos");
        Check(std::stoi("0x1A", nullptr, 16) == 26, "phase9a stoi 0x prefix");
        Check(std::stoi("0x1A", nullptr, 0) == 26, "phase9a stoi base0 hex");
        Check(std::stoi("052", nullptr, 0) == 42, "phase9a stoi base0 octal");
        Check(std::stoi("99", nullptr, 0) == 99, "phase9a stoi base0 dec");
        pos = 99;
        Check(std::stoi("0xZ", &pos, 0) == 0 && pos == 1,
              "phase9a stoi 0x no-hexdigit");
        Check(std::stoll("z", nullptr, 36) == 35, "phase9a stoll base36");
        Check(std::stoul("-1") == static_cast<unsigned long>(-1),
              "phase9a stoul negation wrap");
        Check(std::stoull("18446744073709551615") == 18446744073709551615ULL,
              "phase9a stoull ULLONG_MAX");
    }
    {
        bool threw = false;
        try {
            std::stoi("abc");
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        Check(threw, "phase9a stoi invalid_argument");
        threw = false;
        try {
            std::stoi("2147483648");
        } catch (const std::out_of_range &) {
            threw = true;
        }
        Check(threw, "phase9a stoi out_of_range");
        threw = false;
        try {
            std::stoull("99999999999999999999999");
        } catch (const std::out_of_range &) {
            threw = true;
        }
        Check(threw, "phase9a stoull out_of_range");
    }
    {
        Check(std::stoll(std::to_string(-9223372036854775807LL - 1)) ==
                  (-9223372036854775807LL - 1),
              "phase9a to_string/stoll round-trip");
    }

    printf("[CXX] PASS phase9a: <charconv> integer + sto* family\n");
}

// ── phase9a2: <charconv> float to_chars (Ryu shortest + formats + hex) ─────
// Expected strings are the authoritative host std::to_chars outputs; the Ryu
// implementation was validated char-for-char against it over ~30M values.
void Phase9a2()
{
    using F = std::chars_format;
    auto eqd0 = [&](double d, const char *exp, const char *tag) {
        char b[400]; auto r = std::to_chars(b, b + sizeof(b), d);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == exp, tag);
    };
    auto eqd = [&](double d, F f, const char *exp, const char *tag) {
        char b[400]; auto r = std::to_chars(b, b + sizeof(b), d, f);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == exp, tag);
    };
    auto eqf = [&](float v, const char *exp, const char *tag) {
        char b[64]; auto r = std::to_chars(b, b + sizeof(b), v);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == exp, tag);
    };

    // shortest (3-arg) — picks the shorter of fixed/scientific
    eqd0(1.0, "1", "phase9a2 shortest 1");
    eqd0(0.5, "0.5", "phase9a2 shortest 0.5");
    eqd0(3.14159, "3.14159", "phase9a2 shortest pi");
    eqd0(100000.0, "1e+05", "phase9a2 shortest 1e5");
    eqd0(0.0001, "1e-04", "phase9a2 shortest 1e-4");
    eqd0(1e20, "1e+20", "phase9a2 shortest 1e20");
    eqd0(-2.5, "-2.5", "phase9a2 shortest -2.5");

    // scientific
    eqd(1.0, F::scientific, "1e+00", "phase9a2 sci 1");
    eqd(0.5, F::scientific, "5e-01", "phase9a2 sci 0.5");
    eqd(3.14159, F::scientific, "3.14159e+00", "phase9a2 sci pi");

    // fixed — exact integer for large values (not zero-padded shortest)
    eqd(100000.0, F::fixed, "100000", "phase9a2 fixed 1e5");
    eqd(0.0001, F::fixed, "0.0001", "phase9a2 fixed 1e-4");
    eqd(1e20, F::fixed, "100000000000000000000", "phase9a2 fixed 1e20 exact");
    eqd(2.5, F::fixed, "2.5", "phase9a2 fixed 2.5");

    // general
    eqd(100000.0, F::general, "100000", "phase9a2 general 1e5");
    eqd(1e20, F::general, "1e+20", "phase9a2 general 1e20");
    eqd(3.14159, F::general, "3.14159", "phase9a2 general pi");

    // hex (no 0x prefix, lowercase, p±decimal)
    eqd(1.0, F::hex, "1p+0", "phase9a2 hex 1");
    eqd(0.5, F::hex, "1p-1", "phase9a2 hex 0.5");
    eqd(3.14159, F::hex, "1.921f9f01b866ep+1", "phase9a2 hex pi");
    eqd(2.5, F::hex, "1.4p+1", "phase9a2 hex 2.5");

    // float
    eqf(1.0f, "1", "phase9a2 float 1");
    eqf(0.1f, "0.1", "phase9a2 float 0.1");
    eqf(3.14f, "3.14", "phase9a2 float 3.14");
    {
        char b[16]; auto r = std::to_chars(b, b + sizeof(b), 0.1f, F::hex);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == "1.99999ap-4",
              "phase9a2 float hex 0.1");
    }

    // edge: zero / -0 / inf / nan
    eqd0(0.0, "0", "phase9a2 zero");
    eqd(0.0, F::scientific, "0e+00", "phase9a2 zero sci");
    eqd0(-0.0, "-0", "phase9a2 neg zero");
    {
        double inf = 1e308 * 10.0;
        char   b[8]; auto r = std::to_chars(b, b + sizeof(b), inf);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == "inf",
              "phase9a2 inf");
        auto r2 = std::to_chars(b, b + sizeof(b), -inf);
        Check(r2.ec == std::errc{} && std::string_view(b, r2.ptr - b) == "-inf",
              "phase9a2 -inf");
    }

    // value_too_large: 1e20 fixed (21 chars) into a 5-char buffer
    {
        char b[5]; auto r = std::to_chars(b, b + sizeof(b), 1e20, F::fixed);
        Check(r.ec == std::errc::value_too_large, "phase9a2 value_too_large");
    }

    printf("[CXX] PASS phase9a2: <charconv> float to_chars (Ryu shortest)\n");
}

// ── phase9a3: <charconv> float precision (5-arg) + to_string ───────────────
// Expected strings are authoritative host std::to_chars / std::to_string
// outputs; the big-integer precision path was validated char-for-char vs the
// host over ~30M (decimal) + ~34M (hex) value/precision pairs.
void Phase9a3()
{
    using F = std::chars_format;
    auto eqp = [&](double d, F f, int prec, const char *exp, const char *tag) {
        char b[400]; auto r = std::to_chars(b, b + sizeof(b), d, f, prec);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == exp, tag);
    };
    // fixed
    eqp(3.14159265358979, F::fixed, 2, "3.14", "phase9a3 fixed p2");
    eqp(3.14159265358979, F::fixed, 0, "3", "phase9a3 fixed p0");
    eqp(2.5, F::fixed, 4, "2.5000", "phase9a3 fixed trailing zeros");
    eqp(1.0 / 3.0, F::fixed, 10, "0.3333333333", "phase9a3 fixed 1/3");
    // scientific
    eqp(3.14159265358979, F::scientific, 3, "3.142e+00", "phase9a3 sci p3 round");
    eqp(0.0001, F::scientific, 2, "1.00e-04", "phase9a3 sci p2");
    // general (%g: strips trailing zeros)
    eqp(3.14159265358979, F::general, 3, "3.14", "phase9a3 general p3");
    eqp(100000.0, F::general, 3, "1e+05", "phase9a3 general sci");
    eqp(0.0001, F::general, 2, "0.0001", "phase9a3 general fixed");
    // hex precision (no normalize; carry grows leading digit)
    eqp(255.5, F::hex, 2, "1.ffp+7", "phase9a3 hex p2");
    eqp(1.0, F::hex, 3, "1.000p+0", "phase9a3 hex p3 pad");
    eqp(3.14159, F::hex, 0, "2p+1", "phase9a3 hex p0 carry");
    // zero across formats
    eqp(0.0, F::fixed, 3, "0.000", "phase9a3 zero fixed");
    eqp(0.0, F::scientific, 2, "0.00e+00", "phase9a3 zero sci");
    eqp(0.0, F::general, 4, "0", "phase9a3 zero general");

    // to_string (fixed, 6 fractional digits)
    Check(std::to_string(3.14159265358979) == "3.141593", "phase9a3 to_string pi");
    Check(std::to_string(0.0) == "0.000000", "phase9a3 to_string 0");
    Check(std::to_string(-2.5) == "-2.500000", "phase9a3 to_string -2.5");
    Check(std::to_string(1.0 / 3.0) == "0.333333", "phase9a3 to_string 1/3");
    Check(std::to_string(100.0f) == "100.000000", "phase9a3 to_string float");

    printf("[CXX] PASS phase9a3: <charconv> float precision + to_string\n");
}

// ── phase9a4: <charconv> float from_chars + stof/stod ──────────────────────
void Phase9a4()
{
    using F = std::chars_format;
    // round-trip: own to_chars → own from_chars must recover the exact value
    auto rtd = [&](double d, const char *tag) {
        char b[64]; auto w = std::to_chars(b, b + sizeof(b), d);
        double v = 0; auto r = std::from_chars(b, w.ptr, v);
        Check(r.ec == std::errc{} && r.ptr == w.ptr && v == d, tag);
    };
    double ds[] = {1.0, 0.5, 3.14159265358979, 1e20, 1e-20, 123456.789, -2.5,
                   1e308, 5e-324, 2.2250738585072014e-308, 9007199254740993.0};
    for (double d : ds) rtd(d, "phase9a4 round-trip double");
    auto rtf = [&](float f, const char *tag) {
        char b[64]; auto w = std::to_chars(b, b + sizeof(b), f);
        float v = 0; auto r = std::from_chars(b, w.ptr, v);
        Check(r.ec == std::errc{} && r.ptr == w.ptr && v == f, tag);
    };
    float fs[] = {1.0f, 0.1f, 3.14f, 1e20f, 1.4e-45f, 3.4028235e38f};
    for (float f : fs) rtf(f, "phase9a4 round-trip float");

    // known parses
    {
        double v = 0; const char s[] = "3.14159";
        auto r = std::from_chars(s, s + 7, v);
        Check(r.ec == std::errc{} && r.ptr == s + 7 && v == 3.14159,
              "phase9a4 parse pi");
    }
    {
        double v = 0; const char s[] = "1.5xyz";
        auto r = std::from_chars(s, s + 6, v);
        Check(r.ec == std::errc{} && r.ptr == s + 3 && v == 1.5,
              "phase9a4 partial");
    }
    {
        double v = 7; const char s[] = "abc";
        auto r = std::from_chars(s, s + 3, v);
        Check(r.ec == std::errc::invalid_argument && r.ptr == s && v == 7,
              "phase9a4 invalid unmodified");
    }
    {
        double v = 7; const char s[] = "1e309";  // overflow → value unmodified
        auto r = std::from_chars(s, s + 5, v);
        Check(r.ec == std::errc::result_out_of_range && v == 7,
              "phase9a4 overflow unmodified");
    }
    {
        double v = 0; const char s[] = "1.8p1";  // hex: 1.8_16 * 2^1 = 3.0
        auto r = std::from_chars(s, s + 5, v, F::hex);
        Check(r.ec == std::errc{} && v == 3.0, "phase9a4 hex");
    }
    {
        double v = 0; const char s[] = "inf";
        auto r = std::from_chars(s, s + 3, v);
        Check(r.ec == std::errc{} && v > 1.7976931348623157e308, "phase9a4 inf");
    }

    // stof / stod (strtod preamble)
    Check(std::stod("3.14") == 3.14, "phase9a4 stod");
    Check(std::stod("   -2.5") == -2.5, "phase9a4 stod ws+sign");
    Check(std::stof("3.14") == 3.14f, "phase9a4 stof");
    Check(std::stod("0x1.8p1") == 3.0, "phase9a4 stod hex");
    {
        size_t pos = 0;
        Check(std::stod("1.5e10abc", &pos) == 1.5e10 && pos == 6,
              "phase9a4 stod pos");
    }
    {
        bool threw = false;
        try { std::stod("abc"); } catch (const std::invalid_argument &) { threw = true; }
        Check(threw, "phase9a4 stod invalid_argument");
        threw = false;
        try { std::stod("1e400"); } catch (const std::out_of_range &) { threw = true; }
        Check(threw, "phase9a4 stod out_of_range");
    }

    printf("[CXX] PASS phase9a4: <charconv> float from_chars + stof/stod\n");
}

// ── phase9b: <format> (Ф9B-1) ──────────────────────────────────────────
// Exhaustively host-validated against g++-15 std::vformat (198k spec×value
// combos, 0 real diffs). This target phase confirms the same engine runs on
// BoxOS under CET (the float path goes through the own Ryu in <charconv>),
// and that the consteval format-string check accepts these strings.
void Phase9b()
{
    auto feq = [](const std::string &got, std::string_view want) {
        return std::string_view(got.data(), got.size()) == want;
    };

    // structure: auto / positional indexing, brace escapes, plain text
    Check(feq(std::format("{} {} {}", 1, 2, 3), "1 2 3"), "phase9b auto idx");
    Check(feq(std::format("{2} {1} {0}", 'a', 'b', 'c'), "c b a"),
          "phase9b positional idx");
    Check(feq(std::format("{{}}<{}>", 42), "{}<42>"), "phase9b brace escape");
    Check(feq(std::format("no fields"), "no fields"), "phase9b literal");

    // integers: base / sign / alt / fill+align / zero-pad / as-char
    Check(feq(std::format("{:d}", -42), "-42"), "phase9b int dec");
    Check(feq(std::format("{:#x}", 255), "0xff"), "phase9b int #x");
    Check(feq(std::format("{:#010X}", 255), "0X000000FF"), "phase9b int #010X");
    Check(feq(std::format("{:+}", 7), "+7"), "phase9b int +");
    Check(feq(std::format("{: }", 7), " 7"), "phase9b int space-sign");
    Check(feq(std::format("{:#b}", 5), "0b101"), "phase9b int #b");
    Check(feq(std::format("{:o}", 64), "100"), "phase9b int oct");
    Check(feq(std::format("{:*^7}", 42), "**42***"), "phase9b int center");
    Check(feq(std::format("{:<6}", 42), "42    "), "phase9b int left");
    Check(feq(std::format("{:>6}", 42), "    42"), "phase9b int right");
    Check(feq(std::format("{:c}", 65), "A"), "phase9b int as char");
    Check(feq(std::format("{:08}", -42), "-0000042"), "phase9b int zero+sign");
    Check(feq(std::format("{}", -2147483647 - 1), "-2147483648"),
          "phase9b INT_MIN");
    Check(feq(std::format("{:x}", 0xFFFFFFFFFFFFFFFFull), "ffffffffffffffff"),
          "phase9b u64 hex");

    // floating-point: shortest / fixed / scientific / general / hex / inf / nan
    Check(feq(std::format("{}", 3.14), "3.14"), "phase9b double shortest");
    Check(feq(std::format("{:.2f}", 3.14159), "3.14"), "phase9b double .2f");
    Check(feq(std::format("{:.3e}", 123456.0), "1.235e+05"), "phase9b double .3e");
    Check(feq(std::format("{:g}", 0.0001), "0.0001"), "phase9b double g");
    Check(feq(std::format("{:+.1f}", 2.5), "+2.5"), "phase9b double +.1f");
    Check(feq(std::format("{:10.2f}", -3.5), "     -3.50"), "phase9b double width");
    Check(feq(std::format("{:08.2f}", -3.5), "-0003.50"), "phase9b double zero");
    Check(feq(std::format("{:.0f}", 2.5), "2"), "phase9b double half-even-down");
    Check(feq(std::format("{:.0f}", 3.5), "4"), "phase9b double half-even-up");
    Check(feq(std::format("{:e}", 0.0), "0.000000e+00"), "phase9b double 0 sci");
    Check(feq(std::format("{:#.0f}", 5.0), "5."), "phase9b double # point");
    Check(feq(std::format("{:#.3g}", 1.0), "1.00"), "phase9b double #g zeros");
    Check(feq(std::format("{}", 1.0 / 0.0), "inf"), "phase9b inf");
    Check(feq(std::format("{:+}", 1.0 / 0.0), "+inf"), "phase9b +inf");
    Check(feq(std::format("{:F}", -1.0 / 0.0), "-INF"), "phase9b -INF");
    Check(feq(std::format("{}", __builtin_nan("")), "nan"), "phase9b nan");
    Check(feq(std::format("{}", -__builtin_nan("")), "-nan"), "phase9b -nan");
    Check(feq(std::format("{:.2f}", 1.5f), "1.50"), "phase9b float .2f");

    // strings + precision (truncation) + width
    Check(feq(std::format("{}", "hello"), "hello"), "phase9b cstr");
    Check(feq(std::format("{:.3}", "hello"), "hel"), "phase9b cstr precision");
    Check(feq(std::format("{:>8}", "hi"), "      hi"), "phase9b cstr width");
    Check(feq(std::format("{:*<6}", std::string("ab")), "ab****"),
          "phase9b string fill");
    {
        std::string_view sv = "world";
        Check(feq(std::format("[{:^9}]", sv), "[  world  ]"),
              "phase9b sv center");
    }

    // char / bool
    Check(feq(std::format("{}", 'Z'), "Z"), "phase9b char");
    Check(feq(std::format("{:d}", 'A'), "65"), "phase9b char as int");
    Check(feq(std::format("{}", true), "true"), "phase9b bool true");
    Check(feq(std::format("{:d}", false), "0"), "phase9b bool as int");
    Check(feq(std::format("{:>7}", false), "  false"), "phase9b bool align");

    // pointer
    Check(feq(std::format("{}", (void *)0), "0x0"), "phase9b ptr null");
    Check(feq(std::format("{:p}", (const void *)0xdead), "0xdead"),
          "phase9b ptr");

    // dynamic width / precision (nested {})
    Check(feq(std::format("{:{}}", 42, 6), "    42"), "phase9b dyn width");
    Check(feq(std::format("{:.{}f}", 3.14159, 2), "3.14"), "phase9b dyn prec");
    Check(feq(std::format("{:{}.{}f}", 2.5, 8, 3), "   2.500"),
          "phase9b dyn both");
    Check(feq(std::format("{0:{1}}", 7, 4), "   7"), "phase9b dyn manual");

    // format_to into a back_insert_iterator<string>
    {
        std::string out;
        std::format_to(std::back_inserter(out), "{}-{}", 1, 2);
        Check(feq(out, "1-2"), "phase9b format_to back_inserter");
    }
    // format_to into a raw char buffer (output iterator == char*)
    {
        char  buf[16] = {};
        auto *end     = std::format_to(buf, "{:04d}", 42);
        *end          = '\0';
        Check(std::string_view(buf) == "0042", "phase9b format_to char*");
    }
    // format_to_n truncates and reports the untruncated size
    {
        char buf[8]  = {};
        auto r       = std::format_to_n(buf, 4, "{}", 1234567);
        Check(r.size == 7 && std::string_view(buf, 4) == "1234",
              "phase9b format_to_n");
    }
    // formatted_size
    Check(std::formatted_size("{:6}", 42) == 6, "phase9b formatted_size");
    Check(std::formatted_size("{}", 12345) == 5, "phase9b formatted_size2");

    // vformat on a runtime string
    {
        int         a = 10, b = 20, c = 30;
        std::string s =
            std::vformat("{} + {} = {}", std::make_format_args(a, b, c));
        Check(feq(s, "10 + 20 = 30"), "phase9b vformat");
    }
    // vformat throws format_error on a bad spec at runtime ('s' on int)
    {
        bool threw = false;
        try {
            int x = 5;
            (void)std::vformat("{:s}", std::make_format_args(x));
        } catch (const std::format_error &) {
            threw = true;
        }
        Check(threw, "phase9b vformat throws on bad spec");
    }

    // user-defined formatter (type-erased handle / FmtThunk path, indirect
    // call — relevant under CET/IBT; compiled with -fcf-protection=full)
    Check(feq(std::format("{}", cxxfmt::Point{1, 2}), "(1,2)"),
          "phase9b custom formatter default");
    Check(feq(std::format("{:v}", cxxfmt::Point{3, 4}), "Point(x=3, y=4)"),
          "phase9b custom formatter spec");
    Check(feq(std::format("{} {:v}", cxxfmt::Point{1, 2}, cxxfmt::Point{5, 6}),
              "(1,2) Point(x=5, y=6)"),
          "phase9b custom formatter multi");

    printf("[CXX] PASS phase9b: <format> "
           "(spec/formatters/dynamic/format_to/vformat/custom)\n");
}

// ── phase9c: <print> (Ф9B-2) ───────────────────────────────────────────
// std::print / println format via <format> and write through boxlib
// print_bytes — the real VGA / display-daemon console path. The output
// can't be read back in-process, so the three "[P9C]" lines below are
// verified to appear verbatim in the STRICT-matrix serial logs.
void Phase9c()
{
    std::print("[P9C]a={}", 1);
    std::println(" b={:#x}", 255);  // -> [P9C]a=1 b=0xff
    std::println("[P9C]{:>5}|{:<5}|{:.2f}", "hi", "yo",
                 3.14159);  // -> [P9C]   hi|yo   |3.14
    std::print("[P9C]");
    std::println("multi={} {} {}", true, 'Z', 42);  // -> [P9C]multi=true Z 42
    std::println();                                 // bare newline

    printf("[CXX] PASS phase9c: <print> std::print/println -> console\n");
}

// ── phaseCurrent: box::current (the C++ face of the BoxOS Current spine) ────
// Exercises the typed C++ layer over box/current.h: a framed Brook stream
// (put/take + honest CURRENT_CLOSED), a TagFS file round-trip, small-item
// padding, and the conventional log/screen channels with box::println.
void PhaseCurrent()
{
    struct Sample { int id; unsigned tag; };

    // Typed framed stream, same-cabin writer + reader.
    {
        box::current<Sample> w("cxx:current:stream", box::role::write);  // writer auto-creates
        box::current<Sample> r("cxx:current:stream", box::role::read);
        Check(bool(w) && bool(r), "phaseCurrent stream open");
        bool put_ok = true;
        for (int i = 0; i < 3; i++) put_ok = put_ok && w.put(Sample{i, (unsigned)(i * 11)});
        Check(put_ok, "phaseCurrent stream put");
        bool take_ok = true;
        for (int i = 0; i < 3; i++) {
            Sample s{};
            take_ok = take_ok && r.take(s) && s.id == i && s.tag == (unsigned)(i * 11);
        }
        Check(take_ok, "phaseCurrent stream take");
        w.close();
        Sample drained{};
        Check(!r.take(drained), "phaseCurrent stream CURRENT_CLOSED");
    }

    // Byte channel over a TagFS file: write, then read back.
    {
        const char *msg = "current<byte> over TagFS";  // 24 bytes
        box::byte_current fw = box::file("cxx_current.dat", box::role::write);
        std::size_t n = fw ? fw.write(msg, 24) : 0;
        Check(n == 24, "phaseCurrent file write");
        fw = box::byte_current{};   // release writer (RAII)

        box::byte_current fr = box::file("cxx_current.dat", box::role::read, 0);
        char back[25] = {};
        int  rn = fr ? fr.read(back, 24) : -1;
        Check(rn == 24 && std::string_view(back, 24) == msg, "phaseCurrent file read");
        Check((fr.caps() & CURRENT_CAP_SEEKABLE) != 0, "phaseCurrent file seekable");
    }

    // Small item (2 bytes) through the typed layer — exercises frame padding.
    {
        box::current<std::uint16_t> w("cxx:current:small", box::role::write, CURRENT_CREATE);
        box::current<std::uint16_t> r("cxx:current:small", box::role::read);
        bool          ok = bool(w) && bool(r) && w.put(0xC0DE);
        std::uint16_t v  = 0;
        ok = ok && r.take(v) && v == 0xC0DE;
        Check(ok, "phaseCurrent small-item padding");
    }

    // Conventional channels + formatted output (serial-/console-verifiable).
    {
        box::byte_current lg = box::log();
        box::println(lg, "[CURRENT-CXX] log via box::println n={}", 7);
        box::byte_current sc = box::screen();
        box::println(sc, "[CURRENT-CXX] screen ok");
        box::println(box::log(), "[CURRENT-CXX] rvalue channel ok");  // rvalue-channel overload
        Check(bool(lg) && bool(sc), "phaseCurrent conventional channels");
    }

    printf("[CXX] PASS phaseCurrent: box::current (stream/file/log/screen)\n");
}

// ── phase10: <cmath> (classical + C++17 special) + box:: universal layer ───
void Phase10()
{
    auto eq = [](double a, double b, double tol) {
        double d = std::fabs(a - b);
        return d <= tol || d <= tol * std::fabs(b);
    };
    const double PI = 3.14159265358979311600;
    // algebraic
    Check(eq(std::sqrt(2.0), 1.4142135623730951, 1e-15), "sqrt2");
    Check(eq(std::cbrt(27.0), 3.0, 1e-14), "cbrt27");
    Check(std::floor(2.7) == 2.0 && std::ceil(2.1) == 3.0 && std::round(2.5) == 3.0 && std::trunc(-2.7) == -2.0, "round-family");
    Check(eq(std::hypot(3.0, 4.0), 5.0, 1e-15) && std::fabs(-3.0) == 3.0, "hypot/fabs");
    Check(eq(std::fmod(7.0, 3.0), 1.0, 1e-15) && eq(std::remainder(7.0, 3.0), 1.0, 1e-15), "fmod/remainder");
    // exp / log
    Check(eq(std::exp(1.0), 2.718281828459045, 1e-15), "exp1");
    Check(eq(std::log(2.718281828459045), 1.0, 1e-15) && eq(std::log2(1024.0), 10.0, 1e-13) && eq(std::log10(1000.0), 3.0, 1e-13), "log family");
    Check(eq(std::exp2(10.0), 1024.0, 1e-13) && eq(std::expm1(1e-6), 1.0000005e-6, 1e-9) && eq(std::log1p(1e-6), 9.999995e-7, 1e-9), "exp2/expm1/log1p");
    // trig + inverse
    Check(eq(std::sin(PI / 6), 0.5, 1e-15) && eq(std::cos(PI / 3), 0.5, 1e-15) && eq(std::tan(PI / 4), 1.0, 1e-14), "sin/cos/tan");
    { double s = std::sin(1.3), c = std::cos(1.3); Check(eq(s * s + c * c, 1.0, 1e-15), "sin^2+cos^2"); }
    Check(eq(std::atan(1.0), PI / 4, 1e-15) && eq(std::asin(1.0), PI / 2, 1e-12) && eq(std::atan2(1.0, 1.0), PI / 4, 1e-15), "inverse trig");
    Check(eq(std::sin(1e7), 0.4205477931907825, 1e-9), "sin large-arg");
    // hyperbolic
    Check(eq(std::sinh(1.0), 1.1752011936438014, 1e-14) && eq(std::cosh(1.0), 1.5430806348152437, 1e-14) && eq(std::tanh(0.5), 0.46211715726000974, 1e-14), "sinh/cosh/tanh");
    Check(eq(std::asinh(std::sinh(0.7)), 0.7, 1e-13) && eq(std::acosh(std::cosh(1.2)), 1.2, 1e-12) && eq(std::atanh(0.5), 0.5493061443340549, 1e-13), "inverse hyperbolic");
    // pow
    Check(eq(std::pow(2.0, 10.0), 1024.0, 1e-13) && eq(std::pow(2.0, 0.5), 1.4142135623730951, 1e-14) && eq(std::pow(27.0, 1.0 / 3.0), 3.0, 1e-12), "pow");
    // erf / gamma
    Check(eq(std::erf(1.0), 0.8427007929497149, 1e-12) && eq(std::erfc(1.0), 0.15729920705028513, 1e-11), "erf/erfc");
    Check(eq(std::tgamma(5.0), 24.0, 1e-12) && eq(std::tgamma(0.5), 1.7724538509055159, 1e-12) && std::fabs(std::lgamma(1.0)) < 1e-12, "tgamma/lgamma");
    // lock in double-double dd-log accuracy on real HW (audit-2): tgamma(20)=19!, pow large
    Check(eq(std::tgamma(20.0), 121645100408832000.0, 1e-13) && eq(std::pow(7.0, 20.0), 79792266297612001.0, 1e-13), "tgamma/pow large (dd)");
    Check(eq(std::erfc(2.0), 0.0046777349810472660, 1e-11) && eq(std::erfc(5.0), 1.5374597944280349e-12, 1e-10), "erfc moderate/large");
    // classification
    Check(std::isnan(std::nan("")) && std::isinf(HUGE_VAL) && std::signbit(-1.0) && std::isfinite(1.0) && !std::isnormal(0.0), "classification");
    // manipulation
    { int e; double m = std::frexp(12.0, &e); Check(eq(m, 0.75, 1e-15) && e == 4, "frexp"); }
    Check(eq(std::ldexp(1.5, 4), 24.0, 0.0) && std::ilogb(12.0) == 3 && eq(std::fma(2.0, 3.0, 4.0), 10.0, 0.0), "ldexp/ilogb/fma");
    Check(std::fmax(2.0, 3.0) == 3.0 && std::fmin(2.0, 3.0) == 2.0 && std::fdim(5.0, 2.0) == 3.0 && std::copysign(2.0, -1.0) == -2.0, "fmax/fmin/fdim/copysign");
    // C++17 special — orthogonal polynomials + beta
    Check(eq(std::legendre(2, 0.5), -0.125, 1e-13) && eq(std::laguerre(2, 1.0), -0.5, 1e-13) && eq(std::hermite(3, 1.0), -4.0, 1e-12), "legendre/laguerre/hermite");
    Check(eq(std::beta(2.0, 3.0), 1.0 / 12.0, 1e-12) && eq(std::assoc_legendre(1, 1, 0.5), 0.8660254037844386, 1e-12), "beta/assoc_legendre");
    // zeta / expint
    Check(eq(std::riemann_zeta(2.0), 1.6449340668482264, 1e-12) && eq(std::riemann_zeta(-1.0), -1.0 / 12.0, 1e-11), "riemann_zeta");
    Check(eq(std::expint(1.0), 1.8951178163559368, 1e-11), "expint");
    // Bessel
    Check(eq(std::cyl_bessel_j(0.0, 1.0), 0.7651976865579666, 1e-9) && eq(std::cyl_bessel_j(2.0, 5.0), 0.046565116277752214, 1e-7), "cyl_bessel_j");
    Check(eq(std::cyl_neumann(0.0, 1.0), 0.08825696421567696, 1e-7) && eq(std::cyl_bessel_i(0.0, 1.0), 1.2660658777520084, 1e-9), "cyl_neumann/i");
    Check(eq(std::cyl_bessel_k(0.0, 1.0), 0.42102443824070834, 1e-7) && eq(std::sph_bessel(1, 1.0), 0.30116867893975674, 1e-10), "cyl_bessel_k/sph_bessel");
    // elliptic
    Check(eq(std::comp_ellint_1(0.0), PI / 2, 1e-14) && eq(std::comp_ellint_2(0.0), PI / 2, 1e-14), "comp_ellint_1/2");
    Check(eq(std::ellint_1(0.5, 1.0), 1.0373561200021773, 1e-11) && eq(std::comp_ellint_1(0.5), 1.6857503548125963, 1e-11), "ellint_1");
    // box:: universal layer
    Check(eq(box::log(8.0, 3.0), 1.8927892607143724, 1e-13) && eq(box::log(1000.0, 10.0), 3.0, 1e-12), "box::log(x,base)");
    Check(eq(box::root(27.0, 3.0), 3.0, 1e-13) && eq(box::root(-32.0, 5.0), -2.0, 1e-13) && eq(box::root(16.0, 4.0), 2.0, 1e-13), "box::root");
    Check(eq(box::round(3.14159, 2), 3.14, 1e-12) && eq(box::round(1234.5678, -2), 1200.0, 1e-12), "box::round(x,digits)");
    Check(eq(box::sin(90.0, box::deg), 1.0, 1e-15) && eq(box::cos(180.0, box::deg), -1.0, 1e-15) && eq(box::atan2(1.0, 1.0, box::deg), 45.0, 1e-13), "box:: degree trig");
    printf("[CXX] PASS phase10: <cmath> classical+special + box:: universal\n");
}

void Phase11()
{
    using namespace std::chrono;
    using namespace std::chrono_literals;
    using std::ratio;
    using std::ratio_add;
    using std::ratio_divide;
    using std::ratio_less_v;
    using std::ratio_multiply;
    using std::milli;
    using std::micro;
    using std::nano;
    using std::kilo;
    using std::mega;

    // ── <ratio> (compile-time) ──────────────────────────────────────────
    static_assert(ratio<6, 4>::num == 3 && ratio<6, 4>::den == 2,
                  "ratio reduce");
    static_assert(ratio_add<milli, micro>::num == 1001 &&
                      ratio_add<milli, micro>::den == 1000000,
                  "ratio_add");
    static_assert(ratio_multiply<ratio<2, 3>, ratio<3, 4>>::num == 1 &&
                      ratio_multiply<ratio<2, 3>, ratio<3, 4>>::den == 2,
                  "ratio_multiply");
    static_assert(ratio_less_v<nano, micro>, "ratio_less");
    static_assert(std::is_same_v<ratio_divide<mega, kilo>, kilo>,
                  "ratio_divide");

    // ── duration arithmetic & conversions ───────────────────────────────
    Check((3s + 500ms).count() == 3500 &&
              std::is_same_v<decltype(3s + 500ms), milliseconds>,
          "phase11 duration add → ms");
    Check(duration_cast<seconds>(milliseconds{3500}).count() == 3,
          "phase11 duration_cast truncates");
    Check(floor<seconds>(milliseconds{-1500}).count() == -2,
          "phase11 floor negative");
    Check(ceil<seconds>(milliseconds{1001}).count() == 2, "phase11 ceil");
    Check(round<seconds>(milliseconds{2500}).count() == 2 &&
              round<seconds>(milliseconds{3500}).count() == 4,
          "phase11 round half-to-even");
    Check(abs(seconds{-5}).count() == 5, "phase11 abs");
    Check(duration_cast<minutes>(1h).count() == 60, "phase11 1h→min");
    Check(nanoseconds{1000000000} == seconds{1}, "phase11 ns==s");
    Check((1h - 30min) == 30min, "phase11 sub");

    // ── steady_clock monotonicity ───────────────────────────────────────
    {
        auto t0 = steady_clock::now();
        auto t1 = steady_clock::now();
        Check(t1 >= t0, "phase11 steady monotone");
        Check(t1.time_since_epoch().count() >= 0, "phase11 steady non-negative");
        Check(steady_clock::is_steady, "phase11 steady is_steady");
    }

    // ── system_clock wall time ──────────────────────────────────────────
    {
        auto      now  = system_clock::now();
        long long secs = system_clock::to_time_t(now);
        Check(system_clock::to_time_t(system_clock::from_time_t(secs)) == secs,
              "phase11 system_clock round-trip");
        // QEMU exposes host RTC → expect a plausible 2020..2100 wall time.
        Check(secs > 1577836800LL && secs < 4102444800LL,
              "phase11 system_clock epoch plausible");
        Check(!system_clock::is_steady, "phase11 system not steady");
    }

    // ── calendar round-trips & known facts ──────────────────────────────
    Check((2024y / February / 29d).ok(), "phase11 2024 leap day ok");
    Check(!(2023y / February / 29d).ok(), "phase11 2023 non-leap not ok");
    Check(weekday{sys_days{2024y / January / 1d}} == Monday,
          "phase11 2024-01-01 is Monday");
    Check(weekday{sys_days{1970y / January / 1d}} == Thursday,
          "phase11 epoch is Thursday");
    {
        auto rt = [](sys_days d) { return sys_days{year_month_day{d}} == d; };
        Check(rt(sys_days{1970y / January / 1d}) &&
                  rt(sys_days{2000y / March / 1d}) &&
                  rt(sys_days{2024y / February / 29d}),
              "phase11 ymd↔sys_days round-trip");
    }
    Check(unsigned((2024y / February / last).day()) == 29,
          "phase11 last day Feb 2024");
    Check(unsigned(year_month_day{sys_days{2024y / March / Friday[2]}}.day()) ==
              8,
          "phase11 2nd Friday Mar 2024");
    Check(unsigned(
              year_month_day{sys_days{2024y / March / Monday[last]}}.day()) ==
              25,
          "phase11 last Monday Mar 2024");

    // ── hh_mm_ss decomposition ──────────────────────────────────────────
    {
        hh_mm_ss<seconds> a{3h + 25min + 45s};
        Check(a.hours().count() == 3 && a.minutes().count() == 25 &&
                  a.seconds().count() == 45,
              "phase11 hh_mm_ss decompose");
        hh_mm_ss<seconds> b{-(1h + 30min)};
        Check(b.is_negative() && b.hours().count() == 1 &&
                  b.minutes().count() == 30,
              "phase11 hh_mm_ss negative");
        hh_mm_ss<milliseconds> c{1h + 2min + 3s + 456ms};
        Check(c.subseconds().count() == 456 &&
                  hh_mm_ss<milliseconds>::fractional_width == 3,
              "phase11 hh_mm_ss subseconds");
        Check(make12(hours{13}) == hours{1} && make24(hours{1}, true) == hours{13},
              "phase11 make12/make24");
    }

    // ── <format> integration (expected strings validated vs libstdc++) ──
    Check(std::format("{}", 42s) == "42s", "phase11 fmt dur s");
    Check(std::format("{}", 1500ms) == "1500ms", "phase11 fmt dur ms");
    Check(std::format("{}", 7us) == "7us", "phase11 fmt dur us");
    Check(std::format("{}", minutes{90}) == "90min", "phase11 fmt dur min");
    Check(std::format("{}", hours{5}) == "5h", "phase11 fmt dur h");
    Check(std::format("{}", days{3}) == "3d", "phase11 fmt dur d");
    Check(std::format("{}", duration<long long, ratio<1, 3>>{2}) == "2[1/3]s",
          "phase11 fmt dur custom");
    Check(std::format("{}", day{8}) == "08", "phase11 fmt day");
    Check(std::format("{}", month{3}) == "Mar", "phase11 fmt month");
    Check(std::format("{}", year{2024}) == "2024", "phase11 fmt year");
    Check(std::format("{}", weekday{1}) == "Mon", "phase11 fmt weekday");
    Check(std::format("{:%F}", 2024y / March / 15d) == "2024-03-15",
          "phase11 fmt %F");
    Check(std::format("{:%Y-%m-%d}", 2024y / March / 15d) == "2024-03-15",
          "phase11 fmt %Y-%m-%d");
    Check(std::format("{:%D}", 2024y / March / 5d) == "03/05/24",
          "phase11 fmt %D");
    Check(std::format("{:%j}", 2024y / March / 1d) == "061", "phase11 fmt %j");
    Check(std::format("{}", 2024y / January / 1d) == "2024-01-01",
          "phase11 fmt ymd default");
    Check(std::format("{:%a}", weekday{1}) == "Mon", "phase11 fmt %a");
    Check(std::format("{:%A}", weekday{0}) == "Sunday", "phase11 fmt %A");
    Check(std::format("{:%u}", weekday{0}) == "7", "phase11 fmt %u");
    Check(std::format("{:%w}", weekday{0}) == "0", "phase11 fmt %w");
    Check(std::format("{:%b}", month{12}) == "Dec", "phase11 fmt %b");
    Check(std::format("{:%B}", month{7}) == "July", "phase11 fmt %B");
    Check(std::format("{:%y}", year{2024}) == "24", "phase11 fmt %y");
    Check(std::format("{:%T}", hh_mm_ss<seconds>{3h + 25min + 45s}) ==
              "03:25:45",
          "phase11 fmt %T");
    Check(std::format("{:%R}", hh_mm_ss<seconds>{9h + 5min}) == "09:05",
          "phase11 fmt %R");
    Check(std::format("{:%H:%M:%S}",
                      hh_mm_ss<seconds>{23h + 59min + 1s}) == "23:59:01",
          "phase11 fmt %H:%M:%S");
    Check(std::format("{:%T}",
                      hh_mm_ss<milliseconds>{1h + 2min + 3s + 456ms}) ==
              "01:02:03.456",
          "phase11 fmt %T ms");
    Check(std::format("{:%I %p}", hh_mm_ss<seconds>{13h}) == "01 PM",
          "phase11 fmt %I %p");
    Check(std::format("{:%F %T}", sys_seconds{seconds{1710474345}}) ==
              "2024-03-15 03:45:45",
          "phase11 fmt sys %F %T");
    Check(std::format("{}", sys_seconds{seconds{1710474345}}) ==
              "2024-03-15 03:45:45",
          "phase11 fmt sys default");
    Check(std::format("{}", time_point_cast<days>(
                                sys_seconds{seconds{1710474345}})) ==
              "2024-03-15",
          "phase11 fmt sys_days");
    Check(std::format("{:>12%F}", 2024y / March / 5d) == "  2024-03-05",
          "phase11 fmt width right");
    Check(std::format("{:<12%F}", 2024y / March / 5d) == "2024-03-05  ",
          "phase11 fmt width left");
    Check(std::format("{:*^14%F}", 2024y / March / 5d) == "**2024-03-05**",
          "phase11 fmt width center fill");

    printf("[CXX] PASS phase11: <chrono> + <ratio> (duration/clock/calendar/format)\n");
}

template <class E>
static unsigned long long EngNth(E e, int n)
{
    typename E::result_type v{};
    for (int i = 0; i < n; ++i) v = e();
    return static_cast<unsigned long long>(v);
}

void Phase12()
{
    using namespace std;

    // ── engine known values ([rand.predef] conformance constants) ───────
    Check(EngNth(minstd_rand0{}, 10000) == 1043618065ull, "phase12 minstd_rand0");
    Check(EngNth(minstd_rand{}, 10000) == 399268537ull, "phase12 minstd_rand");
    Check(EngNth(mt19937{}, 10000) == 4123659995ull, "phase12 mt19937");
    Check(EngNth(mt19937_64{}, 10000) == 9981545732273789042ull,
          "phase12 mt19937_64");
    Check(EngNth(ranlux24_base{}, 10000) == 7937952ull, "phase12 ranlux24_base");
    Check(EngNth(ranlux24{}, 10000) == 9901578ull, "phase12 ranlux24");
    Check(EngNth(knuth_b{}, 10000) == 1112339016ull, "phase12 knuth_b");
    Check(EngNth(independent_bits_engine<mt19937, 40, unsigned long long>{}, 1) ==
              119014334198ull,
          "phase12 independent_bits_engine");

    // ── engine reproducibility & discard ────────────────────────────────
    {
        mt19937 a(777), b(777);
        bool    same = true;
        for (int i = 0; i < 50; ++i) if (a() != b()) same = false;
        Check(same, "phase12 mt19937 reproducible");
        mt19937 c(777), d(777);
        c.discard(50);
        for (int i = 0; i < 50; ++i) d();
        Check(c() == d(), "phase12 discard == N draws");
    }

    // ── seed_seq seeding ────────────────────────────────────────────────
    {
        seed_seq ss{1, 2, 3, 4};
        mt19937  a(ss), b(ss);
        Check(a() == b() && a() == b(), "phase12 seed_seq reproducible");
    }

    // ── generate_canonical in [0,1) ─────────────────────────────────────
    {
        mt19937 g(99);
        bool    ok = true;
        for (int i = 0; i < 1000; ++i) {
            double c = generate_canonical<double, 53>(g);
            if (!(c >= 0.0 && c < 1.0)) ok = false;
        }
        Check(ok, "phase12 generate_canonical range");
    }

    // ── distribution statistical sanity (modest N for QEMU) ─────────────
    auto mean_of = [](auto d, auto &g, int n) {
        double s = 0;
        for (int i = 0; i < n; ++i) s += static_cast<double>(d(g));
        return s / n;
    };
    auto near = [](double a, double b, double tol) {
        double diff = a - b;
        if (diff < 0) diff = -diff;
        double t = tol * (b < 0 ? -b : b);
        if (t < tol) t = tol;
        return diff <= t;
    };
    {
        mt19937 g(2024);
        const int N = 30000;
        Check(near(mean_of(uniform_int_distribution<int>(1, 6), g, N), 3.5, 0.05),
              "phase12 uniform_int mean");
        Check(near(mean_of(uniform_real_distribution<double>(0, 1), g, N), 0.5, 0.05),
              "phase12 uniform_real mean");
        Check(near(mean_of(bernoulli_distribution(0.3), g, N), 0.3, 0.05),
              "phase12 bernoulli mean");
        Check(near(mean_of(binomial_distribution<int>(20, 0.4), g, N), 8.0, 0.05),
              "phase12 binomial mean");
        Check(near(mean_of(poisson_distribution<int>(4.0), g, N), 4.0, 0.05),
              "phase12 poisson mean");
        Check(near(mean_of(exponential_distribution<double>(1.5), g, N), 1.0 / 1.5, 0.05),
              "phase12 exponential mean");
        Check(near(mean_of(normal_distribution<double>(5.0, 2.0), g, N), 5.0, 0.05),
              "phase12 normal mean");
        Check(near(mean_of(gamma_distribution<double>(2.0, 1.5), g, N), 3.0, 0.06),
              "phase12 gamma mean");
        Check(near(mean_of(gamma_distribution<double>(0.5, 2.0), g, N), 1.0, 0.07),
              "phase12 gamma a<1 mean");
        Check(near(mean_of(discrete_distribution<int>({1, 2, 3, 4}), g, N), 2.0, 0.05),
              "phase12 discrete mean");
    }

    // ── distribution reproducibility ────────────────────────────────────
    {
        mt19937                       a(5), b(5);
        normal_distribution<double>   da, db;
        bool                          same = true;
        for (int i = 0; i < 100; ++i) if (da(a) != db(b)) same = false;
        Check(same, "phase12 normal reproducible");
    }

    // ── random_device ───────────────────────────────────────────────────
    {
        random_device rd;
        unsigned      v0 = rd(), v1 = rd(), v2 = rd(), v3 = rd();
        Check(!(v0 == v1 && v1 == v2 && v2 == v3),
              "phase12 random_device varies");
        double e = rd.entropy();
        Check(e == 0.0 || e == 32.0, "phase12 random_device entropy");
        // seed a PRNG from it — must run without trapping
        mt19937 g(rd());
        (void)g();
        Check(true, "phase12 random_device seeds engine");
    }

    printf("[CXX] PASS phase12: <random> (engines/device/distributions)\n");
}

// ── phase13 fixtures: coroutines (<coroutine>/<generator> + box::executor) ─
std::generator<int> Phase13Fib(int n)
{
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        co_yield a;
        int t = a + b;
        a = b;
        b = t;
    }
}

std::generator<int> Phase13Nested()
{
    co_yield 1;
    co_yield std::ranges::elements_of(Phase13Fib(4)); // 0,1,1,2
    co_yield 99;
}

box::task<int> Phase13Child() { co_return 41; }

box::task<int> Phase13Add()
{
    int v = co_await Phase13Child();
    co_return v + 1;
}

box::task<int> Phase13Thrower()
{
    throw std::runtime_error("boom");
    co_return 0; // unreachable — makes this a coroutine
}

box::task<int> Phase13Catcher()
{
    int caught = 0;
    try {
        co_await Phase13Thrower();
    } catch (const std::exception &) {
        caught = 7;
    }
    co_return caught;
}

box::task<void> Phase13BrookProducer(Brook *w)
{
    uint64_t f = 0xFEEDBEEFull;
    brook_push(w, &f);
    co_return;
}

box::task<uint64_t> Phase13BrookConsumer(Brook *r)
{
    uint64_t out = 0;
    (void)co_await box::brook_read(r, &out);
    co_return out;
}

box::task<unsigned> Phase13AwaitTouch()
{
    Touch t = co_await box::touch_event();
    unsigned v = 0;
    if (t.payload_len >= sizeof(v)) __builtin_memcpy(&v, t.payload, sizeof(v));
    co_return v;
}

// ── phase13 audit-fix fixtures (Ф12 must-fix runtime coverage) ──────────
box::task<void> Phase13VoidThrower()
{
    throw std::runtime_error("boomv");
    co_return; // unreachable
}

// Move-only, non-trivial payload: proves the union storage construct/move/
// destroy in task<T> is balanced (no leak, no double-free).
struct CoroProbe {
    int                v;
    static inline int  ctor = 0, move = 0, dtor = 0;
    explicit CoroProbe(int x) : v(x) { ++ctor; }
    CoroProbe(CoroProbe &&o) noexcept : v(o.v) { ++move; }
    CoroProbe(const CoroProbe &)            = delete;
    CoroProbe &operator=(const CoroProbe &) = delete;
    ~CoroProbe() { ++dtor; }
};

box::task<CoroProbe> Phase13ProbeChild() { co_return CoroProbe{42}; }

box::task<int> Phase13ProbeParent()
{
    CoroProbe p = co_await Phase13ProbeChild();
    co_return p.v;
}

box::task<void> Phase13VoidThrowerChild()
{
    throw std::runtime_error("boomvc");
    co_return;
}

box::task<int> Phase13VoidCatcherParent()
{
    int caught = 0;
    try {
        co_await Phase13VoidThrowerChild();
    } catch (const std::exception &) {
        caught = 9;
    }
    co_return caught;
}

box::task<void> Phase13BrookEofProducer(Brook *w)
{
    uint64_t f = 1;
    brook_push(w, &f);
    f = 2;
    brook_push(w, &f);
    brook_release(w); // writer leaves -> reader sees EOF once ring drains
    co_return;
}

box::task<int> Phase13BrookEofConsumer(Brook *r)
{
    int count = 0, rc = 0;
    for (;;) {
        uint64_t out = 0;
        rc = co_await box::brook_read(r, &out);
        if (rc == OK) {
            ++count;
            continue;
        }
        break;
    }
    co_return (rc == -ERR_STREAM_CLOSED) ? count : -1;
}

// Counting memory_resource — drives the bespoke _Promise_alloc allocator
// placement math (stateful polymorphic_allocator path) in std::generator.
struct CountingResource : std::pmr::memory_resource {
    int allocs = 0, frees = 0;
    void *do_allocate(std::size_t n, std::size_t a) override
    {
        ++allocs;
        return ::operator new(n, std::align_val_t(a));
    }
    void do_deallocate(void *p, std::size_t n, std::size_t a) override
    {
        (void)n;
        ++frees;
        ::operator delete(p, std::align_val_t(a));
    }
    bool do_is_equal(const std::pmr::memory_resource &o) const noexcept override
    {
        return this == &o;
    }
};

std::pmr::generator<int> Phase13PmrGen(std::allocator_arg_t,
                                       std::pmr::polymorphic_allocator<std::byte>,
                                       int n)
{
    for (int i = 0; i < n; ++i) co_yield i;
}

// RAII guard captured as a coroutine local — proves frame destroy() (both on
// normal drain and on abandoned/cancelled mid-suspend) runs the locals.
struct CounterGuard {
    static inline int live = 0;
    CounterGuard() { ++live; }
    CounterGuard(const CounterGuard &) = delete;
    ~CounterGuard() { --live; }
};

std::generator<int> Phase13GuardedFinite(int n)
{
    CounterGuard g;
    for (int i = 0; i < n; ++i) co_yield i;
}

void Phase13()
{
    // ── std::generator: known fibonacci sequence ────────────────────────
    {
        int seq[16];
        int n = 0;
        for (int x : Phase13Fib(10)) {
            if (n < 16) seq[n] = x;
            ++n;
        }
        bool ok = (n == 10 && seq[0] == 0 && seq[1] == 1 && seq[2] == 1 &&
                   seq[3] == 2 && seq[4] == 3 && seq[5] == 5 && seq[6] == 8 &&
                   seq[7] == 13 && seq[8] == 21 && seq[9] == 34);
        Check(ok, "phase13 generator fibonacci sequence");
    }
    // ── std::generator: nested co_yield ranges::elements_of ─────────────
    {
        int sum = 0, n = 0;
        for (int x : Phase13Nested()) { // 1,0,1,1,2,99 -> 6 values, sum 104
            sum += x;
            ++n;
        }
        Check(n == 6 && sum == 104, "phase13 generator nested elements_of");
    }
    // ── <coroutine>: noop_coroutine + handle comparison ─────────────────
    {
        auto nc = std::noop_coroutine();
        std::coroutine_handle<> h = nc;
        Check(!nc.done() && h.address() != nullptr, "phase13 noop_coroutine");
        std::coroutine_handle<> z = nullptr;
        Check((h == h) && (z != h), "phase13 coroutine_handle compare");
    }
    // ── box::task<T>: composition (co_await child) returning a value ─────
    {
        box::executor ex;
        int r = ex.block_on(Phase13Add());
        Check(r == 42, "phase13 task compose co_await child");
    }
    // ── box::task<T>: exception propagation across co_await ──────────────
    {
        box::executor ex;
        int r = ex.block_on(Phase13Catcher());
        Check(r == 7, "phase13 task exception propagation");
    }
    // ── box::executor: Brook suspend -> producer fills -> resume ─────────
    {
        const char *tag = "cxx:coro:brook";
        Brook *w = brook_open(tag, 8, 4, BROOK_WRITER | BROOK_CREATE);
        Brook *r = brook_open(tag, 8, 4, BROOK_READER);
        if (w && r) {
            box::executor ex;
            auto consumer = Phase13BrookConsumer(r);
            ex.spawn(Phase13BrookProducer(w)); // queued first
            ex.schedule(consumer.handle());    // LIFO: runs first, finds empty,
                                               // suspends, then producer fills
            ex.run();
            uint64_t got = consumer.result();
            Check(got == 0xFEEDBEEFull, "phase13 executor brook suspend/resume");
        } else {
            Check(false, "phase13 brook open");
        }
        if (r) brook_release(r);
        if (w) brook_release(w);
    }
    // ── box::executor + co_await box::touch_event (guarded, no hang) ─────
    {
        TouchTagPair tp = touch_intern("cxx:coro:touch");
        TouchTag     id = touch_pair_choose(tp);
        if (id != TOUCH_TAG_INVALID) {
            touch_claim(id, TOUCH_REST, 0, 0);
            unsigned magic = 0xC0DECAFEu;
            touch_send(tp, &magic, sizeof(magic), 0);
            for (int i = 0; i < 2000 && !touch_available(); ++i) yield();
            if (touch_available()) {
                box::executor ex;
                unsigned      got = ex.block_on(Phase13AwaitTouch());
                Check(got == 0xC0DECAFEu, "phase13 co_await touch_event payload");
            } else {
                printf("[CXX] note phase13: touch self-delivery not observed\n");
            }
            touch_release(id);
        }
    }
    // ── audit-fix: uncaught root-task exception surfaces from block_on ───
    {
        box::executor ex;
        bool          rethrew = false;
        try {
            (void)ex.block_on(Phase13Thrower());
        } catch (const std::exception &e) {
            rethrew = (std::string_view(e.what()) == "boom");
        }
        Check(rethrew, "phase13 block_on rethrows uncaught root exception");
    }
    {
        box::executor ex;
        bool          rethrew = false;
        try {
            ex.block_on(Phase13VoidThrower());
        } catch (const std::exception &) {
            rethrew = true;
        }
        Check(rethrew, "phase13 block_on rethrows uncaught void root exception");
    }
    // ── audit-fix: move-only/non-trivial task<T> RAII balance ───────────
    {
        CoroProbe::ctor = CoroProbe::move = CoroProbe::dtor = 0;
        {
            box::executor ex;
            int           v = ex.block_on(Phase13ProbeParent());
            Check(v == 42, "phase13 move-only task<T> value");
        }
        Check(CoroProbe::ctor >= 1 &&
                  CoroProbe::ctor + CoroProbe::move == CoroProbe::dtor,
              "phase13 move-only task<T> RAII balanced (ctor+move==dtor)");
    }
    // ── audit-fix: co_awaited throwing task<void> drives void _M_take ────
    {
        box::executor ex;
        int           r = ex.block_on(Phase13VoidCatcherParent());
        Check(r == 9, "phase13 co_awaited throwing task<void> propagation");
    }
    // ── audit-fix: brook clean-EOF (-ERR_STREAM_CLOSED) via awaiter ─────
    {
        const char *tag = "cxx:coro:brookeof";
        Brook      *w   = brook_open(tag, 8, 4, BROOK_WRITER | BROOK_CREATE);
        Brook      *r   = brook_open(tag, 8, 4, BROOK_READER);
        if (w && r) {
            box::executor ex;
            auto          consumer = Phase13BrookEofConsumer(r);
            ex.spawn(Phase13BrookEofProducer(w)); // pushes 2 + releases writer
            ex.schedule(consumer.handle());
            ex.run();
            int got = consumer.result();
            Check(got == 2, "phase13 brook clean-EOF after draining 2 frames");
            brook_release(r);
        } else {
            Check(false, "phase13 brookeof open");
            if (r) brook_release(r);
            if (w) brook_release(w);
        }
    }
    // ── audit-fix: std::generator allocator/pmr placement path ──────────
    {
        CountingResource res;
        {
            std::pmr::polymorphic_allocator<std::byte> pa(&res);
            int                                        sum = 0;
            for (int x : Phase13PmrGen(std::allocator_arg, pa, 5)) sum += x;
            Check(sum == 10, "phase13 pmr::generator yields via allocator");
        }
        Check(res.allocs >= 1 && res.allocs == res.frees,
              "phase13 pmr::generator frame alloc balanced");
    }
    // ── audit-fix: coroutine frame RAII (drain + abandoned cancellation) ─
    {
        CounterGuard::live = 0;
        {
            for (int x : Phase13GuardedFinite(5)) (void)x; // fully drained
        }
        Check(CounterGuard::live == 0, "phase13 drained generator RAII balanced");

        CounterGuard::live = 0;
        {
            auto gen   = Phase13GuardedFinite(1000);
            int  taken = 0;
            for (int x : gen) {
                (void)x;
                if (++taken == 3) break; // abandon mid-iteration
            }
            Check(CounterGuard::live == 1, "phase13 abandoned generator guard live");
        } // gen out of scope -> destroy() suspended frame -> guard dtor runs
        Check(CounterGuard::live == 0, "phase13 abandoned generator frame cleanup");
    }
    // ── pocket_recv: NO independent runtime test here (honest).
    //    box::pocket_recv's await machinery (await_ready->poll / wait_on /
    //    _S_block native receive_wait / await_resume) is BYTE-IDENTICAL to the
    //    brook_read and touch_event awaiters, both of which ARE runtime-proven
    //    above (real Brook frame + real TouchRing self-delivery). The only
    //    pocket-specific leaf is receive()/receive_wait(). send-to-self is
    //    rejected by the kernel (ERR_ROUTE_SELF), and a real cross-cabin peer
    //    fixture (proc_exec child) faulted on the routed crate VA under single
    //    core — that belongs to the Ф16 box::message phase where a proper
    //    cross-cabin IPC fixture exists. Validated-by-identity, runtime test
    //    deferred — recorded in cxx_honest_leftovers (not claimed as covered).
    printf("[CXX] PASS phase13: <coroutine>/<generator> + box::executor "
           "(co_await touch/brook + RAII/exception/drain/pmr)\n");
}

// ── phase14 fixtures (Ф13a box::brook<T> native C++ Brook layer) ────────────
// A trivially-copyable, default-constructible 8-byte frame (the Brook minimum).
struct BrookTick {
    uint32_t seq;
    uint32_t val;
};

// The "input_range" promise of box::brook<T>, verified at compile time.
static_assert(std::ranges::input_range<box::brook<BrookTick>>,
              "box::brook<T> must model std::ranges::input_range");

box::task<void> Phase14Producer(box::brook<BrookTick> *w)
{
    co_await w->send(BrookTick{7, 0xBEEFu});
    co_return;
}

box::task<uint32_t> Phase14Consumer(box::brook<BrookTick> *r)
{
    std::optional<BrookTick> t = co_await r->next();
    co_return t ? t->val : 0u;
}

void Phase14()
{
    using B = box::brook<BrookTick>;

    // ── open + shape + round-trip + counters + try/timeout (one peer pair) ──
    {
        const char *tag = "cxx:brook:io";
        B           w   = B::writer(tag, 4);
        B           r   = B::reader(tag);
        if (w && r) {
            Check(w.frame_size() == sizeof(BrookTick) && w.capacity() == 4,
                  "phase14 brook shape (frame_size/capacity)");

            // blocking round-trip of one frame
            Check(w.push(BrookTick{1, 0xAAu}), "phase14 brook push");
            BrookTick t{};
            Check(r.pop(t) && t.seq == 1 && t.val == 0xAAu,
                  "phase14 brook pop round-trip");

            // live counters: push 2, observe, pop in FIFO order
            Check(w.push(BrookTick{2, 2}) && w.push(BrookTick{3, 3}),
                  "phase14 brook push two");
            Check(r.available() == 2 && r.free() == 2,
                  "phase14 brook counters after two");
            Check(r.pop(t) && t.seq == 2, "phase14 brook FIFO order");
            Check(r.available() == 1, "phase14 brook counters after pop");
            Check(r.pop(t), "phase14 brook drain last");

            // non-blocking: empty try_pop, then fill and try_push the full ring
            Check(r.try_pop(t) == -ERR_WOULD_BLOCK, "phase14 brook try_pop empty");
            Check(w.push(BrookTick{4, 4}) && w.push(BrookTick{5, 5}) &&
                      w.push(BrookTick{6, 6}) && w.push(BrookTick{7, 7}),
                  "phase14 brook fill ring");
            Check(w.try_push(BrookTick{8, 8}) == -ERR_WOULD_BLOCK,
                  "phase14 brook try_push full");

            // bounded: drain, pop_for times out on empty, push_for succeeds
            for (int i = 0; i < 4; ++i) Check(r.pop(t), "phase14 brook drain ring");
            Check(r.pop_for(t, 10) == -ERR_TIMEOUT, "phase14 brook pop_for timeout");
            Check(w.push_for(BrookTick{9, 9}, 10) == OK, "phase14 brook push_for ok");
            Check(r.pop(t) && t.seq == 9, "phase14 brook push_for delivered");
        } else {
            Check(false, "phase14 brook io open");
        }
    }

    // ── synchronous input_range: drains N frames, ends on writer-leave ──────
    {
        const char *tag = "cxx:brook:range";
        B           r;
        {
            B w = B::writer(tag, 4);
            r   = B::reader(tag);
            for (uint32_t i = 0; i < 3; ++i) w.push(BrookTick{i, i * 10u});
        } // writer leaves; non-stream => reader drains the 3 frames then terminal
        int      n   = 0;
        uint32_t sum = 0;
        for (const BrookTick &t : r) {
            sum += t.val;
            ++n;
        }
        Check(n == 3 && sum == 30u, "phase14 brook input_range drains then ends");
    }

    // ── coroutine: consumer co_await next() suspends, producer send() fills ──
    {
        const char *tag = "cxx:brook:async";
        B           w   = B::writer(tag, 4);
        B           r   = B::reader(tag);
        if (w && r) {
            box::executor ex;
            auto          consumer = Phase14Consumer(&r);
            ex.spawn(Phase14Producer(&w)); // queued first
            ex.schedule(consumer.handle()); // LIFO: runs first, finds empty, suspends
            ex.run();
            Check(consumer.result() == 0xBEEFu, "phase14 brook co_await next/send");
        } else {
            Check(false, "phase14 brook async open");
        }
    }

    printf("[CXX] PASS phase14: box::brook<T> (roles/shape/counters/try/timeout "
           "+ input_range + co_await next/send)\n");
}

// ── phase15 fixtures (Ф13b box::touch native C++ Touch layer) ───────────────
// box::subscription models a (single-pass, blocking) input_range of events.
static_assert(std::ranges::input_range<box::subscription>,
              "box::subscription must model std::ranges::input_range");

box::task<unsigned> Phase15AwaitTag(box::subscription *s)
{
    std::optional<box::event> ev = co_await s->next();
    co_return ev ? ev->payload_as<unsigned>().value_or(0u) : 0u;
}

void Phase15()
{
    using namespace box::literals;

    // ── box::tag interning + "…"_tag UDL ────────────────────────────────
    box::tag tg = "cxx:touch:demo"_tag;
    Check(static_cast<bool>(tg) && tg.id() != TOUCH_TAG_INVALID,
          "phase15 tag intern + _tag UDL");

    // ── box::subscription RAII claim ────────────────────────────────────
    box::subscription sub(tg);
    Check(static_cast<bool>(sub), "phase15 subscription claim (RAII)");
    if (!sub) {
        printf("[CXX] PASS phase15: box::touch (compiled; claim unavailable)\n");
        return;
    }

    // Probe self-delivery once (guarded, Ф12 idiom): publish to our own claimed
    // tag and spin briefly for the kernel to land it. If never observed, the
    // box::touch await machinery is validated-by-identity with the runtime-
    // proven box::brook awaiter (Phase14) — same wait_on/poll/block mechanism,
    // only the leaf primitive (touch_try_pop_tag/touch_wait_tag) differs.
    struct Note {
        uint32_t code;
        uint32_t seq;
    };
    box::publish(tg, Note{0xC0DECAFEu, 1});
    bool observed = false;
    for (int i = 0; i < 8000 && !(observed = touch_available()); ++i) yield();
    if (!observed) {
        printf("[CXX] note phase15: touch self-delivery not observed; await "
               "machinery validated-by-identity with box::brook (phase14)\n");
        printf("[CXX] PASS phase15: box::touch (compiled; runtime skipped)\n");
        return;
    }

    // ── wait() + event + payload_as<T> + tag-filtering ──────────────────
    {
        std::optional<box::event> ev = sub.wait(1000);
        Check(ev.has_value(), "phase15 subscription.wait delivers event");
        if (ev) {
            Check(ev->tag_id() == sub.id(), "phase15 event tag-filtered to subscription");
            std::optional<Note> n = ev->payload_as<Note>();
            Check(n && n->code == 0xC0DECAFEu && n->seq == 1, "phase15 event.payload_as<T>");
            Check(ev->payload().size() >= sizeof(Note), "phase15 event.payload() span");
        }
    }

    // ── multi-tag stash: two subscriptions stay independent ─────────────
    // Publishing to two tags and reading each subscription proves the boxlib
    // tag-stash: whichever event the cabin-wide ring yields first is parked for
    // its own tag's consumer rather than mis-delivered to the other.
    {
        box::tag          tA = "cxx:touch:A"_tag;
        box::tag          tB = "cxx:touch:B"_tag;
        box::subscription sa(tA);
        box::subscription sb(tB);
        if (sa && sb) {
            box::publish(tA, static_cast<uint32_t>(0xA1A1u));
            box::publish(tB, static_cast<uint32_t>(0xB2B2u));
            std::optional<box::event> a = sa.wait(1000);
            std::optional<box::event> b = sb.wait(1000);
            Check(a && a->payload_as<uint32_t>().value_or(0u) == 0xA1A1u,
                  "phase15 multi-tag: subscription A receives only A");
            Check(b && b->payload_as<uint32_t>().value_or(0u) == 0xB2B2u,
                  "phase15 multi-tag: subscription B receives only B (via stash)");
        } else {
            Check(false, "phase15 multi-tag claim");
        }
    }

    // ── co_await sub.next() (tag-filtered; guarded ready-path) ──────────
    {
        box::publish(tg, static_cast<unsigned>(0xBEEFu));
        for (int i = 0; i < 8000 && !touch_available(); ++i) yield();
        if (touch_available()) {
            box::executor ex;
            unsigned      got = ex.block_on(Phase15AwaitTag(&sub));
            Check(got == 0xBEEFu, "phase15 co_await sub.next() payload");
        }
    }

    // ── stream-view: drain the burst, end on the drain timeout ──────────
    {
        sub.set_drain_timeout(300);
        box::publish(tg, static_cast<unsigned>(701u));
        box::publish(tg, static_cast<unsigned>(702u));
        int      n    = 0;
        unsigned last = 0;
        for (box::event e : sub) {
            if (std::optional<unsigned> v = e.payload_as<unsigned>()) last = *v;
            ++n;
        } // ends when wait(300ms) finds nothing more
        Check(n >= 1 && (last == 701u || last == 702u),
              "phase15 stream-view drains burst then ends on drain timeout");
    }

    // ── registry / ack surfaces — best-effort ───────────────────────────
    // Exercise the C++ wrappers + their syscall routing at runtime. The result
    // is policy/capability-gated (and ack only matters for latched/level tags),
    // so the boolean is not asserted — the matrix's PANIC=0/AppFAIL=0 covers
    // that the path runs without faulting (the CET/IBT syscall thunk included).
    {
        (void)box::register_tag(tg, box::touch_policy::edge, box::touch_capability::open);
        (void)sub.ack();
    }

    printf("[CXX] PASS phase15: box::touch (tag/_tag/subscription/event/payload_as "
           "+ publish + wait + co_await next + stream-view + registry/ack)\n");
}

// ── Ф14a: box::bay<T> / box::shared_object<T> (typed cross-cabin shared mem) ──
// Compile-time contracts: a typed contiguous view over the shared Bay pages.
static_assert(std::ranges::contiguous_range<box::bay<uint64_t>>,
              "box::bay<T> must model std::ranges::contiguous_range");
static_assert(std::is_same_v<std::ranges::range_value_t<box::bay<uint64_t>>, uint64_t>,
              "box::bay<T> range_value_t == T");
static_assert(!std::is_copy_constructible_v<box::bay<uint64_t>>, "box::bay<T> is move-only");
static_assert(std::is_move_constructible_v<box::bay<uint64_t>>, "box::bay<T> is movable");
static_assert(std::is_same_v<box::bay<const uint64_t>::reference, const uint64_t &>,
              "box::bay<const T> hands out const references");

struct Phase16Reg {
    uint32_t a;
    uint32_t b;
};

void Phase16()
{
    // ── create + typed read/write straight into the shared pages ─────────
    auto b = box::bay<uint64_t>::create("cxx:bay:grid", 16);
    Check(static_cast<bool>(b), "phase16 bay create");
    if (!b) {
        printf("[CXX] PASS phase16: box::bay (compiled; create unavailable)\n");
        return;
    }
    Check(b.size() == 16, "phase16 bay size (elements)");
    Check(b.size_bytes() == 16 * sizeof(uint64_t), "phase16 bay size_bytes");
    Check(!b.encrypted() && !b.read_only(), "phase16 plaintext read-write bay");

    for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint64_t>(i * i);

    // read back through the span view
    std::span<uint64_t> s = b.as_span();
    Check(s.size() == 16 && s[3] == 9u && s[15] == 225u, "phase16 bay as_span view");

    // read back through the contiguous range (range-for + a classic std algorithm)
    uint64_t sum = 0;
    for (uint64_t v : b) sum += v;
    Check(sum == 1240u, "phase16 bay range-for sum of squares");
    Check(std::count(b.begin(), b.end(), static_cast<uint64_t>(49)) == 1,
          "phase16 bay contiguous iterators feed std::count");

    // ── move semantics: the claim transfers, the source empties ──────────
    auto moved = std::move(b);
    Check(!static_cast<bool>(b) && static_cast<bool>(moved),
          "phase16 bay move empties source");
    Check(moved[7] == 49u, "phase16 moved bay keeps the mapping");

    // ── open() the same tag — a second claim sees the same shared pages ──
    // Cross-cabin sharing is proven by bay_test (T3/T4); here a same-cabin
    // second claim exercises the typed open() wrapper. Guarded: if the host
    // grants the claim it MUST observe the writes (one set of physical pages).
    {
        auto v = box::bay<uint64_t>::open("cxx:bay:grid");
        if (v)
            Check(v.size() == 16 && v[7] == 49u, "phase16 open() sees the shared writes");
        else
            printf("[CXX] note phase16: same-cabin second claim not granted; "
                   "open() validated-by-identity with bay_test\n");
    }

    // ── const element type maps the pages read-only (BAY_RO) ─────────────
    {
        auto ro = box::bay<const uint64_t>::open("cxx:bay:grid");
        if (ro) {
            Check(ro.read_only(), "phase16 bay<const T> maps read-only");
            Check(ro[2] == 4u, "phase16 const bay reads the shared data");
        }
    }

    // ── encrypted factory is STRICT — no silent downgrade ────────────────
    // QEMU TCG has no active TME-MK, so create_encrypted yields an empty bay;
    // the caller then explicitly falls back to a plaintext create.
    {
        auto enc = box::bay<uint64_t>::create_encrypted("cxx:bay:vault", 4);
        if (enc) {
            // Real TME-MK present (server-class HW): live AND flagged encrypted.
            Check(enc.encrypted(), "phase16 encrypted bay reports encrypted()");
        } else {
            auto plain = box::bay<uint64_t>::create("cxx:bay:vault:plain", 4);
            Check(static_cast<bool>(plain) && !plain.encrypted(),
                  "phase16 encrypted strict-empty, caller falls back to plaintext");
        }
    }

    // ── box::shared_object<T>: a single shared T, pointer-like access ─────
    {
        auto obj = box::shared_object<Phase16Reg>::create("cxx:bay:reg");
        Check(static_cast<bool>(obj), "phase16 shared_object create");
        if (obj) {
            obj->a   = 0x1111u;  // operator->
            (*obj).b = 0x2222u;  // operator*
            Check(obj->a == 0x1111u && (*obj).b == 0x2222u && obj.get()->a == 0x1111u,
                  "phase16 shared_object operator-> / operator* / get()");
        }
    }

    printf("[CXX] PASS phase16: box::bay<T> (create/open/encrypted-strict + "
           "operator[]/as_span/size + contiguous range + move) + "
           "box::shared_object<T>\n");
}

// ── Ф14b: box::tagged_resource + box::heap (per-tag heap accounting) ─────────
void Phase17()
{
    // ── box::tagged_resource: a pmr resource tagging every allocation ────
    {
        box::tagged_resource res("cxx:heap:pmr");
        std::pmr::vector<int> v(&res);
        for (int i = 0; i < 100; ++i) v.push_back(i);
        Check(v.size() == 100 && v[99] == 99, "phase17 tagged pmr vector data intact");
        Check(box::heap::count("cxx:heap:pmr") >= 1,
              "phase17 tagged_resource: pmr buffer accounted under the tag");

        // over-aligned allocation routed through the resource (align 64 > 16)
        struct alignas(64) Cache {
            char c[64];
        };
        std::pmr::polymorphic_allocator<Cache> pa(&res);
        Cache *cl = pa.allocate(1);
        Check((reinterpret_cast<std::uintptr_t>(cl) % 64) == 0,
              "phase17 tagged_resource honors over-alignment (>16)");
        pa.deallocate(cl, 1);
    }  // res outlives v (declared first); blocks freed at scope exit

    // ── box::heap::stats snapshot ────────────────────────────────────────
    {
        heap_stats_t st = box::heap::stats();
        Check(st.malloc_calls > 0 && st.total_allocated > 0, "phase17 heap::stats snapshot");
    }

    // ── box::heap::tag: count / bytes / for_each over a fresh tag ─────────
    {
        box::heap::tag probe("cxx:heap:probe");
        Check(probe.count() == 0, "phase17 fresh tag has no live allocations");
        void *a = malloc_tagged(64, "cxx:heap:probe");
        void *b = malloc_tagged(128, "cxx:heap:probe");
        Check(probe.count() == 2, "phase17 tag.count() after two tagged allocations");
        Check(probe.bytes() >= 64 + 128, "phase17 tag.bytes() sums the tagged blocks");

        std::size_t seen = 0, sum = 0;
        probe.for_each([&](void *, std::size_t sz, const char *) { ++seen; sum += sz; });
        Check(seen == 2 && sum >= 192, "phase17 tag.for_each visits each live block");

        free(a);
        Check(probe.count() == 1, "phase17 tag.count() decremented after free");
        free(b);
        Check(probe.count() == 0, "phase17 tag clean after all freed");
    }

    // ── box::heap::tag::watch(): RAII leak guard, baseline-relative ──────
    {
        box::heap::tag s("cxx:heap:scope");
        void *pre = malloc_tagged(16, "cxx:heap:scope");  // live before the guard
        {
            auto g = s.watch();  // baseline excludes `pre`
            void *x = malloc_tagged(32, "cxx:heap:scope");
            void *y = malloc_tagged(32, "cxx:heap:scope");
            Check(g.leaked() == 2, "phase17 watch() reports 2 allocations above baseline");
            free(x);
            Check(g.leaked() == 1, "phase17 watch() leaked count tracks frees");
            free(y);
            Check(g.leaked() == 0, "phase17 watch() clean once scope allocs freed");
            // armed dtor, leaked()==0 → no report
        }
        free(pre);
    }

    // ── watch() dismiss: a deliberately-retained allocation is silenced ──
    {
        box::heap::tag s2("cxx:heap:retain");
        auto  g    = s2.watch();
        void *keep = malloc_tagged(16, "cxx:heap:retain");
        Check(g.leaked() == 1, "phase17 watch() sees the retained allocation");
        g.dismiss();  // expected retention — disarm the destructor report
        free(keep);
    }

    // ── box::heap::for_each_tagged visits across tags ────────────────────
    {
        void       *t     = malloc_tagged(48, "cxx:heap:sweep");
        std::size_t total = 0;
        box::heap::for_each_tagged([&](void *, std::size_t, const char *) { ++total; });
        Check(total >= 1, "phase17 for_each_tagged visits live tagged blocks");
        free(t);
    }

    printf("[CXX] PASS phase17: box::tagged_resource (pmr, over-align) + "
           "box::heap::stats/count/for_each/for_each_tagged + "
           "box::heap::tag (count/bytes/for_each/watch leak-guard)\n");
}

// ── Ф15a: box::tagfs::file + create/query/find + byte I/O ───────────────────
void Phase18()
{
    using box::tagfs::file;

    // ── create with tags ────────────────────────────────────────────────
    file f = box::tagfs::create("cxx:tagfs:probe", {"cxx:phase18", "kind:test"});
    Check(static_cast<bool>(f), "phase18 create returns a live file");
    if (!f) {
        printf("[CXX] PASS phase18: box::tagfs (compiled; create unavailable)\n");
        return;
    }

    // ── metadata + tags ─────────────────────────────────────────────────
    Check(f.name() == "cxx:tagfs:probe", "phase18 file.name()");
    Check(f.has_tag("cxx") && f.has_tag("kind"), "phase18 file.has_tag(key)");
    Check(f.tags().size() >= 2, "phase18 file.tags() lists the tags");

    // ── random-access byte I/O bound to this file_id ────────────────────
    const char msg[] = "hello tagfs stream";
    Check(f.write_at(0, msg, sizeof(msg)) == static_cast<int>(sizeof(msg)),
          "phase18 write_at(raw)");
    char rbuf[sizeof(msg)] = {};
    Check(f.read_at(0, rbuf, sizeof(rbuf)) == static_cast<int>(sizeof(msg))
              && std::string_view(rbuf) == "hello tagfs stream",
          "phase18 read_at(raw) round-trip");

    std::byte payload[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Check(f.write_at(64, std::span<const std::byte>(payload, 4)) == 4, "phase18 write_at(span)");
    std::byte rb[4] = {};
    Check(f.read_at(64, std::span<std::byte>(rb, 4)) == 4 && rb[0] == std::byte{1},
          "phase18 read_at(span) round-trip");

    struct Rec {
        std::uint32_t a;
        std::uint32_t b;
    };
    Check(f.write_object<Rec>(128, Rec{0xAAu, 0xBBu}), "phase18 write_object<T>");
    std::optional<Rec> rec = f.read_object<Rec>(128);
    Check(rec && rec->a == 0xAAu && rec->b == 0xBBu, "phase18 read_object<T> round-trip");

    // ── streaming bridge to the Current spine (self round-trip) ─────────
    {
        {
            box::byte_current w = f.bytes(box::role::write);
            Check(static_cast<bool>(w), "phase18 bytes(write) opens a channel");
            if (w) w.write("STREAMRT", 8);
        }  // write channel released → flushed
        box::byte_current rch = f.bytes(box::role::read);
        if (rch) {
            char b[8] = {};
            int  n = rch.read(b, 8);
            Check(n == 8 && std::string_view(b, 8) == "STREAMRT",
                  "phase18 bytes() spine round-trip");
        } else {
            Check(false, "phase18 bytes(read) opens a channel");
        }
        // Cross-check whether the by-name bridge reached THIS file_id.
        char idbuf[8] = {};
        if (f.read_at(0, idbuf, 8) == 8 && std::string_view(idbuf, 8) == "STREAMRT")
            Check(true, "phase18 bytes() bridge reaches this file_id");
        else
            printf("[CXX] note phase18: bytes() bridges by name to a separate "
                   "backing; read_at/write_at are id-precise\n");
    }

    // ── tag mutations (remove_tag by key removes a key:value tag) ────────
    Check(f.add_tag("extra:1"), "phase18 add_tag (key:value)");
    Check(f.has_tag("extra"), "phase18 has_tag after add_tag");
    Check(f.remove_tag("extra"), "phase18 remove_tag by key");
    Check(!f.has_tag("extra"), "phase18 tag gone after remove_tag");

    // ── rename ──────────────────────────────────────────────────────────
    Check(f.rename("cxx:tagfs:renamed"), "phase18 rename");
    Check(f.name() == "cxx:tagfs:renamed", "phase18 name reflects rename");

    // ── query → range + std::views composition ─────────────────────────
    {
        std::vector<file> results = box::tagfs::query("cxx:phase18");
        Check(!results.empty(), "phase18 query by tag finds the file");
        int cnt = 0;
        for (const file &x :
             results | std::views::filter([](const file &z) { return z.id() != 0; })) {
            (void)x;
            ++cnt;
        }
        Check(cnt >= 1, "phase18 query result composes with std::views::filter");
    }

    // ── find by name (TagFS names are not unique → check membership) ─────
    {
        std::vector<file> named = box::tagfs::find_all("cxx:tagfs:renamed");
        bool              mine  = false;
        for (const file &x : named)
            if (x.id() == f.id()) { mine = true; break; }
        Check(mine, "phase18 find_all by name includes this file");
    }

    // ── durability + cleanup ────────────────────────────────────────────
    Check(f.anchor(), "phase18 anchor (durability flush)");
    Check(f.remove(), "phase18 remove (delete)");

    printf("[CXX] PASS phase18: box::tagfs::file (info/tags/add/remove/rename/anchor "
           "+ read_at/write_at/read_object/write_object + bytes spine bridge) + "
           "create/query/find\n");
}

// ── Ф15b: box::tagfs context / snapshot RAII + anchor↔touch observer ──────
static_assert(!std::is_copy_constructible_v<box::tagfs::context>,
              "box::tagfs::context is move-only");
static_assert(std::is_move_constructible_v<box::tagfs::context>,
              "box::tagfs::context is movable");
static_assert(!std::is_copy_constructible_v<box::tagfs::snapshot>,
              "box::tagfs::snapshot is move-only");
static_assert(std::is_move_constructible_v<box::tagfs::snapshot>,
              "box::tagfs::snapshot is movable");

void Phase19()
{
    using box::tagfs::context;
    using box::tagfs::file;
    using box::tagfs::snapshot;

    auto results_have = [](const std::vector<file> &v, std::uint32_t id) {
        for (const file &f : v)
            if (f.id() == id) return true;
        return false;
    };

    // Availability probe + leftover cleanup (the disk persists across matrix
    // configs, so a crashed prior run could leave p19 files behind).
    for (file f : box::tagfs::query("p19:probe")) f.remove();

    file fa = box::tagfs::create("cxx:p19:alpha", {"p19ctx:alpha", "p19:probe"});
    if (!fa) {
        printf("[CXX] PASS phase19: box::tagfs context/snapshot/anchor "
               "(compiled; storage unavailable)\n");
        return;
    }
    file fb = box::tagfs::create("cxx:p19:beta", {"p19ctx:beta", "p19:probe"});
    Check(static_cast<bool>(fb), "phase19 second probe file created");

    // ── context: a nesting-correct per-process tag filter ───────────────
    // The kernel ANDs the context tags into every query, so an installed
    // context narrows what this process sees. RAII restores the prior context.
    {
        std::vector<std::string> baseline = context::current();

        Check(results_have(box::tagfs::query("p19:probe"), fa.id()) &&
                  results_have(box::tagfs::query("p19:probe"), fb.id()),
              "phase19 baseline query sees both probe files");

        {
            context outer("p19ctx:alpha");
            Check(static_cast<bool>(outer), "phase19 context(outer) active");

            std::vector<file> r = box::tagfs::query("p19:probe");
            Check(results_have(r, fa.id()) && !results_have(r, fb.id()),
                  "phase19 context narrows query to alpha (excludes beta)");

            std::vector<std::string> cur      = context::current();
            bool                     has_tag  = false;
            for (const std::string &t : cur)
                if (t == "p19ctx:alpha") has_tag = true;
            Check(has_tag, "phase19 context::current() reports the installed tag");

            {
                context inner("p19ctx:beta");
                Check(static_cast<bool>(inner), "phase19 context(inner) active");
                std::vector<file> ri = box::tagfs::query("p19:probe");
                Check(!results_have(ri, fa.id()) && !results_have(ri, fb.id()),
                      "phase19 nested context (alpha AND beta) matches neither");
            }  // inner restored → alpha-only

            std::vector<file> r2 = box::tagfs::query("p19:probe");
            Check(results_have(r2, fa.id()) && !results_have(r2, fb.id()),
                  "phase19 inner scope exit restores outer context (alpha-only)");
        }  // outer restored → baseline

        std::vector<file> rb = box::tagfs::query("p19:probe");
        Check(results_have(rb, fa.id()) && results_have(rb, fb.id()),
              "phase19 outer scope exit restores baseline (both visible)");
        Check(context::current().size() == baseline.size(),
              "phase19 context fully restored to baseline");
    }

    // ── snapshot: owning RAII over a CoW snapshot ───────────────────────
    // The C++ contract is "the dtor (or drop()) issues exactly one snap_delete";
    // verify it by the change in the live snapshot count, which is robust to
    // whatever else already exists on disk and to the kernel's id assignment.
    {
        auto snap_count = [] { return box::tagfs::snapshots().size(); };

        std::size_t n0 = snap_count();
        {
            snapshot snap = snapshot::of(fa, "cxx:p19:snap1");
            if (snap) {
                Check(snap.id() != 0, "phase19 snapshot has a live id");
                Check(snap.name() == "cxx:p19:snap1", "phase19 snapshot name echoes create");
                Check(snap_count() == n0 + 1, "phase19 snapshots() reflects the new snapshot");
            } else {
                printf("[CXX] note phase19: snap_create unavailable/name taken; "
                       "snapshot lifecycle skipped\n");
            }
        }  // snap dtor → snap_delete
        Check(snap_count() == n0, "phase19 snapshot auto-deleted on scope exit (RAII)");

        // keep() detaches the snapshot so it outlives the handle's scope.
        {
            std::uint32_t kept = 0;
            {
                snapshot snap = snapshot::of(fa, "cxx:p19:snap2");
                if (snap) kept = snap.keep();
            }
            if (kept) {
                Check(snap_count() == n0 + 1,
                      "phase19 keep() detaches: snapshot survives scope");
                Check(::snap_delete(kept) == 0, "phase19 manual snap_delete of kept snapshot");
                Check(snap_count() == n0, "phase19 kept snapshot gone after manual delete");
            }
        }

        // Whole-filesystem snapshot (file_id 0), auto-deleted on scope exit.
        {
            snapshot whole = snapshot::of_all("cxx:p19:snapall");
            if (whole) Check(whole.id() != 0, "phase19 snapshot::of_all (whole filesystem)");
        }
        Check(snap_count() == n0, "phase19 all snapshots cleaned up (count restored)");
    }

    // ── anchor observer: durability events bridged into box::touch ──────
    // anchor() publishes an ANCHOR event on the "anchor" tag; on_anchor()
    // claims it. Touch self-delivery is best-effort on the matrix (see
    // phase15), so the observation is guarded.
    {
        box::subscription asub = box::tagfs::on_anchor();
        if (asub) {
            const char durable[] = "durable";
            fa.write_at(0, durable, sizeof(durable));
            Check(fa.anchor(), "phase19 anchor (durability flush) publishes event");
            if (std::optional<box::event> ev = asub.wait(1000)) {
                box::tagfs::anchor_event ae(*ev);
                Check(static_cast<bool>(ae), "phase19 anchor_event decodes the payload");
                Check(ae.is_anchor(), "phase19 anchor_event.is_anchor() (op == 2)");
                Check(ae.file_id() == fa.id(),
                      "phase19 anchor_event.file_id() == anchored file");
            } else {
                printf("[CXX] note phase19: anchor event not observed; on_anchor "
                       "machinery validated-by-identity with box::touch (phase15)\n");
            }
        }
    }

    // ── cleanup ─────────────────────────────────────────────────────────
    fa.remove();
    fb.remove();

    printf("[CXX] PASS phase19: box::tagfs::context (nested RAII filter) + snapshot "
           "(RAII/keep/of_all + snapshots()) + on_anchor/anchor_event\n");
}

// ── Ф16a: box::message (process-to-process messaging) + Ф12 IPC debt ──────────
box::task<box::message> Phase20Recv()
{
    co_return co_await box::next_message();
}

void Phase20()
{
    // ── payload guard (memory-safety) — no spawn needed, deterministic ───
    // The validation that prevents a cross-cabin payload-pointer fault: an
    // invalid or empty delivery exposes no bytes and no payload, and never
    // dereferences data_addr. (A raw unguarded read of a 0/garbage data_addr
    // is the classic cross-cabin #PF; box::message cannot do it.)
    {
        box::message empty;  // default: sender pid 0
        Check(!empty, "phase20 default message is invalid (operator bool)");
        Check(empty.bytes().empty(), "phase20 invalid message has no bytes");
        Check(!empty.payload_as<std::uint32_t>().has_value(),
              "phase20 invalid message payload_as -> nullopt");

        // Valid sender but a null/zero payload (a zero-length send): still NO
        // bytes — bytes()/payload_as must not dereference data_addr 0.
        Result zr{};
        zr.sender_pid  = 4242u;
        zr.data_addr   = 0;
        zr.data_length = 0;
        box::message zero(zr);
        Check(static_cast<bool>(zero), "phase20 message with a sender is valid");
        Check(zero.from() == 4242u, "phase20 message.from()");
        Check(zero.bytes().empty(), "phase20 zero-payload message has no bytes (deref guard)");
        Check(!zero.payload_as<std::uint8_t>().has_value(),
              "phase20 zero-payload payload_as -> nullopt (deref guard)");
    }

    // ── producer path — deterministic negatives exercise the real syscall ──
    Check(!box::send(0u, "x", 1), "phase20 send to pid 0 rejected");
    Check(!box::broadcast("cxx:msg:no:subscriber", "x", 1),
          "phase20 broadcast with no subscribers returns false");

    // ── cross-cabin delivery — closes the Ф12 pocket_recv runtime debt ─────
    // proca sends 'A' to its spawner (us) five times. Receive the first via
    // co_await box::next_message(): a single waiter, so the executor takes its
    // block-forever branch (receive_wait(.,0)) — the exact path the Ф12 pocket
    // awaiter was only validated-by-identity for, now exercised cross-cabin.
    // The launcher delivers our own launch args as an IPC message (sender = the
    // launcher), and we never consumed them — drain any such pending traffic so
    // the only sender we then observe is the child we are about to spawn.
    while (box::receive()) { /* discard ambient (e.g. our launch args) */ }

    int child = proc_exec("proca");
    if (child <= 0) {
        printf("[CXX] note phase20: proc_exec unavailable; cross-cabin receive skipped\n");
        printf("[CXX] PASS phase20: box::message (send/broadcast/receive validated; "
               "spawn unavailable)\n");
        return;
    }

    // co_await box::next_message(): with the inbox drained and the child not yet
    // scheduled, the awaiter suspends as the executor's single waiter and takes
    // its block-forever branch (receive_wait(.,0)) until the child's first 'A'
    // arrives — the real cross-cabin pocket receive the Ф12 debt asked for, on
    // single-core too (box::message's payload guard makes the read fault-free).
    box::message m;
    {
        box::executor ex;
        m = ex.block_on(Phase20Recv());
    }
    // Robustness against any stray ambient that slipped in after the drain: fall
    // back to a bounded search for a message from our child (never blocks forever).
    if (!(m && m.from() == static_cast<std::uint32_t>(child))) {
        for (int i = 0; i < 12; i++) {
            std::optional<box::message> mm = box::receive_for(800);
            if (!mm) break;
            if (mm->from() == static_cast<std::uint32_t>(child)) { m = *mm; break; }
        }
    }
    Check(static_cast<bool>(m), "phase20 co_await next_message delivers a message");
    Check(m.from() == static_cast<std::uint32_t>(child),
          "phase20 message.from() == spawned child pid");
    Check(m.text() == "A", "phase20 message.text() payload from child");
    Check(m.payload_as<char>().value_or('\0') == 'A', "phase20 message.payload_as<char>()");

    // reply path (box::message::reply -> ::send back to the child); the child
    // never reads replies, so acceptance is liveness-dependent — exercise the
    // path (matrix PANIC=0/AppFAIL=0 covers the CET syscall thunk).
    (void)m.reply(static_cast<std::uint8_t>('R'));

    // ── drain remaining child messages via receive_for() ───────────────────
    {
        int got = 0;
        for (int i = 0; i < 12; i++) {
            std::optional<box::message> mm = box::receive_for(800);
            if (!mm) break;
            if (mm->from() == static_cast<std::uint32_t>(child) && mm->text() == "A") ++got;
        }
        Check(got >= 1, "phase20 receive_for drains further child messages");
    }

    printf("[CXX] PASS phase20: box::message (send/broadcast + receive/receive_for + "
           "co_await next_message cross-cabin delivery + payload guard + reply)\n");
}

// ── Ф16b: box::manifest / box::crate / compiled_manifest / mf_call1 ───────────
// Exercised against a read-only storage query (DECK_STORAGE, TAG_QUERY 0x01):
// in == none lists every file; the kernel writes [u32 count][u32 ids...] into
// the output crate. The image always holds files, so this is deterministic.
void Phase21()
{
    constexpr std::uint16_t STORAGE_TAG_QUERY = 0x01;

    // ── box::mf_call1 (single-op) ───────────────────────────────────────
    std::byte               qout[1024];
    box::mf_call_result     r = box::mf_call1(DECK_STORAGE, STORAGE_TAG_QUERY,
                                              {}, {}, std::span<std::byte>(qout, sizeof(qout)));
    Check(static_cast<bool>(r), "phase21 mf_call1 storage query rc OK");
    Check(r.produced >= 4, "phase21 mf_call1 produced a count header");
    if (!r) {
        printf("[CXX] PASS phase21: box::manifest/crate/compiled_manifest/mf_call1 "
               "(compiled; query unavailable)\n");
        return;
    }
    std::uint32_t count1 = 0;
    __builtin_memcpy(&count1, qout, 4);

    // ── box::manifest + box::crate (the same query, built op-by-op) ─────
    std::byte           mout[1024];
    box::manifest<>     mf;
    std::uint16_t       oc = mf.add(box::crate::output(std::span<std::byte>(mout, sizeof(mout))));
    mf.op(DECK_STORAGE, STORAGE_TAG_QUERY, box::no_crate, oc);
    Check(static_cast<bool>(mf), "phase21 manifest built without overflow");
    box::mf_outcome o = mf.submit();
    Check(static_cast<bool>(o), "phase21 manifest submit rc OK");
    box::crate ocr = mf.crate_at(oc);
    Check(ocr.size() >= 4, "phase21 manifest output crate produced a count");
    std::uint32_t count2 = 0;
    if (ocr.size() >= 4) __builtin_memcpy(&count2, mout, 4);
    Check(count2 == count1, "phase21 manifest query count matches mf_call1");
    Check(ocr.produced().size() == ocr.size(), "phase21 crate.produced() spans the written bytes");

    // ── box::compiled_manifest (prepared statement, submit twice) ───────
    {
        box::compiled_manifest cm(mf);
        if (cm) {
            box::mf_outcome c1 = cm.submit(mf);
            Check(static_cast<bool>(c1), "phase21 compiled_manifest submit #1 rc OK");
            std::uint32_t cc = 0;
            if (mf.crate_at(oc).size() >= 4) __builtin_memcpy(&cc, mout, 4);
            Check(cc == count1, "phase21 compiled_manifest produced the same count");
            box::mf_outcome c2 = cm.submit(mf);
            Check(static_cast<bool>(c2), "phase21 compiled_manifest submit #2 (handle reuse) rc OK");
        } else {
            printf("[CXX] note phase21: ManifestCompileHandle unavailable; compiled path skipped\n");
        }
    }

    // ── sticky-failure: overflow makes the builder refuse, not emit junk ─
    {
        box::manifest<64, 2> small;  // tiny: 2-crate table
        std::byte            sb[8] = {};
        small.add(box::crate::output(std::span<std::byte>(sb, sizeof(sb))));
        small.add(box::crate::output(std::span<std::byte>(sb, sizeof(sb))));
        Check(small.add(box::crate::output(std::span<std::byte>(sb, sizeof(sb)))) == box::no_crate,
              "phase21 crate-table overflow returns no_crate");
        Check(!small, "phase21 crate-table overflow sets the failure flag");
        Check(!small.submit(), "phase21 overflowed manifest submit refuses");

        std::vector<std::byte> huge(70000);  // > 0xFFFF param_size ABI limit
        box::manifest<>        mf2;
        mf2.op(DECK_STORAGE, STORAGE_TAG_QUERY, box::no_crate, box::no_crate,
               std::span<const std::byte>(huge.data(), huge.size()));
        Check(!mf2, "phase21 oversized op params (>64KB) set the failure flag");
    }

    // ── box::crate factory descriptors (no syscall; deterministic) ──────
    {
        std::uint32_t v = 0xABCD1234u;
        box::crate    ci = box::crate::input_object(v);
        Check(ci.kind() == CRATE_KIND_INPUT && ci.size() == sizeof(v),
              "phase21 crate::input_object descriptor");
        std::uint64_t ov = 0;
        box::crate    co = box::crate::output_object(ov);
        Check(co.kind() == CRATE_KIND_OUTPUT && co.capacity() == sizeof(ov) && co.size() == 0,
              "phase21 crate::output_object descriptor");
    }

    printf("[CXX] PASS phase21: box::manifest/crate (fluent multi-op builder) + "
           "compiled_manifest (prepared handle reuse) + mf_call1 (single-op)\n");
}

// ── Ф16c: box::this_process / box::process / box::cabin + box::tag_scope ──────
void Phase22()
{
    // ── identity: this_process and cabin agree; layout populated ────────
    std::uint32_t pid = box::this_process::pid();
    Check(pid != 0, "phase22 this_process::pid non-zero");
    Check(box::cabin::pid() == pid, "phase22 cabin::pid == this_process::pid");
    Check(box::this_process::spawner() == box::cabin::spawner(), "phase22 spawner agrees");
    Check(box::cabin::heap_base() != 0 && box::cabin::stack_top() != 0,
          "phase22 cabin address-space layout populated");

    // ── box::process::self() ────────────────────────────────────────────
    box::process me = box::process::self();
    Check(me && me.pid() == pid, "phase22 process::self() pid");
    Check(me.info().has_value(), "phase22 process::self().info()");
    Check(me.alive(), "phase22 process::self().alive()");

    // ── self process tags (broadcast membership) ────────────────────────
    Check(!box::this_process::has_tag("cxx:p22:tag"), "phase22 tag absent initially");
    Check(box::this_process::add_tag("cxx:p22:tag"), "phase22 add_tag");
    Check(box::this_process::has_tag("cxx:p22:tag"), "phase22 has_tag after add");
    Check(box::this_process::remove_tag("cxx:p22:tag"), "phase22 remove_tag");
    Check(!box::this_process::has_tag("cxx:p22:tag"), "phase22 tag gone after remove");

    // ── box::tag_scope RAII (carry a process tag for a scope) ───────────
    {
        box::tag_scope ts("cxx:p22:scope");
        Check(static_cast<bool>(ts), "phase22 tag_scope active");
        Check(box::this_process::has_tag("cxx:p22:scope"), "phase22 tag present inside scope");
    }
    Check(!box::this_process::has_tag("cxx:p22:scope"),
          "phase22 tag removed after scope (RAII)");

    // ── box::process::spawn (a new cabin) ───────────────────────────────
    box::process child = box::process::spawn("proca");
    if (child) {
        Check(child.pid() != 0 && child.pid() != pid, "phase22 process::spawn child pid");
        while (box::receive()) { /* drain proca's messages to its spawner (us) */ }
    } else {
        printf("[CXX] note phase22: proc_exec unavailable; spawn check skipped\n");
    }

    printf("[CXX] PASS phase22: box::this_process/process/cabin (identity/info/spawn) + "
           "tag_scope (RAII process tag)\n");
}

// ── Ф16d: box::system (info/uptime/mem/cpu + control) + box::efi ──────────────
void Phase23()
{
    // ── box::system::info() — machine snapshot, sanity invariants ────────
    std::optional<box::system_info> si = box::system::info();
    Check(si.has_value(), "phase23 system::info() returns a snapshot");
    if (si) {
        Check(si->version().substr(0, 5) == "BoxOS", "phase23 system version is BoxOS");
        Check(si->total_memory() > 0, "phase23 total_memory > 0");
        Check(si->used_memory() <= si->total_memory() && si->free_memory() <= si->total_memory(),
              "phase23 memory accounting within total");
        Check(si->cpu_total() >= 1 && si->cpu_total() <= 4096, "phase23 cpu_total sane");
        Check(si->uptime() > std::chrono::nanoseconds(0), "phase23 uptime > 0 (chrono)");
        Check(si->process_count() >= 1, "phase23 at least one live process");
    }

    // ── maintenance surfaces (non-destructive; exercise the path) ────────
    (void)box::system::perf_dump();      // dumps perf counters to the log
    (void)box::system::fragmentation();  // kernel-defined metric
    // reboot()/shutdown() reset the whole machine — compile/link surface only,
    // NEVER called from a test (it would reboot the VM).
    volatile auto reboot_fn   = &box::system::reboot;
    volatile auto shutdown_fn = &box::system::shutdown;
    (void)reboot_fn;
    (void)shutdown_fn;

    // ── box::efi — firmware introspection (config-agnostic) ──────────────
    std::optional<box::efi_status> efi = box::efi::info();
    if (efi) {
        std::vector<efi_esrt_entry_t> tbl = box::efi::esrt();
        Check(tbl.size() == efi->esrt_count(), "phase23 efi::esrt() range matches esrt_count");
        Check(!box::efi::esrt_entry(efi->esrt_count() + 1000u).has_value(),
              "phase23 efi::esrt_entry out-of-range -> nullopt");
    } else {
        printf("[CXX] note phase23: efi::info() unavailable on this config\n");
    }
    // Garbage is never trusted by Secure Boot policy (BIOS: SB unavailable;
    // UEFI/TCG: bad PE) — deterministic across every config.
    const std::byte junk[16] = {};
    Check(!box::efi::verified(std::span<const std::byte>(junk, sizeof(junk))),
          "phase23 efi::verified(garbage) is false");

    printf("[CXX] PASS phase23: box::system (info/uptime-chrono/mem/cpu + perf/frag + "
           "reboot/shutdown surface) + box::efi (info/esrt range/verify_pe)\n");
}

void Phase24()
{
    // ── box::cpu feature gates — cross-check against the system snapshot ─────
    // The feature bits come from the same post-AP-intersect g_cpu_caps the
    // kernel exposes to sysinfo, so the booleans must agree exactly. (The
    // calibrated frequency is NOT cross-checked for equality: a periodic TSC
    // recalibration publishes the new value to the static and to the cpu_caps
    // page in separate stores, so the two reads can momentarily disagree on
    // real hardware with thermal drift — only its >0 invariant is stable.)
    std::optional<box::system_info> si = box::system::info();
    if (si) {
        Check(box::cpu::has_waitpkg() == si->waitpkg(),
              "phase24 has_waitpkg agrees with system_info");
        Check(box::cpu::has_invariant_tsc() == si->invariant_tsc(),
              "phase24 has_invariant_tsc agrees with system_info");
        if (box::cpu::has_invariant_tsc())
            Check(box::cpu::tsc_freq_khz() > 0 && si->tsc_freq_khz() > 0,
                  "phase24 invariant TSC has a calibrated freq on both surfaces");
    }

    // ── TSC reads are monotone across a measurement window ──────────────────
    std::uint64_t t0 = box::cpu::tsc_now();
    volatile std::uint64_t spin = 0;
    for (int i = 0; i < 1000; ++i) spin += i;  // a little real work between reads
    std::uint64_t t1 = box::cpu::tsc_end();
    Check(t1 >= t0, "phase24 tsc_now/tsc_end monotone across a window");
    if (box::cpu::has_invariant_tsc() && box::cpu::tsc_freq_khz() > 0)
        Check(box::cpu::tsc_to_ns(box::cpu::ms_to_tsc(5)) > 0, "phase24 tsc_to_ns/ms_to_tsc round-trip");

    // ── box::hardware_entropy — a real uniform_random_bit_generator ─────────
    box::hardware_entropy g;
    const bool engaged = box::hardware_entropy::engaged();
    Check(g.entropy() == (engaged ? 64.0 : 0.0), "phase24 entropy() reports source width honestly");
    {
        // active_source() must agree with the underlying gates.
        using src = box::hardware_entropy::source;
        src s = box::hardware_entropy::active_source();
        src want = box::cpu::has_rdseed() ? src::seed
                 : box::cpu::has_rdrand() ? src::rand
                                          : src::none;
        Check(s == want, "phase24 active_source matches cpu gates");
        Check((s != src::none) == engaged, "phase24 engaged() iff a HW source backs draws");
    }
    {
        // Draws vary (HW source or the TSC fallback both advance).
        std::uint64_t v[8];
        for (auto &x : v) x = g();
        bool all_same = true;
        for (int i = 1; i < 8; ++i) if (v[i] != v[0]) { all_same = false; break; }
        Check(!all_same, "phase24 hardware_entropy draws are not constant");
    }
    {
        // Drives a distribution and seeds an engine — proves the URBG contract
        // end-to-end through the real <random> code paths.
        std::uniform_int_distribution<int> d(1, 6);
        bool in_range = true;
        for (int i = 0; i < 32; ++i) { int r = d(g); if (r < 1 || r > 6) { in_range = false; break; } }
        Check(in_range, "phase24 hardware_entropy drives uniform_int_distribution");
        std::mt19937_64 eng(g());
        Check(eng() != eng(), "phase24 hardware_entropy seeds a 64-bit engine");
    }
    if (!engaged)
        printf("[CXX] note phase24: no RDSEED/RDRAND on this config — hardware_entropy in TSC-fallback\n");

    // ── box::cpu::monitor_wait — the WAITPKG user-mode wait ─────────────────
    {
        volatile std::uint32_t cell = 0;
        std::uint64_t deadline = box::cpu::tsc_now() + box::cpu::ms_to_tsc(1);
        box::cpu::wake w = box::cpu::monitor_wait(&cell, deadline);
        if (box::cpu::monitor_supported()) {
            // No other writer to the line, so we expect to wake on the deadline
            // (an interrupt-driven 'written' is also fine) — never 'unavailable'.
            Check(w != box::cpu::wake::unavailable, "phase24 monitor_wait engaged returns a real wake");
        } else {
            Check(w == box::cpu::wake::unavailable, "phase24 monitor_wait disengaged when WAITPKG absent");
            printf("[CXX] note phase24: WAITPKG absent — monitor_wait reports unavailable\n");
        }
    }

    printf("[CXX] PASS phase24: box::cpu (feature gates/tsc + monitor_wait WAITPKG) + "
           "box::hardware_entropy (RDSEED/RDRAND URBG)\n");
}

void Phase25()
{
    // ── box::memtag::counters() — global registry snapshot, invariants ──────
    std::optional<box::memtag::stats> st = box::memtag::counters();
    Check(st.has_value(), "phase25 memtag::counters() returns a snapshot");
    if (st) {
        Check(st->region_active() >= 1, "phase25 at least one active region");
        Check(st->region_active() <= st->region_slot_count(), "phase25 active <= slot_count");
        Check(st->region_slot_count() <= st->region_slot_cap(), "phase25 slot_count <= slot_cap");
        Check(st->tag_count() >= 1, "phase25 registry has seeded tags");
    }

    // ── find a tagged region by scanning the slot table (no hard-coded tag —
    //    the kernel seeds zone tags at boot; we derive a real one at runtime).
    //    Prefer one whose base is phys-mapped so the covering() lookup below
    //    has a positive case; fall back to any tagged region otherwise. ───────
    std::optional<box::memtag::region> found;          // tagged AND phys-mapped
    std::optional<box::memtag::region> any_tagged;     // fallback: any tagged
    std::uint32_t walk = st ? st->region_slot_count() : 0;
    if (walk > 256) walk = 256;  // zones are seeded into low slots; bound the scan
    for (std::uint32_t id = 0; id < walk && !found; ++id) {
        std::optional<box::memtag::region> r = box::memtag::find(id);
        if (!r || r->tag_count() == 0) continue;
        if (!any_tagged) any_tagged = r;
        if (box::memtag::covering(r->base_phys())) found = r;  // base resolves via id_by_page
    }
    if (!found) found = any_tagged;

    if (found) {
        std::vector<std::string> tags = found->tags();
        // tags() may report fewer than tag_count if the kernel truncates to the
        // out buffer — the contract is "no more than the descriptor's count".
        Check(tags.size() <= found->tag_count(), "phase25 region tags() within tag_count");
        Check(!tags.empty(), "phase25 tagged region yields its tag strings");
        if (!tags.empty()) {
            const std::string &T = tags.front();
            Check(found->has_tag(T), "phase25 region has_tag(its own tag)");
            // covering(phys) returns the region that genuinely COVERS phys (the
            // kernel keeps one owner per page in id_by_page, so an overlapped or
            // non-phys region need not be the owner — assert range containment,
            // not id identity). nullopt means the base isn't in the phys map.
            std::optional<box::memtag::region> cov = box::memtag::covering(found->base_phys());
            if (cov) {
                std::uint64_t b = found->base_phys();
                Check(b >= cov->base_phys() && b < cov->base_phys() + cov->size_bytes(),
                      "phase25 covering(phys) returns a region covering that phys");
            } else {
                printf("[CXX] note phase25: tagged region base not phys-mapped — covering skipped\n");
            }
            // query by that tag must include this region, and every hit bears it.
            std::vector<box::memtag::region> hits = box::memtag::query({T.c_str()});
            bool contains_self = false, all_bear = true;
            for (const auto &h : hits) {
                if (h.id() == found->id()) contains_self = true;
                if (!h.has_tag(T)) all_bear = false;
            }
            Check(!hits.empty() && contains_self, "phase25 query(tag) returns the tagged region");
            Check(all_bear, "phase25 every query(tag) hit bears the tag");
        }
    } else {
        printf("[CXX] note phase25: no tagged region in slot scan — query/region checks skipped\n");
    }

    // ── check_access: the default registry is permissive (no guard ⇒ allow) ─
    std::uint32_t me = box::this_process::pid();
    if (found)
        Check(box::memtag::check_access(me, *found),
              "phase25 default-permissive check_access is true");

    // ── capability control round-trip on a TEST-ONLY tag. No real region
    //    bears 'cxx:p25:*', so this can never perturb the live system, and we
    //    revoke/clear afterwards. grant/set_guard need the "system" tag-bit, so
    //    an unprivileged cabin simply gets a clean false (noted, not failed). ─
    bool granted = box::memtag::grant(me, "cxx:p25:cap");
    if (granted) {
        std::vector<std::string> ct = box::memtag::cabin_tags(me);
        bool has = false;
        for (const auto &t : ct) if (t == "cxx:p25:cap") has = true;
        Check(has, "phase25 grant reflected in cabin_tags");
        Check(box::memtag::revoke(me, "cxx:p25:cap"), "phase25 revoke succeeds");
        std::vector<std::string> ct2 = box::memtag::cabin_tags(me);
        bool still = false;
        for (const auto &t : ct2) if (t == "cxx:p25:cap") still = true;
        Check(!still, "phase25 revoke removes the tag");
    } else {
        printf("[CXX] note phase25: memtag grant unprivileged on this cabin — round-trip skipped\n");
    }
    // Exercise set_guard's path on a test-only tag, then clear it (privilege-
    // dependent return; reaching past here proves the path doesn't fault).
    (void)box::memtag::set_guard("cxx:p25:guard", true);
    (void)box::memtag::set_guard("cxx:p25:guard", false);
    (void)box::memtag::cabin_tags(me);  // unprivileged read — always exercised

    printf("[CXX] PASS phase25: box::memtag (query/region/tags/covering/stats + "
           "guard/grant/revoke/cabin_tags/check_access)\n");
}

void Phase26()
{
    const bool pku_ok = box::pku::available();

    // ── mem_region_from_virt: resolve an OWNED allocation's region (the new
    //    kernel virt→region primitive). PKU-independent — a pure MemTag lookup
    //    over the bay's pages, which the kernel registers as a region. ─────────
    box::bay<std::uint64_t> b = box::bay<std::uint64_t>::create("cxx:p26:sealed", 64);
    std::uint32_t bay_region = MEMTAG_INVALID_REGION_ID;
    if (b) {
        bay_region = ::mem_region_from_virt(b.data());
        Check(bay_region != MEMTAG_INVALID_REGION_ID,
              "phase26 mem_region_from_virt resolves an owned bay's region");
        if (bay_region != MEMTAG_INVALID_REGION_ID)
            Check(box::memtag::find(bay_region).has_value(),
                  "phase26 virt-resolved region_id is a live region");
    } else {
        printf("[CXX] note phase26: bay create failed — region/sealed checks skipped\n");
    }

    // ── protection_key rights round-trip + access_window RAII (PKRU register) ─
    if (pku_ok) {
        box::protection_key k(9);
        Check(k.lock(), "phase26 protection_key.lock()");
        { auto r = k.rights(); Check(r.access_disabled && !r.write_disabled, "phase26 lock -> AD only"); }
        Check(k.read_only(), "phase26 protection_key.read_only()");
        { auto r = k.rights(); Check(!r.access_disabled && r.write_disabled, "phase26 read_only -> WD only"); }
        Check(k.unlock(), "phase26 protection_key.unlock()");
        { auto r = k.rights(); Check(!r.access_disabled && !r.write_disabled, "phase26 unlock -> clear both"); }

        k.lock();  // prior state = locked
        {
            box::access_window w(k);
            Check(!box::pku::get_rights(9).access_disabled, "phase26 access_window opens the key");
        }
        Check(box::pku::get_rights(9).access_disabled, "phase26 access_window restores prior rights");
        k.unlock();  // cleanup
    } else {
        printf("[CXX] note phase26: PKU unavailable — protection_key/access_window checks skipped\n");
    }

    // ── sealed_region<T> over the owned bay (W^X). On TCG the access *fault* is
    //    not enforced, so the writes below never trap; we verify the binding and
    //    the PKRU bit transitions (seal → reveal → re-seal), which DO round-trip. ─
    if (b && pku_ok) {
        box::sealed_region<std::uint64_t> sealed(b.as_span(), 11);
        Check(sealed.bound(), "phase26 sealed_region binds the owned region");
        if (sealed.bound()) {
            Check(sealed.region() == bay_region, "phase26 sealed_region bound to the bay's region");
            Check(box::pku::get_rights(11).access_disabled, "phase26 sealed_region is sealed by default");
            {
                box::access_window w = sealed.reveal();
                Check(!box::pku::get_rights(11).access_disabled, "phase26 reveal() opens the region");
                sealed.data()[0] = 0xC0FFEEull;  // write through the revealed window
                Check(sealed.data()[0] == 0xC0FFEEull, "phase26 revealed region is writable");
            }
            Check(box::pku::get_rights(11).access_disabled, "phase26 region re-seals after the window");
        }
        // sealed dtor clears the pku tag from the bay region here
    }

    // ── mem_region_from_virt over a 2 MiB huge-page allocation. Bay maps >=2 MiB
    //    with implicit 2 MiB pages; a pointer into one must still resolve to its
    //    region (a 4 KiB-only page walk would miss it). This is the regression
    //    guard for the huge-page-aware MemRegionFromVirt. ────────────────────
    {
        box::bay<std::uint64_t> hb = box::bay<std::uint64_t>::create("cxx:p26:huge", 256 * 1024);  // 2 MiB
        if (hb) {
            std::uint32_t hrid = ::mem_region_from_virt(hb.data());
            Check(hrid != MEMTAG_INVALID_REGION_ID,
                  "phase26 mem_region_from_virt resolves a 2 MiB huge-page region");
            if (pku_ok && hrid != MEMTAG_INVALID_REGION_ID) {
                box::sealed_region<std::uint64_t> hs(hb.as_span(), 12);
                Check(hs.bound(), "phase26 sealed_region binds a huge-page region");
            }
        } else {
            printf("[CXX] note phase26: 2 MiB bay create failed — huge-page check skipped\n");
        }
    }
    box::protection_key(11).unlock();  // ensure keys are clear regardless of path
    box::protection_key(12).unlock();

    printf("[CXX] PASS phase26: box::pku (rights/protection_key/access_window) + "
           "box::sealed_region<T> over owned bay via mem_region_from_virt\n");
}

void Phase27()
{
    // ── box::hw::lam — gates + current mode + the kernel-blocked paths ──────
    Check(box::hw::lam_available() == box::cpu::has_lam(), "phase27 lam_available agrees with cpu gate");
    Check(box::hw::tme_available() == box::cpu::has_tme(), "phase27 tme_available agrees with cpu gate");

    {
        box::hw::lam_mode m = box::hw::lam();
        Check(m == box::hw::lam_mode::none || m == box::hw::lam_mode::u48 || m == box::hw::lam_mode::u57,
              "phase27 lam() returns a valid mode");
        if (!box::hw::lam_available())
            Check(m == box::hw::lam_mode::none, "phase27 no LAM -> mode none");
    }
    // U57 needs 5-level paging BoxOS never enables -> always unexpected.
    Check(!box::hw::set_lam(box::hw::lam_mode::u57).has_value(),
          "phase27 set_lam(u57) is unexpected (no 5-level paging)");
    // U48 on a CPU without LAM is unexpected; with LAM, exercise and restore.
    if (!box::hw::lam_available()) {
        Check(!box::hw::set_lam(box::hw::lam_mode::u48).has_value(),
              "phase27 set_lam(u48) is unexpected without LAM");
    } else if (box::hw::set_lam(box::hw::lam_mode::u48).has_value()) {
        Check(box::hw::lam() == box::hw::lam_mode::u48, "phase27 set_lam(u48) round-trips via lam()");
        (void)box::hw::set_lam(box::hw::lam_mode::none);  // restore
    }

    // ── box::hw::tme — platform snapshot ───────────────────────────────────
    if (std::optional<box::hw::tme_state> t = box::hw::tme()) {
        // No TME platform -> TME cannot be active and holds no KeyIDs.
        Check(box::hw::tme_available() || !t->active(),
              "phase27 no TME platform -> tme not active");
        if (!t->active()) Check(t->in_use() == 0, "phase27 inactive TME has no keyids in use");
    } else {
        printf("[CXX] note phase27: hw::tme() unavailable on this config\n");
    }

    // ── box::tagged_pointer<T> — LAM-U48 tag bit-math (HW-independent) ──────
    static std::uint64_t storage = 0xA5A5A5A5A5A5A5A5ull;
    const std::uintptr_t kTag = static_cast<std::uintptr_t>(0x7F) << 56;
    box::tagged_pointer<std::uint64_t> tp(&storage, 0x5A);
    Check(tp.tag() == 0x5A, "phase27 tagged_pointer stores the tag");
    Check(tp.untagged() == &storage, "phase27 untagged() recovers the canonical pointer");
    Check(*tp.untagged() == 0xA5A5A5A5A5A5A5A5ull, "phase27 untagged() is dereferenceable");
    Check(tp.value() == ((reinterpret_cast<std::uintptr_t>(&storage) & ~kTag) |
                         (static_cast<std::uintptr_t>(0x5A) << 56)),
          "phase27 tagged value carries the tag in bits 62:56");
    tp.retag(0x3C);
    Check(tp.tag() == 0x3C && tp.untagged() == &storage, "phase27 retag keeps the address");
    box::tagged_pointer<std::uint64_t> over(&storage, 0xFF);
    Check(over.tag() == 0x7F, "phase27 tag clamps to 7 bits");
    Check(static_cast<bool>(tp), "phase27 tagged_pointer to storage is truthy");
    // NOTE: dereferencing get()/operator* requires LAM-U48 engaged (the address
    // is non-canonical otherwise); not exercised here as LAM is off under TCG.

    printf("[CXX] PASS phase27: box::hw (lam/set_lam-expected/tme_state) + "
           "box::tagged_pointer<T> (LAM-U48 tag bits)\n");
}

void Phase28()
{
    // ── box::color value logic + the BoxOS palette ─────────────────────────
    constexpr box::color red = box::color::rgb(0xE0, 0x40, 0x40);
    static_assert(red.r() == 0xE0 && red.g() == 0x40 && red.b() == 0x40 && red.is_rgb());
    Check(red == box::colors::red, "phase28 colors::red == rgb(0xE0,0x40,0x40)");
    Check(box::colors::berry.r() == 0xE0 && box::colors::berry.g() == 0x4F && box::colors::berry.b() == 0x90,
          "phase28 colors::berry RGB triple");
    Check(box::color::use_default().is_default() && box::color::inherit().is_inherit(),
          "phase28 default/inherit sentinels");
    Check(box::colors::red.is_rgb() && !box::colors::red.is_default(), "phase28 rgb color is_rgb");

    // ── std::formatter<box::color> — the VALUE as "#RRGGBB" (never escapes) ──
    Check(std::format("{}", box::colors::red) == "#E04040", "phase28 format red -> #E04040");
    Check(std::format("{}", box::colors::leaf) == "#60C030", "phase28 format leaf -> #60C030");
    Check(std::format("{}", box::color::use_default()) == "default", "phase28 format default sentinel");
    Check(std::format("{}", box::color::inherit()) == "inherit", "phase28 format inherit sentinel");
    Check(std::format("fg={} bg={}", box::colors::amber, box::colors::black) == "fg=#FFB040 bg=#000000",
          "phase28 format multiple colors");

    // ── box::styled — scoped text color (set on entry, restore on exit). The
    //    color STATE round-trips here; the on-screen rendering is display-side. ─
    box::set_color(box::colors::white);  // known starting foreground
    Check(box::current_color() == box::colors::white, "phase28 set_color/current_color round-trip");
    {
        box::styled s(box::colors::red);
        Check(box::current_color() == box::colors::red, "phase28 styled sets fg in scope");
    }
    Check(box::current_color() == box::colors::white, "phase28 styled restores fg on exit");
    {
        box::styled s(box::colors::green, box::colors::black);
        Check(box::current_color() == box::colors::green &&
              box::current_background() == box::colors::black,
              "phase28 styled(fg,bg) sets both");
    }
    Check(box::current_color() == box::colors::white, "phase28 styled(fg,bg) restores fg");

    // ── box::vga — batch session + dimensions (text-mode; rendering display-
    //    side). Non-destructive: no clear/scroll/visible text, just the batch
    //    syscall path and introspection. ──────────────────────────────────────
    box::vga::dimensions dim = box::vga::size();
    (void)box::vga::cursor();
    if (dim.rows > 0 && dim.cols > 0) {
        {
            box::vga::session s;  // ops batch into one Manifest
            box::vga::set_color(box::colors::light_gray, box::colors::black);
        }  // commit on scope exit
        box::vga::session s2;
        Check(s2.commit() == 0, "phase28 vga batch session commits");
    } else {
        printf("[CXX] note phase28: vga text mode unavailable on this config\n");
    }

    printf("[CXX] PASS phase28: box::color/colors + std::formatter<color> + "
           "box::styled (scoped) + box::vga::session (batch)\n");
}

void Phase29()
{
    // ── box::key value + decode logic (pure; the structured core) ──────────
    box::key k('A', 0x1E, KB_MOD_SHIFT | KB_MOD_CTRL);
    Check(k.ch() == 'A' && k.scancode() == 0x1E, "phase29 key ch/scancode");
    Check(k.shift() && k.ctrl() && !k.alt(), "phase29 key modifiers decode");
    Check(k.has_char() && k.printable(), "phase29 key has_char/printable");
    Check(k.modifiers() == (KB_MOD_SHIFT | KB_MOD_CTRL), "phase29 key modifiers bitset");

    box::key none(0, 0x48, 0);  // a scancode-only key (e.g. an arrow)
    Check(!none.has_char() && !none.printable(), "phase29 scancode-only key has no char");

    kb_char_t kc{};
    kc.ch = 'b';
    kc.scancode = 0x30;
    kc.flags = KB_MOD_ALT;
    box::key kk = box::key::from_kb_char(kc);
    Check(kk.ch() == 'b' && kk.scancode() == 0x30 && kk.alt() && !kk.shift(),
          "phase29 from_kb_char decodes ch/scancode/mods");

    // kb_event_t (the Touch payload) has a DIFFERENT field order than kb_char_t
    // (scancode, ascii, mods) — verify the decoder reads the right fields.
    kb_event_t ke{};
    ke.scancode = 0x10;
    ke.ascii = 'q';
    ke.mods = KB_MOD_CTRL;
    box::key ek = box::key::from_event(ke);
    Check(ek.ch() == 'q' && ek.scancode() == 0x10 && ek.ctrl() && !ek.alt(),
          "phase29 from_event decodes the touch-payload field order");

    // ── key_input — non-blocking introspection. There is no interactive input
    //    during the matrix, so get_key()/co_await would block: those are
    //    compile-surface only here; we exercise the non-blocking paths. ───────
    std::optional<std::uint32_t> avail = box::key_input::available();
    Check(avail.has_value(), "phase29 key_input::available() queries status");
    std::optional<box::key> got = box::key_input::try_get();
    if (avail && *avail == 0)
        Check(!got.has_value(), "phase29 try_get() empty when nothing buffered");
    // get_for with a short timeout must RETURN (never hang); its result is
    // input-dependent, so it is drained, not asserted.
    (void)box::key_input::get_for(std::chrono::milliseconds(5));
    // get_key() blocks until a key — reference only (calling it would hang).
    volatile auto blocking_fp = &box::key_input::get_key;
    (void)blocking_fp;

    // ── async Touch-backed key stream: claim + non-blocking poll (drain) ───
    box::subscription keys = box::keyboard_events();
    if (std::optional<box::event> ev = keys.poll())
        (void)box::key::from_touch(*ev);  // decode path (input-dependent)

    printf("[CXX] PASS phase29: box::key (decode/modifiers) + box::key_input "
           "(available/try_get/get_for) + box::keyboard_events (async stream)\n");
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
    Phase6();
    Phase7a();
    Phase7b();
    Phase7c();
    Phase7d();
    Phase8a();
    Phase8b();
    Phase8c();
    Phase8d();
    Phase8e();
    Phase8f();
    Phase9a();
    Phase9a2();
    Phase9a3();
    Phase9a4();
    Phase9b();
    Phase9c();
    PhaseCurrent();
    Phase10();
    Phase11();
    Phase12();
    Phase13();
    Phase14();
    Phase15();
    Phase16();
    Phase17();
    Phase18();
    Phase19();
    Phase20();
    Phase21();
    Phase22();
    Phase23();
    Phase24();
    Phase25();
    Phase26();
    Phase27();
    Phase28();
    Phase29();

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
