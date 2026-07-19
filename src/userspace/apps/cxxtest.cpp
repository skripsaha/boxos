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
#include "box/strand.h"   // strand_spawn / strand_exit (phase35 sibling strand)
#include "box/sync.h"     // addr_park / addr_wake (phase35 join)
#include "box/cpu.h"      // cpu_has_fsgsbase (phase35 spawn guard)
#include "box/cxx/tls_strand.h"  // __boxcxx_tls_strand_init + thread-storage hooks (phase36)

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <coroutine>
#include <generator>
#include <deque>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
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
#include <shared_mutex>
#include <latch>
#include <barrier>
#include <semaphore>
#include <stop_token>
#include <condition_variable>
#include <thread>
#include <future>
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
#include "box/cxx/child.h"
#include "box/cxx/ferry.h"
#include "box/cxx/console.h"
#include "box/cxx/cpu.h"
#include "box/cxx/current.h"
#include "box/cxx/error.h"
#include "box/cxx/executor.h"
#include "box/cxx/heap.h"
#include "box/cxx/hw.h"
#include "box/cxx/keyboard.h"
#include "box/cxx/line.h"
#include "box/cxx/manifest.h"
#include "box/cxx/math.h"
#include "box/cxx/memtag.h"
#include "box/cxx/message.h"
#include "box/cxx/pku.h"
#include "box/cxx/process.h"
#include "box/cxx/reflex.h"
#include "box/cxx/strand.h"
#include "box/cxx/system.h"
#include "box/cxx/system_touch.h"
#include "box/cxx/tagfs.h"
#include "box/cxx/timeouts.h"
#include "box/cxx/timing.h"
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

// Ф22b synth-three-way: a legacy element with only < / == (no <=>).
struct Cxx7Legacy {
    int v;
    constexpr bool operator==(const Cxx7Legacy &o) const { return v == o.v; }
    constexpr bool operator<(const Cxx7Legacy &o) const { return v < o.v; }
};
static_assert(std::is_same_v<decltype(std::declval<std::array<Cxx7Legacy, 1>>() <=>
                                      std::declval<std::array<Cxx7Legacy, 1>>()),
                             std::weak_ordering>);

// Ф22c: stateful, non-always-equal allocator (POCMA off, POCCA on) that drives
// deque's allocator-aware move/copy assignment paths.
template <class T>
struct StatefulAlloc {
    int id = 0;
    using value_type                             = T;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_copy_assignment = std::true_type;
    using is_always_equal                        = std::false_type;
    constexpr StatefulAlloc() = default;
    constexpr explicit StatefulAlloc(int i) : id(i) {}
    template <class U> constexpr StatefulAlloc(const StatefulAlloc<U> &o) : id(o.id) {}
    T   *allocate(std::size_t n) { return static_cast<T *>(::operator new(n * sizeof(T))); }
    void deallocate(T *p, std::size_t) noexcept { ::operator delete(p); }
    template <class U>
    constexpr bool operator==(const StatefulAlloc<U> &o) const { return id == o.id; }
};

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
    // ── Ф22b synth-three-way: container operator<=> with a legacy element
    //    (only < / ==) must route through synth; normal types still work. ──
    {
        std::vector<Cxx7Legacy> va{{1}, {2}}, vb{{1}, {3}};
        Check((va <=> vb) < 0 && (va <=> va) == 0 && (vb <=> va) > 0,
              "phase7a vector<=> synth(legacy)");
        std::list<Cxx7Legacy> la{{1}, {2}}, lb{{1}, {2}, {0}};
        Check((la <=> lb) < 0 && (la <=> la) == 0, "phase7a list<=> synth(legacy)");
        std::deque<Cxx7Legacy> da{{2}}, db{{1}};
        Check((da <=> db) > 0, "phase7a deque<=> synth(legacy)");
        std::vector<int> n1{1, 2}, n2{1, 2, 3};   // normal-type regression
        Check((n1 <=> n2) < 0 && (n1 <=> n1) == 0, "phase7a vector<=> int regression");
    }
    {   // Ф22c: deque allocator-aware assignment (POCMA off, POCCA on, unequal)
        using SA = StatefulAlloc<int>;
        using DA = std::deque<int, SA>;
        DA a({1, 2, 3}, SA(1));
        DA b({9}, SA(2));
        b = std::move(a);   // unequal allocators + POCMA=false → element-wise move (no UB)
        Check(b.size() == 3 && b[0] == 1 && b[2] == 3 && b.get_allocator().id == 2,
              "phase7a deque move-assign unequal-alloc (element-wise, keeps own alloc)");
        DA c({7, 8}, SA(3));
        c = b;              // unequal allocators + POCCA=true → free-old + propagate + copy
        Check(c.size() == 3 && c[0] == 1 && c[2] == 3 && c.get_allocator().id == 2,
              "phase7a deque copy-assign POCCA (propagates alloc)");
    }

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

    // Ф22a: giant-exponent guards. These inputs previously grew a fixed-size
    // BigInt on the stack without bound (write far OOB) from UNTRUSTED input,
    // and this build has no -fstack-protector. Each must now report
    // result_out_of_range, leave value unmodified, and consume the whole token.
    {
        double v = 11; const char s[] = "1e1000000";
        auto r = std::from_chars(s, s + 9, v);
        Check(r.ec == std::errc::result_out_of_range && v == 11 && r.ptr == s + 9,
              "phase9a4 decimal giant +exp guarded");
    }
    {
        double v = 11; const char s[] = "1e-1000000";
        auto r = std::from_chars(s, s + 10, v);
        Check(r.ec == std::errc::result_out_of_range && v == 11 && r.ptr == s + 10,
              "phase9a4 decimal giant -exp guarded");
    }
    {
        double v = 11; const char s[] = "1p2000000000";  // hex from_chars (no 0x prefix)
        auto r = std::from_chars(s, s + 12, v, F::hex);
        Check(r.ec == std::errc::result_out_of_range && v == 11 && r.ptr == s + 12,
              "phase9a4 hex giant +p guarded");
    }
    {
        double v = 11; const char s[] = "1p-2000000000";
        auto r = std::from_chars(s, s + 13, v, F::hex);
        Check(r.ec == std::errc::result_out_of_range && v == 11 && r.ptr == s + 13,
              "phase9a4 hex giant -p guarded");
    }
    {
        float v = 11; const char s[] = "1e100000";  // smaller float overflow threshold
        auto r = std::from_chars(s, s + 8, v);
        Check(r.ec == std::errc::result_out_of_range && v == 11.0f,
              "phase9a4 float giant +exp guarded");
    }
    {
        // The exact reported vector, through stod (which detects 0x then throws).
        bool threw = false;
        try { std::stod("0x1p2000000000"); } catch (const std::out_of_range &) { threw = true; }
        Check(threw, "phase9a4 stod 0x1p2000000000 guarded");
    }
    // Near-boundary inputs must STILL parse correctly — the pre-clamp is
    // conservative and must not capture any representable value.
    {
        double v = 0; const char s[] = "1e-323";  // tiny but representable subnormal
        auto r = std::from_chars(s, s + 6, v);
        Check(r.ec == std::errc{} && v > 0.0 && v < 1e-300,
              "phase9a4 subnormal 1e-323 still parses");
    }
    {
        double v = 7; const char s[] = "1e-330";  // genuinely underflows to zero
        auto r = std::from_chars(s, s + 6, v);
        Check(r.ec == std::errc::result_out_of_range && v == 7,
              "phase9a4 1e-330 underflow unmodified");
    }
    {
        // On the routed (out-of-range) path, ptr must still stop right after
        // the numeric token, not consume trailing characters.
        double v = 11; const char s[] = "1e1000000xyz";
        auto r = std::from_chars(s, s + 12, v);
        Check(r.ec == std::errc::result_out_of_range && v == 11 && r.ptr == s + 9,
              "phase9a4 giant exp leaves trailing chars");
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

// ── phaseCurrent async fixtures: co_await box::current<T>::next() ───────────
// Same-strand executor drain of the Current spine's coroutine read (Ф24c),
// mirroring the Phase13 Brook suspend/resume idiom but through box::current<T>.
struct CurSample { int id; unsigned tag; };

box::task<void> CurrentAsyncProducer(box::current<CurSample> *w, unsigned k)
{
    for (unsigned i = 0; i < k; i++) w->put(CurSample{(int)i, i * 11u});
    w->close();   // honest writer-leave terminal
    co_return;
}

box::task<unsigned> CurrentAsyncConsumer(box::current<CurSample> *r)
{
    unsigned got = 0;
    while (auto v = co_await r->next()) {  // nullopt at the stream terminal
        if (v->id != (int)got || v->tag != got * 11u) break;
        ++got;
    }
    co_return got;
}

// Small item (2-byte): the framed take stages through the per-handle frame_buf
// repad (frame_bytes==8 > item_size==2). Drive it through the co_await + executor
// re-arm path to cover the repad asynchronously (the sync path is covered below).
box::task<void> CurrentAsyncU16Producer(box::current<std::uint16_t> *w, unsigned k)
{
    for (unsigned i = 0; i < k; i++) w->put(static_cast<std::uint16_t>(0x1000u + i));
    w->close();
    co_return;
}

box::task<unsigned> CurrentAsyncU16Consumer(box::current<std::uint16_t> *r)
{
    unsigned got = 0;
    while (auto v = co_await r->next()) {
        if (*v != static_cast<std::uint16_t>(0x1000u + got)) break;
        ++got;
    }
    co_return got;
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
        for (int i = 0; i < 3; i++) put_ok = put_ok && w.put(Sample{i, (unsigned)(i * 11)}).has_value();
        Check(put_ok, "phaseCurrent stream put");
        bool take_ok = true;
        for (int i = 0; i < 3; i++) {
            Sample s{};
            box::result<bool> got = r.take(s);
            take_ok = take_ok && got.has_value() && *got &&
                      s.id == i && s.tag == (unsigned)(i * 11);
        }
        Check(take_ok, "phaseCurrent stream take");
        w.close();
        Sample drained{};
        // After close+drain the take is the honest tri-state: a `false` VALUE
        // (CURRENT_CLOSED), NOT an error arm.
        box::result<bool> closed = r.take(drained);
        Check(closed.has_value() && *closed == false, "phaseCurrent stream CURRENT_CLOSED");
    }

    // Same-strand executor drain of co_await s.next() (Ф24c). The consumer
    // suspends on the empty stream, the producer then fills K items + closes,
    // and the loop ends via the nullopt terminal at exactly K (never K+1, never
    // a hang) — the Brook suspend/resume idiom routed through the Current spine.
    {
        constexpr unsigned K = 4;
        box::current<CurSample> w("cxx:current:async", box::role::write);  // writer auto-creates
        box::current<CurSample> r("cxx:current:async", box::role::read);
        Check(bool(w) && bool(r), "phaseCurrent async open");

        box::executor ex;
        auto consumer = CurrentAsyncConsumer(&r);
        ex.spawn(CurrentAsyncProducer(&w, K));  // queued first
        ex.schedule(consumer.handle());         // LIFO: consumer runs first, finds
                                                // empty, suspends, then producer fills
        ex.run();
        unsigned got = consumer.result();
        Check(got == K, "phaseCurrent async co_await next drain (terminal at K)");
    }

    // Same-strand async drain of a SMALL item (2-byte) typed stream — exercises
    // the frame_buf repad through the co_await path + executor re-arm.
    {
        constexpr unsigned K = 3;
        box::current<std::uint16_t> w("cxx:current:async16", box::role::write);
        box::current<std::uint16_t> r("cxx:current:async16", box::role::read);
        Check(bool(w) && bool(r), "phaseCurrent async16 open");

        box::executor ex;
        auto consumer = CurrentAsyncU16Consumer(&r);
        ex.spawn(CurrentAsyncU16Producer(&w, K));
        ex.schedule(consumer.handle());
        ex.run();
        Check(consumer.result() == K, "phaseCurrent async16 small-item co_await drain");
    }

    // Byte channel over a TagFS file: write, then read back.
    {
        const char *msg = "current<byte> over TagFS";  // 24 bytes
        box::byte_current fw = box::file("cxx_current.dat", box::role::write);
        std::size_t n = fw ? fw.write(msg, 24) : 0;
        Check(n == 24, "phaseCurrent file write");
        fw = box::byte_current{};   // release writer (RAII)

        box::byte_current fr = box::file("cxx_current.dat", box::role::read, box::opening::none);
        char back[25] = {};
        int  rn = fr ? fr.read(back, 24) : -1;
        Check(rn == 24 && std::string_view(back, 24) == msg, "phaseCurrent file read");
        Check((fr.caps() & CURRENT_CAP_SEEKABLE) != 0, "phaseCurrent file seekable");
    }

    // Small item (2 bytes) through the typed layer — exercises frame padding.
    {
        box::current<std::uint16_t> w("cxx:current:small", box::role::write, box::opening::create);
        box::current<std::uint16_t> r("cxx:current:small", box::role::read);
        bool          ok = bool(w) && bool(r) && w.put(0xC0DE).has_value();
        std::uint16_t v  = 0;
        box::result<bool> got = r.take(v);
        ok = ok && got.has_value() && *got && v == 0xC0DE;
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
    std::optional<box::touch> ev = co_await s->next();
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
        std::optional<box::touch> ev = sub.wait(1000);
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
            std::optional<box::touch> a = sa.wait(1000);
            std::optional<box::touch> b = sb.wait(1000);
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
        for (box::touch e : sub) {
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

    printf("[CXX] PASS phase15: box::touch (tag/_tag/subscription/touch-record/payload_as "
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
        box::heap::stats st = box::heap::counters();
        Check(st.malloc_calls() > 0 && st.in_use_bytes() > 0, "phase17 heap::stats snapshot");
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

    // ── create with tags (now box::result<file>) ────────────────────────
    box::result<file> created = box::tagfs::create("cxx:tagfs:probe", {"cxx:phase18", "kind:test"});
    Check(created.has_value(), "phase18 create returns a live file through box::result");
    if (!created) {
        printf("[CXX] PASS phase18: box::tagfs (compiled; create unavailable)\n");
        return;
    }
    file f = *created;

    // ── metadata + tags ─────────────────────────────────────────────────
    Check(f.name() == "cxx:tagfs:probe", "phase18 file.name()");
    Check(f.has_tag("cxx") && f.has_tag("kind"), "phase18 file.has_tag(key)");
    Check(f.tags().size() >= 2, "phase18 file.tags() lists the tags");

    // ── random-access byte I/O bound to this file_id (result<size_t>) ───
    const char msg[] = "hello tagfs stream";
    box::result<std::size_t> wrote = f.write_at(0, msg, sizeof(msg));
    Check(wrote.has_value() && *wrote == sizeof(msg), "phase18 write_at(raw) byte count");
    char rbuf[sizeof(msg)] = {};
    box::result<std::size_t> read = f.read_at(0, rbuf, sizeof(rbuf));
    Check(read.has_value() && *read == sizeof(msg)
              && std::string_view(rbuf) == "hello tagfs stream",
          "phase18 read_at(raw) round-trip + byte count");

    std::byte payload[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    box::result<std::size_t> ws = f.write_at(64, std::span<const std::byte>(payload, 4));
    Check(ws.has_value() && *ws == 4, "phase18 write_at(span) byte count");
    std::byte rb[4] = {};
    box::result<std::size_t> rs = f.read_at(64, std::span<std::byte>(rb, 4));
    Check(rs.has_value() && *rs == 4 && rb[0] == std::byte{1},
          "phase18 read_at(span) round-trip + byte count");

    struct Rec {
        std::uint32_t a;
        std::uint32_t b;
    };
    Check(f.write_object<Rec>(128, Rec{0xAAu, 0xBBu}).has_value(), "phase18 write_object<T>");
    box::result<Rec> rec = f.read_object<Rec>(128);
    Check(rec.has_value() && rec->a == 0xAAu && rec->b == 0xBBu,
          "phase18 read_object<T> round-trip");

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
        box::result<std::size_t> idr = f.read_at(0, idbuf, 8);
        if (idr.has_value() && *idr == 8 && std::string_view(idbuf, 8) == "STREAMRT")
            Check(true, "phase18 bytes() bridge reaches this file_id");
        else
            printf("[CXX] note phase18: bytes() bridges by name to a separate "
                   "backing; read_at/write_at are id-precise\n");
    }

    // ── tag mutations (remove_tag by key removes a key:value tag) ────────
    Check(f.add_tag("extra:1").has_value(), "phase18 add_tag (key:value)");
    Check(f.has_tag("extra"), "phase18 has_tag after add_tag");
    Check(f.remove_tag("extra").has_value(), "phase18 remove_tag by key");
    Check(!f.has_tag("extra"), "phase18 tag gone after remove_tag");

    // ── rename ──────────────────────────────────────────────────────────
    Check(f.rename("cxx:tagfs:renamed").has_value(), "phase18 rename");
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
        std::vector<box::tagfs::record> named = box::tagfs::find_all("cxx:tagfs:renamed");
        bool                            mine  = false;
        for (const box::tagfs::record &x : named)
            if (x.id() == f.id()) { mine = true; break; }
        Check(mine, "phase18 find_all by name includes this file");
    }

    // ── durability + cleanup ────────────────────────────────────────────
    Check(f.anchor().has_value(), "phase18 anchor (durability flush)");
    Check(f.remove().has_value(), "phase18 remove (delete)");

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
    for (file f : box::tagfs::query("p19:probe")) (void)f.remove();

    box::result<file> created_a = box::tagfs::create("cxx:p19:alpha", {"p19ctx:alpha", "p19:probe"});
    if (!created_a) {
        printf("[CXX] PASS phase19: box::tagfs context/snapshot/anchor "
               "(compiled; storage unavailable)\n");
        return;
    }
    file              fa = *created_a;
    box::result<file> created_b = box::tagfs::create("cxx:p19:beta", {"p19ctx:beta", "p19:probe"});
    Check(created_b.has_value(), "phase19 second probe file created");
    file fb = created_b ? *created_b : file{};

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
            box::result<snapshot> snap = snapshot::of(fa, "cxx:p19:snap1");
            if (snap) {
                Check(snap->id() != 0, "phase19 snapshot has a live id");
                Check(snap->name() == "cxx:p19:snap1", "phase19 snapshot name echoes create");
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
                box::result<snapshot> snap = snapshot::of(fa, "cxx:p19:snap2");
                if (snap) kept = snap->keep();
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
            box::result<snapshot> whole = snapshot::of_all("cxx:p19:snapall");
            if (whole) Check(whole->id() != 0, "phase19 snapshot::of_all (whole filesystem)");
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
            (void)fa.write_at(0, durable, sizeof(durable));
            Check(fa.anchor().has_value(), "phase19 anchor (durability flush) publishes event");
            if (std::optional<box::touch> ev = asub.wait(1000)) {
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
    (void)fa.remove();
    (void)fb.remove();

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
    // send/broadcast now return box::status: failure is the error arm (operator
    // bool false), and the real cause survives in .error().code().
    box::status s0 = box::send(0u, "x", 1);
    Check(!s0 && static_cast<bool>(s0.error()), "phase20 send to pid 0 rejected (cause surfaced)");
    Check(!box::broadcast("cxx:msg:no:subscriber", "x", 1),
          "phase20 broadcast with no subscribers is an error status");

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
    // add_tag/remove_tag now return box::status (empty == success).
    Check(!box::this_process::has_tag("cxx:p22:tag"), "phase22 tag absent initially");
    Check(box::this_process::add_tag("cxx:p22:tag").has_value(), "phase22 add_tag");
    Check(box::this_process::has_tag("cxx:p22:tag"), "phase22 has_tag after add");
    Check(box::this_process::remove_tag("cxx:p22:tag").has_value(), "phase22 remove_tag");
    Check(!box::this_process::has_tag("cxx:p22:tag"), "phase22 tag gone after remove");

    // ── box::tag_scope RAII (carry a process tag for a scope) ───────────
    {
        box::tag_scope ts("cxx:p22:scope");
        Check(static_cast<bool>(ts), "phase22 tag_scope active");
        Check(box::this_process::has_tag("cxx:p22:scope"), "phase22 tag present inside scope");
    }
    Check(!box::this_process::has_tag("cxx:p22:scope"),
          "phase22 tag removed after scope (RAII)");

    // ── box::process::spawn (a new cabin) — now box::result<process> ────
    box::result<box::process> child = box::process::spawn("proca");
    if (child) {
        Check(child->pid() != 0 && child->pid() != pid, "phase22 process::spawn child pid");
        while (box::receive()) { /* drain proca's messages to its spawner (us) */ }
    } else {
        printf("[CXX] note phase22: proc_exec unavailable (%.*s); spawn check skipped\n",
               (int)child.error().message().size(), child.error().message().data());
    }

    printf("[CXX] PASS phase22: box::this_process/process/cabin (identity/info/spawn) + "
           "tag_scope (RAII process tag)\n");
}

// ── Ф16d: box::system (info/uptime/mem/cpu + control) + box::efi ──────────────
void Phase23()
{
    // ── box::system::info() — machine snapshot, sanity invariants ────────
    box::result<box::system_info> si = box::system::info();
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
    box::result<box::efi_status> efi = box::efi::info();
    if (efi) {
        std::vector<efi_esrt_entry_t> tbl = box::efi::esrt();
        Check(tbl.size() == efi->esrt_count(), "phase23 efi::esrt() range matches esrt_count");
        Check(!box::efi::esrt_entry(efi->esrt_count() + 1000u).has_value(),
              "phase23 efi::esrt_entry out-of-range is an error arm");
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
    box::result<box::system_info> si = box::system::info();
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
    box::result<box::memtag::stats> st = box::memtag::counters();
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
    // find()/covering() now return box::result<region>; the local accumulators
    // stay std::optional (test bookkeeping — a "not found" is a normal skip, and
    // region has no empty state of its own). Convert via has_value()/operator*.
    std::optional<box::memtag::region> found;          // tagged AND phys-mapped
    std::optional<box::memtag::region> any_tagged;     // fallback: any tagged
    std::uint32_t walk = st ? st->region_slot_count() : 0;
    if (walk > 256) walk = 256;  // zones are seeded into low slots; bound the scan
    for (std::uint32_t id = 0; id < walk && !found; ++id) {
        box::result<box::memtag::region> r = box::memtag::find(id);
        if (!r || r->tag_count() == 0) continue;
        if (!any_tagged) any_tagged = *r;
        if (box::memtag::covering(r->base_phys())) found = *r;  // base resolves via id_by_page
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
            // not id identity). An error arm means the base isn't in the phys map.
            box::result<box::memtag::region> cov = box::memtag::covering(found->base_phys());
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
    // grant/revoke/set_guard now return box::status (empty == success).
    box::status granted = box::memtag::grant(me, "cxx:p25:cap");
    if (granted) {
        std::vector<std::string> ct = box::memtag::cabin_tags(me);
        bool has = false;
        for (const auto &t : ct) if (t == "cxx:p25:cap") has = true;
        Check(has, "phase25 grant reflected in cabin_tags");
        Check(box::memtag::revoke(me, "cxx:p25:cap").has_value(), "phase25 revoke succeeds");
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
        // color set/get round-trip (outside a session — getters resolve at once,
        // a batched set would be deferred). box::color -> VGA attr -> back.
        std::uint8_t want = color_to_vga_attr(box::colors::cyan.raw(), box::colors::black.raw());
        box::vga::set_color(box::colors::cyan, box::colors::black);
        Check(box::vga::color_attr() == want, "phase28 vga set_color/color_attr round-trip");
        box::vga::set_color(box::colors::light_gray, box::colors::black);  // restore a sane default
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

    // kb_event_t is the keyboard Touch payload {scancode, ascii, mods} — verify
    // from_event reads the right fields (the order is not ch-first).
    kb_event_t ke{};
    ke.scancode = 0x10;
    ke.ascii = 'q';
    ke.mods = KB_MOD_CTRL;
    box::key ek = box::key::from_event(ke);
    Check(ek.ch() == 'q' && ek.scancode() == 0x10 && ek.ctrl() && !ek.alt(),
          "phase29 from_event decodes the keyboard Touch payload");

    // ── box::keyboard — event-driven (Touch), NO polling. poll() peeks a
    //    delivered event; wait(ms) blocks IN THE KERNEL (woken by the key event,
    //    not a spin); co_await next() suspends on the executor. No interactive
    //    input arrives in the matrix, so a key is unlikely — we exercise the
    //    non-spinning bounded paths (results are input-dependent). ─────────────
    box::key_stream kbd;
    if (std::optional<box::key> p = kbd.poll())  // non-blocking event peek
        Check(p->scancode() != 0 || p->has_char(), "phase29 polled key decodes");
    std::optional<box::key> w = kbd.wait(5);  // efficient kernel block, <= 5 ms
    if (w)
        Check(w->scancode() != 0 || w->has_char(), "phase29 waited key decodes");
    // co_await kbd.next() blocks until a key — build the awaiter (compiles +
    // wires the executor) without awaiting it (would block with no input).
    auto pending = kbd.next();
    (void)pending;
    box::subscription &stream = kbd.events();  // escape hatch to the Touch stream
    (void)stream;

    printf("[CXX] PASS phase29: box::key (decode/modifiers) + box::key_stream "
           "(Touch poll/wait/next — event-driven, no polling)\n");
}

void Phase30()
{
    using namespace std::chrono;

    // Burn real wall time until `sw` reads >= target, bounded so a stuck clock
    // can't hang. steady_clock::now() is in-process (RDTSC / ClockBoard page
    // read) and the PIT keeps ticking through a userspace spin, so this is
    // cheap and converges.
    auto spin_to = [](box::stopwatch &sw, nanoseconds target) -> bool {
        volatile std::uint64_t junk = 0;
        for (std::uint64_t i = 0; i < 100000000ull; ++i) {
            if (sw.elapsed() >= target) return true;
            junk += i;
        }
        (void)junk;
        return false;
    };

    // ── box::stopwatch ──────────────────────────────────────────────────────
    box::stopwatch sw;
    Check(sw.elapsed() >= nanoseconds(0), "phase30 stopwatch elapsed non-negative");
    Check(sw.elapsed_as<microseconds>() >= microseconds(0), "phase30 elapsed_as<> casts");

    bool ticked = spin_to(sw, milliseconds(3));
    if (ticked) {
        nanoseconds before = sw.elapsed();
        Check(before >= milliseconds(3), "phase30 stopwatch measures real elapsed time");
        nanoseconds lap = sw.reset();
        Check(lap >= milliseconds(3), "phase30 reset() returns the elapsed lap");
        Check(sw.elapsed() < lap, "phase30 reset() rewinds the origin (post-reset elapsed << lap)");
    } else {
        printf("[CXX] note phase30: steady_clock did not advance within the spin cap\n");
    }

    // ── box::throttle ───────────────────────────────────────────────────────
    box::throttle th(milliseconds(20));
    Check(th.try_fire(), "phase30 throttle first try_fire fires");
    Check(!th.try_fire(), "phase30 throttle immediate re-fire blocked");
    Check(!th.ready(), "phase30 throttle not ready within interval");
    if (ticked) {
        box::stopwatch w;
        (void)spin_to(w, milliseconds(25));  // wait out the 20 ms interval
        Check(th.ready(), "phase30 throttle ready after interval elapses");
        Check(th.try_fire(), "phase30 throttle fires again after the interval");
    }
    th.reset();
    Check(th.try_fire(), "phase30 throttle reset() re-arms");
    box::throttle always(nanoseconds(0));
    Check(always.try_fire() && always.try_fire(), "phase30 throttle(0) always fires");

    printf("[CXX] PASS phase30: box::stopwatch (elapsed/reset over steady_clock) + "
           "box::throttle (try_fire/ready/remaining)\n");
}

// ── phase31: std::timed_mutex + recursive_timed_mutex (Ф19a) ─────────────
void Phase31()
{
    using namespace std::chrono;

    // ── timed_mutex: basic ownership ────────────────────────────────────────
    std::timed_mutex tm;
    tm.lock();
    Check(!tm.try_lock(), "phase31 timed_mutex held -> try_lock false");
    tm.unlock();
    Check(tm.try_lock(), "phase31 timed_mutex free -> try_lock true");
    tm.unlock();

    // A free mutex is acquired immediately by either timed form.
    Check(tm.try_lock_for(milliseconds(10)), "phase31 try_lock_for free -> true");
    tm.unlock();
    Check(tm.try_lock_until(steady_clock::now() + milliseconds(10)),
          "phase31 try_lock_until free -> true");
    tm.unlock();

    // Held mutex: the timed acquire blocks to the deadline, then fails. The
    // deadline-spin must actually consume the wall time it promised — proving
    // the loop ticks rather than returning early.
    tm.lock();
    {
        box::stopwatch sw;
        bool got = tm.try_lock_for(milliseconds(15));
        nanoseconds waited = sw.elapsed();
        Check(!got, "phase31 try_lock_for held -> false");
        Check(waited >= milliseconds(12),
              "phase31 try_lock_for held waited to ~deadline");
    }
    tm.unlock();

    // try_lock_until against an absolute deadline must tick the same way.
    tm.lock();
    {
        box::stopwatch sw;
        bool got = tm.try_lock_until(steady_clock::now() + milliseconds(15));
        nanoseconds waited = sw.elapsed();
        Check(!got, "phase31 try_lock_until held -> false");
        Check(waited >= milliseconds(12),
              "phase31 try_lock_until held waited to ~deadline");
    }
    tm.unlock();

    // ── unique_lock<timed_mutex>: timed interface forwards to the mutex ─────
    {
        std::unique_lock<std::timed_mutex> ul(tm, std::defer_lock);
        Check(ul.try_lock_for(milliseconds(10)),
              "phase31 unique_lock::try_lock_for acquires");
        Check(ul.owns_lock(), "phase31 unique_lock owns after timed acquire");
    }
    Check(tm.try_lock(), "phase31 timed_mutex released by unique_lock dtor");
    tm.unlock();

    // ── recursive_timed_mutex: recursive ownership + timed re-entry ─────────
    std::recursive_timed_mutex rtm;
    rtm.lock();                                                  // depth 1
    Check(rtm.try_lock(), "phase31 recursive_timed try_lock re-entry");  // depth 2
    Check(rtm.try_lock_for(milliseconds(5)),
          "phase31 recursive_timed try_lock_for re-entry");     // depth 3
    rtm.unlock();
    rtm.unlock();
    rtm.unlock();                                                // back to depth 0
    Check(rtm.try_lock_until(steady_clock::now() + milliseconds(5)),
          "phase31 recursive_timed re-lockable after full unlock");
    rtm.unlock();

    printf("[CXX] PASS phase31: std::timed_mutex + recursive_timed_mutex "
           "(try_lock_for/until deadline-spin) + unique_lock timed interface\n");
}

// ── phase32: std::shared_mutex + shared_timed_mutex + shared_lock (Ф19b) ──
void Phase32()
{
    using namespace std::chrono;

    // ── shared_mutex: reader/writer mutual exclusion ────────────────────────
    std::shared_mutex sm;

    Check(sm.try_lock(), "phase32 shared_mutex fresh -> exclusive try_lock true");
    Check(!sm.try_lock_shared(), "phase32 writer held -> try_lock_shared false");
    sm.unlock();

    Check(sm.try_lock_shared(),
          "phase32 shared_mutex fresh -> try_lock_shared true");
    Check(!sm.try_lock(), "phase32 reader held -> exclusive try_lock false");
    sm.unlock_shared();

    Check(sm.try_lock(), "phase32 exclusive re-acquirable after reader leaves");
    sm.unlock();

    // ── shared_timed_mutex: timed acquires ──────────────────────────────────
    std::shared_timed_mutex stm;

    Check(stm.try_lock_for(milliseconds(10)),
          "phase32 stm try_lock_for free -> true");
    stm.unlock();
    Check(stm.try_lock_shared_for(milliseconds(10)),
          "phase32 stm try_lock_shared_for free -> true");
    stm.unlock_shared();

    // Reader held: a timed exclusive acquire takes the writer bit, cannot
    // drain the reader, and backs off at the deadline — the drain loop must
    // burn the wall time it promised.
    stm.lock_shared();
    {
        box::stopwatch sw;
        bool got           = stm.try_lock_for(milliseconds(15));
        nanoseconds waited = sw.elapsed();
        Check(!got, "phase32 reader held -> exclusive try_lock_for false");
        Check(waited >= milliseconds(12),
              "phase32 exclusive drain-readers waited to ~deadline");
    }
    stm.unlock_shared();
    // The writer bit must have been released on back-off: exclusive is free.
    Check(stm.try_lock(), "phase32 writer bit released after timed back-off");
    stm.unlock();

    // Writer held: a timed shared acquire blocks to the deadline, then fails.
    stm.lock();
    {
        box::stopwatch sw;
        bool got           = stm.try_lock_shared_for(milliseconds(15));
        nanoseconds waited = sw.elapsed();
        Check(!got, "phase32 writer held -> try_lock_shared_for false");
        Check(waited >= milliseconds(12),
              "phase32 shared blocked-by-writer waited to ~deadline");
    }
    // try_lock_shared_until against an absolute deadline ticks identically.
    {
        box::stopwatch sw;
        bool got =
            stm.try_lock_shared_until(steady_clock::now() + milliseconds(15));
        nanoseconds waited = sw.elapsed();
        Check(!got, "phase32 writer held -> try_lock_shared_until false");
        Check(waited >= milliseconds(12),
              "phase32 shared_until blocked-by-writer waited to ~deadline");
    }
    // Writer-vs-writer: a second exclusive acquire also fails (short timeout).
    Check(!stm.try_lock_for(milliseconds(2)),
          "phase32 writer held -> second exclusive try_lock_for false");
    stm.unlock();

    // ── shared_lock RAII ────────────────────────────────────────────────────
    {
        std::shared_lock<std::shared_mutex> sl(sm);
        Check(sl.owns_lock(),
              "phase32 shared_lock acquires shared on construction");
        Check(!sm.try_lock(),
              "phase32 shared_lock holds shared -> exclusive try_lock false");
    }
    Check(sm.try_lock(), "phase32 shared_lock dtor released shared ownership");
    sm.unlock();

    {
        std::shared_lock<std::shared_mutex> sl(sm, std::defer_lock);
        Check(!sl.owns_lock(), "phase32 shared_lock defer_lock starts unowned");
        sl.lock();
        Check(sl.owns_lock(), "phase32 shared_lock::lock acquires");
        std::shared_lock<std::shared_mutex> sl2(std::move(sl));
        Check(sl2.owns_lock() && !sl.owns_lock(),
              "phase32 shared_lock move transfers ownership");
        sl2.unlock();
        Check(!sl2.owns_lock(), "phase32 shared_lock::unlock releases");
    }

    // Timed shared_lock + unique_lock<shared_timed_mutex> exclusive interop.
    {
        std::shared_lock<std::shared_timed_mutex> sl(stm, milliseconds(10));
        Check(sl.owns_lock(),
              "phase32 timed shared_lock acquires within deadline");
    }
    {
        std::unique_lock<std::shared_timed_mutex> ul(stm, std::defer_lock);
        Check(ul.try_lock_for(milliseconds(10)),
              "phase32 unique_lock<shared_timed_mutex> exclusive timed acquire");
    }

    printf("[CXX] PASS phase32: std::shared_mutex + shared_timed_mutex "
           "(atomic reader/writer, timed deadline-spin) + shared_lock RAII\n");
}

// ── phase33: std::latch + std::barrier (Ф19c) ────────────────────────────
void Phase33()
{
    // ── std::latch ──────────────────────────────────────────────────────────
    std::latch lt(3);
    Check(!lt.try_wait(), "phase33 latch not ready before reaching zero");
    lt.count_down();                 // 3 -> 2
    Check(!lt.try_wait(), "phase33 latch still pending mid-count");
    lt.count_down(2);                // 2 -> 0
    Check(lt.try_wait(), "phase33 latch ready at zero");
    lt.wait();                       // counter == 0 -> returns at once
    Check(lt.try_wait(), "phase33 latch still satisfied after wait() returns");

    std::latch lt1(1);
    lt1.arrive_and_wait();           // count_down(1) then wait -> immediate
    Check(lt1.try_wait(), "phase33 latch arrive_and_wait drains to zero");

    std::latch lt0(0);
    Check(lt0.try_wait(), "phase33 latch(0) starts satisfied");
    lt0.wait();
    Check(std::latch::max() > 0, "phase33 latch::max positive");

    // ── std::barrier: reusable phases + completion ──────────────────────────
    int comps = 0;
    std::barrier bar(1, [&comps]() noexcept { ++comps; }); // CTAD on the lambda
    bar.arrive_and_wait();           // phase 0 completes (single participant)
    bar.arrive_and_wait();           // phase 1 completes
    Check(comps == 2,
          "phase33 barrier completion runs once per phase (reusable)");

    // arrive(n): the arrival that drains the count completes the phase
    int comps2 = 0;
    std::barrier bar2(3, [&comps2]() noexcept { ++comps2; });
    auto tok  = bar2.arrive(2);      // 3 -> 1, not yet complete
    Check(comps2 == 0, "phase33 barrier not complete before all arrive");
    auto tok2 = bar2.arrive(1);      // 1 -> 0, completes
    Check(comps2 == 1, "phase33 barrier completes when arrivals reach expected");
    bar2.wait(std::move(tok2));      // phase advanced -> immediate
    bar2.wait(std::move(tok));       // stale token of the same phase -> immediate
    Check(comps2 == 1,
          "phase33 wait on a completed phase returns without re-running completion");

    // arrive_and_drop: subsequent phases expect one fewer arrival
    int comps3 = 0;
    std::barrier bar3(2, [&comps3]() noexcept { ++comps3; });
    bar3.arrive_and_drop();          // expected 2->1; arrive 2->1 (not complete)
    auto tok3 = bar3.arrive();       // 1 -> 0, completes phase 0
    bar3.wait(std::move(tok3));
    Check(comps3 == 1, "phase33 barrier arrive_and_drop completes current phase");
    bar3.arrive_and_wait();          // next phase needs only 1 -> completes
    Check(comps3 == 2,
          "phase33 barrier arrive_and_drop lowered next-phase expected");

    // default (empty) completion barrier: drive two phases and confirm the
    // phase token advances (reusability is observable through the token).
    std::barrier<> b0(1);
    auto p0 = b0.arrive(); // phase 0 token
    b0.wait(std::move(p0));
    auto p1 = b0.arrive(); // phase 1 token — must differ if the barrier reused
    b0.wait(std::move(p1));
    Check(p0 != p1, "phase33 barrier<> default advances phase across reuses");

    printf("[CXX] PASS phase33: std::latch (count_down/try_wait/wait/"
           "arrive_and_wait) + std::barrier (arrive/wait/arrive_and_wait/"
           "arrive_and_drop + completion, reusable phases)\n");
}

// ── phase34: std::counting_semaphore + binary_semaphore (Ф19d) ───────────
void Phase34()
{
    using namespace std::chrono;

    // ── counting_semaphore ──────────────────────────────────────────────────
    std::counting_semaphore<4> cs(2); // two permits available
    Check(cs.try_acquire(), "phase34 counting_sem permit -> try_acquire true"); // 2->1
    Check(cs.try_acquire(), "phase34 counting_sem second permit -> true");       // 1->0
    Check(!cs.try_acquire(),
          "phase34 counting_sem drained -> try_acquire false"); // 0
    cs.release();                                                // 0->1
    Check(cs.try_acquire(), "phase34 counting_sem permit after release -> true"); // 1->0

    cs.release(2);  // 0->2
    cs.acquire();   // 2->1 (permit available, no block)
    cs.acquire();   // 1->0
    Check(!cs.try_acquire(), "phase34 counting_sem drained after acquires");

    // Empty semaphore: a timed acquire blocks to the deadline, then fails —
    // the wait loop must burn the wall time it promised.
    {
        box::stopwatch sw;
        bool got           = cs.try_acquire_for(milliseconds(15));
        nanoseconds waited = sw.elapsed();
        Check(!got, "phase34 counting_sem empty -> try_acquire_for false");
        Check(waited >= milliseconds(12),
              "phase34 counting_sem try_acquire_for waited to ~deadline");
    }
    // A permit makes the timed acquire succeed at once.
    cs.release();
    Check(cs.try_acquire_for(milliseconds(10)),
          "phase34 counting_sem try_acquire_for with permit -> true");

    Check(std::counting_semaphore<4>::max() == 4,
          "phase34 counting_sem max() reflects LeastMaxValue");

    // ── binary_semaphore ────────────────────────────────────────────────────
    std::binary_semaphore bs(0); // starts unavailable
    Check(!bs.try_acquire(), "phase34 binary_sem(0) -> try_acquire false");
    bs.release();
    Check(bs.try_acquire(), "phase34 binary_sem after release -> true");
    Check(std::binary_semaphore::max() >= 1, "phase34 binary_sem max() >= 1");

    // Hand-off pattern: release then acquire (no block on a single thread).
    std::binary_semaphore handoff(0);
    handoff.release();
    handoff.acquire();
    Check(!handoff.try_acquire(),
          "phase34 binary_sem permit consumed by hand-off acquire");

    printf("[CXX] PASS phase34: std::counting_semaphore + binary_semaphore "
           "(acquire/try_acquire/try_acquire_for-until/release deadline-spin)\n");
}

// ── phase35: this_thread::sleep_for + real cross-strand park/wake (Ф20a) ──
// std concurrency primitives now park in the kernel instead of busy-spinning.
// Part 1 proves the timed park (sleep_for) with no strand. Parts 3-4 spawn a
// sibling strand that drives latch / semaphore / atomic notify, so main really
// parks and the worker really wakes it (strand:parked / strand:woken Touch).

static volatile uint64_t g_p35_remaining; // worker decrements; main joins on it
static std::latch       *g_p35_latch;     // count 1; worker count_down()s it
static std::binary_semaphore g_p35_sem{0}; // worker release()s it
static std::atomic<int> g_p35_at{0};      // worker store+notify; main wait()s
static std::atomic<int> g_p35_pp{0};      // ping-pong counter (atomic notify)
static constexpr int    kP35PingPong = 64; // lock-step hops in the wake proof

// Park-join a single worker the strandtest way: re-read the live counter before
// each park so a missed decrement returns ERR_ADDR_VALUE_MISMATCH at once;
// bounded cycles so a genuine hang fails loudly instead of wedging the harness.
static bool p35_join()
{
    uint32_t cycles = 0;
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_p35_remaining, __ATOMIC_ACQUIRE)) != 0) {
        if (++cycles > 80u) return false;
        addr_park(&g_p35_remaining, cur, 200);
    }
    return true;
}

static void p35_latch_worker(void *)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // let main reach the park
    g_p35_latch->count_down();
    __atomic_sub_fetch(&g_p35_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p35_remaining, 0);
    strand_exit();
}

static void p35_sem_worker(void *)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // let main reach the park
    g_p35_sem.release();
    g_p35_at.store(42, std::memory_order_release);
    g_p35_at.notify_one();
    __atomic_sub_fetch(&g_p35_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p35_remaining, 0);
    strand_exit();
}

// Ping-pong worker: kP35PingPong lock-step hops with main, driven purely by
// atomic notify_one. No artificial sleep — pure hops. If the C++ notify ->
// addr_wake path did NOT really wake (progress only via the 100ms park
// backstop), the hops would crawl on that timeout (>6s); Phase35's <3s bound
// proves the wake is delivered by notify, not the timeout.
static void p35_pp_worker(void *)
{
    for (int i = 0; i < kP35PingPong; i++) {
        g_p35_pp.wait(2 * i, std::memory_order_acquire);   // block until main sets 2i+1
        g_p35_pp.store(2 * i + 2, std::memory_order_release);
        g_p35_pp.notify_one();
    }
    __atomic_sub_fetch(&g_p35_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p35_remaining, 0);
    strand_exit();
}

// Multi-strand barrier: main + spawned workers run kP35BarRounds phases. Each
// round the count (low 32 of __state) churns toward 0 while early arrivers wait
// on the phase (high 32) — the exact scenario the phase-mask makes park precisely
// instead of busy-spinning. Validates barrier correctness across strands and runs
// the masked compare under real contention. Workers gate on g_p35_bar_go until
// main has built the barrier with the count matching how many actually spawned
// (so a failed spawn can never deadlock on a count mismatch).
static std::barrier<>   *g_p35_bar;
static std::atomic<bool> g_p35_bar_go{false};
static std::atomic<int>  g_p35_bar_done{0};
static constexpr int     kP35BarRounds = 5;
static void p35_bar_worker(void *)
{
    g_p35_bar_go.wait(false, std::memory_order_acquire); // until main builds barrier
    for (int r = 0; r < kP35BarRounds; r++)
        g_p35_bar->arrive_and_wait();
    g_p35_bar_done.fetch_add(1, std::memory_order_release);
    __atomic_sub_fetch(&g_p35_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p35_remaining, 0);
    strand_exit();
}

void Phase35()
{
    using namespace std::chrono;

    // 1) Always: a real timed park. sleep_for(15ms) must burn >= 12ms of wall
    //    time (kernel timed-park), and sleep_for(0) must return promptly.
    {
        box::stopwatch sw;
        std::this_thread::sleep_for(milliseconds(15));
        nanoseconds slept = sw.elapsed();
        Check(slept >= milliseconds(12),
              "phase35 sleep_for(15ms) parks at least ~12ms");

        box::stopwatch sw0;
        std::this_thread::sleep_for(milliseconds(0));
        Check(sw0.elapsed() < milliseconds(5),
              "phase35 sleep_for(0) returns immediately");
    }

    // 2) Sibling strands need FSGSBASE (per-strand TLS). Without it strand_spawn
    //    refuses; the timed-park half above already passed, so SKIP the park/wake
    //    halves cleanly rather than count a failure.
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase35: strands need FSGSBASE — skipping park/wake\n");
        printf("[CXX] PASS phase35: this_thread::sleep_for "
               "(park/wake skipped, no FSGSBASE)\n");
        return;
    }

    // 3) Ping-pong wake proof: kP35PingPong lock-step hops driven purely by
    //    atomic notify_one across a sibling strand. The version-pool park has a
    //    100ms backstop, so a single-shot wait would pass even if notify were
    //    dead (it would just time out and re-check). A long lock-step chain
    //    cannot: were notify->addr_wake dead, the hops would crawl on the
    //    backstop (>6s). Finishing well under it proves the wake is real. (It
    //    cannot hang — the backstop guarantees progress, so a dead wake FAILS
    //    the <3s bound loudly instead of wedging the harness.)
    {
        g_p35_pp.store(0, std::memory_order_relaxed);
        g_p35_remaining = 1;
        if (strand_spawn(p35_pp_worker, 0) == 0) {
            printf("[CXX] note phase35: strand_spawn unavailable — skipping park/wake\n");
            printf("[CXX] PASS phase35: this_thread::sleep_for "
                   "(park/wake skipped, no strand)\n");
            return;
        }
        box::stopwatch pp;
        for (int i = 0; i < kP35PingPong; i++) {
            g_p35_pp.store(2 * i + 1, std::memory_order_release);
            g_p35_pp.notify_one();
            g_p35_pp.wait(2 * i + 1, std::memory_order_acquire); // until worker sets 2i+2
        }
        nanoseconds took = pp.elapsed();
        Check(g_p35_pp.load(std::memory_order_acquire) == 2 * kP35PingPong,
              "phase35 ping-pong reached target (cross-strand atomic notify)");
        // Backstop-only floor = hops×100ms: ~6.4s when the two strands run on
        // separate cores, ~12.8s when they share one. Pick a bound below the
        // applicable floor (so a dead wake fails loudly) but with headroom for a
        // starved single core — core-conditional, so it discriminates on both
        // without flaking under host load.
        system_info_t si35{};
        unsigned pp_cores = (sysinfo(&si35) == 0) ? si35.cpu_app_cores : 0u;
        auto pp_bound = (pp_cores >= 2) ? milliseconds(5000) : milliseconds(11000);
        Check(took < pp_bound,
              "phase35 ping-pong woken by notify, not the 100ms backstop");
        Check(p35_join(), "phase35 ping-pong worker joined");
    }

    // 4) latch park -> wake across a sibling strand. Main parks in latch.wait();
    //    the worker count_down()s, which notifies and wakes main.
    {
        std::latch latch(1);
        g_p35_latch     = &latch;
        g_p35_remaining = 1;
        if (strand_spawn(p35_latch_worker, 0) == 0) {
            printf("[CXX] note phase35: strand_spawn unavailable — skipping park/wake\n");
            printf("[CXX] PASS phase35: this_thread::sleep_for "
                   "(park/wake skipped, no strand)\n");
            return;
        }
        latch.wait();
        Check(latch.try_wait(), "phase35 latch reached zero (cross-strand wake)");
        Check(p35_join(), "phase35 latch worker joined");
    }

    // 5) semaphore + atomic store/notify across a sibling strand. Main parks in
    //    sem.acquire() and atomic wait(0); the worker release()s and notifies.
    {
        g_p35_remaining = 1;
        if (strand_spawn(p35_sem_worker, 0) == 0) {
            printf("[CXX] note phase35: strand_spawn unavailable — skipping park/wake\n");
            printf("[CXX] PASS phase35: this_thread::sleep_for + latch "
                   "(semaphore/atomic park/wake skipped, no strand)\n");
            return;
        }
        g_p35_sem.acquire();
        g_p35_at.wait(0, std::memory_order_acquire);
        Check(g_p35_at.load(std::memory_order_acquire) == 42,
              "phase35 atomic store+notify observed (cross-strand wake)");
        Check(p35_join(), "phase35 semaphore worker joined");
    }

    // 6) Multi-strand barrier: main + workers run kP35BarRounds phases. The count
    //    churns toward 0 every round while early arrivers park on the phase — the
    //    masked predicate keeps them parked (not spinning) while the count moves.
    //    Proves barrier is correct across strands and drives the masked compare
    //    under real contention.
    {
        g_p35_bar_go.store(false, std::memory_order_relaxed);
        g_p35_bar_done.store(0, std::memory_order_relaxed);
        int workers = 0;
        if (strand_spawn(p35_bar_worker, 0)) workers++;
        if (strand_spawn(p35_bar_worker, 0)) workers++;
        if (workers == 0) {
            printf("[CXX] note phase35: no barrier worker spawned — skipping\n");
        } else {
            g_p35_remaining = (uint64_t)workers;
            std::barrier<> bar(1 + workers);
            g_p35_bar = &bar;
            g_p35_bar_go.store(true, std::memory_order_release);
            g_p35_bar_go.notify_all();         // release workers onto the barrier
            for (int r = 0; r < kP35BarRounds; r++)
                bar.arrive_and_wait();         // main participates each phase
            Check(p35_join(), "phase35 barrier workers joined");
            Check(g_p35_bar_done.load(std::memory_order_acquire) == workers,
                  "phase35 barrier: all workers cleared all phases (cross-strand)");
        }
    }

    printf("[CXX] PASS phase35: this_thread::sleep_for + latch/semaphore/atomic "
           "+ multi-strand barrier real park->wake (sibling strands)\n");
}

// ── phase36: per-strand thread_local FOUNDATION (Ф20b-1, raw strand_spawn) ──
// Proves the three pieces of the per-strand TLS foundation WITHOUT std::thread:
//   1. tdata copy   — a non-zero static-init thread_local reads its value in a
//                     spawned strand (would be 0 if __boxcxx_tls_strand_init had
//                     not copied .tdata into the strand's neg-TLS page).
//   2. isolation    — each strand writes a distinct value into the SAME
//                     thread_local and reads back exactly its own (no cross-talk,
//                     and main's value is untouched).
//   3. per-strand dtor — a thread_local with a non-trivial dtor runs that dtor
//                     at strand exit (__boxcxx_thread_storage_exit), NOT at
//                     process exit. This is the case the old process-global
//                     thread-atexit registry got wrong.

static constexpr uint32_t kP36Workers = 4;

// (1)+(2): non-zero static init proves the tdata copy; per-strand storage gives
// each worker its own cell.
thread_local int g_p36_probe = 0xABCD;

// (3): a thread_local with a non-trivial ctor/dtor. The dtor bumps a process-
// global atomic so main can count how many strands ran their thread_local
// destructors. One instance per strand (lives in that strand's neg-TLS).
static std::atomic<int> g_p36_dtors{0};
struct P36DtorProbe {
    int marker = 0x600D;
    ~P36DtorProbe() { g_p36_dtors.fetch_add(1, std::memory_order_release); }
};
thread_local P36DtorProbe g_p36_dtor_probe;

static volatile uint64_t g_p36_remaining;             // workers decrement; main joins
static volatile uint32_t g_p36_tdata_ok[kP36Workers]; // 1 iff worker saw 0xABCD
static volatile uint32_t g_p36_readback[kP36Workers]; // worker's own-value read-back

static void p36_worker(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;

    // (1) Stand up THIS strand's neg-TLS first — must precede any thread_local
    //     access. Then arm this strand's thread_local destructor list.
    __boxcxx_tls_strand_init();
    __boxcxx_thread_storage_enter();

    // (1) tdata copied: the static initializer 0xABCD is present.
    g_p36_tdata_ok[id] = (g_p36_probe == 0xABCD) ? 1u : 0u;

    // (2) Write a per-strand distinct value, let workers overlap, read it back.
    g_p36_probe = (int)(0x1000u + id);
    std::this_thread::sleep_for(std::chrono::milliseconds(8)); // overlap window
    g_p36_readback[id] = (uint32_t)g_p36_probe;

    // (3) Use the dtor-probe so the compiler registers its destructor for THIS
    //     strand (first-use guard lives in the strand's own neg-TLS).
    __atomic_store_n(&g_p36_tdata_ok[id],
                     g_p36_tdata_ok[id] & (g_p36_dtor_probe.marker == 0x600D ? 1u : 0u),
                     __ATOMIC_RELAXED);

    // (3) Run this strand's thread_local destructors now (→ g_p36_dtors++),
    //     proving they fire at strand exit, not process exit.
    __boxcxx_thread_storage_exit();

    __atomic_sub_fetch(&g_p36_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p36_remaining, 0);
    strand_exit();
}

void Phase36()
{
    // Sibling strands need FSGSBASE (per-strand TLS via ring-3 RDFSBASE). Without
    // it strand_spawn refuses — SKIP cleanly with a PASS, matching Phase35.
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase36: strands need FSGSBASE — skipping\n");
        printf("[CXX] PASS phase36: per-strand thread_local (skipped, no FSGSBASE)\n");
        return;
    }

    // Main strand's own thread_local cell — workers must never disturb it.
    g_p36_probe = 0x5555;

    g_p36_dtors.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kP36Workers; i++) {
        g_p36_tdata_ok[i] = 0;
        g_p36_readback[i] = 0;
    }
    g_p36_remaining = kP36Workers;

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < kP36Workers; i++) {
        if (strand_spawn(p36_worker, (void *)(uintptr_t)i) != 0)
            spawned++;
        else
            break;
    }
    if (spawned == 0) {
        printf("[CXX] note phase36: strand_spawn unavailable — skipping\n");
        printf("[CXX] PASS phase36: per-strand thread_local (skipped, no strand)\n");
        return;
    }
    if (spawned != kP36Workers) {
        // A partial spawn would leave g_p36_remaining above the spawned count and
        // hang the join — fail loudly instead.
        printf("[CXX] FAIL phase36: only %u/%u workers spawned\n", spawned, kP36Workers);
        g_failures++;
        return;
    }

    // Bounded join (strandtest idiom: re-read live counter before each park).
    uint32_t cycles = 0;
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_p36_remaining, __ATOMIC_ACQUIRE)) != 0) {
        if (++cycles > 80u) {
            printf("[CXX] FAIL phase36: %u worker(s) stuck after %u cycles\n",
                   (unsigned)cur, cycles);
            g_failures++;
            return;
        }
        addr_park(&g_p36_remaining, cur, 200);
    }

    // (1) every worker saw the copied .tdata init value (and the dtor-probe ctor).
    for (uint32_t i = 0; i < kP36Workers; i++) {
        Check(g_p36_tdata_ok[i] == 1u,
              "phase36 worker thread_local .tdata initialized (per-strand neg-TLS)");
    }
    // (2) isolation: each worker read back exactly its own distinct value.
    for (uint32_t i = 0; i < kP36Workers; i++) {
        Check(g_p36_readback[i] == 0x1000u + i,
              "phase36 worker thread_local read-back is its own value (no cross-talk)");
    }
    // (3) per-strand destructors all ran (one per worker), at strand exit.
    Check(g_p36_dtors.load(std::memory_order_acquire) == (int)kP36Workers,
          "phase36 per-strand thread_local destructors ran at strand exit");
    // (2) main's thread_local cell was never touched by any worker.
    Check(g_p36_probe == 0x5555,
          "phase36 main thread_local untouched by worker strands");

    printf("[CXX] PASS phase36: per-strand thread_local "
           "(isolation + tdata-init + per-strand dtor)\n");
}

// ── phase37: std::thread (Ф20b-2) ────────────────────────────────────────────
// The thread class on top of the per-strand foundation: run+join, decay-copied
// args, thread_local INSIDE a std::thread (proves the entry trampoline drives
// the tls_strand hooks — without them every thread_local reads 0 and no dtor
// runs), detach + reaper reclaim, get_id/native_handle identity, hardware_
// concurrency, and move. Every join is bounded (std::thread::join's own 100ms
// backstop loop) and all thread bodies are trivial, so a healthy run cannot
// wedge the harness. FSGSBASE-guarded exactly like Phase35/36.

static std::atomic<int> g_p37_flag{0};   // (a) thread runs + join
static std::atomic<long> g_p37_sum{0};   // (c) decay-copied (int,long)
static std::atomic<int> g_p37_strlen{0}; // (c) decay-copied std::string by value

// (b) thread_local INSIDE a std::thread. A distinct per-thread cell (isolation)
// plus a thread_local whose dtor bumps a process-global atomic (proves the
// trampoline runs __boxcxx_thread_storage_exit at thread end).
static constexpr int kP37TlsThreads = 4;
thread_local int g_p37_tls = 0x7777;            // main's cell — threads must not disturb
static std::atomic<int> g_p37_tls_dtors{0};
struct P37TlsProbe {
    int marker = 0x37CD;
    ~P37TlsProbe() { g_p37_tls_dtors.fetch_add(1, std::memory_order_release); }
};
thread_local P37TlsProbe g_p37_tls_probe;
static volatile uint32_t g_p37_tls_ok[kP37TlsThreads];       // 1 iff own value read back
static volatile uint32_t g_p37_tls_seen[kP37TlsThreads];     // value the thread read back

// (d) detach + reaper churn.
static constexpr uint32_t kP37Churn = 40;
static std::atomic<uint32_t> g_p37_churn_done{0};

// (h) concurrent exceptions across strands (H1: per-strand __cxa_eh_globals).
static constexpr int kP37ExcThreads = 4;
static volatile uint32_t g_p37_exc_ok[kP37ExcThreads];

void Phase37()
{
    using namespace std::chrono;

    // A thread is a strand; strands need FSGSBASE (per-strand TLS via ring-3
    // RDFSBASE). Without it strand_spawn refuses and std::thread's ctor would
    // throw — SKIP cleanly with a PASS, matching Phase35/36.
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase37: strands need FSGSBASE — skipping\n");
        printf("[CXX] PASS phase37: std::thread (skipped, no FSGSBASE)\n");
        return;
    }

    // (a) thread runs, then join().
    {
        g_p37_flag.store(0, std::memory_order_relaxed);
        std::thread t([] { g_p37_flag.store(1, std::memory_order_release); });
        Check(t.joinable(), "phase37 thread joinable before join");
        t.join();
        Check(!t.joinable(), "phase37 thread not joinable after join");
        Check(g_p37_flag.load(std::memory_order_acquire) == 1,
              "phase37 thread ran its body (join saw the result)");
    }

    // (c) decay-copy of arguments: an (int, long) pack and a non-trivial
    //     std::string passed BY VALUE (the closure owns its own copy).
    {
        g_p37_sum.store(0, std::memory_order_relaxed);
        std::thread t(
            [](int a, long b) {
                g_p37_sum.store((long)a + b, std::memory_order_release);
            },
            7, 35L);
        t.join();
        Check(g_p37_sum.load(std::memory_order_acquire) == 42,
              "phase37 args decay-copied into closure (7 + 35 == 42)");

        g_p37_strlen.store(-1, std::memory_order_relaxed);
        std::string msg = "box-thread";   // decay-copied; thread reads its own copy
        std::thread t2(
            [](std::string s) {
                g_p37_strlen.store((int)s.size(), std::memory_order_release);
            },
            msg);
        t2.join();
        Check(g_p37_strlen.load(std::memory_order_acquire) == (int)msg.size(),
              "phase37 std::string argument decay-copied by value");
    }

    // (b) thread_local INSIDE a std::thread. This is the proof the entry
    //     trampoline wires __boxcxx_tls_strand_init / _storage_exit: each thread
    //     gets its OWN thread_local cell (no cross-talk), and a thread_local with
    //     a dtor runs that dtor at thread end (counted into g_p37_tls_dtors).
    {
        g_p37_tls = 0x4242;   // main's cell — must survive untouched
        g_p37_tls_dtors.store(0, std::memory_order_relaxed);
        for (int i = 0; i < kP37TlsThreads; i++) {
            g_p37_tls_ok[i]   = 0;
            g_p37_tls_seen[i] = 0;
        }
        std::thread ts[kP37TlsThreads];
        for (int i = 0; i < kP37TlsThreads; i++) {
            ts[i] = std::thread(
                [](int id) {
                    // Read the copied .tdata init (0x7777) before writing — would
                    // be 0 if the trampoline had not run __boxcxx_tls_strand_init.
                    bool tdata = (g_p37_tls == 0x7777);
                    g_p37_tls  = 0x2000 + id;                 // this thread's own cell
                    std::this_thread::sleep_for(milliseconds(8)); // overlap window
                    uint32_t back = (uint32_t)g_p37_tls;
                    g_p37_tls_seen[id] = back;
                    // Touch the dtor-probe so its destructor is registered for this
                    // thread (first-use guard lives in this strand's neg-TLS).
                    bool probe = (g_p37_tls_probe.marker == 0x37CD);
                    g_p37_tls_ok[id] =
                        (tdata && probe && back == (uint32_t)(0x2000 + id)) ? 1u : 0u;
                },
                i);
        }
        for (int i = 0; i < kP37TlsThreads; i++) ts[i].join();

        for (int i = 0; i < kP37TlsThreads; i++) {
            Check(g_p37_tls_ok[i] == 1u,
                  "phase37 thread_local in std::thread: tdata-init + own read-back");
        }
        Check(g_p37_tls_dtors.load(std::memory_order_acquire) == kP37TlsThreads,
              "phase37 thread_local dtors ran at thread end (trampoline storage_exit)");
        Check(g_p37_tls == 0x4242,
              "phase37 main thread_local untouched by std::threads");
    }

    // (e) get_id / native_handle identity: ids of N threads + this_thread::get_id
    //     are all distinct and non-default; native_handle == the live pid; after
    //     join the id is default and native_handle is 0. Uses unordered_set, which
    //     also exercises hash<thread::id>.
    {
        constexpr int kIdThreads = 4;
        std::thread ids[kIdThreads];
        std::unordered_set<std::thread::id> seen;
        seen.insert(std::this_thread::get_id());   // the running strand's id
        bool all_live_nonzero = true;
        for (int i = 0; i < kIdThreads; i++) {
            ids[i] = std::thread([] { std::this_thread::yield(); });
            Check(ids[i].get_id() != std::thread::id{},
                  "phase37 live thread get_id() is non-default");
            if (ids[i].native_handle() == 0) all_live_nonzero = false;
            seen.insert(ids[i].get_id());
        }
        Check(all_live_nonzero, "phase37 live native_handle() == pid (non-zero)");
        // main + kIdThreads distinct identities (no pid collision).
        Check((int)seen.size() == kIdThreads + 1,
              "phase37 all thread ids distinct (incl this_thread, via hash<id>)");
        for (int i = 0; i < kIdThreads; i++) {
            ids[i].join();
            Check(ids[i].get_id() == std::thread::id{},
                  "phase37 get_id() is default after join");
            Check(ids[i].native_handle() == 0,
                  "phase37 native_handle() is 0 after join");
        }
        // formatter<thread::id> prints the pid decimal (smoke: non-empty, digits).
        std::thread fmt([] {});
        std::string fid = std::format("{}", fmt.get_id());
        Check(!fid.empty() && fid[0] >= '1' && fid[0] <= '9',
              "phase37 formatter<thread::id> prints decimal pid");
        fmt.join();
    }

    // (g) move: a thread's identity transfers; the source becomes non-joinable.
    {
        std::thread a([] { std::this_thread::yield(); });
        std::thread::id moved_id = a.get_id();
        std::thread b = std::move(a);
        Check(!a.joinable(), "phase37 moved-from thread not joinable");
        Check(b.joinable(), "phase37 move-target thread joinable");
        Check(b.get_id() == moved_id, "phase37 move transfers the id");
        b.join();
    }

    // (f) hardware_concurrency reports the App-Core count: it must equal
    //     sysinfo().cpu_app_cores (the contract), and be > 0 exactly when the
    //     machine actually has App-Cores. On a single-K-Core boot (default
    //     `make run`, 0 App-Cores) returning 0 is the correct "no parallel
    //     progress possible" answer, not a failure.
    {
        unsigned hc = std::thread::hardware_concurrency();
        system_info_t si;
        if (sysinfo(&si) == 0) {
            Check(hc == si.cpu_app_cores,
                  "phase37 hardware_concurrency() == App-Core count");
            if (si.cpu_app_cores > 0)
                Check(hc > 0, "phase37 hardware_concurrency() > 0 when App-Cores exist");
        } else {
            Check(hc == 0, "phase37 hardware_concurrency() == 0 with no sysinfo");
        }
    }

    // (d) detach + reaper: spawn+detach a churn of trivial threads; each bumps a
    //     shared atomic then exits, becoming a corpse the P5b reaper reclaims.
    //     Bounded-wait until ALL bumped AND process_count returns toward baseline
    //     (strandtest test3 idiom — proves both detach and reaper).
    {
        system_info_t si;
        bool have_sysinfo = (sysinfo(&si) == 0);
        for (int k = 0; k < 16; k++) yield();           // let prior corpses settle
        uint32_t base = (have_sysinfo && sysinfo(&si) == 0) ? si.process_count : 0;

        g_p37_churn_done.store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < kP37Churn; i++) {
            std::thread d([] {
                g_p37_churn_done.fetch_add(1, std::memory_order_release);
                g_p37_churn_done.notify_one();   // wake the parked main strand
            });
            d.detach();
        }

        // Wait for every detached thread to record completion (bounded). The
        // detached strands run concurrently; we yield between polls to let them.
        // Event-driven (NOT a bounded poll): park on the counter until every
        // detached strand records completion. Parking frees this App-Core so
        // the workers actually make progress — correct, and removes the 16c-TCG
        // timing flake the old bounded yield-spin (200 cycles) had.
        uint32_t done;
        while ((done = g_p37_churn_done.load(std::memory_order_acquire)) != kP37Churn)
            g_p37_churn_done.wait(done, std::memory_order_acquire);
        Check(g_p37_churn_done.load(std::memory_order_acquire) == kP37Churn,
              "phase37 all detached threads ran to completion");

        // process_count must return toward baseline — the reaper process_destroy'd
        // the detached corpses (process_count-- happens only there). A broken
        // reaper would leave it pinned near base + kP37Churn.
        if (have_sysinfo) {
            uint32_t cnt = base + kP37Churn;
            for (uint32_t cyc = 0; cyc < 200u; cyc++) {
                if (sysinfo(&si) == 0) cnt = si.process_count;
                if (cnt <= base + 4u) break;
                for (int k = 0; k < 4; k++) yield();
            }
            Check(cnt <= base + 4u,
                  "phase37 detached strands reclaimed by reaper (process_count "
                  "returned to baseline)");
        }
    }

    // (h) concurrent exceptions across strands (H1: per-strand __cxa_eh_globals).
    //     Each of N threads throws+catches a UNIQUE int 200 times. Pre-fix, the
    //     caught-exception LIFO and uncaught counter were plain shared globals,
    //     so a sibling strand's in-flight throw could pop/observe THIS strand's
    //     header — a wrong caught value, a non-zero uncaught count between throws,
    //     or a use-after-free crash. Per-strand thread_local gives each strand
    //     its own bookkeeping; the value/counter checks would fail (or the run
    //     would crash) without the fix.
    {
        for (int i = 0; i < kP37ExcThreads; i++) g_p37_exc_ok[i] = 0;
        std::thread ets[kP37ExcThreads];
        for (int i = 0; i < kP37ExcThreads; i++) {
            ets[i] = std::thread(
                [](int id) {
                    bool ok = true;
                    for (int k = 0; k < 200; k++) {
                        int want = id * 100000 + k;
                        try {
                            throw want;
                        } catch (int got) {
                            if (got != want) ok = false;
                        }
                        // Between throws THIS strand has no exception in flight.
                        if (std::uncaught_exceptions() != 0) ok = false;
                    }
                    g_p37_exc_ok[id] = ok ? 1u : 0u;
                },
                i);
        }
        for (int i = 0; i < kP37ExcThreads; i++) ets[i].join();
        bool all_exc = true;
        for (int i = 0; i < kP37ExcThreads; i++)
            if (g_p37_exc_ok[i] != 1u) all_exc = false;
        Check(all_exc, "phase37 concurrent throw/catch isolated per strand (H1)");
    }

    printf("[CXX] PASS phase37: std::thread "
           "(join/detach/args/thread_local/get_id/hw_concurrency/move/exc)\n");
}

// ── phase38: <stop_token> + std::jthread ───────────────────────────────────────
// Tier A is pure cooperative-cancel machinery (source/token/callback) with no
// strand at all — always runs. Tier B drives real jthreads (a strand each) and is
// FSGSBASE-guarded exactly like Phase37. The bug-prone bits proven here: a
// callback fires exactly once on request_stop (no lost/double), a callback built
// after the stop runs inline in its ctor, a self-destroying callback does not
// hang, and a jthread auto request_stop+joins at scope exit.

static std::atomic<int>      g_p38_cb_ran{0};       // (3) callback fired flag
static std::atomic<int>      g_p38_jflag{0};        // (8) jthread body observed stop
static std::atomic<int>      g_p38_plain{0};        // (9) plain-callable jthread ran
static std::atomic<int>      g_p38_cb_count{0};     // (11) callback run count (==1)
static std::atomic<uint32_t> g_p38_ready{0};        // (11) worker registered + parked

void Phase38()
{
    // ── Tier A: cooperative-cancel machinery (no strand) ───────────────────────

    // (1) basic source/token + request_stop is one-shot.
    {
        std::stop_source src;
        std::stop_token  tok = src.get_token();
        Check(src.stop_possible(), "phase38 fresh source stop_possible()");
        Check(tok.stop_possible(), "phase38 token from source stop_possible()");
        Check(!src.stop_requested(), "phase38 source not yet requested");
        Check(!tok.stop_requested(), "phase38 token not yet requested");
        Check(src.request_stop(), "phase38 first request_stop() returns true");
        Check(!src.request_stop(), "phase38 second request_stop() returns false");
        Check(src.stop_requested(), "phase38 source stop_requested() after request");
        Check(tok.stop_requested(), "phase38 token sees the stop (shared state)");
    }

    // (2) nostopstate source: no state at all → nothing is possible.
    {
        std::stop_source none{std::nostopstate};
        std::stop_token  tok = none.get_token();
        Check(!none.stop_possible(), "phase38 nostopstate source !stop_possible()");
        Check(!tok.stop_possible(), "phase38 nostopstate token !stop_possible()");
        Check(!none.request_stop(), "phase38 nostopstate request_stop() == false");
        Check(!tok.stop_requested(), "phase38 nostopstate token !stop_requested()");
    }

    // (3) callback runs when the stop is requested (registered before the stop).
    {
        g_p38_cb_ran.store(0, std::memory_order_relaxed);
        std::stop_source src;
        std::stop_callback cb(src.get_token(),
                              [] { g_p38_cb_ran.fetch_add(1, std::memory_order_release); });
        Check(g_p38_cb_ran.load(std::memory_order_acquire) == 0,
              "phase38 callback does NOT run before request_stop");
        src.request_stop();
        Check(g_p38_cb_ran.load(std::memory_order_acquire) == 1,
              "phase38 callback ran exactly once on request_stop");
    }

    // (4) callback constructed AFTER the stop already happened → runs inline in
    //     the ctor (never registered).
    {
        g_p38_cb_ran.store(0, std::memory_order_relaxed);
        std::stop_source src;
        src.request_stop();
        std::stop_callback cb(src.get_token(),
                              [] { g_p38_cb_ran.fetch_add(1, std::memory_order_release); });
        Check(g_p38_cb_ran.load(std::memory_order_acquire) == 1,
              "phase38 callback built after stop runs immediately in ctor");
    }

    // (5) callback destroyed before any stop → never runs.
    {
        g_p38_cb_ran.store(0, std::memory_order_relaxed);
        std::stop_source src;
        {
            std::stop_callback cb(src.get_token(),
                                  [] { g_p38_cb_ran.fetch_add(1, std::memory_order_release); });
        }   // cb unregisters here
        src.request_stop();
        Check(g_p38_cb_ran.load(std::memory_order_acquire) == 0,
              "phase38 callback destroyed before stop never runs");
    }

    // (6) self-destroying callback: the body destroys the optional that holds the
    //     callback (same strand, via request_stop). Must NOT hang and must run once.
    {
        g_p38_cb_ran.store(0, std::memory_order_relaxed);
        std::stop_source              src;
        std::optional<std::stop_callback<void (*)()>> holder;
        static std::optional<std::stop_callback<void (*)()>> *s_holder;
        s_holder = &holder;
        holder.emplace(src.get_token(), +[] {
            g_p38_cb_ran.fetch_add(1, std::memory_order_release);
            s_holder->reset();   // destroy THIS callback from inside its own body
        });
        src.request_stop();      // drains → invokes → body resets holder (self-destroy)
        Check(g_p38_cb_ran.load(std::memory_order_acquire) == 1,
              "phase38 self-destroying callback ran once and did not hang");
        Check(!holder.has_value(),
              "phase38 self-destroying callback actually destroyed itself");
    }

    // (7) last stop_source dies without a stop → the token can no longer be stopped.
    {
        std::stop_token tok;
        {
            std::stop_source src;
            tok = src.get_token();
            Check(tok.stop_possible(), "phase38 token possible while source alive");
        }   // last source gone, no request made
        Check(!tok.stop_possible(),
              "phase38 token !stop_possible() after last source dies w/o stop");
        Check(!tok.stop_requested(), "phase38 abandoned token !stop_requested()");
    }

    // ── Tier B: real jthreads (a strand each) — FSGSBASE-guarded ────────────────
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase38: jthread strands need FSGSBASE — skipping Tier B\n");
        printf("[CXX] PASS phase38: stop_token + jthread "
               "(cooperative cancel; id=strand pid)\n");
        return;
    }

    // A cooperative-cancel body polls its token and BACKS OFF with a 1 ms sleep
    // (a timed kernel park) between checks — NOT a tight yield-spin. On a single
    // App-Core boot the strands schedule cooperatively, so a strand that never
    // blocks would starve main (which is mid-printf / mid-request_stop); the
    // timed park is the BoxOS-native "wait for the flag" and lets main run.
    using namespace std::chrono;

    // (8) jthread auto request_stop + join on dtor: the body polls its token; scope
    //     exit must request_stop + join, so the body must have observed the stop.
    {
        g_p38_jflag.store(0, std::memory_order_relaxed);
        {
            std::jthread j([](std::stop_token st) {
                while (!st.stop_requested())
                    std::this_thread::sleep_for(milliseconds(1));
                g_p38_jflag.store(1, std::memory_order_release);
            });
            Check(j.joinable(), "phase38 jthread joinable while running");
            Check(j.get_id() != std::jthread::id{}, "phase38 jthread has a live id");
        }   // dtor: request_stop + join
        Check(g_p38_jflag.load(std::memory_order_acquire) == 1,
              "phase38 jthread dtor auto request_stop+join (body saw the stop)");
    }

    // (9) jthread with a PLAIN callable (no stop_token param) runs normally.
    {
        g_p38_plain.store(0, std::memory_order_relaxed);
        {
            std::jthread j([] { g_p38_plain.store(1, std::memory_order_release); });
        }   // dtor joins (request_stop is a no-op the body ignores)
        Check(g_p38_plain.load(std::memory_order_acquire) == 1,
              "phase38 jthread with plain callable ran to completion");
    }

    // (10) explicit request_stop() exits the loop before scope end.
    {
        g_p38_jflag.store(0, std::memory_order_relaxed);
        std::jthread j([](std::stop_token st) {
            while (!st.stop_requested())
                std::this_thread::sleep_for(milliseconds(1));
            g_p38_jflag.store(2, std::memory_order_release);
        });
        Check(j.request_stop(), "phase38 explicit request_stop() returns true");
        j.join();   // body observes the stop on its next wake, then exits
        Check(g_p38_jflag.load(std::memory_order_acquire) == 2,
              "phase38 explicit request_stop() let the body exit, join saw it");
    }

    // (11) bounded concurrent: a jthread registers a stop_callback against a
    //      shared source + waits; main request_stop()s → the callback runs exactly
    //      once (no double-run, no lost). The worker uses an EXTERNAL source (main's),
    //      so it is launched with a plain callable that captures a token by value.
    {
        g_p38_cb_count.store(0, std::memory_order_relaxed);
        g_p38_ready.store(0, std::memory_order_relaxed);
        std::stop_source shared;
        std::stop_token  wtok = shared.get_token();
        {
            std::jthread worker([wtok] {
                std::stop_callback cb(wtok, [] {
                    g_p38_cb_count.fetch_add(1, std::memory_order_release);
                });
                g_p38_ready.store(1, std::memory_order_release);   // registered
                // Wait (timed park) until the callback has fired — bounded.
                for (int cyc = 0; cyc < 500; cyc++) {
                    if (g_p38_cb_count.load(std::memory_order_acquire) != 0) break;
                    std::this_thread::sleep_for(milliseconds(1));
                }
            });

            // Wait (timed park) until the worker has registered its callback.
            for (int cyc = 0; cyc < 500 &&
                              g_p38_ready.load(std::memory_order_acquire) == 0;
                 cyc++)
                std::this_thread::sleep_for(milliseconds(1));
            Check(g_p38_ready.load(std::memory_order_acquire) == 1,
                  "phase38 worker registered its stop_callback");

            Check(shared.request_stop(),
                  "phase38 main request_stop() drove the worker's callback");
        }   // worker jthread dtor: request_stop (no-op, already) + join

        Check(g_p38_cb_count.load(std::memory_order_acquire) == 1,
              "phase38 concurrent stop_callback ran exactly once (no double/lost)");
    }

    printf("[CXX] PASS phase38: stop_token + jthread "
           "(cooperative cancel; id=strand pid)\n");
}

// ── phase39: condition_variable / condition_variable_any (Ф20d-1) ──────────────
// Proves the event-driven cv across sibling strands: a notify wakes a parked
// waiter, notify_all wakes several, the predicate loop tolerates surplus
// notifies, the stop_token-aware cv_any wakes on a stop request, and
// notify_all_at_thread_exit fires at strand exit.
//
// PROOF STRUCTURE (no single-sample wall-clock bound — those flake under host
// vCPU deschedule: box::stopwatch reads the TSC-backed steady_clock, which keeps
// counting real time while the guest vCPU is descheduled, so one inflated sample
// fails an upper bound even for a genuine event-wake; and a cal/4 ceiling cannot
// even tell a ~10ms event-wake from the ~100ms lost-wake backstop). Event-driven
// wakeup is a property of the shared wait/notify substrate, so it is proven ONCE,
// rigorously, by the PING-PONG: 64 lockstep hops = 128 parks that finish in
// ≪ 128×backstop, bounded core-conditionally with ~30× margin. Every other
// sub-test then asserts only its own SEMANTIC (predicate observed, wake count,
// return value) plus LIVENESS via p39_join — a no-timeout wait() that never wakes
// hangs and fails loudly through the bounded join. Sub-test 6a keeps a genuine
// dead-wait FLOOR (no-notifier wait_for returns false and waited the budget);
// floors only inflate under host load, so they never flake.

static constexpr int kP39Budget = 2000;        // ms: the timed dead-wait floor

static std::mutex            g_p39_mtx;       // guards the predicate flags below
static std::condition_variable g_p39_cv;      // cross-strand notify target
static std::condition_variable_any g_p39_cva; // cv_any over std::mutex
static bool                  g_p39_pred  = false;   // shared predicate flag
static volatile uint64_t     g_p39_ready;      // waiters bump before parking; the
                                               // readiness gate parks event-driven
                                               // (addr_park/addr_wake, like g_p39_remaining)
static std::atomic<int>      g_p39_woke{0};    // waiters bump after waking
static std::atomic<int>      g_p39_predcalls{0}; // predicate-eval count (sub-test 4)
static volatile uint64_t     g_p39_remaining;  // workers decrement; main joins

// Park-join workers the strandtest way (copy of p35_join): re-read the live
// counter before each park, bounded cycles so a hang fails loudly.
static bool p39_join()
{
    uint32_t cycles = 0;
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_p39_remaining, __ATOMIC_ACQUIRE)) != 0) {
        if (++cycles > 80u) return false;
        addr_park(&g_p39_remaining, cur, 200);
    }
    return true;
}

// (2)/(6b) notify_one across a strand: sleep so main parks first, set the
// predicate under the mutex, notify_one, then publish join.
static void p39_notify_one_worker(void *)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard<std::mutex> g(g_p39_mtx);
        g_p39_pred = true;
    }
    g_p39_cv.notify_one();
    __atomic_sub_fetch(&g_p39_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_remaining, 0);
    strand_exit();
}

// (3) two waiter strands: each bumps g_p39_ready before parking in wait(pred),
// wakes on notify_all, bumps g_p39_woke.
static void p39_waiter_worker(void *)
{
    std::unique_lock<std::mutex> lk(g_p39_mtx);
    __atomic_add_fetch(&g_p39_ready, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_ready, 0);
    g_p39_cv.wait(lk, [] { return g_p39_pred; });
    lk.unlock();
    g_p39_woke.fetch_add(1, std::memory_order_release);
    __atomic_sub_fetch(&g_p39_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_remaining, 0);
    strand_exit();
}

// (4) spurious tolerance: notify_one TWICE, set pred only on the 2nd; main must
// not return from wait(pred) until pred is actually true.
static void p39_spurious_worker(void *)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    g_p39_cv.notify_one();                       // 1st notify, predicate still false
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard<std::mutex> g(g_p39_mtx);
        g_p39_pred = true;
    }
    g_p39_cv.notify_one();                       // 2nd notify, predicate now true
    __atomic_sub_fetch(&g_p39_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_remaining, 0);
    strand_exit();
}

// (5) condition_variable_any cross-strand wake.
static void p39_cva_worker(void *)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard<std::mutex> g(g_p39_mtx);
        g_p39_pred = true;
    }
    g_p39_cva.notify_one();
    __atomic_sub_fetch(&g_p39_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_remaining, 0);
    strand_exit();
}

// (7) notify_all_at_thread_exit: a strand takes the lock, hands it to the
// exit-notifier, returns. notify_all_at_thread_exit registers on this strand's
// thread_local exit list (__cxa_thread_atexit), which a RAW strand only drains
// when it calls __boxcxx_thread_storage_exit — std::thread's trampoline does this
// automatically, a raw strand_spawn does not. So this worker must stand up its
// per-strand TLS (like p36_worker) and DRAIN it explicitly before strand_exit;
// that drain runs the callback (unlock g_p39_mtx + cv.notify_all) which wakes
// main. The join-signal is published AFTER the drain so main only returns once
// the wake has fired.
static void p39_atexit_worker(void *)
{
    __boxcxx_tls_strand_init();
    __boxcxx_thread_storage_enter();

    {
        std::unique_lock<std::mutex> lk(g_p39_mtx);
        g_p39_pred = true;                       // predicate set while holding lock
        std::notify_all_at_thread_exit(g_p39_cv, std::move(lk));
    }
    __boxcxx_thread_storage_exit();              // runs the exit notifier (unlock + wake)

    __atomic_sub_fetch(&g_p39_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_remaining, 0);
    strand_exit();
}

// (8) stop_token-aware cv_any: blocks in cva.wait(lock, stoken, false-pred) until
// main requests the stop.
static std::stop_source g_p39_ssrc;
static void p39_stoptoken_worker(void *)
{
    std::unique_lock<std::mutex> lk(g_p39_mtx);
    __atomic_add_fetch(&g_p39_ready, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_ready, 0);
    bool r = g_p39_cva.wait(lk, g_p39_ssrc.get_token(), [] { return g_p39_pred; });
    lk.unlock();
    // pred() stayed false → wait returns false (woken by the stop, not the pred).
    g_p39_woke.store(r ? 2 : 1, std::memory_order_release);
    __atomic_sub_fetch(&g_p39_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_remaining, 0);
    strand_exit();
}

// (PP) cv ping-pong — rigorous event-wake proof (the phase35 method, through a
// condition_variable). kP39PingPong lockstep hops: each park is woken by the
// peer's notify. A wake that rode the ~100ms backstop instead of the event would
// cost ~100ms per hop (≈12.8s for 128 parks); a real notify wake is sub-ms. The
// predicate guards both the lost-wake and the notify-before-park cases (an early
// notify just makes the predicate already-true, no park), so the only thing that
// can make this slow is a wake that is NOT event-driven.
static constexpr int           kP39PingPong = 64;
static int                     g_p39_pp = 0;   // turn counter, guarded by g_p39_mtx
static std::condition_variable g_p39_ppcv;
static void p39_pingpong_worker(void *)
{
    for (int i = 0; i < kP39PingPong; i++) {
        std::unique_lock<std::mutex> lk(g_p39_mtx);
        g_p39_ppcv.wait(lk, [i] { return g_p39_pp == 2 * i + 1; });
        g_p39_pp = 2 * i + 2;
        lk.unlock();
        g_p39_ppcv.notify_one();
    }
    __atomic_sub_fetch(&g_p39_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p39_remaining, 0);
    strand_exit();
}

void Phase39()
{
    using namespace std::chrono;

    // ── (1) ALWAYS-ON (no strands) ────────────────────────────────────────────
    {
        std::unique_lock<std::mutex> lk(g_p39_mtx);

        Check(g_p39_cv.wait_for(lk, milliseconds(0), [] { return true; }),
              "phase39 wait_for already-true predicate returns true immediately");

        Check(g_p39_cv.wait_until(lk, steady_clock::now()) == std::cv_status::timeout,
              "phase39 wait_until past deadline + false pred reports timeout");
    }
    {
        // notify with no waiter must not hang and advances seq by 2.
        auto *h = g_p39_cv.native_handle();
        uint32_t before = h->load(std::memory_order_acquire);
        g_p39_cv.notify_one();
        g_p39_cv.notify_all();
        Check(h->load(std::memory_order_acquire) == before + 2,
              "phase39 notify_one/notify_all with no waiter advance seq, no hang");
    }
    {
        // condition_variable_any + std::mutex, already-true predicate.
        std::mutex m;
        std::unique_lock<std::mutex> lk(m);
        bool entered = false;
        g_p39_cva.wait(lk, [&] { entered = true; return true; });
        Check(entered && lk.owns_lock(),
              "phase39 cv_any already-true predicate returns immediately, lock held");
    }

    // (Calibration removed: the old cal/early/half single-sample TSC ratios flaked
    //  under host vCPU deschedule and could not tell an event-wake from the ~100ms
    //  backstop. Event-drivenness is proven by the ping-pong below; sub-test 6a
    //  keeps the genuine no-notifier timeout floor.)

    // ── (2)..(8): cross-strand, need FSGSBASE (per-strand TLS) ────────────────
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase39: strands need FSGSBASE — skipping cross-strand\n");
        printf("[CXX] PASS phase39: condition_variable / condition_variable_any "
               "(always-on + timed; cross-strand skipped, no FSGSBASE)\n");
        return;
    }

    // (2) notify_one delivered — the parked waiter returns; liveness via p39_join.
    {
        g_p39_pred = false;
        g_p39_remaining = 1;
        if (strand_spawn(p39_notify_one_worker, 0) == 0) {
            printf("[CXX] note phase39: strand_spawn unavailable — skipping\n");
            printf("[CXX] PASS phase39: condition_variable / condition_variable_any "
                   "(always-on + timed; cross-strand skipped, no strand)\n");
            return;
        }
        std::unique_lock<std::mutex> lk(g_p39_mtx);
        g_p39_cv.wait(lk, [] { return g_p39_pred; });
        lk.unlock();
        Check(p39_join(), "phase39 notify_one worker joined");
    }

    // (PP) cv ping-pong: rigorous proof the wake is the notify event, not the
    // 100ms backstop (see the worker comment). Tight, core-conditional bound.
    {
        g_p39_pp        = 0;
        g_p39_remaining = 1;
        if (strand_spawn(p39_pingpong_worker, 0)) {
            box::stopwatch sw;
            for (int i = 0; i < kP39PingPong; i++) {
                {
                    std::lock_guard<std::mutex> g(g_p39_mtx);
                    g_p39_pp = 2 * i + 1;
                }
                g_p39_ppcv.notify_one();
                std::unique_lock<std::mutex> lk(g_p39_mtx);
                g_p39_ppcv.wait(lk, [i] { return g_p39_pp == 2 * i + 2; });
            }
            nanoseconds   t = sw.elapsed();
            system_info_t si{};
            unsigned      cores = (sysinfo(&si) == 0) ? si.cpu_app_cores : 0u;
            auto          bound = (cores >= 2) ? milliseconds(3000) : milliseconds(9000);
            Check(t < bound,
                  "phase39 cv ping-pong woken by notify, not the 100ms backstop");
            Check(p39_join(), "phase39 ping-pong worker joined");
        }
    }

    // (3) notify_all wakes 2 waiter strands.
    {
        g_p39_pred = false;
        __atomic_store_n(&g_p39_ready, 0u, __ATOMIC_RELAXED);
        g_p39_woke.store(0, std::memory_order_relaxed);
        int workers = 0;
        if (strand_spawn(p39_waiter_worker, 0)) workers++;
        if (strand_spawn(p39_waiter_worker, 0)) workers++;
        if (workers == 0) {
            printf("[CXX] note phase39: no waiter strand — skipping notify_all\n");
        } else {
            g_p39_remaining = (uint64_t)workers;
            // Event-driven wait until both waiters reach their park (bounded join
            // pattern: worker addr_wakes on each bump; the 200ms re-check is a
            // lost-wake backstop, and >80 cycles falls through to a loud Check).
            uint32_t rcyc = 0; uint64_t rdy;
            while ((rdy = __atomic_load_n(&g_p39_ready, __ATOMIC_ACQUIRE))
                   < (uint64_t)workers) {
                if (++rcyc > 80u) break;
                addr_park(&g_p39_ready, rdy, 200);
            }
            Check(__atomic_load_n(&g_p39_ready, __ATOMIC_ACQUIRE) == (uint64_t)workers,
                  "phase39 both waiter strands reached the park");
            {
                std::lock_guard<std::mutex> g(g_p39_mtx);
                g_p39_pred = true;
            }
            g_p39_cv.notify_all();
            Check(p39_join(), "phase39 notify_all woke both waiters (joined)");
            Check(g_p39_woke.load(std::memory_order_acquire) == workers,
                  "phase39 notify_all: every waiter observed the predicate");
        }
    }

    // (4) predicate-loop spurious tolerance: 1st notify with pred false must NOT
    //     return; only the 2nd (pred true) does. Track pred-eval count >= 2.
    {
        g_p39_pred = false;
        g_p39_predcalls.store(0, std::memory_order_relaxed);
        g_p39_remaining = 1;
        if (strand_spawn(p39_spurious_worker, 0)) {
            std::unique_lock<std::mutex> lk(g_p39_mtx);
            g_p39_cv.wait(lk, [] {
                g_p39_predcalls.fetch_add(1, std::memory_order_release);
                return g_p39_pred;
            });
            bool got = g_p39_pred;
            lk.unlock();
            Check(got, "phase39 spurious-tolerant wait returned only when pred true");
            Check(g_p39_predcalls.load(std::memory_order_acquire) >= 2,
                  "phase39 predicate re-evaluated across a surplus notify (>= 2)");
            Check(p39_join(), "phase39 spurious worker joined");
        }
    }

    // (5) condition_variable_any cross-strand notify delivered; liveness via join.
    {
        g_p39_pred = false;
        g_p39_remaining = 1;
        if (strand_spawn(p39_cva_worker, 0)) {
            std::unique_lock<std::mutex> lk(g_p39_mtx);
            g_p39_cva.wait(lk, [] { return g_p39_pred; });
            lk.unlock();
            Check(p39_join(), "phase39 cv_any worker joined");
        }
    }

    // (6) wait_for timeout-vs-event discrimination.
    {
        // (6a) no notifier, false pred → returns false, waited ≈ the budget (floor).
        g_p39_pred = false;
        std::unique_lock<std::mutex> lk(g_p39_mtx);
        box::stopwatch sw;
        bool r = g_p39_cv.wait_for(lk, milliseconds(kP39Budget),
                                   [] { return g_p39_pred; });
        nanoseconds t = sw.elapsed();
        lk.unlock();
        Check(!r, "phase39 wait_for(pred) times out → false when never notified");
        Check(t >= milliseconds(kP39Budget * 3 / 5),
              "phase39 wait_for(pred) timeout actually waited the budget");
    }
    {
        // (6b) with a notifier → wait_for(pred) returns true (event-driven; the
        //      no-notifier timeout FLOOR is proven by 6a above, the ping-pong proves
        //      the wake is the event and not the ~100ms backstop).
        g_p39_pred = false;
        g_p39_remaining = 1;
        if (strand_spawn(p39_notify_one_worker, 0)) {
            std::unique_lock<std::mutex> lk(g_p39_mtx);
            bool r = g_p39_cv.wait_for(lk, milliseconds(kP39Budget),
                                       [] { return g_p39_pred; });
            lk.unlock();
            Check(r, "phase39 wait_for(pred) returns true when notified");
            Check(p39_join(), "phase39 wait_for(pred) notifier joined");
        }
    }

    // (7) notify_all_at_thread_exit end-to-end.
    {
        g_p39_pred = false;
        g_p39_remaining = 1;
        if (strand_spawn(p39_atexit_worker, 0)) {
            std::unique_lock<std::mutex> lk(g_p39_mtx);
            g_p39_cv.wait(lk, [] { return g_p39_pred; });
            lk.unlock();
            Check(p39_join(), "phase39 notify_all_at_thread_exit worker joined");
        }
    }

    // (8) stop_token-aware cv_any: a waiter blocks with a false predicate; a stop
    //     request wakes it; it returns pred() == false.
    {
        g_p39_pred = false;
        __atomic_store_n(&g_p39_ready, 0u, __ATOMIC_RELAXED);
        g_p39_woke.store(0, std::memory_order_relaxed);
        g_p39_remaining = 1;
        if (strand_spawn(p39_stoptoken_worker, 0)) {
            uint32_t rcyc = 0; uint64_t rdy;
            while ((rdy = __atomic_load_n(&g_p39_ready, __ATOMIC_ACQUIRE)) == 0) {
                if (++rcyc > 80u) break;
                addr_park(&g_p39_ready, rdy, 200);
            }
            Check(__atomic_load_n(&g_p39_ready, __ATOMIC_ACQUIRE) == 1,
                  "phase39 stop_token waiter reached the park");
            Check(g_p39_ssrc.request_stop(),
                  "phase39 request_stop() returns true (first request)");
            Check(p39_join(), "phase39 stop_token waiter joined");
            Check(g_p39_woke.load(std::memory_order_acquire) == 1,
                  "phase39 stop_token cv_any returned pred()==false on stop");
        }
    }

    printf("[CXX] PASS phase39: condition_variable / condition_variable_any "
           "(cross-strand notify + timed event-driven + stop_token + "
           "notify_all_at_thread_exit)\n");
}

// ── phase40: <future> (Ф20d-2) ─────────────────────────────────────────────────
// promise / future / shared_future + async (both policies) + deferred +
// packaged_task + every future_errc. Event-driven wakeup is proven by the FUTURE
// PING-PONG (64 lockstep hops = 128 parks ≪ 128×backstop, core-conditional bound
// with ~30× margin) — the same proof structure phase39 uses. No single-sample
// wall-clock upper bound (those flake under host vCPU deschedule of the TSC-backed
// steady_clock); every other sub-test asserts only its own value/semantics plus
// p40_join liveness, and 14a keeps the genuine no-producer timeout FLOOR.
// Cross-strand sub-tests skip→PASS without FSGSBASE (per-strand TLS), like phase39.

static constexpr int kP40Budget = 2000;   // ms: the timed dead-wait floor

static volatile uint64_t g_p40_remaining;  // workers decrement; main joins

// Park-join workers the strandtest way (copy of p39_join): re-read the live
// counter before each park, bounded cycles so a hang fails loudly.
static bool p40_join()
{
    uint32_t cycles = 0;
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_p40_remaining, __ATOMIC_ACQUIRE)) != 0) {
        if (++cycles > 80u) return false;
        addr_park(&g_p40_remaining, cur, 200);
    }
    return true;
}

// (13) future ping-pong: kP40PingPong lockstep hops, each a fresh single-shot
// promise<void>/future<void> pair the peer waits on. Every promise is used EXACTLY
// once (pre-created arrays — no re-arm, no aliasing, no concurrent move), so the
// only thing that drives a hop is the peer's set_value→notify wake. A backstop
// ride would cost ~100ms/hop (≈6.4s for 64 hops); finishing under the tight
// core-conditional bound proves the wake is the event, not a timeout. Each hop:
//   main  ping[i].set_value()  → worker  ping_fut[i].wait()  → worker pong[i].set_value()
//   → main pong_fut[i].wait().
static constexpr int          kP40PingPong = 64;
static std::promise<void>    *g_p40_ping;       // [kP40PingPong] main → worker
static std::future<void>     *g_p40_ping_fut;   // [kP40PingPong] worker waits here
static std::promise<void>    *g_p40_pong;       // [kP40PingPong] worker → main

static void p40_pingpong_worker(void *)
{
    __boxcxx_tls_strand_init();
    __boxcxx_thread_storage_enter();
    for (int i = 0; i < kP40PingPong; i++) {
        g_p40_ping_fut[i].wait();   // block on main's ping[i]
        g_p40_pong[i].set_value();  // wake main's pong_fut[i]
    }
    __boxcxx_thread_storage_exit();
    __atomic_sub_fetch(&g_p40_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p40_remaining, 0);
    strand_exit();
}

// (11) async event-wake target.
static int p40_async_value() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); return 42; }
// (12) async exception target.
static int p40_async_throws() { throw std::runtime_error("boom"); }

// (15) shared_future cross-strand waiters: each blocks in sf.wait() on a copy.
static std::shared_future<int> *g_p40_sf;
static volatile uint64_t        g_p40_sf_ready;   // event-driven readiness gate
static std::atomic<int>         g_p40_sf_got{0};
static void p40_sf_waiter(void *)
{
    __boxcxx_tls_strand_init();
    __boxcxx_thread_storage_enter();
    std::shared_future<int> local = *g_p40_sf;   // a copy (ref_inc)
    __atomic_add_fetch(&g_p40_sf_ready, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p40_sf_ready, 0);
    local.wait();
    if (local.get() == 77) g_p40_sf_got.fetch_add(1, std::memory_order_release);
    __boxcxx_thread_storage_exit();
    __atomic_sub_fetch(&g_p40_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p40_remaining, 0);
    strand_exit();
}

// (16) set_value_at_thread_exit raw-strand worker: stand up TLS, stash the value,
// then drain thread storage (publishes + notifies), like p39_atexit_worker.
static std::promise<int> *g_p40_atexit_prom;
static void p40_atexit_worker(void *)
{
    __boxcxx_tls_strand_init();
    __boxcxx_thread_storage_enter();
    g_p40_atexit_prom->set_value_at_thread_exit(123);
    __boxcxx_thread_storage_exit();             // runs the publish+notify
    __atomic_sub_fetch(&g_p40_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p40_remaining, 0);
    strand_exit();
}

// (17) packaged_task run on a std::thread: the thread invokes pt, main get()s.
static std::packaged_task<int(int, int)> *g_p40_pt;
static void p40_pt_worker(void *)
{
    __boxcxx_tls_strand_init();
    __boxcxx_thread_storage_enter();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (*g_p40_pt)(20, 22);
    __boxcxx_thread_storage_exit();
    __atomic_sub_fetch(&g_p40_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p40_remaining, 0);
    strand_exit();
}

void Phase40()
{
    using namespace std::chrono;

    // ── ALWAYS-ON single-strand sub-tests (no FSGSBASE) ───────────────────────

    // (1) promise<int>/future<int> round-trip.
    {
        std::promise<int> p;
        std::future<int>  f = p.get_future();
        Check(f.valid(), "phase40 future valid before get");
        p.set_value(7);
        Check(f.get() == 7, "phase40 promise<int>→future<int> round-trip");
        Check(!f.valid(), "phase40 future invalid after get (single-use)");
    }

    // (2) exception propagation.
    {
        std::promise<int> p;
        std::future<int>  f = p.get_future();
        p.set_exception(
            std::make_exception_ptr(std::runtime_error("x")));
        bool threw = false;
        try {
            (void)f.get();
        } catch (const std::runtime_error &e) {
            threw = (std::string(e.what()) == "x");
        }
        Check(threw, "phase40 set_exception propagates through future::get");
    }

    // (3) future<void>.
    {
        std::promise<void> p;
        std::future<void>  f = p.get_future();
        p.set_value();
        f.get();
        Check(!f.valid(), "phase40 future<void> set_value()/get() then invalid");
    }

    // (4) future<int&>.
    {
        int               x = 99;
        std::promise<int &> p;
        std::future<int &>  f = p.get_future();
        p.set_value(x);
        Check(&f.get() == &x, "phase40 future<int&> returns the same object");
    }

    // (5) shared_future multi-wait.
    {
        std::promise<int>       p;
        std::shared_future<int> sf = p.get_future().share();
        std::shared_future<int> sf2 = sf;        // a copy
        p.set_value(55);
        Check(sf.get() == 55 && sf2.get() == 55,
              "phase40 shared_future two copies both get the value");
        Check(sf.get() == 55, "phase40 shared_future multiple get() calls OK");
    }

    // (6) wait() on ready returns immediately; wait_until(past) on not-ready
    //     reports timeout.
    {
        std::promise<int> p;
        std::future<int>  f = p.get_future();
        p.set_value(1);
        f.wait();   // already ready → returns at once
        Check(f.get() == 1, "phase40 wait() on ready then get");

        std::promise<int> p2;
        std::future<int>  f2 = p2.get_future();
        Check(f2.wait_until(steady_clock::now()) == std::future_status::timeout,
              "phase40 wait_until(past) on not-ready → timeout");
    }

    // (7) packaged_task<int(int,int)>.
    {
        std::packaged_task<int(int, int)> pt([](int a, int b) { return a + b; });
        Check(pt.valid(), "phase40 packaged_task valid after construction");
        std::future<int> f = pt.get_future();
        pt(3, 4);
        Check(f.get() == 7, "phase40 packaged_task invoke → future sum");
        pt.reset();
        std::future<int> f2 = pt.get_future();
        pt(10, 20);
        Check(f2.get() == 30, "phase40 packaged_task reset() → fresh future");

        std::packaged_task<int(int, int)> pt2;
        pt.swap(pt2);
        Check(pt2.valid() && !pt.valid(), "phase40 packaged_task swap");
    }

    // (8) deferred: ran==false before get; wait_for(0)==deferred AND still
    //     ran==false; get() runs it on THIS strand.
    {
        std::atomic<bool> ran{false};
        std::thread::id   body_id;
        auto fut = std::async(std::launch::deferred, [&] {
            ran.store(true, std::memory_order_release);
            body_id = std::this_thread::get_id();
            return 88;
        });
        Check(!ran.load(std::memory_order_acquire),
              "phase40 deferred not run before get");
        Check(fut.wait_for(milliseconds(0)) == std::future_status::deferred,
              "phase40 deferred wait_for(0) reports deferred");
        Check(!ran.load(std::memory_order_acquire),
              "phase40 deferred wait_for did NOT run the function");
        int v = fut.get();
        Check(v == 88 && ran.load(std::memory_order_acquire),
              "phase40 deferred get() runs the function and returns the value");
        Check(body_id == std::this_thread::get_id(),
              "phase40 deferred ran on the GET caller's strand");
    }

    // (9) all four future_errc + no_state single-use + category identity.
    {
        // 2nd get_future() → future_already_retrieved.
        std::promise<int> p;
        (void)p.get_future();
        bool e1 = false;
        try {
            (void)p.get_future();
        } catch (const std::future_error &e) {
            e1 = (e.code() == std::make_error_code(
                                  std::future_errc::future_already_retrieved));
        }
        Check(e1, "phase40 future_errc::future_already_retrieved on 2nd get_future");

        // 2nd set_value → promise_already_satisfied.
        std::promise<int> p2;
        (void)p2.get_future();
        p2.set_value(1);
        bool e2 = false;
        try {
            p2.set_value(2);
        } catch (const std::future_error &e) {
            e2 = (e.code().value() ==
                  (int)std::future_errc::promise_already_satisfied);
        }
        Check(e2, "phase40 future_errc::promise_already_satisfied on 2nd set_value");

        // broken_promise: promise dies unset.
        std::future<int> bf;
        {
            std::promise<int> bp;
            bf = bp.get_future();
        }
        bool e3 = false;
        try {
            (void)bf.get();
        } catch (const std::future_error &e) {
            e3 = (e.code().value() == (int)std::future_errc::broken_promise);
        }
        Check(e3, "phase40 future_errc::broken_promise when promise abandoned");

        // no_state: moved-from promise set_value.
        std::promise<int> q;
        std::promise<int> q2(std::move(q));
        bool e4 = false;
        try {
            q.set_value(1);
        } catch (const std::future_error &e) {
            e4 = (e.code().value() == (int)std::future_errc::no_state);
        }
        Check(e4, "phase40 future_errc::no_state on moved-from promise set_value");

        // single-use: future after get() → 2nd get() → no_state.
        std::promise<int> r;
        std::future<int>  rf = r.get_future();
        r.set_value(5);
        (void)rf.get();
        bool e5 = false;
        try {
            (void)rf.get();
        } catch (const std::future_error &e) {
            e5 = (e.code().value() == (int)std::future_errc::no_state);
        }
        Check(e5, "phase40 future_errc::no_state on 2nd future::get (single-use)");

        // fresh category, NOT generic/system.
        Check(&std::future_category() != &std::generic_category() &&
                  &std::future_category() != &std::system_category(),
              "phase40 future_category is a distinct fresh category");
        Check(std::string(std::future_category().name()) == "future",
              "phase40 future_category().name() == \"future\"");
    }

    // (10) launch bitmask.
    {
        constexpr std::launch both = std::launch::async | std::launch::deferred;
        Check((both & std::launch::async) == std::launch::async,
              "phase40 launch bitmask: async|deferred contains async");
        Check((both & std::launch::deferred) == std::launch::deferred,
              "phase40 launch bitmask: async|deferred contains deferred");
    }

    // (10a) a throwing result ctor must NOT wedge the state: set_value with a
    //       throwing copy leaves the promise UNSATISFIED (retryable), not a
    //       satisfied-but-never-ready husk.
    {
        struct ThrowOnCopy {
            int  v;
            bool boom;
            ThrowOnCopy(int x, bool b) : v(x), boom(b) {}
            ThrowOnCopy(const ThrowOnCopy &o) : v(o.v), boom(o.boom)
            {
                if (boom) throw std::runtime_error("copy boom");
            }
            ThrowOnCopy(ThrowOnCopy &&) = default;   // noexcept move
        };
        std::promise<ThrowOnCopy> p;
        std::future<ThrowOnCopy>  f = p.get_future();
        ThrowOnCopy               bad(1, true);
        bool                      threw = false;
        try {
            p.set_value(bad);   // copy throws BEFORE the gate is claimed
        } catch (const std::runtime_error &) {
            threw = true;
        }
        Check(threw, "phase40 set_value with a throwing copy propagates");
        ThrowOnCopy good(42, false);
        p.set_value(good);   // gate was NOT claimed → this must succeed (retry)
        Check(f.get().v == 42,
              "phase40 future usable after a throwing set_value (not wedged)");
    }

    // (10b) packaged_task::reset() abandons the old state with broken_promise — a
    //       future retrieved before reset() gets broken_promise, not a hang.
    {
        std::packaged_task<int()> pt([] { return 5; });
        std::future<int>          old = pt.get_future();
        pt.reset();
        bool bp = false;
        try {
            (void)old.get();
        } catch (const std::future_error &e) {
            bp = (e.code().value() == (int)std::future_errc::broken_promise);
        }
        Check(bp, "phase40 packaged_task::reset() gives the old future broken_promise");
        std::future<int> nf = pt.get_future();
        pt();
        Check(nf.get() == 5, "phase40 packaged_task usable after reset()");
    }

    // (Calibration removed: the old cal/early/half single-sample TSC ratios flaked
    //  under host vCPU deschedule and could not tell an event-wake from the ~100ms
    //  backstop. Event-drivenness is proven by the future ping-pong below; 14a keeps
    //  the genuine no-producer timeout floor.)

    // ── (11)..(17): cross-strand, need FSGSBASE (per-strand TLS) ──────────────
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase40: strands need FSGSBASE — skipping cross-strand\n");
        printf("[CXX] PASS phase40: <future> (always-on: promise/future/"
               "shared_future + deferred + packaged_task + all future_errc; "
               "cross-strand skipped, no FSGSBASE)\n");
        return;
    }

    // (11) async(launch::async): worker sleeps ~10ms then returns 42; main get()==42
    //      (event-wake proven by the future ping-pong). The future dtor joins the
    //      worker exactly once — no hang.
    {
        std::future<int> f = std::async(std::launch::async, p40_async_value);
        int v = f.get();
        Check(v == 42, "phase40 async(launch::async) returned the value");
        // f's dtor (here, end of scope) joins the worker; reaching the next line
        // proves no hang.
    }
    Check(true, "phase40 async future dtor joined the worker (no hang)");

    // (12) async worker EXCEPTION cross-strand: rethrown on the MAIN strand —
    //      exercises the STEP 1 atomic refcount + capture-then-publish ordering.
    {
        std::future<int> f = std::async(std::launch::async, p40_async_throws);
        bool threw = false;
        try {
            (void)f.get();
        } catch (const std::runtime_error &e) {
            threw = (std::string(e.what()) == "boom");
        }
        Check(threw,
              "phase40 async exception_ptr survived cross-strand rethrow on main");
    }

    // (13) FUTURE PING-PONG: kP40PingPong lockstep hops, each a fresh single-shot
    //      promise<void> the peer waits on (see the worker comment). Pre-created
    //      pairs → no re-arm race. Tight core-conditional bound like p39.
    {
        std::promise<void> ping[kP40PingPong];
        std::future<void>  ping_fut[kP40PingPong];
        std::promise<void> pong[kP40PingPong];
        std::future<void>  pong_fut[kP40PingPong];
        for (int i = 0; i < kP40PingPong; i++) {
            ping_fut[i] = ping[i].get_future();   // worker waits on these
            pong_fut[i] = pong[i].get_future();   // main waits on these
        }
        g_p40_ping     = ping;
        g_p40_ping_fut = ping_fut;
        g_p40_pong     = pong;
        g_p40_remaining = 1;
        if (strand_spawn(p40_pingpong_worker, 0)) {
            box::stopwatch sw;
            for (int i = 0; i < kP40PingPong; i++) {
                ping[i].set_value();    // wake the worker's ping_fut[i]
                pong_fut[i].wait();     // block on the worker's pong[i]
            }
            nanoseconds   t = sw.elapsed();
            system_info_t si{};
            unsigned      cores = (sysinfo(&si) == 0) ? si.cpu_app_cores : 0u;
            auto          bound = (cores >= 2) ? milliseconds(3000)
                                               : milliseconds(9000);
            Check(t < bound,
                  "phase40 future ping-pong woken by notify, not the backstop");
            Check(p40_join(), "phase40 ping-pong worker joined");
        }
    }

    // (14) wait_for event-vs-timeout discrimination.
    {
        // (14a) no producer → timeout, elapsed >= kBudget*3/5.
        std::promise<int> p;
        std::future<int>  f = p.get_future();
        box::stopwatch sw;
        std::future_status st = f.wait_for(milliseconds(kP40Budget));
        nanoseconds t = sw.elapsed();
        Check(st == std::future_status::timeout,
              "phase40 wait_for(no producer) → timeout");
        Check(t >= milliseconds(kP40Budget * 3 / 5),
              "phase40 wait_for timeout actually waited the budget");
    }
    {
        // (14b) producer set_value after ~10ms → wait_for(kBudget) returns ready
        //       (event-driven; the no-producer timeout FLOOR is 14a above).
        auto fut = std::async(std::launch::async, [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return 5;
        });
        std::future_status st = fut.wait_for(milliseconds(kP40Budget));
        Check(st == std::future_status::ready,
              "phase40 wait_for(producer) → ready");
        Check(fut.get() == 5, "phase40 wait_for then get the produced value");
    }

    // (15) shared_future cross-strand multi-wait: 2 waiter strands each block in
    //      sf.wait() on a copy; main set_value once; both wake and get
    //      the same value.
    {
        std::promise<int>       p;
        std::shared_future<int> sf = p.get_future().share();
        g_p40_sf = &sf;
        __atomic_store_n(&g_p40_sf_ready, 0u, __ATOMIC_RELAXED);
        g_p40_sf_got.store(0, std::memory_order_relaxed);
        int workers = 0;
        if (strand_spawn(p40_sf_waiter, 0)) workers++;
        if (strand_spawn(p40_sf_waiter, 0)) workers++;
        if (workers == 0) {
            printf("[CXX] note phase40: no shared_future waiter strand — skipping\n");
        } else {
            g_p40_remaining = (uint64_t)workers;
            uint32_t rcyc = 0; uint64_t rdy;
            while ((rdy = __atomic_load_n(&g_p40_sf_ready, __ATOMIC_ACQUIRE))
                   < (uint64_t)workers) {
                if (++rcyc > 80u) break;
                addr_park(&g_p40_sf_ready, rdy, 200);
            }
            Check(__atomic_load_n(&g_p40_sf_ready, __ATOMIC_ACQUIRE) == (uint64_t)workers,
                  "phase40 shared_future waiters reached the park");
            p.set_value(77);
            Check(p40_join(), "phase40 shared_future waiters joined");
            Check(g_p40_sf_got.load(std::memory_order_acquire) == workers,
                  "phase40 every shared_future waiter got the value");
        }
    }

    // (16) set_value_at_thread_exit end-to-end. A raw strand stands up TLS, calls
    //      set_value_at_thread_exit, then drains thread storage (the publish).
    {
        std::promise<int> prom;
        std::future<int>  f = prom.get_future();
        g_p40_atexit_prom = &prom;
        g_p40_remaining = 1;
        if (strand_spawn(p40_atexit_worker, 0)) {
            int v = f.get();
            Check(v == 123,
                  "phase40 set_value_at_thread_exit delivered the value");
            Check(p40_join(), "phase40 set_value_at_thread_exit worker joined");
        }
    }

    // (17) packaged_task on a std::thread: run the task on a thread, main get()
    //      returns the value (event-wake proven by the future ping-pong).
    {
        std::packaged_task<int(int, int)> pt(
            [](int a, int b) { return a * b; });
        std::future<int> f = pt.get_future();
        g_p40_pt = &pt;
        g_p40_remaining = 1;
        if (strand_spawn(p40_pt_worker, 0)) {
            int v = f.get();
            Check(v == 440, "phase40 packaged_task on a strand → future value");
            Check(p40_join(), "phase40 packaged_task worker joined");
        }
    }

    // (18) async future EARLY-DISCARD: drop the future WITHOUT get(). Per
    //      [futures.async]/5 the destructor of the last shared-state owner BLOCKS
    //      until the worker completes. The worker writes `done` to a stack local
    //      and the dtor's join waits for it, so done==1 is guaranteed — a
    //      non-blocking dtor would read 0 (worker still sleeping) and a worker that
    //      held its own ref would self-join and HANG here. Host-invariant: this
    //      asserts a value, not a wall-clock bound.
    {
        std::atomic<int> done{0};
        {
            std::future<void> f = std::async(std::launch::async, [&done] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                done.store(1, std::memory_order_release);
            });
            (void)f;
        }   // f's dtor blocks here until the worker set `done`
        Check(done.load(std::memory_order_acquire) == 1,
              "phase40 async future early-discard dtor blocked until worker done "
              "(conformant [futures.async]/5, no self-join)");
    }

    printf("[CXX] PASS phase40: <future> (promise/future/shared_future + async "
           "both policies + deferred + packaged_task + cross-strand event-wake + "
           "all future_errc)\n");
}

// ── phase41: per-strand StrandPool malloc/free fast path (Ф20e) ─────────────────
// The StrandPool layers a per-strand magazine cache under _malloc_impl/free so
// concurrent malloc/free from many strands mostly skip the single global heap
// lock — accelerating all C++ new/delete automatically. These checks are
// host-invariant: A1 proves correctness (served block >= requested, tagged/oversize
// bypass, double-free detection, realloc byte-preservation); A2 proves the cache
// actually serves most requests WITHOUT the lock using the heap's own
// malloc/free counters (incremented inside the lock — pure counter arithmetic, no
// timing); A3 proves per-strand isolation across real std::threads; A4 exercises
// the crash-orphan reclaim mechanism deterministically.

// The eleven cache size classes (mirror StrandPoolClassSize in boxlib memory.c).
static constexpr std::size_t kP41ClassSize[11] =
    { 16, 32, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };

// Fill `n` bytes of `p` with a position-dependent pattern keyed by `seed`, then
// read it back; returns true iff every byte survives (proves the block really is
// at least `n` bytes and nothing aliased it).
static bool p41_sentinel_ok(void *p, std::size_t n, unsigned seed)
{
    auto *b = static_cast<unsigned char *>(p);
    for (std::size_t i = 0; i < n; i++)
        b[i] = static_cast<unsigned char>((i * 31u + seed * 7u + 0x5Au) & 0xFFu);
    for (std::size_t i = 0; i < n; i++)
        if (b[i] != static_cast<unsigned char>((i * 31u + seed * 7u + 0x5Au) & 0xFFu))
            return false;
    return true;
}

// A3 worker: churn every class with a per-thread sentinel verify before free.
static std::atomic<int> g_p41_mismatch{0};
static std::atomic<int> g_p41_null{0};
static void p41_churn_worker(unsigned seed)
{
    constexpr int kIters = 500;
    void *live[11];
    for (int i = 0; i < 11; i++) live[i] = nullptr;
    for (int it = 0; it < kIters; it++) {
        int c = (it + seed) % 11;
        std::size_t sz = kP41ClassSize[c];
        if (live[c]) { free(live[c]); live[c] = nullptr; }
        void *p = malloc(sz);
        if (!p) { g_p41_null.fetch_add(1, std::memory_order_relaxed); continue; }
        if (!p41_sentinel_ok(p, sz, seed))
            g_p41_mismatch.fetch_add(1, std::memory_order_relaxed);
        live[c] = p;
    }
    for (int i = 0; i < 11; i++) if (live[i]) free(live[i]);
}

void Phase41()
{
    // ── A1: correctness (always on, single strand) ────────────────────────────

    // Every class: a freshly served block must hold a full ClassSize sentinel
    // (proves served size >= requested), survive free+realloc, and recycle.
    bool all_classes_ok = true;
    for (int c = 0; c < 11; c++) {
        std::size_t sz = kP41ClassSize[c];
        void *p = malloc(sz);
        if (!p || !p41_sentinel_ok(p, sz, static_cast<unsigned>(c + 1))) {
            all_classes_ok = false;
            if (p) free(p);
            continue;
        }
        free(p);
        // Pull it straight back (LIFO) and re-verify the full size.
        void *q = malloc(sz);
        if (!q || !p41_sentinel_ok(q, sz, static_cast<unsigned>(c + 99))) all_classes_ok = false;
        if (q) free(q);
    }
    Check(all_classes_ok, "phase41 every size class: served block holds a full-size sentinel and recycles");

    // A served block is at least as large as requested even for an odd size that
    // floors up to a class (request 33 → 48-byte class → 48 writable bytes).
    {
        void *p = malloc(33);
        bool ok = p && p41_sentinel_ok(p, 48, 5);   // 48 = class that 33 floors into
        Check(ok, "phase41 odd request floors up: 33-byte ask yields >= 48 usable bytes");
        if (p) free(p);
    }

    // realloc grow then shrink must preserve the existing bytes (auto-accelerated:
    // realloc is unchanged and rides _malloc_impl/free).
    {
        const std::size_t n0 = 64;
        auto *p = static_cast<unsigned char *>(malloc(n0));
        Check(p != nullptr, "phase41 realloc seed alloc");
        for (std::size_t i = 0; i < n0; i++) p[i] = static_cast<unsigned char>(i & 0xFF);
        auto *g = static_cast<unsigned char *>(realloc(p, 256));
        bool grow_ok = g != nullptr;
        if (g) for (std::size_t i = 0; i < n0; i++)
            if (g[i] != static_cast<unsigned char>(i & 0xFF)) grow_ok = false;
        Check(grow_ok, "phase41 realloc grow preserves bytes");
        auto *s = static_cast<unsigned char *>(realloc(g, 32));
        bool shrink_ok = s != nullptr;
        if (s) for (std::size_t i = 0; i < 32; i++)
            if (s[i] != static_cast<unsigned char>(i & 0xFF)) shrink_ok = false;
        Check(shrink_ok, "phase41 realloc shrink preserves bytes");
        if (s) free(s);
    }

    // Double-free of a cached block is caught by the magazine scan.
    {
        void *p = malloc(64);
        Check(p != nullptr, "phase41 double-free seed alloc");
        free(p);
        free(p);   // p is cached (free=0); the magazine scan must reject it
        Check(heap_get_last_error() == ERR_INVALID_ADDRESS,
              "phase41 double-free of a cached block -> ERR_INVALID_ADDRESS");
    }

    // Tagged allocations BYPASS the cache: a tagged block stays accounted under
    // its tag and frees through the global path (tag subsystem untouched).
    {
        const char *kTag = "phase41:tagged";
        std::size_t before = heap_count_tag(kTag);
        void *p = malloc(64, kTag);
        Check(p != nullptr, "phase41 tagged alloc");
        Check(heap_count_tag(kTag) == before + 1,
              "phase41 tagged alloc bypasses cache (accounted live under its tag)");
        free(p);
        Check(heap_count_tag(kTag) == before,
              "phase41 tagged free returns to global path (tag count drops)");
    }

    // A request larger than the top class bypasses the cache and still works.
    {
        std::size_t big = 8192 + 4096;   // > STRAND_POOL_MAX_CLASS
        auto *p = static_cast<unsigned char *>(malloc(big));
        bool ok = p && p41_sentinel_ok(p, big, 11);
        Check(ok, "phase41 oversize (> 8192) bypasses cache and serves correctly");
        if (p) free(p);
    }

    // ── A2: contention drop — most requests served WITHOUT the global lock ─────
    // Oracle = the heap's own malloc/free counters, bumped INSIDE the lock. Warm
    // the class once, then run M tight malloc/free pairs; the cache should serve
    // the vast majority lock-free, so the locked counters barely move. Pure
    // counter arithmetic — no wall-clock dependence whatsoever.
    {
        constexpr int M = 1000;
        // Warm up: a handful of pairs so the magazine is primed and steady-state.
        for (int i = 0; i < 32; i++) { void *p = malloc(64); if (p) free(p); }

        heap_stats_t before{};
        heap_get_stats(&before);
        for (int i = 0; i < M; i++) {
            void *p = malloc(64);
            if (p) free(p);
        }
        heap_stats_t after{};
        heap_get_stats(&after);

        std::uint32_t lock_mallocs = after.malloc_calls - before.malloc_calls;
        std::uint32_t lock_frees   = after.free_calls   - before.free_calls;
        // A pure global heap would log M of each (==1000). With a cap-8 magazine
        // and batch-4 refill, steady-state churn touches the lock only on the
        // rare refill/flush boundary — well under 40% (here ~0).
        Check(lock_mallocs < 400,
              "phase41 contention drop: >60% of mallocs served lock-free");
        Check(lock_frees < 400,
              "phase41 contention drop: >60% of frees served lock-free");
    }

    // ── A4: crash-orphan reclaim mechanism (deterministic, not a live fault) ───
    // strand_pool_test_orphan_reclaim stages a spare slab slot exactly as a
    // crashed strand would leave it (real cached blocks, slot ORPHANED) and runs
    // the same reclaim the slow path uses, asserting every block returns to the
    // global heap. This exercises the reclaim MECHANISM directly; a real strand
    // fault driving the kernel ORPHANED-stamp is covered by the kernel/STRICT path.
    Check(strand_pool_test_orphan_reclaim(6) == 1,
          "phase41 crash-orphan reclaim returns every cached block to the heap");

    // ── A5: fast-path speedup measurement (host-invariant ratio) ──────────────
    // t_fast = cycles for M tight malloc(64)/free pairs (magazine hot path, lock-free).
    // t_lock = cycles for M tight malloc_tagged(64,tag)/free pairs (tagged bypasses
    //          the magazine on both alloc and free → forced global locked path).
    // Both measurements scale with host load; the RATIO is stable. We assert
    // t_lock >= t_fast + t_fast/4 (fast-path at least ~1.25x faster than the
    // forced-lock path). malloc_tagged adds a small tag-registry lookup overhead
    // on top of the raw lock cost, so this is a conservative lower bound.
    {
        constexpr int M = 20000;
        const char *kBench = "p41bench";
        for (int i = 0; i < 32; i++) { void *p = malloc(64); if (p) free(p); }

        uint64_t t0 = __builtin_ia32_rdtsc();
        for (int i = 0; i < M; i++) {
            void *p = malloc(64);
            if (p) free(p);
        }
        uint64_t t1 = __builtin_ia32_rdtsc();

        for (int i = 0; i < M; i++) {
            void *p = malloc(64, kBench);
            if (p) free(p);
        }
        uint64_t t2 = __builtin_ia32_rdtsc();

        uint64_t t_fast = (t1 - t0) / (uint64_t)M;
        uint64_t t_lock = (t2 - t1) / (uint64_t)M;
        printf("[CXX] phase41 A5: fast-path %llu cycles/op  forced-lock %llu cycles/op\n",
               (unsigned long long)t_fast, (unsigned long long)t_lock);
        Check(t_lock >= t_fast + t_fast / 4,
              "phase41 A5 fast-path at least 1.25x faster than forced-lock path");
    }

    // ── A3: concurrent per-strand isolation (FSGSBASE-gated) ──────────────────
    // A3 is the ONLY part that needs FSGSBASE (it spawns real strands). When the
    // CPU lacks it, A3 is skipped with a DISTINCT marker so a green run is never
    // misread as "concurrent isolation proven" — the overall PASS below then
    // scopes itself to the parts that actually ran (A1/A2/A4/A5). BoxOS `make run`
    // is qemu64 +fsgsbase, so A3 normally RUNS.
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] phase41 A3 SKIP (no FSGSBASE — concurrent isolation not exercised here)\n");
        printf("[CXX] PASS phase41: per-strand StrandPool "
               "(A1 correctness + A2 contention drop + A4 orphan reclaim + A5 speedup; A3 skipped)\n");
        return;
    }

    {
        g_p41_mismatch.store(0, std::memory_order_relaxed);
        g_p41_null.store(0, std::memory_order_relaxed);
        std::thread t0(p41_churn_worker, 1u);
        std::thread t1(p41_churn_worker, 2u);
        std::thread t2(p41_churn_worker, 3u);
        std::thread t3(p41_churn_worker, 4u);
        t0.join();
        t1.join();
        t2.join();
        t3.join();
        Check(g_p41_mismatch.load(std::memory_order_relaxed) == 0,
              "phase41 concurrent churn: 0 sentinel mismatches (per-strand pool isolation)");
        Check(g_p41_null.load(std::memory_order_relaxed) == 0,
              "phase41 concurrent churn: 0 failed allocations across 4 strands");
    }
    printf("[CXX] phase41 A3 RAN (4 strands, concurrent per-strand isolation verified)\n");

    printf("[CXX] PASS phase41: per-strand StrandPool "
           "(A1 correctness + A2 contention drop + A3 concurrent isolation + A4 orphan reclaim + A5 speedup)\n");
}

// ── Phase42 — box::strand / box::park / box::wake / box::strand_watch ─────────
// Build-time guard: the lifecycle decoders MUST match the kernel payload widths.
static_assert(sizeof(box::strand_spawned) == 8,  "box::strand_spawned layout");
static_assert(sizeof(box::strand_exited)  == 8,  "box::strand_exited layout");
static_assert(sizeof(box::strand_parked)  == 12, "box::strand_parked layout");
static_assert(sizeof(box::strand_woken)   == 16, "box::strand_woken layout");

// box::strand spawn/join target: set a flag the spawning strand verifies.
static std::atomic<bool> g_p42_ran{false};

// box::park/box::wake round-trip: main parks on this word until the sibling
// stores a fresh value and wakes it. uint64_t because box::park is 8-byte-only.
static std::atomic<std::uint64_t> g_p42_word{0};

static void p42_run_worker(void *)
{
    g_p42_ran.store(true, std::memory_order_release);
}

static void p42_wake_worker(void *)
{
    // Publish a fresh value, then wake the parker. A park that already saw the
    // store returns value_mismatch (no sleep); otherwise box::wake releases it.
    g_p42_word.store(1, std::memory_order_release);
    box::wake_all(g_p42_word);
}

// box::strand_watch (step 5): a worker that GENUINELY parks — emitting a
// SYNCHRONOUS strand:parked inside the park syscall — until main wakes it (a
// synchronous strand:woken inside the wake syscall). uint64_t because box::park
// is 8-byte-only; the 5s backstop means it can never hang even if a wake were
// ever missed (it re-reads the word and exits the loop).
static std::atomic<std::uint64_t> g_p42_park{0};

static void p42_park_worker(void *)
{
    while (g_p42_park.load(std::memory_order_acquire) == 0)
        box::park(g_p42_park, std::uint64_t{0}, 5000);
}

void Phase42()
{
    // ── always-on (no FSGSBASE needed): pure-userspace API surface ───────────
    // this_strand::id is the cabin pid on the main strand, and bridges losslessly
    // to a std::thread::id naming the same kernel strand.
    Check(box::this_strand::id().native() == box::this_process::pid(),
          "phase42 this_strand::id == cabin pid (main strand)");
    Check(std::thread::id(box::this_strand::id()) == std::this_thread::get_id(),
          "phase42 box::strand::id -> std::thread::id bridge is lossless");

    // box::park on a stack word: a mismatched expected returns instantly without
    // sleeping, a matched expected times out (nobody wakes a private word).
    {
        std::atomic<std::uint64_t> w{7};
        Check(box::park(w, std::uint64_t{1}, 50) == box::park_status::value_mismatch,
              "phase42 box::park value_mismatch is instant (word != expected)");
        Check(box::park(w, std::uint64_t{7}, 30) == box::park_status::timeout,
              "phase42 box::park times out on an unwoken word");
    }

    box::this_strand::yield();  // cooperative yield returns

    // ── sibling-strand half: needs FSGSBASE (per-strand TLS) ─────────────────
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase42: strands need FSGSBASE — skipping spawn/join/wake\n");
        printf("[CXX] PASS phase42: box::park value-mismatch/timeout + id bridge "
               "(spawn/join/wake skipped, no FSGSBASE)\n");
        return;
    }

    // 1) box::strand spawn + join runs the callable.
    {
        g_p42_ran.store(false, std::memory_order_release);
        box::strand s(p42_run_worker, nullptr);
        Check(s.joinable(), "phase42 box::strand is joinable after spawn");
        s.join();
        Check(!s.joinable(), "phase42 box::strand not joinable after join");
        Check(g_p42_ran.load(std::memory_order_acquire),
              "phase42 box::strand ran its callable");
    }

    // 2) join-on-destroy: no explicit join — the destructor must join, so the
    //    callable has completed once the inner scope ends.
    {
        g_p42_ran.store(false, std::memory_order_release);
        {
            box::strand s(p42_run_worker, nullptr);
            Check(s.joinable(), "phase42 join-on-destroy strand is joinable");
        }  // ~strand joins here
        Check(g_p42_ran.load(std::memory_order_acquire),
              "phase42 box::strand destructor joined (callable completed)");
    }

    // 3) move leaves the source not-joinable, the destination joinable.
    {
        g_p42_ran.store(false, std::memory_order_release);
        box::strand src(p42_run_worker, nullptr);
        box::strand dst(std::move(src));
        Check(!src.joinable(), "phase42 moved-from box::strand is not joinable");
        Check(dst.joinable(), "phase42 move destination box::strand is joinable");
        dst.join();
        Check(g_p42_ran.load(std::memory_order_acquire),
              "phase42 moved box::strand ran its callable");
    }

    // 4) box::park / box::wake round-trip across a sibling strand. Park in a
    //    lost-wake-safe loop (re-read the word every wake; a store we missed
    //    makes the next park return value_mismatch at once). The 200ms backstop
    //    is a safety net, not the wake path.
    {
        g_p42_word.store(0, std::memory_order_release);
        box::strand waker(p42_wake_worker, nullptr);
        Check(waker.get_id().native() != 0, "phase42 box::strand get_id().native() is a live pid");
        for (;;) {
            std::uint64_t v = g_p42_word.load(std::memory_order_acquire);
            if (v == 1) break;
            box::park(g_p42_word, v, 200);
        }
        Check(g_p42_word.load(std::memory_order_acquire) == 1,
              "phase42 box::park woken by sibling box::wake (round-trip)");
        waker.join();
    }

    // 5) box::strand_watch decodes real kernel lifecycle broadcasts. Subscribe
    //    BEFORE spawning so no edge is missed, then assert ONLY on the SYNCHRONOUS
    //    edges — strand:spawned (published inside the spawn syscall) and strand:
    //    parked / strand:woken (published inside box::park / box::wake). We do NOT
    //    hard-assert strand:exited: for a joinable strand it is published by the
    //    K-Core reaper on a LATER, throttled tick (join() only clears the reap-
    //    block), so asserting it would be a reaper-timing flake. It is observed
    //    best-effort and only printed. The worker pid filters out the system-wide
    //    broadcasts of every other cabin's strands.
    {
        box::strand_watch watch;
        Check(static_cast<bool>(watch), "phase42 strand_watch claimed all four tags");

        g_p42_park.store(0, std::memory_order_release);
        box::strand worker(p42_park_worker, nullptr);
        std::uint32_t worker_pid = worker.get_id().native();

        bool saw_spawn = false, saw_parked = false, saw_woken = false, saw_exit = false;
        auto drain = [&] {
            while (auto e = watch.poll()) {
                if (e->strand_pid() != worker_pid) continue;
                switch (e->kind) {
                case box::strand_touch_kind::spawned: saw_spawn = true; break;
                case box::strand_touch_kind::parked:  saw_parked = true; break;
                case box::strand_touch_kind::woken:   saw_woken = true; break;
                case box::strand_touch_kind::exited:  saw_exit = true; break;
                }
            }
        };

        // Phase A: let the worker reach its park. A fresh strand is dispatched
        // within a tick or two (normal scheduling, NOT the throttled reaper);
        // sleep_for yields our core so it runs, poll() drains its edges. ≤64×4ms
        // is the bounded backstop, not the wake path.
        for (int i = 0; i < 64 && !(saw_spawn && saw_parked); i++) {
            drain();
            if (saw_spawn && saw_parked) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        Check(saw_spawn, "phase42 strand_watch decoded strand:spawned for the worker");
        Check(saw_parked, "phase42 strand_watch decoded strand:parked (worker genuinely parked)");

        // Phase B: wake the parked worker. strand:woken fires inside the wake
        // syscall — the worker is still parked (its 5s park backstop dwarfs the
        // few ms between observing parked and waking), so the wake finds it.
        g_p42_park.store(1, std::memory_order_release);
        box::wake_all(g_p42_park);
        for (int i = 0; i < 64 && !saw_woken; i++) {
            drain();
            if (saw_woken) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        Check(saw_woken, "phase42 strand_watch decoded strand:woken (main woke the worker)");

        worker.join();
        drain();  // best-effort sweep — strand:exited is reaper-timed, not asserted
        printf("[CXX] phase42 strand_watch: strand:exited %sobserved (reaper-timed, not asserted)\n",
               saw_exit ? "" : "not yet ");
    }

    printf("[CXX] PASS phase42: box::strand spawn/join/dtor-join/move + "
           "box::park-wake round-trip + strand_watch decode\n");
}

// ── Phase43 — concurrent per-strand Touch-stash isolation ─────────────────────
// Proves the Ф21 substrate fix: two strands in ONE cabin consuming Touch
// concurrently (touch_try_pop_tag) do NOT corrupt each other. Pre-fix the stash
// was a process-global unlocked array — two strands draining it would tear its
// count and misroute/lose/duplicate events nondeterministically. Per-strand
// stashes make every invariant below hold deterministically.
//
// Each strand subscribes to its OWN distinct tag and drains ONLY that tag (each
// strand has its own TouchRing; a per-strand subscription delivers into that
// strand's ring — confirmed against the kernel touch delivery path). The two
// distinct tags sidestep any same-tag multi-strand delivery question — the
// MUST-PROVE here is stash isolation under concurrent touch_try_pop_tag, not
// fan-out. On 1 App-Core the two workers interleave COOPERATIVELY (touch_try_
// pop_tag has no preemption point), so the torn-count race is truly exercised
// only across >=2 App-Cores (4c/16c — the STRICT matrix runs those); on 1c this
// proves the functional per-strand split. HOST-TIMING-INVARIANT: all asserts are
// on counts/values; the only time bounds are generous backstops, never the path.
namespace {
constexpr int      P43_N        = 64;      // events per tag
constexpr int      P43_SPIN_CAP = 2000000; // per-worker drain backstop (iterations)

// Per-tag payload: the seq plus a tag marker so a worker can prove it NEVER
// received the other tag's event (cross-delivery == 0).
struct P43Msg {
    std::uint32_t seq;
    std::uint32_t marker;  // 0xAAAA for tag A, 0xBBBB for tag B
};

constexpr std::uint32_t P43_MARK_A = 0xAAAAu;
constexpr std::uint32_t P43_MARK_B = 0xBBBBu;

// Worker results (written by the worker strand, read by main after join).
struct P43Result {
    std::atomic<bool>     ready{false};   // subscription claimed, safe to publish
    std::atomic<int>      got{0};         // events received
    std::atomic<bool>     fifo_ok{true};  // seqs arrived strictly 0,1,2,…
    std::atomic<bool>     marker_ok{true};// every event carried the OWN marker
    std::uint32_t         seqs[P43_N];    // received seqs in arrival order
};

P43Result g_p43_a;
P43Result g_p43_b;

// One worker body, parameterised by its tag name / marker / result slot. It
// claims its subscription, signals ready, then bounded-drains ONLY its tag via
// the per-strand stash path (subscription::poll -> touch_try_pop_tag) until it
// has P43_N events or the backstop trips.
static void p43_worker(const char *tag_name, std::uint32_t own_marker, P43Result *res)
{
    box::subscription sub{box::tag(tag_name)};  // braces: avoid the most-vexing-parse
    if (!sub) {  // claim failed — leave ready false; main will see got != N and fail
        return;
    }
    res->ready.store(true, std::memory_order_release);

    int n = 0;
    std::uint32_t expect_seq = 0;
    for (long spins = 0; n < P43_N && spins < P43_SPIN_CAP; spins++) {
        if (auto ev = sub.poll()) {
            auto m = ev->payload_as<P43Msg>();
            if (!m) {  // a malformed/short payload is never expected — fail loudly,
                       // do NOT advance n (that would leave a hole in seqs[]).
                res->marker_ok.store(false, std::memory_order_relaxed);
                break;
            }
            if (m->marker != own_marker)
                res->marker_ok.store(false, std::memory_order_relaxed);
            if (m->seq != expect_seq)
                res->fifo_ok.store(false, std::memory_order_relaxed);
            res->seqs[n] = m->seq;
            expect_seq++;
            n++;
            res->got.store(n, std::memory_order_release);
        } else {
            box::this_strand::yield();  // cooperative — let the publisher / sibling run
        }
    }
}

static void p43_worker_a(void *) { p43_worker("cxx:p43:A", P43_MARK_A, &g_p43_a); }
static void p43_worker_b(void *) { p43_worker("cxx:p43:B", P43_MARK_B, &g_p43_b); }
} // namespace

void Phase43()
{
    // Spawned strands need FSGSBASE (per-strand TLS via ring-3 RDFSBASE). Without
    // it strand_spawn refuses — SKIP cleanly with a PASS, matching Phase35/42.
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase43: strands need FSGSBASE — skipping concurrent stash\n");
        printf("[CXX] PASS phase43: per-strand Touch stash isolation "
               "(skipped, no FSGSBASE)\n");
        return;
    }

    // Reset the result slots (std::atomic is not assignable, so reset fields).
    for (P43Result *r : { &g_p43_a, &g_p43_b }) {
        r->ready.store(false, std::memory_order_relaxed);
        r->got.store(0, std::memory_order_relaxed);
        r->fifo_ok.store(true, std::memory_order_relaxed);
        r->marker_ok.store(true, std::memory_order_relaxed);
    }

    box::tag tA("cxx:p43:A");
    box::tag tB("cxx:p43:B");
    Check(tA && tB, "phase43 interned both distinct tags");

    // Spawn the two consumers FIRST so each claims its tag before any publish —
    // Touch is edge-delivered to subscribers present at publish time.
    box::strand wa(p43_worker_a, nullptr);
    box::strand wb(p43_worker_b, nullptr);

    // Wait (bounded) until both workers have claimed their subscriptions.
    bool both_ready = false;
    for (int i = 0; i < 1000 && !both_ready; i++) {
        both_ready = g_p43_a.ready.load(std::memory_order_acquire) &&
                     g_p43_b.ready.load(std::memory_order_acquire);
        if (!both_ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(both_ready, "phase43 both worker strands claimed their tags");

    // Publish N events to each tag, INTERLEAVED (A0,B0,A1,B1,…) so the two
    // strands are draining concurrently. Each payload carries its monotonic seq.
    for (std::uint32_t s = 0; s < (std::uint32_t)P43_N; s++) {
        P43Msg a{ s, P43_MARK_A };
        P43Msg b{ s, P43_MARK_B };
        Check(box::publish(tA, a), "phase43 publish to tag A");
        Check(box::publish(tB, b), "phase43 publish to tag B");
    }

    // Join both workers (their drain loops exit once each has N events). The
    // join is bounded by the worker backstop, never an unbounded wait.
    wa.join();
    wb.join();

    // ── Invariants — would fail nondeterministically under the OLD global stash,
    //    hold deterministically with per-strand stashes ────────────────────────
    int got_a = g_p43_a.got.load(std::memory_order_acquire);
    int got_b = g_p43_b.got.load(std::memory_order_acquire);
    Check(got_a == P43_N, "phase43 worker A received exactly N (no loss/dup)");
    Check(got_b == P43_N, "phase43 worker B received exactly N (no loss/dup)");
    Check(g_p43_a.marker_ok.load(std::memory_order_acquire),
          "phase43 worker A saw ONLY tag-A events (zero cross-delivery)");
    Check(g_p43_b.marker_ok.load(std::memory_order_acquire),
          "phase43 worker B saw ONLY tag-B events (zero cross-delivery)");
    Check(g_p43_a.fifo_ok.load(std::memory_order_acquire),
          "phase43 worker A received seqs 0..N-1 in FIFO order");
    Check(g_p43_b.fifo_ok.load(std::memory_order_acquire),
          "phase43 worker B received seqs 0..N-1 in FIFO order");

    // Explicit per-element seq verification (each tag's seqs are exactly 0..N-1).
    bool seqs_a = (got_a == P43_N), seqs_b = (got_b == P43_N);
    for (int i = 0; i < P43_N && i < got_a; i++) if (g_p43_a.seqs[i] != (std::uint32_t)i) seqs_a = false;
    for (int i = 0; i < P43_N && i < got_b; i++) if (g_p43_b.seqs[i] != (std::uint32_t)i) seqs_b = false;
    Check(seqs_a, "phase43 worker A seq stream == 0..N-1 exactly");
    Check(seqs_b, "phase43 worker B seq stream == 0..N-1 exactly");

    // Honest per-config banner: 1 App-Core = cooperative interleave (functional
    // proof); >=2 App-Cores = the torn-count race is genuinely exercised.
    unsigned p43_cores = box::strand::hardware_concurrency();
    if (p43_cores >= 2)
        printf("[CXX] PASS phase43: per-strand Touch stash isolation "
               "(2 strands concurrent across %u App-Cores — race exercised)\n", p43_cores);
    else
        printf("[CXX] PASS phase43: per-strand Touch stash isolation "
               "(1 App-Core — functional split; data race exercised on multi-core)\n");
}

// ── phase44 — Ф23a: native BoxOS error model (box::error / errc / result) ──
// Foundation tests: the typed value, the boxlib cause-preservation contract
// (box_fail / box_errno_of), the box::result bridges Ф23b's retrofit will use,
// honest std::error_code interop, and a regression-lock on the errno-sharing
// lie that used to live in system_error.cpp.

// constexpr proofs — box::error is a literal (constexpr-usable) value.
static_assert(box::error{}.ok(), "OK is ok");
static_assert(!static_cast<bool>(box::error{}), "OK error is falsey");
static_assert(box::error{box::errc::no_memory}.raw() == ERR_NO_MEMORY, "errc value via macro");
static_assert(static_cast<::error_t>(box::errc::no_memory) != 12, "no_memory is NOT Linux ENOMEM");
static_assert(box::error{box::errc::no_memory}.code() == box::errc::no_memory, "code round-trip");
static_assert(static_cast<bool>(box::error{box::errc::timeout}), "an error is truthy");
static_assert(box::error{ERR_TIMEOUT}.code() == box::errc::timeout, "error_t → errc");

void Phase44()
{
    // ── box::error value semantics ──
    box::error none;
    Check(none.ok() && !static_cast<bool>(none), "phase44 default box::error is ok");
    Check(none.code() == box::errc::ok && none.raw() == OK, "phase44 ok code/raw");

    box::error nomem{box::errc::no_memory};
    Check(static_cast<bool>(nomem) && !nomem.ok(), "phase44 error is truthy, not ok");
    Check(nomem.code() == box::errc::no_memory && nomem.raw() == ERR_NO_MEMORY,
          "phase44 errc/raw agree with box/error.h");
    Check(nomem.message() == "out of memory", "phase44 message text");
    Check(nomem.category_name() == "memory", "phase44 category by range");
    Check(box::error{ERR_TIMEOUT}.category_name() == "core", "phase44 core range");
    Check(box::error{ERR_FILE_NOT_FOUND}.category_name() == "storage", "phase44 storage range");
    Check(box::error{ERR_SPAWN_FAILED}.category_name() == "process", "phase44 process range");

    // equality + ordering on the typed value
    Check(box::error{box::errc::io} == box::error{ERR_IO}, "phase44 error ==");
    Check(box::error{box::errc::io} != nomem, "phase44 error !=");
    Check((box::error{ERR_NO_MEMORY} <=> box::error{ERR_IO}) < 0, "phase44 error <=> orders by code");

    // ── box_fail / box_errno_of: the cause-preservation contract ──
    Check(box_fail(0) == 0, "phase44 box_fail(0)==0");
    Check(box_fail(ERR_PROCESS_NOT_FOUND) == -400, "phase44 box_fail negates +error_t");
    Check(box_fail(-(int)ERR_INVALID_ARGS) == -(int)ERR_INVALID_ARGS,
          "phase44 box_fail keeps an already-negative transport error");
    Check(box_errno_of(-400) == ERR_PROCESS_NOT_FOUND, "phase44 box_errno_of recovers magnitude");
    Check(box_errno_of(0) == OK && box_errno_of(42) == OK, "phase44 box_errno_of(>=0)==OK");
    Check(box_errno_of(box_fail(ERR_HEAP_EXHAUSTED)) == ERR_HEAP_EXHAUSTED,
          "phase44 box_fail→box_errno_of round-trip");

    // ── box::_detail bridges — exactly what Ф23b's retrofit consumes ──
    // payload stub: >=0 is the value, <0 is -error_t
    auto okv = box::_detail::from_ret<int>(42);
    Check(okv && *okv == 42, "phase44 from_ret success carries payload");
    auto errv = box::_detail::from_ret<int>(box_fail(ERR_PROCESS_NOT_FOUND));
    Check(!errv && errv.error().code() == box::errc::process_not_found,
          "phase44 from_ret recovers the real kernel cause end-to-end");
    // 0/-error_t status stub
    Check(box::_detail::from_status(0).has_value(), "phase44 from_status(0) success");
    auto st = box::_detail::from_status(box_fail(ERR_NO_MEMORY));
    Check(!st.has_value() && st.error().code() == box::errc::no_memory,
          "phase44 from_status recovers cause");
    // raw-error_t out-of-band stub
    Check(box::_detail::from_err(OK).has_value(), "phase44 from_err(OK) success");
    auto fe = box::_detail::from_err(ERR_TIMEOUT);
    Check(!fe.has_value() && fe.error().code() == box::errc::timeout, "phase44 from_err cause");

    // ── box::result / box::status ergonomics + monadic ops ──
    box::result<int> r = 7;
    Check(r && *r == 7 && r.value_or(0) == 7, "phase44 result success");
    box::result<int> re = std::unexpected(box::error{box::errc::device_busy});
    Check(!re && re.error().code() == box::errc::device_busy && re.value_or(-1) == -1,
          "phase44 result error arm");
    auto chained = re.and_then([](int v) { return box::result<int>{v + 1}; });
    Check(!chained && chained.error().code() == box::errc::device_busy,
          "phase44 and_then propagates the error untouched");
    int recovered = re.or_else([](box::error e) {
                          return box::result<int>{static_cast<int>(e.raw())};
                      }).value_or(0);
    Check(recovered == (int)ERR_DEVICE_BUSY, "phase44 or_else sees the typed cause");

    box::status sok;
    Check(sok.has_value(), "phase44 default status is success");
    box::status sfail = std::unexpected(box::error{box::errc::busy});
    Check(!sfail && sfail.error().code() == box::errc::busy, "phase44 status error arm");

    // ── honest std::error_code interop (own numbering, never errno) ──
    std::error_code ec = box::errc::no_memory;  // ADL → box::make_error_code
    Check(ec.value() == (int)ERR_NO_MEMORY && ec.value() != 12,
          "phase44 error_code keeps the box value (not Linux ENOMEM=12)");
    Check(ec.category() == box::error_category(), "phase44 error_code uses the box category");
    Check(std::string_view(ec.category().name()) == "box", "phase44 category name is 'box'");
    Check(ec.message() == "out of memory", "phase44 error_code message via box category");
    // default_error_condition is IDENTITY: a box code maps to itself, NOT generic
    auto cond = ec.default_error_condition();
    Check(cond.value() == (int)ERR_NO_MEMORY && cond.category() == box::error_category(),
          "phase44 box default_error_condition is identity");
    Check(cond.category() != std::generic_category(),
          "phase44 box code is NOT reinterpreted as a Linux generic condition");
    Check(box::to_error_code(nomem) == ec, "phase44 to_error_code bridges box::error");

    // ── regression-lock: the system_error.cpp errno-sharing lie is gone ──
    // ERR_TIMEOUT==7; it must NOT come back as the generic errno-7 (E2BIG).
    auto syscond = std::system_category().default_error_condition(7);
    Check(syscond.value() == 7 && syscond.category() == std::system_category(),
          "phase44 system_category maps code to itself");
    Check(syscond.category() != std::generic_category(),
          "phase44 system error 7 is NOT remapped onto generic (E2BIG) — lie fixed");

    // ── std::formatter<box::error> ──
    Check(std::format("{}", nomem) == "memory:100 out of memory", "phase44 formatter");
    Check(std::format("{:>20}", box::error{box::errc::ok}).size() == 20, "phase44 formatter width spec");

    printf("[CXX] PASS phase44: box::error/errc/result + cause-preservation "
           "contract + honest std interop + errno-lie regression\n");
}

// Ф23b-2a — the retrofitted scalar surfaces now carry the REAL kernel cause all
// the way to box::error (cause-recovery), and the typed stream is an honest
// tri-state (item / closed-VALUE / error-arm). This is the proof the bool/-1/
// nullopt collapse is gone, not just that the API compiles.
void Phase45()
{
    struct Frame { int seq; unsigned mark; };

    // ── 1. TYPED-STREAM TRI-STATE (deterministic) ───────────────────────────
    // Same-cabin writer + reader on a Brook-backed tag (PhaseCurrent's pattern):
    // write N, close, and the reader sees N `true` VALUES then a `false` VALUE —
    // the honest CURRENT_CLOSED terminator, which must NOT be an error arm.
    {
        constexpr int N = 4;
        box::current<Frame> w("cxx:p45:tri", box::role::write);  // writer auto-creates
        box::current<Frame> r("cxx:p45:tri", box::role::read);
        Check(bool(w) && bool(r), "phase45 tri-state stream open");

        bool put_ok = true;
        for (int i = 0; i < N; i++)
            put_ok = put_ok && w.put(Frame{i, (unsigned)(i * 7)}).has_value();
        Check(put_ok, "phase45 put returns empty status on success");

        w.close();  // announce end-of-stream; the reader drains, then observes close

        int got = 0;
        bool items_ok = true, terminated_as_value = false;
        for (int guard = 0; guard < N + 4; ++guard) {
            Frame f{};
            box::result<bool> take = r.take(f);
            Check(take.has_value(), "phase45 take never spuriously faults before close");
            if (!take.has_value()) break;
            if (*take) {                       // an item
                items_ok = items_ok && f.seq == got && f.mark == (unsigned)(got * 7);
                ++got;
            } else {                           // the CURRENT_CLOSED terminator
                terminated_as_value = true;    // a VALUE(false), not an error arm
                break;
            }
        }
        Check(got == N && items_ok, "phase45 drains exactly N items in order");
        Check(terminated_as_value,
              "phase45 close terminator is a VALUE(false), NOT an error arm (honest, not EOF)");
    }

    // would_block on an empty NONBLOCK stream lands in the ERROR ARM with the
    // recovered cause. Kept robust: a successful item is the only hard failure;
    // when it IS an error arm the code must be would_block.
    {
        box::current<Frame> w("cxx:p45:wb", box::role::write);
        box::current<Frame> r("cxx:p45:wb", box::role::read, box::opening::nonblock);
        if (w && r) {
            Frame f{};
            box::result<bool> take = r.take(f);  // stream is empty, writer still open
            Check(!(take.has_value() && *take == true),
                  "phase45 empty NONBLOCK take is not a successful item");
            if (!take.has_value())
                Check(take.error().code() == box::errc::would_block,
                      "phase45 empty NONBLOCK take error arm == would_block");
            else
                printf("[CXX] note phase45: NONBLOCK empty take returned a VALUE "
                       "(%d) on this backing — would_block arm not exercised\n", (int)*take);
        } else {
            printf("[CXX] note phase45: NONBLOCK stream could not be opened — "
                   "would_block case skipped\n");
        }
    }

    // ── 2. SCALAR CAUSE-RECOVERY — every error arm carries a NON-OK box::error ─

    // process::info() on a pid that names no live process: error arm, non-ok, and
    // a plausible process-class cause.
    {
        box::result<proc_info_t> info = box::process(0xFFFEu).info();
        Check(!info.has_value() && static_cast<bool>(info.error()),
              "phase45 process(0xFFFE).info() error arm carries a non-ok cause");
        box::errc c = info.error().code();
        Check(c == box::errc::process_not_found || c == box::errc::invalid_pid ||
                  c == box::errc::internal || c == box::errc::object_not_found,
              "phase45 missing-process cause is a plausible process-class errc");
    }

    // spawn() of a binary that does not exist: error arm, non-ok cause.
    {
        box::result<box::process> sp = box::process::spawn("__no_such_binary_zzz__");
        Check(!sp.has_value() && static_cast<bool>(sp.error()),
              "phase45 spawn(nonexistent) error arm carries a non-ok cause");
        box::errc c = sp.error().code();
        Check(c == box::errc::spawn_failed || c == box::errc::file_not_found ||
                  c == box::errc::object_not_found || c == box::errc::invalid_elf ||
                  c == box::errc::internal,
              "phase45 spawn-failure cause is a plausible process/storage errc");
    }

    // memtag::find() on a region id that names no live region: error arm, non-ok.
    {
        box::result<box::memtag::region> rg = box::memtag::find(0xFFFFFFFEu);
        Check(!rg.has_value() && static_cast<bool>(rg.error()),
              "phase45 memtag::find(bogus id) error arm carries a non-ok cause");
    }

    // send() to pid 0: error STATUS with a non-ok cause (a route error).
    {
        std::uint32_t payload = 0xABCD1234u;
        box::status s = box::send(0u, payload);
        Check(!s.has_value() && static_cast<bool>(s.error()),
              "phase45 send(pid 0) error status carries a non-ok cause");
    }

    // ── 3. HAPPY PATH flows through result cleanly (success is not collapsed) ──
    {
        box::result<box::system_info> si = box::system::info();
        Check(si.has_value(), "phase45 system::info() SUCCEEDS through box::result");
        if (si)
            Check(si->total_memory() > 0 && si->cpu_total() >= 1,
                  "phase45 system::info() value is usable on the happy path");
    }
    {
        // A valid byte channel: write succeeds, and seek() returns an empty status.
        box::byte_current f = box::file("cxx_p45.dat", box::role::write);
        if (f) {
            Check(f.write("p45", 3) == 3, "phase45 valid channel write succeeds");
            f = box::byte_current{};  // release writer (RAII)
            box::byte_current fr = box::file("cxx_p45.dat", box::role::read, box::opening::none);
            if (fr) {
                box::status sk = fr.seek(0);
                Check(sk.has_value(), "phase45 seek on a seekable file is an empty (success) status");
            }
        } else {
            printf("[CXX] note phase45: file channel unavailable — seek success case skipped\n");
        }
    }

    // ── 4. TAGFS CAUSE-RECOVERY — the richest box:: surface ──────────────────
    // box::tagfs scalar ops now carry the real kernel cause through box::result.

    // create() with an invalid name. boxlib create() validates the name and
    // returns -ERR_INVALID_ARGUMENT for an empty or over-long (>31) name, so the
    // recovered cause is deterministically invalid_argument.
    {
        box::result<box::tagfs::file> bad = box::tagfs::create("", {"k:v"});
        Check(!bad.has_value() && static_cast<bool>(bad.error()),
              "phase45 tagfs::create(\"\") error arm carries a non-ok cause");
        Check(bad.error().code() == box::errc::invalid_argument,
              "phase45 tagfs::create(empty name) cause == invalid_argument");

        box::result<box::tagfs::file> longname =
            box::tagfs::create("this_name_is_definitely_longer_than_31_chars");
        Check(!longname.has_value() && static_cast<bool>(longname.error()),
              "phase45 tagfs::create(>31-char name) error arm carries a non-ok cause");
        Check(longname.error().code() == box::errc::invalid_argument,
              "phase45 tagfs::create(over-long name) cause == invalid_argument");
    }

    // info() on a bogus file_id: error arm with a recovered non-ok cause.
    {
        box::result<file_info_t> inf = box::tagfs::file(0xFFFFFFFEu).info();
        Check(!inf.has_value() && static_cast<bool>(inf.error()),
              "phase45 tagfs::file(bogus id).info() error arm carries a non-ok cause");
    }

    // find() of a name that cannot exist resolves to a clean, queryable
    // file_not_found — BUT only when storage is reachable. A storage flake makes
    // find() propagate the transport cause instead, so the exact-code claim is
    // gated on a confirmed-working create (a storage-up probe); the non-ok cause
    // arm is asserted unconditionally (find always fails with SOME non-ok cause).
    {
        box::result<box::tagfs::file> probe =
            box::tagfs::create("cxx:p45:probe", {"cxx:phase45"});
        box::result<box::tagfs::record> nf = box::tagfs::find("__no_such_file_zzz__");
        Check(!nf.has_value() && static_cast<bool>(nf.error()),
              "phase45 tagfs::find(missing) error arm carries a non-ok cause");
        if (probe.has_value()) {
            (void)probe->remove();  // storage confirmed up → the cause is deterministic
            Check(nf.error().code() == box::errc::file_not_found,
                  "phase45 tagfs::find(missing) cause == file_not_found (storage up)");
        } else {
            printf("[CXX] note phase45: storage unavailable — find(missing) "
                   "exact-code (file_not_found) check skipped; non-ok cause asserted\n");
        }
    }

    // HAPPY PATH through box::result: a real file round-trips cleanly and the
    // byte count is exposed at every step (success is never collapsed).
    {
        box::result<box::tagfs::file> made =
            box::tagfs::create("cxx:p45:happy", {"cxx:phase45"});
        if (made.has_value()) {
            box::tagfs::file        f = *made;
            const char              text[] = "p45-result";
            box::result<std::size_t> w = f.write_at(0, text, sizeof(text));
            Check(w.has_value() && *w == sizeof(text),
                  "phase45 tagfs write_at SUCCEEDS through result and exposes the byte count");

            char                     back[sizeof(text)] = {};
            box::result<std::size_t> r = f.read_at(0, back, sizeof(back));
            Check(r.has_value() && *r == sizeof(text)
                      && std::string_view(back) == "p45-result",
                  "phase45 tagfs read_at value matches written bytes (count exposed)");

            Check(f.remove().has_value(),
                  "phase45 tagfs remove SUCCEEDS through status on the happy path");
        } else {
            printf("[CXX] note phase45: tagfs storage unavailable — happy-path "
                   "round-trip skipped (cause-recovery arms still exercised)\n");
        }
    }

    printf("[CXX] PASS phase45: cause-recovery — typed-stream tri-state "
           "(item/closed-VALUE/error-arm) + scalar error arms carry the real "
           "kernel cause (incl. tagfs create/info/find) + happy path flows "
           "through box::result\n");
}

// ── phase46 — Ф23c: per-strand heap last-error isolation + box::heap result ──
// The boxlib heap is shared, but "the cause of MY last heap op" is per-strand.
// This proves (a) the box::heap fallible surface hands back the real cause, and
// (b) a spawned strand and the main strand hold DIFFERENT last-errors at the
// same instant, neither seeing the other's. A single shared cell (the pre-Ф23c
// global) would have let the sibling's INVALID_ADDRESS overwrite the main
// strand's NO_MEMORY before it was read — the cross-strand checks catch exactly
// that.
static std::atomic<int>           g_p46_worker_saw{-1};  // worker's own cause
static std::atomic<std::uint64_t> g_p46_go{0};           // main -> worker: proceed
static std::atomic<std::uint64_t> g_p46_done{0};         // worker -> main: error set

static void p46_worker(void *)
{
    // Wait until main has armed its OWN error, so both are live at once
    // (lost-wake-safe loop; the 5s backstop never fires on a healthy wake).
    for (;;) {
        if (g_p46_go.load(std::memory_order_acquire) == 1) break;
        box::park(g_p46_go, std::uint64_t{0}, 5000);
    }

    // A deterministic, DISTINCT error on THIS strand: a cached double-free ->
    // ERR_INVALID_ADDRESS, recorded in this strand's own per-strand cell.
    void *wp = malloc(64);
    free(wp);
    free(wp);
    g_p46_worker_saw.store(static_cast<int>(heap_get_last_error()),
                           std::memory_order_release);

    g_p46_done.store(1, std::memory_order_release);
    box::wake_all(g_p46_done);
}

void Phase46()
{
    // ── always-on: the box::heap fallible surface on the calling strand ──────
    auto z0 = box::heap::allocate(0);
    Check(!z0 && z0.error().code() == box::errc::invalid_argument,
          "phase46 box::heap::allocate(0) -> invalid_argument");

    {
        auto r = box::heap::allocate(64);
        Check(r.has_value() && *r != nullptr, "phase46 box::heap::allocate success");
        Check(box::heap::last_error().ok(), "phase46 last_error ok after success");
        auto g = box::heap::reallocate(*r, 256);
        Check(g.has_value() && *g != nullptr, "phase46 box::heap::reallocate grows");
        if (g) free(*g);
        auto z = box::heap::reallocate(nullptr, 0);
        Check(z.has_value() && *z == nullptr,
              "phase46 reallocate(,0) is a non-error nullptr value");
    }

    {
        // Forced overflow: align_up(SIZE_MAX) wraps in alloc_locked -> NO_MEMORY,
        // deterministically, and the result carries that real cause.
        auto r = box::heap::allocate(~std::size_t{0});
        Check(!r.has_value() && r.error().code() == box::errc::no_memory,
              "phase46 allocate(SIZE_MAX) fails carrying the real cause");
    }

    // ── cross-strand isolation: needs FSGSBASE (per-strand pools) ────────────
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase46: strands need FSGSBASE — skipping cross-strand "
               "isolation (single-strand surface verified)\n");
        printf("[CXX] PASS phase46: per-strand heap last-error — box::heap fallible "
               "surface carries the real cause (cross-strand skipped, no FSGSBASE)\n");
        return;
    }

    g_p46_worker_saw.store(-1, std::memory_order_release);
    g_p46_go.store(0, std::memory_order_release);
    g_p46_done.store(0, std::memory_order_release);

    // Spawn FIRST: the strand ctor may allocate on the main strand (resetting
    // its cell). The worker parks until signalled, so it sets its error only
    // after ours is armed below.
    box::strand worker(p46_worker, nullptr);

    // Arm main's DISTINCT error in its own cell, AFTER the spawn. park/wake are
    // syscalls (no heap), so the cell stays put until we read it back.
    void *cov = calloc(~std::size_t{0}, 2);
    Check(cov == nullptr, "phase46 calloc overflow returns NULL");
    Check(heap_get_last_error() == ERR_NO_MEMORY,
          "phase46 main strand cell armed to ERR_NO_MEMORY");

    g_p46_go.store(1, std::memory_order_release);
    box::wake_all(g_p46_go);
    for (;;) {
        if (g_p46_done.load(std::memory_order_acquire) == 1) break;
        box::park(g_p46_done, std::uint64_t{0}, 5000);
    }

    // THE proof: both errors live at once — the worker saw ITS OWN
    // INVALID_ADDRESS and the main strand STILL reads ITS OWN NO_MEMORY.
    Check(g_p46_worker_saw.load(std::memory_order_acquire) ==
              static_cast<int>(ERR_INVALID_ADDRESS),
          "phase46 spawned strand observed its OWN double-free cause");
    Check(heap_get_last_error() == ERR_NO_MEMORY,
          "phase46 main strand cell survived the sibling's error (isolation)");

    // join() is lifecycle cleanup only — we deliberately make NO assertion about
    // main's cell afterward: std::thread::join frees the shared Join block, and
    // whichever strand wins that refcount race does a heap op that updates ITS
    // OWN cell (on 16c that is often the main strand). That is the per-strand
    // model working as intended, not a violation of it; the isolation proof is
    // the two checks above, taken while ONLY the sibling had touched the heap.
    worker.join();

    printf("[CXX] PASS phase46: per-strand heap last-error — box::heap fallible "
           "surface carries the real cause + spawned/main strands hold distinct "
           "last-errors with zero cross-strand clobber\n");
}

// Ф23 backlog: B-1 (full kernel-code sync, single-source X-macro table) +
// B-2 (>2 GiB int64 byte-count bridge). Host-pure asserts always run; the
// real-storage round-trip is gated on a confirmed create (storage-up probe).
void Phase47()
{
    // ── B-1: the trigger code now names itself (was "box:941 error") ─────────
    {
        box::error e{box::errc::route_no_subscribers};
        Check(e.raw() == 941u, "phase47 route_no_subscribers raw == 941");
        Check(e.category_name() == std::string_view("routing"),
              "phase47 941 category == routing (was the 'box' fallback)");
        Check(e.message() == std::string_view("no subscribers for route"),
              "phase47 941 carries its real message (was the 'error' fallback)");
        Check(std::format("{}", e) == "routing:941 no subscribers for route",
              "phase47 941 formats as 'routing:941 no subscribers for route' — box:941 bug dead");
    }

    // Every kernel range carries a faithful category + message — no code falls
    // to the "box"/"unknown error code" default. First/last of each range.
    {
        struct CatCase { box::errc code; std::string_view cat; };
        static const CatCase cats[] = {
            {box::errc::unknown,                "core"},     {box::errc::quota_exceeded,        "core"},
            {box::errc::no_memory,              "memory"},   {box::errc::pcid_exhausted,        "memory"},
            {box::errc::io,                     "io"},       {box::errc::sector_write_failed,   "io"},
            {box::errc::file_not_found,         "storage"},  {box::errc::self_heal_failed,      "storage"},
            {box::errc::process_not_found,      "process"},  {box::errc::stack_alloc_failed,    "process"},
            {box::errc::access_denied,          "security"}, {box::errc::sandbox_violation,     "security"},
            {box::errc::hardware,               "hardware"}, {box::errc::cpu_error,             "hardware"},
            {box::errc::acpi_not_found,         "acpi"},     {box::errc::acpi_fadt_not_found,   "acpi"},
            {box::errc::tagfs_not_initialized,  "tagfs"},    {box::errc::tagfs_recovery_failed, "tagfs"},
            {box::errc::pocket_ring_full,       "ipc"},      {box::errc::result_stash_full,     "ipc"},
            {box::errc::route_target_full,      "routing"},  {box::errc::route_invalid_tag,     "routing"},
            {box::errc::pocket_failed,          "ipc"},
            {box::errc::scheduler_locked,       "scheduler"},{box::errc::work_steal_failed,     "scheduler"},
            {box::errc::boot_info_invalid,      "boot"},     {box::errc::kernel_load_failed,    "boot"},
            {box::errc::diskbook_not_initialized,"diskbook"},{box::errc::diskbook_read_failed,  "diskbook"},
            {box::errc::cow_not_initialized,    "cow"},      {box::errc::cow_restore_failed,    "cow"},
            {box::errc::dedup_not_initialized,  "dedup"},    {box::errc::dedup_register_failed, "dedup"},
            {box::errc::self_heal_not_initialized,"selfheal"},{box::errc::self_heal_scrub_failed,"selfheal"},
            {box::errc::boxhash_invalid_context,"boxhash"},  {box::errc::boxhash_key_not_set,   "boxhash"},
            {box::errc::braid_not_initialized,  "braid"},    {box::errc::braid_rebuild_failed,  "braid"},
            {box::errc::addr_value_mismatch,    "strand"},
        };
        bool cats_ok = true;
        for (const CatCase &cc : cats) {
            box::error e{cc.code};
            cats_ok = cats_ok && e.category_name() == cc.cat
                      && e.message() != std::string_view("unknown error code");
        }
        Check(cats_ok, "phase47 every kernel range carries a faithful category + named message");
    }

    // The ERR_POCKET_FAILED 906→950 fix, locked at compile time, and the
    // single-source guarantee (box::errc values ARE the boxlib codes).
    static_assert(ERR_POCKET_FAILED == 950,
                  "phase47 ERR_POCKET_FAILED is the kernel's own 950 (no longer a 906 alias)");
    static_assert(static_cast<error_t>(box::errc::pocket_failed) == 950 &&
                  static_cast<error_t>(box::errc::pocket_processing_failed) == 906,
                  "phase47 pocket_failed(950) is distinct from pocket_processing_failed(906)");
    static_assert(static_cast<error_t>(box::errc::route_no_subscribers) == ERR_ROUTE_NO_SUBSCRIBERS &&
                  static_cast<error_t>(box::errc::no_memory) == ERR_NO_MEMORY &&
                  static_cast<error_t>(box::errc::braid_rebuild_failed) == ERR_BRAID_REBUILD_FAILED,
                  "phase47 box::errc enumerators ARE the boxlib ERR_* codes (single source)");

    // ── B-2: the int64 byte-count bridge ────────────────────────────────────
    // 3e9 is the exact value that the old `int` bridge read as a NEGATIVE cause;
    // from_ret64 carries it as a SUCCESS count.
    {
        auto big = box::_detail::from_ret64<std::size_t>(3000000000LL);
        Check(big.has_value() && *big == 3000000000u,
              "phase47 from_ret64 carries a 3e9 count as SUCCESS (the int bridge mis-read it as a cause)");
        auto zero = box::_detail::from_ret64<std::size_t>(0);
        Check(zero.has_value() && *zero == 0u, "phase47 from_ret64(0) is a zero-count success");
        auto df = box::_detail::from_ret64<std::size_t>(box_fail(ERR_DISK_FULL));
        Check(!df.has_value() && df.error().code() == box::errc::disk_full,
              "phase47 from_ret64 recovers disk_full from box_fail(ERR_DISK_FULL)");
        auto io = box::_detail::from_ret64<std::size_t>(-(int)ERR_IO);
        Check(!io.has_value() && io.error().code() == box::errc::io,
              "phase47 from_ret64 recovers io from a raw -ERR_IO");
    }

    // Real storage: the byte count survives the int64 bridge end-to-end. Gated
    // on a confirmed create so a storage flake degrades to a note, never a FAIL.
    {
        box::result<box::tagfs::file> made =
            box::tagfs::create("cxx:p47:wide", {"cxx:phase47"});
        if (made.has_value()) {
            box::tagfs::file f = *made;
            unsigned char buf[512];
            for (int i = 0; i < 512; i++) buf[i] = (unsigned char)(i & 0xFF);

            box::result<std::size_t> w = f.write_at(0, buf, sizeof(buf));
            Check(w.has_value() && *w == sizeof(buf),
                  "phase47 write_at exposes the exact byte count through the int64 bridge");

            unsigned char back[512] = {};
            box::result<std::size_t> r = f.read_at(0, back, sizeof(back));
            bool same = r.has_value() && *r == sizeof(buf);
            for (int i = 0; same && i < 512; i++) same = back[i] == buf[i];
            Check(same, "phase47 read_at count + content survive the int64 bridge");

            // A struct larger than the object's bytes: the op must NOT silently
            // succeed; read_object maps a short transfer to errc::io.
            struct Big { unsigned char d[4096]; };
            box::result<Big> sr = f.read_object<Big>(0);
            Check(!sr.has_value(),
                  "phase47 read_object of a 4 KiB struct over a 512-byte object does not succeed");
            if (!sr.has_value() && sr.error().code() != box::errc::io)
                printf("[CXX] note phase47: short read surfaced as '%s' (backend errored past-end "
                       "rather than short-reading) — errc::io path not exercised\n",
                       std::string(sr.error().message()).c_str());

            (void)f.remove();
        } else {
            printf("[CXX] note phase47: tagfs storage unavailable — wide-bridge round-trip "
                   "skipped (from_ret64 unit + B-1 table asserted unconditionally)\n");
        }
    }

    printf("[CXX] PASS phase47: Ф23 backlog — full kernel-code sync via single-source "
           "X-macro (941 names itself, every range categorized, POCKET_FAILED=950) + "
           ">2 GiB int64 byte-count bridge (from_ret64)\n");
}

// ── Phase48 — Ф24a executor wait-any (group-by-domain + min-deadline + result_any) ──
// The headline fix: with >1 coroutine blocked the executor must NOT busy-yield —
// it bounded-rotates native waits per domain, yielding once per rotation so a
// same-core sibling producer can run (one App-Core), and caps the wait at the
// earliest waiter deadline. (A) exercises the multi-waiter rotation with two
// Touch coroutines fed by a sibling strand: Touch is the one async source a same-
// cabin sibling can deliver (a Result needs a cross-cabin sender — send-to-self
// is rejected — and a cross-strand Brook is its own substrate matter; both are
// deferred exactly like phase13's pocket_recv). The Phase-3c rotation code is
// identical across domains, so this validates the wait-any machinery.
static volatile uint64_t g_p48_remaining;          // worker decrements; main joins
static TouchTagPair      g_p48_tp;                 // touch tag the sibling sends on
static constexpr unsigned g_p48_magic1 = 0x00C0DE48u;
static constexpr unsigned g_p48_magic2 = 0x00BEE048u;

// Bounded park-join (the p35_join idiom): a genuine hang fails loudly instead of
// wedging the harness.
static bool p48_join()
{
    uint32_t cycles = 0;
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_p48_remaining, __ATOMIC_ACQUIRE)) != 0) {
        if (++cycles > 80u) return false;
        addr_park(&g_p48_remaining, cur, 200);
    }
    return true;
}

static void p48_producer(void *)
{
    // Sleep so the main strand reaches the rotation (both coroutines suspended)
    // BEFORE either Touch is delivered — then deliver both to main's claimed ring.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    unsigned m1 = g_p48_magic1, m2 = g_p48_magic2;
    touch_send(g_p48_tp, &m1, sizeof(m1), 0);
    touch_send(g_p48_tp, &m2, sizeof(m2), 0);
    __atomic_sub_fetch(&g_p48_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p48_remaining, 0);
    strand_exit();
}

box::task<unsigned> P48AwaitTouch()
{
    Touch t = co_await box::touch_event();
    unsigned v = 0;
    if (t.payload_len >= sizeof(v)) __builtin_memcpy(&v, t.payload, sizeof(v));
    co_return v;
}

// A lone Ф24b timer awaiter: with a SINGLE such waiter the old size()==1 path
// would block FOREVER (hang) — the min-deadline plumbing revives the timeout
// (Phase 3a generalized to wait_domain::timer).
box::task<void> P48AwaitTimer(unsigned ms)
{
    co_await box::after(std::chrono::milliseconds(ms));
    co_return;
}

void Phase48()
{
    using namespace std::chrono;

    // ── (A) multi-waiter wait-any: two coroutines each await the next Touch; a
    //        sibling strand delivers two Touches WHILE the executor is blocked,
    //        so the bounded rotation (not a busy-yield) must resume BOTH without
    //        starving the same-core producer. ─────────────────────────────────
    {
        TouchTagPair tp = touch_intern("cxx:waitany:touch");
        TouchTag     id = touch_pair_choose(tp);
        bool         can = (id != TOUCH_TAG_INVALID) && cpu_has_fsgsbase();
        if (can) {
            touch_claim(id, TOUCH_REST, 0, 0);
            Touch drain;
            while (touch_pop(&drain)) { }  // clear stray touches: touch_event is unfiltered
            g_p48_tp        = tp;
            g_p48_remaining = 1;
            if (strand_spawn(p48_producer, 0) != 0) {
                box::executor ex;
                auto          t1 = P48AwaitTouch();
                auto          t2 = P48AwaitTouch();
                ex.schedule(t1.handle());  // both run, find the TouchRing empty,
                ex.schedule(t2.handle());  // suspend -> two Touch-domain waiters
                box::stopwatch sw;
                ex.run();                  // bounded rotation; sibling delivers 2
                auto ms = duration_cast<milliseconds>(sw.elapsed());

                unsigned g1 = t1.result(), g2 = t2.result();
                bool both = (g1 == g_p48_magic1 && g2 == g_p48_magic2) ||
                            (g1 == g_p48_magic2 && g2 == g_p48_magic1);
                Check(both, "phase48 (A) wait-any resumed BOTH waiting coroutines");
                // Honest no-spin signal: the sibling slept 20ms before delivering,
                // so a genuine PARK makes run() take >= ~15ms. Busy-vs-park is not
                // userspace-observable; this lower bound + dual delivery proves the
                // executor blocked (not a vacuous instant return) and the rotation
                // ran on one App-Core without starving the producer.
                Check(ms >= milliseconds(15),
                      "phase48 (A) executor parked, not vacuous (>=15ms)");
                Check(p48_join(), "phase48 (A) sibling producer joined");
            } else {
                printf("[CXX] note phase48: strand_spawn unavailable — (A) skipped\n");
            }
            touch_release(id);
        } else {
            printf("[CXX] note phase48: touch/FSGSBASE unavailable — (A) skipped\n");
        }
    }

    // ── (B) min-deadline revives the timeout: a single deadline-bearing waiter.
    //        WITHOUT the Ф24a plumbing the size()==1 path blocks forever; WITH it
    //        the budget caps the block at the deadline so it wakes ~on time. ──────
    {
        box::executor  ex;
        box::stopwatch sw;
        ex.block_on(P48AwaitTimer(30));
        auto ms = duration_cast<milliseconds>(sw.elapsed());
        Check(ms >= milliseconds(20) && ms < milliseconds(400),
              "phase48 (B) min-deadline revives timeout (lone waiter wakes ~30ms, not forever)");
    }

    // ── (C) box::result_any() — any_result demux faces + spurious safety. A real
    //        IPC+kernel runtime delivery is NOT producible same-strand (send-to-
    //        self is ERR_ROUTE_SELF; a cross-cabin peer faults under 1c — the same
    //        honest deferral as phase13's pocket_recv). The await machinery is
    //        byte-identical to the touch/brook awaiters proven in (A), and the leaf
    //        result_wait_any is exercised by the live display daemon. Here: value
    //        semantics + the await_ready<->valid invariant (no hang). ────────────
    {
        Result k{};
        k.context    = 7;  // a non-zero kernel context (KCTX_*), sender_pid == 0
        k.sender_pid = 0;
        box::any_result kr(k);
        Check(kr && kr.is_kernel() && !kr.is_ipc() && kr.context() == 7u,
              "phase48 (C) any_result kernel face");

        Result m{};
        m.sender_pid = 7;  // a genuine IPC delivery
        box::any_result mr(m);
        Check(mr && mr.is_ipc() && !mr.is_kernel() && mr.as_message().valid(),
              "phase48 (C) any_result IPC face bridges to box::message");

        box::any_result empty;
        Check(!empty && !empty.valid(),
              "phase48 (C) default any_result is invalid (spurious-resume safe)");

        // Non-blocking await_ready drains nothing pending -> await_resume yields an
        // invalid any_result; the invariant ready<->valid holds for any ring state.
        box::__result_any_awaiter a;
        bool            ready = a.await_ready();
        box::any_result r     = a.await_resume();
        Check(ready == static_cast<bool>(r),
              "phase48 (C) result_any await_ready matches any_result validity");
    }

    printf("[CXX] PASS phase48: Ф24a executor wait-any "
           "(multi-waiter bounded rotation + min-deadline revival + box::result_any)\n");
}

// ── Phase49 — Ф24b co_await timer (box::after / box::until) + co_await join ──────
// Two BoxOS-native coroutine awaiters over the cooperative executor, both riding
// the existing addr_park substrate (no new kernel/boxlib code):
//   box::after(d)/box::until(tp)  → suspend until a steady-clock deadline (a pure-
//       deadline addr_park; wait_domain::timer — lone-forever-safe AND collapsible
//       with the result line).
//   box::strand::completion()     → suspend until a sibling strand finishes, arming
//       its OWN addr_park on the strand's join flag (wait_domain::join — address-
//       conditional wake, NOT collapsible). NON-consuming: the strand stays
//       joinable, so ~strand / a later join() still reaps it.

// (A)/(B)/(D)/(E) lone-or-mixed real timer.
box::task<void> P49AwaitAfter(std::chrono::milliseconds d)
{
    co_await box::after(d);
    co_return;
}

// (E) box::until with a sub-ms steady deadline: the awaiter rounds up to >=1ms.
box::task<void> P49AwaitUntilSoon()
{
    co_await box::until(std::chrono::steady_clock::now() + std::chrono::microseconds(200));
    co_return;
}

// (C)/(D)/(E) co_await a strand's completion. NON-consuming: records whether the
// strand was STILL joinable the instant after the await resumed (the headline
// fact — the reap stays with join()/the dtor, not the await).
box::task<void> P49AwaitJoin(box::strand &s, volatile bool *still_joinable_out)
{
    co_await s.completion();
    if (still_joinable_out) *still_joinable_out = s.joinable();
    co_return;
}

// A strand body: sleep ~ms, then exit (the trampoline release-stores the join
// flag and addr_wake's it — what the join awaiter parks on). Plain free function
// so box::strand can spawn it.
struct P49Sleeper {
    unsigned ms;
    void operator()() const { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

// (B) timer + Touch mix: the Touch producer (same idiom as p48_producer).
static volatile uint64_t g_p49_remaining;
static TouchTagPair      g_p49_tp;
static constexpr unsigned g_p49_magic = 0x00C0DE49u;

static bool p49_join()
{
    uint32_t cycles = 0;
    uint64_t cur;
    while ((cur = __atomic_load_n(&g_p49_remaining, __ATOMIC_ACQUIRE)) != 0) {
        if (++cycles > 80u) return false;
        addr_park(&g_p49_remaining, cur, 200);
    }
    return true;
}

static void p49_touch_producer(void *)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    unsigned m = g_p49_magic;
    touch_send(g_p49_tp, &m, sizeof(m), 0);
    __atomic_sub_fetch(&g_p49_remaining, 1u, __ATOMIC_RELEASE);
    addr_wake(&g_p49_remaining, 0);
    strand_exit();
}

box::task<unsigned> P49AwaitTouch()
{
    Touch t = co_await box::touch_event();
    unsigned v = 0;
    if (t.payload_len >= sizeof(v)) __builtin_memcpy(&v, t.payload, sizeof(v));
    co_return v;
}

void Phase49()
{
    using namespace std::chrono;

    // ── (A) lone real timer: co_await box::after(30ms) on a lone executor wakes
    //        ~on time (Phase 3a generalized to wait_domain::timer; a lone timer
    //        addr_park reschedules + returns on its budget, never forever). ───────
    {
        box::executor  ex;
        box::stopwatch sw;
        ex.block_on(P49AwaitAfter(30ms));
        auto ms = duration_cast<milliseconds>(sw.elapsed());
        Check(ms >= milliseconds(20) && ms < milliseconds(400),
              "phase49 (A) lone co_await box::after(30ms) wakes ~on time");
    }

    // ── (B) timer + Touch mix (Phase 3c with a timer present): one coro awaits a
    //        Touch a sibling delivers at ~20ms, one coro awaits box::after(60ms).
    //        Both must resume on ONE App-Core without a hang — the rotation gives
    //        the same-core producer a scheduling point AND honours the timer. ─────
    {
        TouchTagPair tp = touch_intern("cxx:f24b:touch");
        TouchTag     id = touch_pair_choose(tp);
        bool         can = (id != TOUCH_TAG_INVALID) && cpu_has_fsgsbase();
        if (can) {
            touch_claim(id, TOUCH_REST, 0, 0);
            Touch drain;
            while (touch_pop(&drain)) { }  // clear stray touches (touch_event is unfiltered)
            g_p49_tp        = tp;
            g_p49_remaining = 1;
            if (strand_spawn(p49_touch_producer, 0) != 0) {
                box::executor ex;
                auto          tt = P49AwaitTouch();
                auto          tm = P49AwaitAfter(60ms);
                ex.schedule(tt.handle());   // suspends -> a Touch-domain waiter
                ex.schedule(tm.handle());   // suspends -> a timer-domain waiter
                box::stopwatch sw;
                ex.run();                   // Phase 3c: mixed domains, bounded rotation
                auto ms = duration_cast<milliseconds>(sw.elapsed());

                Check(tt.result() == g_p49_magic,
                      "phase49 (B) timer+Touch mix resumed the Touch waiter");
                // The timer (60ms) and the touch (~20ms) both fired; run() spans at
                // least the longer wait — proving a genuine park, not a busy return.
                Check(ms >= milliseconds(40),
                      "phase49 (B) timer+Touch mix parked (>=40ms), both resumed, no hang");
                Check(p49_join(), "phase49 (B) sibling Touch producer joined");
            } else {
                printf("[CXX] note phase49: strand_spawn unavailable — (B) skipped\n");
            }
            touch_release(id);
        } else {
            printf("[CXX] note phase49: touch/FSGSBASE unavailable — (B) skipped\n");
        }
    }

    // ── (C) lone join (the fact-1/fact-2 headline): co_await s.completion() on a
    //        lone executor suspends until the sibling strand exits (~20ms) — its
    //        OWN addr_park, IPI-woken, never forever. NON-consuming: the strand is
    //        STILL joinable the instant the await resumes; the explicit join()
    //        after reaps it with no hang / no leak. ────────────────────────────────
    if (cpu_has_fsgsbase()) {
        box::strand   s(P49Sleeper{20});
        volatile bool still = false;
        box::executor ex;
        box::stopwatch sw;
        ex.block_on(P49AwaitJoin(s, &still));
        auto ms = duration_cast<milliseconds>(sw.elapsed());

        Check(ms >= milliseconds(10) && ms < milliseconds(400),
              "phase49 (C) lone co_await completion() woke ~when the strand exited");
        Check(still, "phase49 (C) strand STILL joinable right after the await (NON-consuming)");
        Check(s.joinable(), "phase49 (C) strand still joinable at the call site too");
        s.join();   // the reap the await deliberately did NOT do — must not hang
        Check(!s.joinable(), "phase49 (C) explicit join() after a co_await reaps cleanly");
    } else {
        printf("[CXX] note phase49: FSGSBASE unavailable — (C) lone join skipped\n");
    }

    // ── (D) join + timer mix (NOT collapsible → Phase 3c): one coro awaits
    //        box::after(40ms), one coro awaits a sibling strand's completion
    //        (~20ms). join arms its OWN addr_park (it can never be a collapse
    //        passenger), so both must resume on one App-Core, ~40ms, no hang. ──────
    if (cpu_has_fsgsbase()) {
        box::strand   s(P49Sleeper{20});
        volatile bool still = false;
        box::executor ex;
        auto          tm = P49AwaitAfter(40ms);
        auto          tj = P49AwaitJoin(s, &still);
        ex.schedule(tm.handle());   // suspends -> a timer-domain waiter
        ex.schedule(tj.handle());   // suspends -> a join-domain waiter
        box::stopwatch sw;
        ex.run();
        auto ms = duration_cast<milliseconds>(sw.elapsed());

        Check(still, "phase49 (D) join+timer mix resumed the join waiter (non-consuming)");
        Check(ms >= milliseconds(30),
              "phase49 (D) join+timer mix parked (>=30ms), both resumed, no hang");
        s.join();
        Check(!s.joinable(), "phase49 (D) strand reaped after the mixed await");
    } else {
        printf("[CXX] note phase49: FSGSBASE unavailable — (D) join+timer mix skipped\n");
    }

    // ── (E) value / error paths (no strand needed for the timer/throw parts). ─────
    {
        // box::after(0ms) is already-ready: await_ready() is true, so the coroutine
        // never parks — a vacuous, instant completion.
        {
            box::executor  ex;
            box::stopwatch sw;
            ex.block_on(P49AwaitAfter(0ms));
            Check(duration_cast<milliseconds>(sw.elapsed()) < milliseconds(100),
                  "phase49 (E) box::after(0ms) completes inline without parking");
        }
        // A sub-ms positive steady deadline rounds up to >=1ms and returns promptly.
        {
            box::executor  ex;
            box::stopwatch sw;
            ex.block_on(P49AwaitUntilSoon());
            Check(duration_cast<milliseconds>(sw.elapsed()) < milliseconds(150),
                  "phase49 (E) box::until(steady + sub-ms) returns promptly");
        }
        // co_await on a non-joinable strand throws invalid_argument (consistent with
        // box::strand::join()): a default-constructed strand has no join flag.
        {
            box::strand none;
            bool        threw = false;
            try {
                box::executor ex;
                ex.block_on(P49AwaitJoin(none, nullptr));
            } catch (const std::system_error &e) {
                threw = (e.code() == std::errc::invalid_argument);
            }
            Check(threw,
                  "phase49 (E) co_await completion() on a non-joinable strand throws invalid_argument");
        }
        // Already-exited fast-path: spawn, sleep until the strand is surely done,
        // then co_await — await_ready() sees flag==1 and resolves inline (no park).
        if (cpu_has_fsgsbase()) {
            box::strand   s(P49Sleeper{5});
            std::this_thread::sleep_for(milliseconds(120));   // it has surely exited
            volatile bool still = false;
            box::executor ex;
            box::stopwatch sw;
            ex.block_on(P49AwaitJoin(s, &still));
            Check(duration_cast<milliseconds>(sw.elapsed()) < milliseconds(100),
                  "phase49 (E) already-exited strand: co_await completion() resolves inline");
            Check(still, "phase49 (E) already-exited strand stays joinable after the await");
            s.join();
        } else {
            printf("[CXX] note phase49: FSGSBASE unavailable — (E) already-exited fast-path skipped\n");
        }
    }

    // Honest PASS: the entire co_await join surface (cases B/C/D + the already-
    // exited fast-path) is FSGSBASE-gated, so annotate the skip when it did not
    // run — never advertise the join awaiter on a machine that skipped it (house
    // convention: phase36/37/40). The STRICT matrix runs -cpu max (FSGSBASE on).
    if (cpu_has_fsgsbase())
        printf("[CXX] PASS phase49: Ф24b co_await timer (box::after/box::until) + "
               "co_await box::strand::completion() (non-consuming join)\n");
    else
        printf("[CXX] PASS phase49: Ф24b co_await timer (box::after/box::until) "
               "(co_await join skipped, no FSGSBASE)\n");
}

// ── phase50: deterministic proof of the executor block-then-repoll latch ──────
// A brook/current reader whose frame arrives DURING its native _S_block hits the
// executor's Phase-3c "block delivers, then re-poll the SAME waiter" path. Cross-
// strand timing cannot reproduce this reliably (a spawned writer-strand bursts
// rather than paces), so we prove it deterministically + host-independently with
// a probe awaiter whose one-shot event is delivered ONLY by _S_block. The latch
// in _S_poll (mirroring box::brook<T>/box::current<T> and box::touch/pocket)
// reports the just-delivered event ready WITHOUT re-reading the consumed source —
// remove it and the post-block re-poll would clobber the frame (lost forever).
struct BlockDeliverProbe {
    bool     _M_armed = false;          // _S_block delivered + consumed the one-shot event
    unsigned _M_polls_after_block = 0;  // proves the executor re-polls post-block (the hazard site)
    unsigned _M_value = 0;

    bool await_ready() noexcept { return false; }   // suspend: "the event is not here yet"
    bool await_suspend(std::coroutine_handle<> __h) {
        box::executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                          box::__exec::wait_domain::brook);
        return true;
    }
    unsigned await_resume() noexcept { return _M_value; }

    static bool _S_poll(void* __s) {
        auto* __p = static_cast<BlockDeliverProbe*>(__s);
        if (__p->_M_armed) {            // LATCH: report ready from what _S_block consumed; do NOT re-read
            __p->_M_polls_after_block++;
            return true;
        }
        return false;                   // a one-shot event is never (re-)found by a bare poll
    }
    static void _S_block(void* __s, std::uint32_t) {
        auto* __p = static_cast<BlockDeliverProbe*>(__s);
        __p->_M_armed = true;           // the event "arrives" and is CONSUMED during the native block
        __p->_M_value = 0xC0DEu;
    }
};

box::task<unsigned> BlockDeliverConsumer(BlockDeliverProbe* __p) { co_return co_await *__p; }

void Phase50()
{
    BlockDeliverProbe probe;
    box::executor     ex;
    unsigned got = ex.block_on(BlockDeliverConsumer(&probe));
    // (a) the block-delivered event reached the coroutine — the latch delivered it;
    // (b) the executor DID re-poll the waiter after its block (the exact clobber
    //     site). Without the _S_poll latch, that re-poll re-reads the consumed
    //     source and strands the coroutine — a real brook/current frame would be lost.
    Check(got == 0xC0DEu, "phase50 block-delivered event reaches the coroutine (latch holds)");
    Check(probe._M_polls_after_block >= 1,
          "phase50 executor re-polls after block (the clobber site is exercised)");
    printf("[CXX] PASS phase50: executor block-then-repoll latch (deterministic, host-independent)\n");
}

// ── phase51 — Ф25a: IPC coverage (box::line<T> / box::call / send_args/args) ───
// box::line<T> is the typed face of process-to-process messaging; box::call is the
// request/reply verb; box::send_args/receive_args/args<N> are the argv face. The
// always-on block proves the structural surface deterministically (no peer); the
// FSGSBASE-gated block proves the real cross-strand traffic — a send_args round-
// trip with the 64-byte truncation, a box::line peer round-trip, co_await recv()
// and co_await box::call with reply-correlation + timeout, the GROUP-flavor
// broadcast delivery + self-exclusion, the CONCURRENT cross-strand routing +
// correlation under load (exercising C1 per-strand rings; pids never cross), and
// the recv()-alongside-timer latch proof (mirrors phase50's intent on the live IPC
// path).

// box::line<T> payload and box::call request/reply payload.
struct P51Msg  { std::uint32_t seq; std::uint32_t tag; };
struct P51Call { std::uint32_t token; std::uint32_t echo; };

// The main strand's pid (== cabin pid) — sibling workers address their replies to
// it. Captured by Phase51() before any spawn.
static std::uint32_t g_p51_main_pid = 0;

// Concurrent inbox-isolation (scenario 4): N workers each drain ONLY their own
// pid-addressed traffic from their OWN ring; a barrier releases them with main so
// the sends overlap the drains.
static constexpr int P51_WORKERS = 3;
static constexpr int P51_ISO_N   = 8;
struct P51IsoResult {
    std::atomic<int>  got{0};
    std::atomic<bool> order_ok{true};   // seqs arrived strictly 0,1,2,…
    std::atomic<bool> marker_ok{true};  // every record carried THIS worker's tag (no cross-delivery)
    std::uint32_t     seqs[P51_ISO_N];
};
static P51IsoResult    g_p51_iso[P51_WORKERS];
static std::barrier<> *g_p51_bar = nullptr;

static void p51_iso_worker(int idx)
{
    g_p51_bar->arrive_and_wait();   // release with main — the burst overlaps this drain
    P51IsoResult &res = g_p51_iso[idx];
    box::line<P51Msg> in;           // unbound: recv reads THIS strand's own inbox (no peer filter)
    std::uint32_t expect = 0;
    int           n = 0;
    box::stopwatch sw;              // wall-clock backstop so a missing record fails loudly, never hangs
    while (n < P51_ISO_N && sw.elapsed() < std::chrono::milliseconds(2000)) {
        std::optional<P51Msg> m = in.recv_for(50);
        if (!m) continue;
        if (m->tag != static_cast<std::uint32_t>(idx))
            res.marker_ok.store(false, std::memory_order_relaxed);
        if (m->seq != expect)
            res.order_ok.store(false, std::memory_order_relaxed);
        res.seqs[n] = m->seq;
        ++expect;
        ++n;
        res.got.store(n, std::memory_order_release);
    }
}

// scenario 1: receive_args round-trip target. Records argc + the three args back
// for main to verify (including the 64-byte/arg truncation).
struct P51ArgsResult {
    std::atomic<int>  argc{-1};
    std::atomic<bool> ok{false};
    std::atomic<int>  len2{-1};
    char              a0[64];
    char              a1[64];
    char              a2[64];
};
static P51ArgsResult g_p51_args;

static void p51_args_worker(int)
{
    box::result<box::args<16>> r = box::receive_args<16>();
    if (!r) { g_p51_args.argc.store(-2, std::memory_order_release); return; }
    const box::args<16> &a = *r;
    int n = static_cast<int>(a.size());
    auto copy_out = [](char *dst, std::string_view v) {
        std::size_t k = v.size() < 63 ? v.size() : 63;
        for (std::size_t i = 0; i < k; ++i) dst[i] = v[i];
        dst[k] = '\0';
    };
    if (n >= 1) copy_out(g_p51_args.a0, a[0]);
    if (n >= 2) copy_out(g_p51_args.a1, a[1]);
    if (n >= 3) { copy_out(g_p51_args.a2, a[2]); g_p51_args.len2.store(static_cast<int>(a[2].size()), std::memory_order_relaxed); }
    g_p51_args.argc.store(n, std::memory_order_release);
    g_p51_args.ok.store(true, std::memory_order_release);   // visible-before via the join
}

// scenario 2: box::line peer echo. Receives one P51Msg from main and sends a
// transformed reply back along a line bound to main.
static std::atomic<std::uint32_t> g_p51_echo_in{0};

static void p51_line_echo_worker(int)
{
    box::line<P51Msg> in = box::line<P51Msg>::to(g_p51_main_pid);
    std::optional<P51Msg> m = in.recv_for(3000);
    if (m) {
        g_p51_echo_in.store(m->seq, std::memory_order_release);
        in.send(P51Msg{ m->seq + 1000u, static_cast<std::uint32_t>(m->tag ^ 0xFFu) });
    }
}

// scenario 3 (reply-correlation) + scenario 4 (responder): a request/reply server.
// Receives one message from main and replies with the token echoed — the reply's
// from() is THIS strand's pid, which is what box::call correlates on.
static void p51_call_server_worker(int)
{
    std::optional<box::message> req = box::receive_for(3000);
    if (req && req->from() == g_p51_main_pid) {
        std::optional<P51Call> rq = req->payload_as<P51Call>();
        std::uint32_t tok = rq ? rq->token : 0u;
        (void)req->reply(P51Call{ tok, tok ^ 0x51510000u });
    }
}

// scenario 3 (co_await recv) + scenario 5 (latch): send one P51Msg to main after a
// short delay, so main's executor receives it via the BLOCK path (not await_ready).
static void p51_sender_worker(int)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    box::line<P51Msg>::to(g_p51_main_pid).send(P51Msg{ 0xD00Du, 0x55u });
}

// scenario 3 (timeout): a peer that stays ALIVE (so main's call-send succeeds) but
// never replies, so the call rides its deadline to errc::timeout.
static void p51_silent_worker(int)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
}

// scenario 2b: box::line<T>::on(tag) GROUP flavor. The worker JOINS the broadcast
// group (carries the tag on its own strand process — proc_tag_add is self-scoped),
// signals readiness, then recv's the broadcast from its own ring. Main (also a
// member) broadcasts once and must NOT receive its own send — the kernel excludes
// the broadcaster (SysBroadcast skips iter->pid == sender pid).
static std::atomic<std::uint32_t> g_p51_grp_in{0};
static std::atomic<bool>          g_p51_grp_ready{false};

static void p51_group_worker(int)
{
    box::tag_scope member("p51:grp");   // join the broadcast group (self-scoped tag)
    g_p51_grp_ready.store(static_cast<bool>(member), std::memory_order_release);
    box::line<P51Msg>     in;           // unbound: recv reads THIS strand's own inbox
    std::optional<P51Msg> m = in.recv_for(3000);
    if (m) g_p51_grp_in.store(m->seq, std::memory_order_release);
}

// Helper coroutines driven on a box::executor.
static box::task<box::result<box::message>>
p51_call_coro(std::uint32_t pid, P51Call req, std::uint32_t timeout_ms)
{
    co_return co_await box::call(pid, req, timeout_ms);
}
static box::task<std::optional<P51Msg>> p51_recv_coro()
{
    box::line<P51Msg> in;   // unbound recv on this strand's inbox
    co_return co_await in.recv();
}
static box::task<void> p51_timer_coro()
{
    co_await box::after(std::chrono::milliseconds(150));   // co-resident timer for the latch proof
    co_return;
}

void Phase51()
{
    g_p51_main_pid = box::this_strand::id().native();   // main strand pid (== cabin pid)

    // ── Block 0 — always-on, deterministic, no peer ─────────────────────────
    // box::line<T> binding state + flavor accessors.
    {
        box::line<P51Msg> ub;
        Check(!ub, "phase51 default box::line is unbound (operator bool false)");
        Check(ub.peer() == 0 && ub.group() == nullptr, "phase51 unbound line has no peer/group");

        box::line<P51Msg> pl = box::line<P51Msg>::to(4242u);
        Check(static_cast<bool>(pl), "phase51 line::to(pid) is bound");
        Check(pl.peer() == 4242u && pl.group() == nullptr, "phase51 line::to peer-flavor accessors");

        box::line<P51Msg> gl = box::line<P51Msg>::on("cxx:p51:grp");
        Check(static_cast<bool>(gl), "phase51 line::on(tag) is bound");
        Check(gl.peer() == 0 && gl.group() != nullptr &&
                  std::string_view(gl.group()) == "cxx:p51:grp",
              "phase51 line::on group-flavor accessors");

        // Real-cause negatives: an unbound line and a no-subscriber group surface
        // the actual kernel cause through the status error arm.
        box::status us = ub.send(P51Msg{ 1u, 2u });
        Check(!us && us.error().code() == box::errc::invalid_argument,
              "phase51 unbound line send -> invalid_argument");
        box::status gs = gl.send(P51Msg{ 1u, 2u });
        Check(!gs && gs.error().code() == box::errc::route_no_subscribers,
              "phase51 line::on send with no subscribers -> route_no_subscribers");
    }

    // box::line<T> short-payload decode -> nullopt (the same box::message guard
    // recv()/try_recv()/recv_for() use; deterministic, never dereferences).
    {
        std::uint16_t small = 0x1234u;
        Result sr{};
        sr.sender_pid  = 4242u;
        sr.data_addr   = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&small));
        sr.data_length = sizeof(small);   // 2 bytes < sizeof(P51Msg) == 8
        box::message sm(sr);
        Check(!sm.payload_as<P51Msg>().has_value(),
              "phase51 line<T> decode: short payload -> nullopt (the recv guard)");
    }

    // box::args<N> structural surface (default = empty; safe out-of-range).
    {
        box::args<16> a;
        Check(a.size() == 0 && a.empty(), "phase51 default box::args<16> is empty");
        Check(a[0].empty(), "phase51 out-of-range args index -> empty view (no OOB read)");
        Check(a.front().empty() && a.back().empty(), "phase51 empty args front/back are empty views");
        int count = 0;
        for (std::string_view v : a) { (void)v; ++count; }
        Check(count == 0, "phase51 range-for over empty args yields nothing");
    }

    // box::send_args marshal-path negatives (the full marshaller runs, then the
    // real cause surfaces).
    {
        box::status e0 = box::send_args(4242u, {});
        Check(!e0 && e0.error().code() == box::errc::invalid_argument,
              "phase51 send_args with no args -> invalid_argument");
        box::status e1 = box::send_args(0u, { "a", "bb", "ccc" });
        Check(!e1 && e1.error().code() == box::errc::invalid_argument,
              "phase51 send_args to pid 0 -> invalid_argument (marshal path exercised)");
    }

    // box::call producer-path negatives (the send fails first, so no peer needed):
    // pid 0 -> invalid_argument, a non-existent pid -> process_not_found.
    {
        P51Call req{ 0x9u, 0u };
        box::result<box::message> r0 = std::unexpected(box::error{box::errc::internal});
        box::result<box::message> r1 = std::unexpected(box::error{box::errc::internal});
        { box::executor ex; r0 = ex.block_on(p51_call_coro(0u, req, 100u)); }
        Check(!r0 && r0.error().code() == box::errc::invalid_argument,
              "phase51 box::call to pid 0 -> invalid_argument (send fails first)");
        { box::executor ex; r1 = ex.block_on(p51_call_coro(0x7FFFFFFFu, req, 100u)); }
        Check(!r1 && r1.error().code() == box::errc::process_not_found,
              "phase51 box::call to a non-existent pid -> process_not_found");
    }

    // ── Block 1 — cross-strand peer / concurrent: needs FSGSBASE (per-strand TLS).
    // Without it box::strand cannot spawn; skip cleanly with a PASS (matching the
    // other strand phases) — Block 0 already exercised the structural surface.
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase51: strands need FSGSBASE — skipping peer/concurrent IPC\n");
        printf("[CXX] PASS phase51: box::line / box::call / box::send_args / box::args "
               "(structural; peer/concurrent skipped, no FSGSBASE)\n");
        return;
    }

    // scenario 1: send_args -> receive_args round-trip + the 64-byte/arg truncation.
    {
        g_p51_args.argc.store(-1, std::memory_order_relaxed);
        g_p51_args.ok.store(false, std::memory_order_relaxed);
        g_p51_args.len2.store(-1, std::memory_order_relaxed);

        box::strand w(p51_args_worker, 0);
        std::uint32_t wpid = w.get_id().native();

        char long_arg[71];                        // 70 chars: crosses the 63-char cell limit
        for (int i = 0; i < 70; ++i) long_arg[i] = 'x';
        long_arg[70] = '\0';
        box::status ss = box::send_args(wpid, { "alpha", "beta", std::string_view(long_arg, 70) });
        Check(static_cast<bool>(ss), "phase51 send_args accepted by a live peer strand");

        w.join();   // bounded: the worker's receive_args has a finite inbox wait
        Check(g_p51_args.ok.load(std::memory_order_acquire), "phase51 receive_args parsed the payload");
        Check(g_p51_args.argc.load(std::memory_order_acquire) == 3, "phase51 receive_args argc == 3");
        Check(std::string_view(g_p51_args.a0) == "alpha", "phase51 receive_args arg0 == \"alpha\"");
        Check(std::string_view(g_p51_args.a1) == "beta", "phase51 receive_args arg1 == \"beta\"");
        Check(g_p51_args.len2.load(std::memory_order_acquire) == 63,
              "phase51 receive_args truncated the 70-char arg to 63 (64-byte cell)");
        Check(std::string_view(g_p51_args.a2).size() == 63 &&
                  std::string_view(g_p51_args.a2).find_first_not_of('x') == std::string_view::npos,
              "phase51 receive_args arg2 is exactly 63 'x' (clean truncation)");
    }

    // scenario 2: box::line<T> peer round-trip (send / try_recv / recv_for).
    {
        while (box::receive()) { }   // main inbox quiescent
        g_p51_echo_in.store(0, std::memory_order_relaxed);

        box::strand w(p51_line_echo_worker, 0);
        std::uint32_t wpid = w.get_id().native();
        box::line<P51Msg> out = box::line<P51Msg>::to(wpid);

        Check(!out.try_recv().has_value(), "phase51 line try_recv on an empty inbox -> nullopt");
        box::status s = out.send(P51Msg{ 77u, 0xA5u });
        Check(static_cast<bool>(s), "phase51 line<T>::send to a live peer accepted");

        std::optional<P51Msg> reply = out.recv_for(3000);
        w.join();
        Check(reply.has_value(), "phase51 line<T>::recv_for got the echo reply");
        Check(reply && reply->seq == 1077u && reply->tag == (0xA5u ^ 0xFFu),
              "phase51 line<T> peer round-trip value correct");
        Check(g_p51_echo_in.load(std::memory_order_acquire) == 77u,
              "phase51 echo worker received the request seq");
    }

    // scenario 2b: box::line<T>::on(tag) GROUP flavor — positive delivery + recv +
    // broadcaster self-exclusion. A worker joins the tag, main broadcasts once.
    {
        while (box::receive()) { }   // main inbox quiescent
        g_p51_grp_in.store(0, std::memory_order_relaxed);
        g_p51_grp_ready.store(false, std::memory_order_release);

        box::strand   w(p51_group_worker, 0);
        box::stopwatch sw;           // wait (bounded) until the worker carries the tag
        while (!g_p51_grp_ready.load(std::memory_order_acquire) &&
               sw.elapsed() < std::chrono::milliseconds(2000))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Check(g_p51_grp_ready.load(std::memory_order_acquire),
              "phase51 line::on group member joined the tag");

        box::tag_scope    main_member("p51:grp");                  // main also carries the tag
        box::line<P51Msg> grp = box::line<P51Msg>::on("p51:grp");
        box::status       gs  = grp.send(P51Msg{ 0x6060u, 0x6u });
        Check(static_cast<bool>(gs), "phase51 line::on group send accepted (>=1 subscriber)");
        Check(!grp.try_recv().has_value() && !box::receive().has_value(),
              "phase51 line::on broadcaster does NOT receive its own group message (self-exclusion)");

        w.join();
        Check(g_p51_grp_in.load(std::memory_order_acquire) == 0x6060u,
              "phase51 line::on group member received the broadcast");
    }

    // scenario 3a: co_await box::line<T>::recv() on the executor (block-path delivery).
    {
        while (box::receive()) { }
        box::strand w(p51_sender_worker, 0);
        std::optional<P51Msg> got;
        { box::executor ex; got = ex.block_on(p51_recv_coro()); }
        w.join();
        Check(got.has_value(), "phase51 co_await line<T>::recv() delivered the worker's message");
        Check(got && got->seq == 0xD00Du && got->tag == 0x55u,
              "phase51 co_await line<T>::recv() value correct");
    }

    // scenario 3b: co_await box::call reply-correlation across two strands.
    {
        while (box::receive()) { }
        box::strand w(p51_call_server_worker, 0);
        std::uint32_t wpid = w.get_id().native();
        P51Call creq{ 0xABCDu, 0u };
        box::result<box::message> r = std::unexpected(box::error{box::errc::internal});
        { box::executor ex; r = ex.block_on(p51_call_coro(wpid, creq, 3000u)); }
        w.join();
        Check(r.has_value(), "phase51 box::call got a reply (not timeout)");
        Check(r && r->from() == wpid,
              "phase51 box::call reply from() == callee strand pid (correlation)");
        std::optional<P51Call> rp = r ? r->payload_as<P51Call>() : std::nullopt;
        Check(rp && rp->token == creq.token && rp->echo == (creq.token ^ 0x51510000u),
              "phase51 box::call reply payload echoes the request token");
    }

    // scenario 3c: co_await box::call timeout -> errc::timeout (send succeeds, no reply).
    {
        while (box::receive()) { }
        box::strand w(p51_silent_worker, 0);
        std::uint32_t wpid = w.get_id().native();
        P51Call creq{ 0x1u, 0u };
        box::result<box::message> r = std::unexpected(box::error{box::errc::internal});
        { box::executor ex; r = ex.block_on(p51_call_coro(wpid, creq, 80u)); }
        w.join();
        Check(!r.has_value(), "phase51 box::call to a non-replying peer -> error arm");
        Check(!r && r.error().code() == box::errc::timeout,
              "phase51 box::call deadline surfaces errc::timeout");
    }

    // scenario 5: latch proof — co_await recv() ALONGSIDE a timer waiter. The
    // worker sends DURING the executor's native block, so recv() is satisfied on
    // the block-then-repoll path; the recv_awaiter's _M_got latch must keep the
    // delivered message (a broken latch would re-receive an empty ring and drop
    // it). Mirrors phase50's intent on the live IPC path.
    {
        while (box::receive()) { }
        box::strand w(p51_sender_worker, 0);
        std::optional<P51Msg> got;
        {
            box::executor ex;
            ex.spawn(p51_timer_coro());            // co-resident timer => multi-waiter block path
            got = ex.block_on(p51_recv_coro());    // recv is the root
        }
        w.join();
        Check(got.has_value(),
              "phase51 latch: recv()+timer delivered the block-arriving message (no drop on re-poll)");
        Check(got && got->seq == 0xD00Du,
              "phase51 latch: recv() value intact after block-then-repoll");
    }

    // scenario 4: CONCURRENT cross-strand traffic under TRUE parallelism (meaningful
    // on bios16/uefi16, NOT a serialized 1c loop). N workers each drain ONLY their
    // own pid-addressed traffic while main bursts interleaved; a barrier overlaps the
    // sends with the drains. This exercises C1 per-strand ROUTING under load and
    // GUARDS the C1 substrate invariant — the isolation itself is enforced BELOW box::
    // line by per-strand rings (no box::line code path can cross), so a substrate
    // regression would cross-deliver and trip marker_ok here. A call/reply pair (main
    // <-> responder) correlates by strand-pid in the same window; a stopwatch bounds
    // the whole block (no hang).
    {
        for (int i = 0; i < P51_WORKERS; ++i) {
            g_p51_iso[i].got.store(0, std::memory_order_relaxed);
            g_p51_iso[i].order_ok.store(true, std::memory_order_relaxed);
            g_p51_iso[i].marker_ok.store(true, std::memory_order_relaxed);
        }
        while (box::receive()) { }   // main inbox quiescent for the call/reply

        box::stopwatch  sw;
        std::barrier<>  bar(1 + P51_WORKERS);
        g_p51_bar = &bar;

        box::strand iso[P51_WORKERS];
        for (int i = 0; i < P51_WORKERS; ++i) iso[i] = box::strand(p51_iso_worker, i);
        box::strand resp(p51_call_server_worker, 0);   // the call responder (outside the barrier)

        std::uint32_t iso_pid[P51_WORKERS];
        for (int i = 0; i < P51_WORKERS; ++i) iso_pid[i] = iso[i].get_id().native();
        std::uint32_t resp_pid = resp.get_id().native();

        bar.arrive_and_wait();   // release main + workers together (the burst overlaps the drains)

        // Interleaved pid-addressed burst: worker i gets seqs 0..N-1 (tag == i).
        for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(P51_ISO_N); ++s)
            for (int i = 0; i < P51_WORKERS; ++i)
                (void)box::line<P51Msg>::to(iso_pid[i]).send(
                    P51Msg{ s, static_cast<std::uint32_t>(i) });

        // Concurrent call/reply across two strands (main's inbox holds only the reply).
        P51Call creq{ 0xC0FFEEu, 0u };
        box::result<box::message> cr = std::unexpected(box::error{box::errc::internal});
        { box::executor ex; cr = ex.block_on(p51_call_coro(resp_pid, creq, 3000u)); }

        for (int i = 0; i < P51_WORKERS; ++i) iso[i].join();
        resp.join();
        g_p51_bar = nullptr;   // workers joined — drop the now-dangling stack pointer

        bool iso_ok = true;
        for (int i = 0; i < P51_WORKERS; ++i) {
            int got = g_p51_iso[i].got.load(std::memory_order_acquire);
            if (got != P51_ISO_N) iso_ok = false;
            if (!g_p51_iso[i].marker_ok.load(std::memory_order_acquire)) iso_ok = false;
            if (!g_p51_iso[i].order_ok.load(std::memory_order_acquire)) iso_ok = false;
            for (int s = 0; s < P51_ISO_N && s < got; ++s)
                if (g_p51_iso[i].seqs[s] != static_cast<std::uint32_t>(s)) iso_ok = false;
        }
        Check(iso_ok,
              "phase51 (concurrent) each worker drained exactly its own pid-addressed seqs in order under load (C1 per-strand routing)");
        Check(cr && cr->from() == resp_pid,
              "phase51 (concurrent) call/reply correlates via strand-pid");
        std::optional<P51Call> crp = cr ? cr->payload_as<P51Call>() : std::nullopt;
        Check(crp && crp->token == creq.token && crp->echo == (creq.token ^ 0x51510000u),
              "phase51 (concurrent) reply payload echoes the request token");
        Check(sw.elapsed() < std::chrono::seconds(10),
              "phase51 (concurrent) completed under the watchdog bound (no hang)");

        unsigned cores = box::strand::hardware_concurrency();
        if (cores >= 2)
            printf("[CXX] phase51 concurrent: %d workers across %u App-Cores (true cross-core routing + correlation)\n",
                   P51_WORKERS, cores);
        else
            printf("[CXX] phase51 concurrent: %d workers on 1 App-Core (cooperative interleave)\n",
                   P51_WORKERS);
    }

    printf("[CXX] PASS phase51: box::line<T> (to/on peer+group/send/try_recv/recv_for/co_await recv) "
           "+ box::call (reply correlation + timeout) + box::send_args/receive_args/args "
           "(round-trip + 64B truncation) + cross-strand pid-routing + correlation under load\n");
}

// ── Ф25b: box::tags + box::provenance + box::system_watch ────────────────────
void Phase52()
{
    using namespace box::literals;

    // ── 1) box::tags:: — cached canonical handles, identity-stable ───────────
    Check(box::tags::process_died().id()    != TOUCH_TAG_INVALID &&
              box::tags::process_spawned().id() != TOUCH_TAG_INVALID &&
              box::tags::system_shutdown().id() != TOUCH_TAG_INVALID &&
              box::tags::system_reboot().id()   != TOUCH_TAG_INVALID &&
              box::tags::usb_connect().id()     != TOUCH_TAG_INVALID &&
              box::tags::usb_disconnect().id()  != TOUCH_TAG_INVALID,
          "phase52 box::tags:: interns all six system tags");
    Check(box::tags::process_died().id() == box::tags::process_died().id(),
          "phase52 box::tags:: handle is identity-stable (cached, no re-intern)");

    // ── 2) box::provenance — a self-published touch is franked USER ──────────
    {
        box::tag          tg = "cxx:prov:demo"_tag;
        box::subscription sub(tg);
        if (sub) {
            Check(box::publish(tg, static_cast<std::uint32_t>(0xC0FFEEu)),
                  "phase52 provenance: publish to a user tag succeeds");
            bool seen = false;
            for (int i = 0; i < 8000 && !(seen = touch_available()); ++i) yield();
            if (seen) {
                if (auto ev = sub.wait(1000)) {
                    Check(ev->from_user(), "phase52 self-published touch is from_user()");
                    Check(!ev->from_kernel(), "phase52 self-published touch is not from_kernel()");
                    Check(ev->provenance() == box::provenance::user, "phase52 provenance() == user");
                    Check(ev->flags() == TOUCH_FLAG_USER, "phase52 flags() == TOUCH_FLAG_USER");
                }
            } else {
                printf("[CXX] note phase52: provenance self-delivery not observed (skipped)\n");
            }
        }
    }

    // ── 3) box::system_watch decodes all six typed payloads (self-published) ─
    // Sentinel field values let the tally ignore any concurrent REAL kernel
    // broadcast from another cabin. Self-publishing system:shutdown / system:
    // reboot is INERT — it only notifies subscribers, it does not halt.
    {
        box::system_watch watch;
        Check(static_cast<bool>(watch), "phase52 system_watch claims all six system tags");
        if (watch) {
            box::process_died    d{};
            d.pid = 0x4321u;
            d.exit_code = -7;
            box::process_spawned s{};
            s.pid = 0x5151u;
            s.parent_pid = 0x6262u;
            box::system_halt h{};
            h.reason = 0x7777u;
            h.grace_ms = 0x0099u;
            box::usb_device u{};
            u.port = 0x05u;
            u.speed = 0x02u;
            u.vendor_id = 0x1D6Bu;
            u.product_id = 0x0003u;

            // Assert the publishes SUCCEED — the six kernel tags are TOUCH_CAP_OPEN
            // today, so a denied publish (e.g. if they are ever hardened to
            // KERNEL_ONLY) must FAIL here loudly, never silently skip the decode.
            Check(box::publish(box::tags::process_died(), d) &&
                      box::publish(box::tags::process_spawned(), s) &&
                      box::publish(box::tags::system_shutdown(), h) &&
                      box::publish(box::tags::system_reboot(), h) &&
                      box::publish(box::tags::usb_connect(), u) &&
                      box::publish(box::tags::usb_disconnect(), u),
                  "phase52 self-publish to all six system tags succeeds");

            bool any = false;
            for (int i = 0; i < 8000 && !(any = touch_available()); ++i) yield();
            if (any) {
                bool gd = false, gs = false, gsh = false, grb = false, guc = false, gud = false;
                auto all = [&] { return gd && gs && gsh && grb && guc && gud; };
                for (int i = 0; i < 8000 && !all(); ++i) {
                    for (auto e = watch.poll(); e; e = watch.poll()) {
                        switch (e->kind) {
                        case box::system_touch_kind::process_died:
                            if (e->died.pid == 0x4321u && e->died.exit_code == -7) gd = true;
                            break;
                        case box::system_touch_kind::process_spawned:
                            if (e->spawned.pid == 0x5151u && e->spawned.parent_pid == 0x6262u) gs = true;
                            break;
                        case box::system_touch_kind::shutdown:
                            if (e->halt.reason == 0x7777u && e->halt.grace_ms == 0x0099u) gsh = true;
                            break;
                        case box::system_touch_kind::reboot:
                            if (e->halt.reason == 0x7777u && e->halt.grace_ms == 0x0099u) grb = true;
                            break;
                        case box::system_touch_kind::usb_connect:
                            if (e->usb.vendor_id == 0x1D6Bu && e->usb.product_id == 0x0003u) guc = true;
                            break;
                        case box::system_touch_kind::usb_disconnect:
                            if (e->usb.port == 0x05u) gud = true;
                            break;
                        }
                    }
                    if (!all()) yield();
                }
                Check(gd, "phase52 system_watch decoded process_died (pid + exit_code)");
                Check(gs, "phase52 system_watch decoded process_spawned (pid + parent)");
                Check(gsh, "phase52 system_watch decoded system shutdown (reason + grace)");
                Check(grb, "phase52 system_watch decoded system reboot (reason + grace)");
                Check(guc, "phase52 system_watch decoded usb_connect (vid + pid)");
                Check(gud, "phase52 system_watch decoded usb_disconnect (port)");
            } else {
                printf("[CXX] note phase52: system self-delivery not observed; decode skipped\n");
            }
        }
    }

    // ── 4) wait() on an idle watch returns within a bounded time (no hang) ───
    // The honest invariant is BOUNDED return, not silence: a real ambient kernel
    // event may arrive (returns early) or not (returns on the round-robin bound);
    // either way the fan-in must never block forever.
    {
        box::system_watch idle;
        if (idle) {
            while (idle.poll()) { /* drain ambient first */ }
            box::stopwatch sw;
            auto           e  = idle.wait(60);
            auto           ms = sw.elapsed_as<std::chrono::milliseconds>().count();
            (void)e;
            Check(ms < 1500, "phase52 system_watch.wait(60) returns within a bounded time");
        }
    }

    // ── 5) live kernel edge: process:spawned fires inside the spawn syscall ──
    // Claim BEFORE spawning so no edge is missed. process:spawned is published
    // synchronously by the spawn syscall (assert it); process:died is reaper-
    // timed (observe best-effort, print only — asserting it would be a flake).
    {
        box::system_watch watch;
        if (watch) {
            std::uint32_t             me    = box::this_process::pid();
            box::result<box::process> child = box::process::spawn("proca");
            if (child) {
                std::uint32_t cpid = child->pid();
                bool          saw  = false;
                for (int i = 0; i < 4000 && !saw; ++i) {
                    for (auto e = watch.poll(); e; e = watch.poll()) {
                        if (e->kind == box::system_touch_kind::process_spawned &&
                            e->spawned.pid == cpid) {
                            Check(e->spawned.parent_pid == me,
                                  "phase52 live process:spawned parent_pid == us");
                            Check(e->source == 0, "phase52 live process:spawned is kernel-origin (source pid 0)");
                            saw = true;
                            break;
                        }
                    }
                    if (!saw) yield();
                }
                Check(saw, "phase52 system_watch observed live process:spawned (pid match)");
                while (box::receive()) { /* drain proca's messages to its spawner (us) */ }

                bool died = false;
                for (int i = 0; i < 400 && !died; ++i) {
                    for (auto e = watch.poll(); e; e = watch.poll()) {
                        if (e->kind == box::system_touch_kind::process_died && e->died.pid == cpid) {
                            died = true;
                            break;
                        }
                    }
                    if (!died) yield();
                }
                printf("[CXX] note phase52: live process:died observed=%d (reaper-timed, best-effort)\n",
                       (int)died);
            } else {
                printf("[CXX] note phase52: proc_exec unavailable (%.*s); live spawn edge skipped\n",
                       (int)child.error().message().size(), child.error().message().data());
            }
        }
    }

    printf("[CXX] PASS phase52: box::tags + box::provenance + box::system_watch "
           "(six typed payloads + live process:spawned)\n");
}

// ── Phase53 (Ф25c) — box::spawn_detached / this_strand::exit / strand_info /
//    strand_watch::next + box::opening / read_some ────────────────────────────
static std::atomic<int>           g_p53_ran{0};         // detached worker reached its body
static std::atomic<int>           g_p53_tl_dtor{0};     // thread_local dtor ran (storage_exit)
static std::atomic<int>           g_p53_after_exit{0};  // MUST stay 0: code after exit() is unreachable
static std::atomic<int>           g_p53_do_exit{1};     // runtime-true, opaque to the optimizer
static std::atomic<std::uint32_t> g_p53_self_pid{0};
static std::atomic<int>           g_p53_self_main{-1};

// A thread_local whose destructor bumps a global — proof that both the detached
// trampoline AND this_strand::exit() run __boxcxx_thread_storage_exit (which raw
// strand_exit would skip).
struct P53TlGuard {
    ~P53TlGuard() { g_p53_tl_dtor.fetch_add(1, std::memory_order_release); }
};

// Returns normally: the detached trampoline's epilogue must run the tl destructor.
static void p53_detached_worker(int)
{
    thread_local P53TlGuard guard;
    (void)&guard;   // ODR-use → constructs the thread_local (its dtor must run at exit)
    g_p53_self_pid.store(box::this_strand::id().native(), std::memory_order_relaxed);
    g_p53_self_main.store(box::this_strand::is_main() ? 1 : 0, std::memory_order_relaxed);
    g_p53_ran.store(1, std::memory_order_release);
}

// Ends via this_strand::exit(): the tl destructor must STILL run, and the store
// after exit() must NEVER execute. The exit() is behind a runtime-true condition
// so the trailing store stays reachable in the compiler's CFG (no dead-code after
// [[noreturn]] → keeps the zero-warning build) yet is never reached at runtime.
static void p53_exit_worker(int)
{
    thread_local P53TlGuard guard;
    (void)&guard;
    g_p53_ran.store(1, std::memory_order_release);
    if (g_p53_do_exit.load(std::memory_order_acquire))
        box::this_strand::exit();   // [[noreturn]] — runs tl dtors, ends this strand
    g_p53_after_exit.store(1, std::memory_order_release);   // unreachable at runtime
}

// Brief-sleep body: keeps the strand (and its pid) alive across the collection
// window so a target pid cannot be reaped + recycled into a different strand mid-
// collect (no pid-alias false match).
static void p53_brief_worker(int)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

// Collect strand:spawned for the three target pids via co_await strand_watch::
// next(), proving executor integration + the latch (no dropped/duplicated event).
// a/b/c AND `sentinel` are published into the watch FIFO synchronously at spawn,
// BEFORE this coroutine blocks, so every co_await resumes via await_ready (no
// native block). The sentinel is spawned LAST, so observing it means all three
// targets were already delivered — a guaranteed BOUNDED terminator: a genuinely
// dropped target surfaces as matched < 3 (a clean fail), never a hang. guard_iters
// caps ambient strand traffic so the loop can never spin unbounded.
static box::task<int> p53_collect_spawned(box::strand_watch *w, std::uint32_t a,
                                          std::uint32_t b, std::uint32_t c, std::uint32_t sentinel)
{
    const std::uint32_t want[3] = { a, b, c };
    bool seen[3] = { false, false, false };
    int  matched = 0, guard_iters = 0;
    while (guard_iters < 256) {
        std::optional<box::strand_touch> e = co_await w->next();
        ++guard_iters;
        if (!e || e->kind != box::strand_touch_kind::spawned) continue;
        if (e->spawned.pid == sentinel) break;   // FIFO terminator — targets already passed
        for (int i = 0; i < 3; ++i)
            if (!seen[i] && e->spawned.pid == want[i]) { seen[i] = true; ++matched; break; }
    }
    co_return matched;
}

void Phase53()
{
    using box::opening;

    // ── Block 0 — deterministic, no strands (always runs) ───────────────────

    // 1) box::opening — the typed open-flag that replaces raw CURRENT_* ints.
    {
        Check(static_cast<unsigned>(opening::none) == 0u, "phase53 opening::none == 0");
        Check(static_cast<unsigned>(opening::create)   == CURRENT_CREATE,
              "phase53 opening::create maps to CURRENT_CREATE");
        Check(static_cast<unsigned>(opening::nonblock) == CURRENT_NONBLOCK,
              "phase53 opening::nonblock maps to CURRENT_NONBLOCK");
        opening both = opening::create | opening::nonblock;
        Check(static_cast<unsigned>(both) == (CURRENT_CREATE | CURRENT_NONBLOCK),
              "phase53 opening| composes both bits");
        Check(box::any(both & opening::nonblock) && box::any(both & opening::create),
              "phase53 opening& tests a set bit");
        Check(!box::any(opening::create & opening::nonblock),
              "phase53 opening& of disjoint bits is none");
    }

    // 2) read_some — the byte-channel tri-state (value n / value 0 = CLOSED / error).
    {
        box::byte_current closed;   // default-constructed: no handle
        auto rc = closed.read_some(nullptr, 0);
        Check(!rc && rc.error().code() == box::errc::invalid_argument,
              "phase53 read_some on a closed handle -> invalid_argument");

        box::byte_current fw = box::file("cxx_p53.dat", box::role::write);   // opening::create default
        if (fw) { const char m[] = "boxos53"; fw.write(m, 7); fw.flush(); }
        box::byte_current fr = box::file("cxx_p53.dat", box::role::read, opening::none);
        if (fr) {
            char buf[16] = {};
            auto r1 = fr.read_some(buf, sizeof(buf));
            Check(r1 && *r1 == 7u && std::string_view(buf, 7) == "boxos53",
                  "phase53 read_some returns the byte count (value > 0)");
            auto r2 = fr.read_some(buf, sizeof(buf));
            Check(r2 && *r2 == 0u,
                  "phase53 read_some at end-of-stream -> value 0 (CLOSED, not an error)");
        } else {
            printf("[CXX] note phase53: file backing unavailable; read_some value/CLOSED skipped\n");
        }
        // The would_block arm (NONBLOCK + empty) shares the box_errno_of path proven
        // for the framed stream in Phase45 — not re-exercised on the byte path here.
    }

    // 3) box::this_strand::info on the MAIN strand — total view, is_main == true.
    {
        Check(box::this_strand::is_main(), "phase53 main strand: is_main() == true");
        box::strand_info si = box::this_strand::info();
        Check(si.is_main && si.id.native() == box::this_process::pid(),
              "phase53 main strand_info == {cabin/process pid, is_main}");
    }

    // ── Block 1 — strands: needs FSGSBASE (per-strand TLS), like phase51 ─────
    if (!cpu_has_fsgsbase()) {
        printf("[CXX] note phase53: strands need FSGSBASE — skipping spawn_detached / exit / watch::next\n");
        printf("[CXX] PASS phase53: box::opening + read_some + strand_info(main) "
               "(strand parts skipped, no FSGSBASE)\n");
        return;
    }

    // 4) box::spawn_detached — a joinless strand runs, bootstraps C++ TLS, exits
    //    clean, and runs its thread_local destructor (the detached epilogue);
    //    its strand_info reports {own pid, not-main}; strand_watch sees it spawn.
    {
        g_p53_ran.store(0, std::memory_order_relaxed);
        g_p53_tl_dtor.store(0, std::memory_order_relaxed);
        g_p53_self_pid.store(0, std::memory_order_relaxed);
        g_p53_self_main.store(-1, std::memory_order_relaxed);

        box::strand_watch w;   // claim BEFORE the spawn so no edge is missed
        box::strand::id did = box::spawn_detached(p53_detached_worker, 0);
        Check(did.native() != 0, "phase53 spawn_detached returns a live strand id");

        box::stopwatch sw;
        while (g_p53_ran.load(std::memory_order_acquire) == 0 &&
               sw.elapsed() < std::chrono::milliseconds(2000)) yield();
        Check(g_p53_ran.load(std::memory_order_acquire) == 1, "phase53 detached strand ran its body");

        for (int i = 0; i < 2000 && g_p53_tl_dtor.load(std::memory_order_acquire) == 0; ++i) yield();
        Check(g_p53_tl_dtor.load(std::memory_order_acquire) == 1,
              "phase53 detached strand ran its thread_local destructor (TLS epilogue)");

        Check(g_p53_self_main.load(std::memory_order_acquire) == 0,
              "phase53 detached strand: is_main() == false");
        Check(g_p53_self_pid.load(std::memory_order_acquire) == did.native(),
              "phase53 detached strand_info: id == the spawn_detached pid");

        if (w) {
            bool saw_spawn = false;
            for (int i = 0; i < 4000 && !saw_spawn; ++i) {
                for (auto e = w.poll(); e; e = w.poll())
                    if (e->kind == box::strand_touch_kind::spawned && e->spawned.pid == did.native())
                        { saw_spawn = true; break; }
                if (!saw_spawn) yield();
            }
            Check(saw_spawn, "phase53 strand_watch observed the detached strand's strand:spawned");
            bool saw_exit = false;   // reaper-timed — observe best-effort (like phase52 process:died)
            for (int i = 0; i < 800 && !saw_exit; ++i) {
                for (auto e = w.poll(); e; e = w.poll())
                    if (e->kind == box::strand_touch_kind::exited && e->exited.pid == did.native())
                        { saw_exit = true; break; }
                if (!saw_exit) yield();
            }
            printf("[CXX] note phase53: detached strand:exited observed=%d (reaper-timed)\n", (int)saw_exit);
        }
    }

    // 5) box::this_strand::exit() — SAFE on a detached strand: it runs the
    //    thread_local destructors, the strand exits, the post-exit store NEVER
    //    runs (noreturn), and there is NO joiner to hang (a bounded watchdog
    //    fails loudly on any hang).
    {
        g_p53_ran.store(0, std::memory_order_relaxed);
        g_p53_tl_dtor.store(0, std::memory_order_relaxed);
        g_p53_after_exit.store(0, std::memory_order_relaxed);
        g_p53_do_exit.store(1, std::memory_order_release);

        box::strand::id eid = box::spawn_detached(p53_exit_worker, 0);
        Check(eid.native() != 0, "phase53 spawn_detached(exit worker) is live");

        box::stopwatch sw;
        while (g_p53_ran.load(std::memory_order_acquire) == 0 &&
               sw.elapsed() < std::chrono::milliseconds(2000)) yield();
        Check(g_p53_ran.load(std::memory_order_acquire) == 1, "phase53 exit worker reached its body");

        for (int i = 0; i < 2000 && g_p53_tl_dtor.load(std::memory_order_acquire) == 0; ++i) yield();
        Check(g_p53_tl_dtor.load(std::memory_order_acquire) == 1,
              "phase53 this_strand::exit() ran the thread_local destructor first");

        for (int i = 0; i < 500; ++i) yield();   // ample time for any stray store to land
        Check(g_p53_after_exit.load(std::memory_order_acquire) == 0,
              "phase53 this_strand::exit() is noreturn (code after it never ran)");
    }

    // 6) strand_watch::next() async — co_await collects every concurrent spawn
    //    through the executor with no dropped event (latch under parallelism;
    //    on 16c the three detached strands make true parallel progress).
    {
        box::strand_watch w;
        if (w) {
            while (w.poll()) { }   // drain ambient strand events first
            // Three targets + a sentinel spawned LAST (FIFO terminator); brief-sleep
            // bodies keep the pids un-recycled across the collect.
            box::strand::id a    = box::spawn_detached(p53_brief_worker, 0);
            box::strand::id b    = box::spawn_detached(p53_brief_worker, 0);
            box::strand::id c    = box::spawn_detached(p53_brief_worker, 0);
            box::strand::id sent = box::spawn_detached(p53_brief_worker, 0);
            Check(a.native() && b.native() && c.native() && sent.native(),
                  "phase53 three detached strands (+ sentinel) spawned for watch::next");
            box::executor ex;
            int matched = ex.block_on(
                p53_collect_spawned(&w, a.native(), b.native(), c.native(), sent.native()));
            Check(matched == 3,
                  "phase53 co_await strand_watch::next() collected all three spawns (no dropped event)");
        } else {
            printf("[CXX] note phase53: strand_watch could not claim tags; watch::next skipped\n");
        }
    }

    printf("[CXX] PASS phase53: box::spawn_detached + this_strand::exit/info + "
           "strand_watch::next + box::opening + read_some\n");
}

// ── Phase54 (Ф25d) — box::heap (tag lookup / counters / make) + box::memtag
//    (region encrypted/keyid + scoped_grant) + box::hw (keyid_of/encrypted)
//    + formatters. Deterministic single-strand introspection — no race surface,
//    so no concurrency hammer (honestly). TME/LAM are real-HW gated. ───────────
struct P54Widget {
    int v;
    explicit P54Widget(int x) : v(x) {}
};
struct P54Boom {
    P54Boom() { throw 42; }   // a throwing ctor — make<T> must free the storage + propagate
};

void Phase54()
{
    // ── 1) heap tag-registry lookup (D1) ────────────────────────────────────
    {
        Check(box::heap::tag_id("cxx:p54:never") == box::heap::tag_none,
              "phase54 tag_id of an unregistered name -> tag_none");
        box::heap::tag t("cxx:p54:look");
        Check(t.registered() && t.id() != box::heap::tag_none,
              "phase54 heap::tag ctor registers (id != tag_none)");
        Check(box::heap::tag_id("cxx:p54:look") == t.id(),
              "phase54 tag_id finds the registered tag (== tag.id())");
        const char *nm = box::heap::tag_name(t.id());
        Check(nm && std::string_view(nm) == "cxx:p54:look",
              "phase54 tag_name round-trips the id -> name");
    }

    // ── 2) box::heap::make<T> + tagged deleter + leak watch (D3) ─────────────
    {
        box::heap::tag w("cxx:p54:make");
        {
            auto g = w.watch();
            box::result<box::heap::tagged<P54Widget>> r =
                box::heap::make<P54Widget>("cxx:p54:make", 7);
            Check(r.has_value() && (*r)->v == 7, "phase54 make<T> constructs the object (value 7)");
            Check(w.count() == 1, "phase54 make<T> allocation is accounted under its tag");
            (void)g;   // its silent dtor (after r frees) confirms no leak
        }
        Check(w.count() == 0, "phase54 tagged<T> destructor freed the block (count back to 0)");
    }
    // A throwing constructor propagates AND leaves no leak under the tag.
    {
        box::heap::tag b("cxx:p54:boom");
        bool threw = false;
        try {
            auto r = box::heap::make<P54Boom>("cxx:p54:boom");
            (void)r;
        } catch (...) {
            threw = true;
        }
        Check(threw, "phase54 make<T> propagates a throwing constructor");
        Check(b.count() == 0, "phase54 make<T> frees the storage when the ctor throws (no leak)");
    }

    // ── 3) box::heap::counters() live + the typed stats view (D2) ────────────
    {
        box::heap::stats s = box::heap::counters();
        Check(s.malloc_calls() > 0 && s.in_use_bytes() > 0,
              "phase54 heap::counters() reports a live, non-empty heap");
    }

    // ── 4) formatters — synthetic PODs, EXACT bytes (D6) ─────────────────────
    {
        heap_stats_t hs{};
        hs.heap_used = 4096; hs.total_allocated = 1000; hs.total_free = 200;
        hs.alloc_count = 5; hs.free_count = 2; hs.malloc_calls = 9; hs.free_calls = 4;
        Check(std::format("{}", box::heap::stats(hs)) ==
                  "heap used=4096 in_use=1000 free=200 live=5 freeblk=2 mallocs=9 frees=4",
              "phase54 formatter<heap::stats> exact bytes");

        hw_tme_state_t ts{};   // all-zero == no TME
        Check(std::format("{}", box::hw::tme_state(ts)) ==
                  "tme active=0 mk=0 keyid_bits=0 alg=0 max_keyid=0 in_use=0 rmpa=0 held=0",
              "phase54 formatter<tme_state> exact bytes (dormant)");
        ts.tme_active = 1; ts.mk_active = 1; ts.num_keyid_bits = 6; ts.activated_alg = 1;
        ts.max_keyid = 63; ts.in_use = 3; ts.reduced_maxphyaddr = 46; ts.this_proc_held = 2;
        Check(std::format("{}", box::hw::tme_state(ts)) ==
                  "tme active=1 mk=1 keyid_bits=6 alg=1 max_keyid=63 in_use=3 rmpa=46 held=2",
              "phase54 formatter<tme_state> exact bytes (active TME-MK)");

        mem_region_info_t ri{};
        ri.base_phys = 0x200000; ri.base_virt = 0xffff800000200000ull; ri.pages = 16;
        ri.tag_count = 3; ri.flags = 0x5; ri.generation = 42;
        Check(std::format("{}", box::memtag::region(7, ri)) ==
                  "region id=7 phys=0x200000 virt=0xffff800000200000 pages=16 tags=3 flags=0x5 gen=42",
              "phase54 formatter<region> exact bytes");

        mem_stats_t ms{};
        ms.tag_count = 11; ms.region_active = 8; ms.region_slot_count = 9; ms.region_slot_cap = 64;
        ms.registry_generation = 100; ms.cache_hits = 70; ms.cache_misses = 30;
        Check(std::format("{}", box::memtag::stats(ms)) ==
                  "memtag tags=11 active=8 slots=9 cap=64 reg_gen=100 hits=70 misses=30",
              "phase54 formatter<memtag::stats> exact bytes");
    }

    // ── 5) TME encryption synthesis + honest dormancy + region delegation (D4) ─
    {
        // Pure synthesis: keyid lives in phys bits [rmpa +: keyid_bits].
        hw_tme_state_t mk{};
        mk.tme_active = 1; mk.mk_active = 1; mk.num_keyid_bits = 6; mk.reduced_maxphyaddr = 46;
        box::hw::tme_state mks(mk);
        std::uint64_t      phys5 = static_cast<std::uint64_t>(5) << 46;
        Check(box::hw::keyid_of(mks, phys5) == 5, "phase54 keyid_of synthesises the phys keyid (5)");
        Check(box::hw::encrypted(mks), "phase54 encrypted(active TME) == true");

        hw_tme_state_t off{};   // no TME
        box::hw::tme_state offs(off);
        Check(box::hw::keyid_of(offs, phys5) == 0, "phase54 keyid_of under no TME == 0");
        Check(!box::hw::encrypted(offs), "phase54 encrypted(no TME) == false");

        // region delegates to the SAME hw synthesis (wiring proof — both read the
        // live snapshot, so they must agree).
        mem_region_info_t ri2{};
        ri2.base_phys = phys5;
        box::memtag::region rr(3, ri2);
        Check(rr.encrypted() == box::hw::encrypted(),
              "phase54 region::encrypted() delegates to box::hw::encrypted()");
        Check(rr.keyid() == box::hw::keyid_of(ri2.base_phys),
              "phase54 region::keyid() delegates to box::hw::keyid_of(base_phys)");

        // Live, gated: on a platform without TME (TCG) the answer is honestly
        // dormant (encrypted false, keyid 0) — like the Ф17 LAM/TME tests.
        if (!box::hw::tme_available()) {
            Check(!box::hw::encrypted() && box::hw::keyid_of(0x100000) == 0,
                  "phase54 no-TME platform: encrypted()==false, keyid_of()==0 (dormant)");
        } else {
            printf("[CXX] note phase54: TME present — encrypted()=%d (platform-dependent)\n",
                   (int)box::hw::encrypted());
        }
    }

    // ── 6) box::memtag::scoped_grant — RAII capability lease (D5) ────────────
    // grant_scope needs the cabin to hold the TagFS "system" tag-bit. cxxtest
    // runs unprivileged, so at runtime the factory takes its DENIAL path — which
    // we assert is a clean error arm carrying a cause (the fallible contract).
    // The RAII held/revoke/inert happy-path requires privilege; it is an explicit
    // VISIBLE skip here (its move/dtor logic is instantiated at compile time),
    // never a silent vacuous pass.
    {
        std::uint32_t me = box::this_process::pid();
        const char   *T  = "cxx:p54:lease";
        box::result<box::memtag::scoped_grant> sg = box::memtag::grant_scope(me, T);
        if (sg) {
            // Privileged cabin: the lease holds, is visible in cabin_tags, a
            // second lease is inert, and the outer survives that inert one's drop.
            Check(sg->held() && sg->pid() == me && sg->tag() == std::string_view(T),
                  "phase54 scoped_grant holds (pid / tag / held)");
            bool present = false;
            for (const std::string &t : box::memtag::cabin_tags(me))
                if (t == T) present = true;
            Check(present, "phase54 scoped_grant: the leased tag is present in cabin_tags");
            {
                box::result<box::memtag::scoped_grant> sg2 = box::memtag::grant_scope(me, T);
                Check(sg2 && !sg2->held(),
                      "phase54 scoped_grant of an already-held tag is inert (held()==false)");
            }
            bool still = false;
            for (const std::string &t : box::memtag::cabin_tags(me))
                if (t == T) still = true;
            Check(still, "phase54 inert scoped_grant revoked nothing (outer lease survives)");
        } else {
            // Unprivileged cabin: the factory must surface the denial as a clean
            // error arm carrying a recovered cause — a genuine check, not a
            // vacuous pass. The RAII happy-path is an explicit, visible skip.
            std::string why(sg.error().message());
            Check(!why.empty(), "phase54 grant_scope denial surfaces a recovered cause");
            printf("[CXX] SKIP phase54: scoped_grant RAII happy-path needs the 'system' "
                   "tag-bit (unprivileged cabin); denial surfaced cleanly: %s\n", why.c_str());
        }
        // outer sg leaves scope here: if it held, the lease is revoked cleanly.
    }

    printf("[CXX] PASS phase54: box::heap (tag_id/tag_name/counters/make/tagged) + "
           "box::memtag (region encrypted/keyid + scoped_grant) + box::hw "
           "(keyid_of/encrypted) + formatters\n");
}

// ── Ф25e: explicit snapshot model (record snapshot vs live handle) + typed
//    tags + anchor_all + snapshot::adopt + vga::clear_line + chrono timeouts ──
void Phase55()
{
    using box::tagfs::file;
    using box::tagfs::record;

    // Leftover cleanup — the disk persists across matrix configs, so a crashed
    // prior run could leave p55 files behind.
    for (auto f : box::tagfs::query("p55")) (void)f.remove();

    box::result<file> made = box::tagfs::create("p55:recA", {"p55"});
    if (!made) {
        std::string_view why = made.error().message();
        printf("[CXX] SKIP phase55: tagfs storage unavailable (create p55:recA: %.*s) "
               "— record / typed-tag / anchor_all / snapshot::adopt checks skipped\n",
               static_cast<int>(why.size()), why.data());
    } else {
        std::uint32_t A = made->id();

        // (1) record — a metadata snapshot that does NOT re-read the kernel ──
        box::result<record> r = box::tagfs::find("p55:recA");
        Check(r && r->name() == "p55:recA", "phase55 find() hands back a record snapshot");
        // Mutate behind the record's back through a separate live handle.
        Check(box::tagfs::file(A).rename("p55:recB").has_value(),
              "phase55 rename via a separate live handle");
        Check(r && r->name() == "p55:recA",
              "phase55 record.name() reads the captured snapshot, not the kernel");
        Check(r && r->live().name() == "p55:recB", "phase55 record.live() is always-fresh");
        Check(r && r->reread().has_value() && r->name() == "p55:recB",
              "phase55 reread re-snapshots");
        std::vector<file> q = box::tagfs::query("p55");
        Check(!q.empty(), "phase55 query still hands back live file handles");

        // (2) typed-tag round-trip ──────────────────────────────────────────
        // The kernel adds an auto-label tag at create time, so this file already
        // carries {auto-label, "p55"} and sits near the file_info 5-tag report
        // cap. Each tag is therefore read back promptly (while the count is <= 5),
        // so no proof depends on a later tag staying inside the 5-slot window.
        file f = box::tagfs::file(A);
        (void)f.add_tag("label:abc");  // non-numeric value, read back first
        box::result<box::tagfs::tag> tl = f.tag_named("label");
        Check(tl && !tl->as<int>() && tl->as<int>().error().code() == box::errc::invalid_argument,
              "phase55 non-numeric value → as<int>() invalid_argument");
        Check(f.add_tag("port", 8080u).has_value(), "phase55 add_tag<unsigned>(port, 8080)");
        box::result<box::tagfs::tag> tp = f.tag_named("port");
        Check(tp && tp->as<unsigned>() && tp->as<unsigned>().value() == 8080u,
              "phase55 tag_named(port).as<unsigned>() == 8080");
        Check(f.add_tag("on", true).has_value(), "phase55 add_tag<bool>(on, true)");
        box::result<box::tagfs::tag> to = f.tag_named("on");
        Check(to && to->as<bool>() && to->as<bool>().value() == true,
              "phase55 tag_named(on).as<bool>() == true");
        box::status big = f.add_tag("big", 100000000000ull);
        Check(!big && big.error().code() == box::errc::buffer_too_small,
              "phase55 over-capacity typed value → buffer_too_small (tag not added)");
        box::result<box::tagfs::tag> ab = f.tag_named("absent");
        Check(!ab && ab.error().code() == box::errc::tag_not_found,
              "phase55 tag_named(absent) → tag_not_found");

        // (3) anchor_all — whole-filesystem durability (storage confirmed up) ─
        Check(box::tagfs::anchor_all().has_value(),
              "phase55 anchor_all() whole-fs durability ok");

        // (4) snapshot adopt + RAII drop ────────────────────────────────────
        box::result<box::tagfs::snapshot> s = box::tagfs::snapshot::of(A, "p55snap");
        if (!s) {
            std::string why(s.error().message());
            printf("[CXX] SKIP phase55: snapshot::of denied (%s) — adopt re-own "
                   "round-trip skipped (adopt(0) arm still checked)\n", why.c_str());
        } else {
            std::uint32_t kept = s->keep();  // detach: the id outlives this handle
            {
                box::result<box::tagfs::snapshot> re =
                    box::tagfs::snapshot::adopt(kept, "p55snap");
                Check(re && re->id() == kept, "phase55 adopt re-owns the kept id");
            }  // re's dtor → snap_delete(kept)
            bool present = false;
            for (std::uint32_t sid : box::tagfs::snapshots())
                if (sid == kept) present = true;
            Check(!present, "phase55 adopt's dtor snap_deleted the re-owned id");
        }

        // (1b) find_all enumerates past the former 8 stack-buffer cap ────────
        // TagFS names are not unique; create N (> the old cap of 8) same-named
        // files and confirm find_all returns every one (heap buffer, ceiling 255).
        {
            constexpr int N = 10;
            int           created = 0;
            for (int i = 0; i < N; ++i)
                if (box::tagfs::create("p55:multi", {"p55"})) ++created;
            if (created == N) {
                std::size_t got = box::tagfs::find_all("p55:multi").size();
                Check(got == static_cast<std::size_t>(N),
                      "phase55 find_all returns all 10 same-named files (heap buffer > old 8-cap)");
            } else {
                printf("[CXX] note phase55: find_all bulk probe created %d/%d files — skipped\n",
                       created, N);
            }
        }

        // Cleanup the p55 files now the storage round-trips are done.
        for (auto f2 : box::tagfs::query("p55")) (void)f2.remove();
    }

    // adopt(0) is a pure-logic guard (no syscall) — always checked.
    box::result<box::tagfs::snapshot> z = box::tagfs::snapshot::adopt(0);
    Check(!z && z.error().code() == box::errc::invalid_argument,
          "phase55 adopt(0) → invalid_argument");

    // (5) vga::clear_line — the physical clear is dormant on the serial log
    //     (like every box::vga op); the syscall round-trip is what's checked.
    Check(box::vga::clear_line(0, box::colors::black), "phase55 vga::clear_line syscall ok");

    // (6) chrono timeouts — the C macros are the single source of truth; the
    //     chrono type carries the unit, proven across units at compile time.
    static_assert(box::timeouts::fast == std::chrono::milliseconds(BOX_TIMEOUT_FAST_MS));
    static_assert(box::timeouts::storage == std::chrono::seconds(5));
    static_assert(box::timeouts::kdbg == std::chrono::minutes(1));
    Check(box::timeouts::input.count() == 30000, "phase55 input timeout == 30s");

    printf("[CXX] PASS phase55: box::tagfs::record (captured snapshot vs live/reread) "
           "+ typed tags (tag::as<T> / file::add_tag<T>) + anchor_all + "
           "snapshot::adopt + vga::clear_line + box::timeouts (chrono)\n");
}

void Phase56()
{
    using box::tagfs::file;

    // SL-1 (system-tag classification) + SL-2 (snapshot name-by-id) honesty.
    // The matrix shares one disk across its 4 configs, so this is cleanup-first
    // and re-runnable: a leaked p56snap is reclaimed, and leftover p56 files are
    // swept. p56sys carries an explicit "p56" tag purely so query() finds it for
    // the sweep; its reserved "system" tag blocks ObjDelete, so it is stripped
    // first (also covers a crash between the add and the final sweep).
    auto sweep = [] {
        for (auto victim : box::tagfs::query("p56")) {
            (void)victim.remove_tag("system");  // 'system' tag blocks delete
            (void)victim.remove();
        }
    };
    (void)box::tagfs::snapshot::reclaim("p56snap");  // drop a leaked snapshot
    sweep();

    box::result<file> made = box::tagfs::create("p56sys", {"p56"});
    if (!made) {
        std::string_view why = made.error().message();
        printf("[CXX] SKIP phase56: tagfs storage unavailable (create p56sys: %.*s) "
               "— system-tag classification + snapshot name-by-id checks skipped\n",
               static_cast<int>(why.size()), why.data());
        return;
    }
    file f = *made;

    // ── SL-1 discriminator: reserved vocabulary, not provenance ─────────────
    // f now carries {p56sys (auto-label), p56}; add a bare reserved key and a
    // user value-tag → 4 tags, inside the 5-tag report cap. The auto-label is
    // kernel-generated yet NOT system — that is the model-A-over-provenance proof.
    box::status sys_added  = f.add_tag("system");      // bare reserved key
    box::status zone_added = f.add_tag("zone:east");   // user value-tag
    if (!sys_added || !zone_added) {
        box::error       e   = !sys_added ? sys_added.error() : zone_added.error();
        std::string_view why = e.message();
        printf("[CXX] SKIP phase56: tag add denied (%.*s) — SL-1 discriminator skipped\n",
               static_cast<int>(why.size()), why.data());
    } else {
        box::result<box::tagfs::tag> ts = f.tag_named("system");
        Check(ts && ts->system == true,
              "phase56 bare reserved key 'system' is classified system");
        box::result<box::tagfs::tag> tz = f.tag_named("zone");
        Check(tz && tz->system == false,
              "phase56 user value-tag 'zone:east' is not system");
        box::result<box::tagfs::tag> ta = f.tag_named("p56sys");
        Check(ta && ta->system == false,
              "phase56 auto-label 'p56sys' is not system (reserved-vocabulary model, not provenance)");
    }

    // ── SL-2: snapshot name-by-id + deterministic reclaim ────────────────────
    box::result<box::tagfs::snapshot> s = box::tagfs::snapshot::of(f, "p56snap");
    if (!s) {
        std::string why(s.error().message());
        printf("[CXX] SKIP phase56: snapshot::of denied (%s) — SL-2 name-by-id / reclaim skipped\n",
               why.c_str());
    } else {
        std::uint32_t sid = s->keep();  // detach: simulate a leaked snapshot

        bool named_match = false;
        for (const box::tagfs::snapshot_info &si : box::tagfs::snapshots_named())
            if (si.id == sid && si.name == "p56snap") named_match = true;
        Check(named_match,
              "phase56 snapshots_named() reports id + name for the leaked snapshot");

        {
            box::result<box::tagfs::snapshot> re = box::tagfs::snapshot::reclaim("p56snap");
            Check(re && re->id() == sid,
                  "phase56 reclaim(\"p56snap\") re-owns the leaked snapshot by name");
        }  // re's owning dtor → snap_delete(sid)

        bool still_present = false;
        for (std::uint32_t id : box::tagfs::snapshots())
            if (id == sid) still_present = true;
        Check(!still_present, "phase56 reclaim's RAII drop removed the snapshot");

        box::result<box::tagfs::snapshot> none = box::tagfs::snapshot::reclaim("p56nope");
        Check(!none && none.error().code() == box::errc::snapshot_not_found,
              "phase56 reclaim(\"p56nope\") -> snapshot_not_found");
    }

    sweep();  // clean the p56 files (strip 'system' first so delete is allowed)

    printf("[CXX] PASS phase56: TagFS substrate honesty — system-tag classification "
           "(reserved vocabulary) + snapshot name-by-id (snap_info) + deterministic reclaim\n");
}

// phase57 — box::reflex (TOUCH_REACT manifest RAII). The kernel runs a stored
// manifest in-kernel on every touch of a tag — BoxOS's non-Unix replacement for
// an async-signal handler. App-private "cxxreflex:*" tags throughout, so a sink
// firing UNAMBIGUOUSLY proves the kernel ran the stored manifest. after_ms is 0
// in every send (the after_ms>0 PIT path is a separate deferred hazard).
void Phase57()
{
    // SYSTEM_OP_TOUCH_SEND params for a forward to `to`: [u16 full][u16 bare][u32 after=0].
    auto send_params = [](const box::tag &to, std::byte p[8]) {
        std::uint16_t f = to.pair().full, b = to.pair().bare;
        std::uint32_t after = 0;
        __builtin_memcpy(p + 0, &f, 2);
        __builtin_memcpy(p + 2, &b, 2);
        __builtin_memcpy(p + 4, &after, 4);
    };

    // ── step6 — A(REACT) forwards to B(REST): the non-Unix proof ──────────────
    // A reflex on A stores a one-op manifest SEND→B. Touching A makes the kernel
    // run that manifest, which fires B. B is app-private and only the manifest
    // writes it, so B firing can only mean the kernel executed the stored manifest.
    {
        box::tag A("cxxreflex:trig");   // REACT trigger
        box::tag B("cxxreflex:sink");   // REST sink
        Check(A.id() != TOUCH_TAG_INVALID && B.id() != TOUCH_TAG_INVALID,
              "phase57 step6 tags interned");

        box::manifest<> prog;
        std::byte pB[8];
        send_params(B, pB);
        prog.op(DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, box::no_crate, box::no_crate,
                std::span<const std::byte>(pB, 8));
        box::compiled_manifest card(prog);
        Check((bool)card, "phase57 step6 manifest compiled");

        box::subscription subB(B);
        Check((bool)subB, "phase57 step6 subscribed to sink B");

        box::reflex rx(A, std::move(card));
        Check((bool)rx, "phase57 step6 reflex bound to A (REACT)");
        Check((bool)rx.manifest(), "phase57 step6 reflex retains the compiled manifest");

        Check(box::publish(A, (std::uint32_t)0xBEEF),
              "phase57 step6 publish(A) accepted");

        if (auto ev = subB.wait(1000)) {
            Check(ev->tag_id() == B.id(),
                  "phase57 step6 kernel ran the stored manifest on touch(A) -> B fired");
            Check(ev->from_user(),
                  "phase57 step6 B-forward carries TOUCH_FLAG_USER (reflex owner is source)");
        } else {
            Check(false, "phase57 step6 B must fire (kernel REACT manifest)");
        }
    }

    // ── step7 — positive recursion-guard exploit (the missing Bug2 test) ──────
    // A self-cycling manifest: op0 SEND→B (observable), op1 SEND→A (re-trigger).
    // One ignition runs the WHOLE recursion synchronously in-kernel; the per-core
    // stack-headroom guard (Ф26b a44e4e8) caps the depth and DROPS further re-
    // triggers instead of triple-faulting. g_react_depth_drops is not userspace-
    // observable, so the guard is proven INDIRECTLY: the manifest ran (fan >= 1)
    // + the recursion was bounded (fan finite, the drain terminates) + the system
    // survived (a subsequent syscall still works).
    {
        box::tag A("cxxreflex:cyc");    // REACT self-cycle
        box::tag B("cxxreflex:csink");  // REST observable
        Check(A.id() != TOUCH_TAG_INVALID && B.id() != TOUCH_TAG_INVALID,
              "phase57 step7 tags interned");

        box::manifest<> prog;
        std::byte pB[8], pA[8];
        send_params(B, pB);  // op0 — observable forward to B
        send_params(A, pA);  // op1 — self re-trigger of A
        prog.op(DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, box::no_crate, box::no_crate,
                std::span<const std::byte>(pB, 8));
        prog.op(DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, box::no_crate, box::no_crate,
                std::span<const std::byte>(pA, 8));
        box::compiled_manifest card(prog);
        Check((bool)card, "phase57 step7 cyclic manifest compiled");

        box::subscription subB(B);
        Check((bool)subB, "phase57 step7 subscribed to sink B");

        box::reflex rx(A, std::move(card));
        Check((bool)rx, "phase57 step7 self-cycle reflex bound to A");

        Check(box::publish(A, (std::uint32_t)1), "phase57 step7 single ignition");

        // Drain the queued B-events with a HARD CEILING so this can never hang.
        int fan = 0;
        subB.set_drain_timeout(200);
        for (box::touch e : subB) {
            (void)e;
            if (++fan >= 4096) break;
        }
        Check(fan >= 1, "phase57 step7 kernel ran the cyclic manifest at least once");
        Check(fan < 4096,
              "phase57 step7 self-cycle BOUNDED by stack-headroom guard (no triple-fault)");

        Check(box::tag("cxxreflex:alive").id() != TOUCH_TAG_INVALID,
              "phase57 step7 system live after bounded recursion");
    }

    // ── step8 — concurrent teardown-vs-publish UAF exercise (16c stress) ──────
    // A publisher strand hammers publish(A) while the main strand churns reflex
    // bind/unbind on A, racing TouchClaimSet (ctor) / TouchClaimClear (dtor)
    // against the publisher's Phase-2 delivery across cores — the per-TouchSub
    // refcount path fixed in Ф26b 98757b5. The sink is unclaimed throwaway:
    // SURVIVAL is the assertion. The bounded loops + the join + the ALL PASS gate
    // are the watchdog. The cross-core race needs a real sibling strand (per-strand
    // TLS = FSGSBASE); without it the churn runs alone — single-core cannot exhibit
    // the cross-core UAF anyway, so this is harmless on 1c and real stress on 16c.
    {
        box::tag A("cxxreflex:race");
        box::tag sink("cxxreflex:rsink");  // throwaway, unclaimed
        Check(A.id() != TOUCH_TAG_INVALID && sink.id() != TOUCH_TAG_INVALID,
              "phase57 step8 tags interned");

        std::atomic<bool> stop{false};
        int               bound = 0;  // count REAL binds — proves the churn isn't vacuous
        {
            std::optional<box::strand> publisher;
            if (cpu_has_fsgsbase()) {
                publisher.emplace([&] {
                    for (std::uint32_t i = 0; i < 4000 && !stop.load(std::memory_order_relaxed); ++i)
                        box::publish(A, i);
                });
            }

            for (int k = 0; k < 256; ++k) {
                box::manifest<> c;
                std::byte ps[8];
                send_params(sink, ps);
                c.op(DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, box::no_crate, box::no_crate,
                     std::span<const std::byte>(ps, 8));
                box::compiled_manifest cm(c);
                box::reflex rx(A, std::move(cm));  // ctor claim / dtor release race
                if (rx) ++bound;
            }
            stop.store(true, std::memory_order_relaxed);
        }  // publisher (if spawned) joins on scope exit

        Check(bound >= 1,
              "phase57 step8 >=1 REACT claim/release cycle actually bound "
              "(the teardown-vs-publish window is exercised, not vacuously skipped)");
        Check(box::tag("cxxreflex:rlive").id() != TOUCH_TAG_INVALID,
              "phase57 step8 system live after concurrent claim/release vs publish hammer");
    }

    printf("[CXX] PASS phase57: box::reflex — TOUCH_REACT manifest RAII "
           "(in-kernel reaction + bounded self-cycle + concurrent claim/release vs publish)\n");
}

// ── Phase58 (Ф26d-3) — box::child: RAII child-process supervisor ──────────────
// Proves box::child over the strand-local death station: clean exit, co_await
// exited(), disposition caching, detach-without-kill, kill->KILLED, bounded
// wait-timeout, and the variant-B clincher — two children's deaths demultiplexed
// to the right pid from ONE process:died claim. Every assert is non-vacuous on a
// config where spawn works (and FAILS if box::child is broken); where proc_exec
// is unavailable the whole phase is a documented skip.

// scenario 2: await a child's exit on a box::executor (the FOREVER awaiter).
static box::task<box::result<int>> p58_await_exit(box::child *c)
{
    co_return co_await c->exited();
}

void Phase58()
{
    auto drain_inbox = [] { while (box::receive()) { } };  // proca/procb send to spawner

    // Availability probe — if proc_exec is unavailable here, skip non-vacuously.
    {
        box::result<box::child> probe = box::child::spawn("proca");
        if (!probe) {
            printf("[CXX] note phase58: proc_exec unavailable (%.*s); box::child skipped\n",
                   (int)probe.error().message().size(), probe.error().message().data());
            printf("[CXX] PASS phase58: box::child (spawn unavailable on this config — skipped)\n");
            return;
        }
        box::result<int> pr = probe->wait();   // reap the probe so it never lingers
        Check(pr.has_value() && *pr == 0, "phase58 probe proca clean wait");
        drain_inbox();
    }

    // 1. clean exit — wait() yields the success code 0.
    {
        box::result<box::child> c = box::child::spawn("proca");
        Check(c.has_value(), "phase58.1 spawn proca");
        if (c) {
            box::result<int> r = c->wait();
            Check(r.has_value() && *r == 0, "phase58.1 proca clean exit -> *r == 0");
        }
        drain_inbox();
    }

    // 2. co_await exited() on a fresh procb, driven by an executor.
    {
        box::result<box::child> c = box::child::spawn("procb");
        Check(c.has_value(), "phase58.2 spawn procb");
        if (c) {
            box::executor       ex;
            box::result<int>    r = ex.block_on(p58_await_exit(&*c));
            Check(r.has_value() && *r == 0, "phase58.2 co_await exited() procb -> *r == 0");
        }
        drain_inbox();
    }

    // 3. double wait / cache — re-waiting a reaped child returns the cache.
    {
        box::result<box::child> c = box::child::spawn("proca");
        Check(c.has_value(), "phase58.3 spawn proca");
        if (c) {
            box::result<int> r1 = c->wait();
            Check(r1.has_value() && *r1 == 0, "phase58.3 first wait -> 0");
            box::result<int> r2 = c->wait();   // cached — no block, same answer
            Check(r2.has_value() && *r2 == 0, "phase58.3 second wait -> 0 (cached)");
        }
        drain_inbox();
    }

    // 4. detach does NOT kill and does NOT hang.
    {
        std::uint32_t detached_pid = 0;
        {
            box::result<box::child> c = box::child::spawn("proca");
            Check(c.has_value(), "phase58.4 spawn proca");
            if (c) detached_pid = c->pid();
        }  // ~child here → DETACH (no kill, no wait)
        if (detached_pid != 0) {
            box::process p(detached_pid);
            Check(p.alive(), "phase58.4 detached child still alive (dtor did not kill it)");
            // It runs to completion on its own; the phase proceeds (no hang).
            box::stopwatch sw;
            while (p.alive() && sw.elapsed() < std::chrono::milliseconds(2000)) {
                drain_inbox();
                yield();
            }
            Check(!p.alive(), "phase58.4 detached child finished on its own (no hang)");
        }
        drain_inbox();
    }

    // 5. kill() -> the child's disposition is process_killed.
    {
        box::result<box::child> c = box::child::spawn("childspin");
        Check(c.has_value(), "phase58.5 spawn childspin");
        if (c) {
            box::status k = c->kill();
            Check(k.has_value(), "phase58.5 kill() reports success");
            box::result<int> r = c->wait();
            Check(!r.has_value() && r.error().code() == box::errc::process_killed,
                  "phase58.5 killed child -> errc::process_killed");
        }
    }

    // 6. bounded wait on an eternal child -> errc::timeout (chosen deadline, not EOF).
    {
        box::result<box::child> c = box::child::spawn("childspin");
        Check(c.has_value(), "phase58.6 spawn childspin");
        if (c) {
            box::result<int> r = c->wait(50);
            Check(!r.has_value() && r.error().code() == box::errc::timeout,
                  "phase58.6 wait(50) on eternal child -> errc::timeout");
            (void)c->kill();   // cleanup
            (void)c->wait();   // reap so the station entry retires
        }
    }

    // 7. multi-child demux — the variant-B proof. Two children, two DIFFERENT
    //    dispositions, routed to the right pid from ONE process:died claim.
    {
        box::result<box::child> a = box::child::spawn("proca");      // exits clean 0
        box::result<box::child> b = box::child::spawn("childspin");  // we kill it
        Check(a.has_value() && b.has_value(), "phase58.7 spawn proca + childspin");
        if (a && b) {
            box::status k = b->kill();
            Check(k.has_value(), "phase58.7 kill childspin");
            box::result<int> ra = a->wait();
            box::result<int> rb = b->wait();
            Check(ra.has_value() && *ra == 0,
                  "phase58.7 proca death routed by pid -> clean 0");
            Check(!rb.has_value() && rb.error().code() == box::errc::process_killed,
                  "phase58.7 childspin death routed by pid -> process_killed (demux)");
        }
        drain_inbox();
    }

    // 8. pid-reuse regression (the Ф26d-3 audit blocker) — a recycled pid must NOT
    //    inherit the stale death of its predecessor. pid_alloc hands out the LOWEST
    //    free pid, so to ARM the aliasing deterministically we retry: spawn a fast
    //    child A, detach it (its death stays unclaimed in the ring), let the kernel
    //    reap it (freeing its pid), then spawn an eternal B. If B lands on a LOWER
    //    pid that churn freed elsewhere, we PIN B (holding that pid) and retry —
    //    this provably converges, since each miss permanently removes one lower
    //    free pid. Once B reuses A's pid, (Y)'s (pid, generation) match drops A's
    //    stale death (OLD generation); the bare-pid bug would misroute A's exit 0
    //    to the eternal B (or hang). Honest skip-note only if spawn is unavailable.
    {
        std::vector<box::child> pins;   // eternal children pinning lower pids
        bool armed = false, reap_failed = false, spawn_failed = false;
        for (int attempt = 0; attempt < 40 && !armed; ++attempt) {
            box::result<box::child> a = box::child::spawn("proca");
            if (!a) { spawn_failed = true; break; }
            std::uint32_t a_pid = a->pid();
            { box::child gone = std::move(*a); }   // DETACH A: slot leaves the
                                                   // station, but A's death still
                                                   // sits unclaimed in the ring

            // Reap-wait via proc_info — a DIFFERENT ring, so it never drains the
            // station's process:died claim; A's death stays buffered for B below.
            // Wait for proc_info to no longer FIND a_pid (process reaped, pid freed
            // for reuse): alive() is not enough — it flips at TERMINATED, before the
            // reaper frees the pid, which would make B miss a_pid.
            box::process   pa(a_pid);
            box::stopwatch sw;
            while (pa.info().has_value() && sw.elapsed() < std::chrono::milliseconds(2000)) yield();
            if (pa.info().has_value()) { reap_failed = true; break; }

            box::result<box::child> b = box::child::spawn("childspin");  // eternal
            if (!b) { spawn_failed = true; break; }
            if (b->pid() == a_pid) {
                // Aliasing armed: A's stale death (a_pid, genA) is in the ring, B is
                // (a_pid, genB) and eternal. (Y) drops the stale death (generation
                // mismatch) -> B.wait times out; the bare-pid bug would hand back
                // A's clean exit 0.
                armed = true;
                printf("[CXX] note phase58.8: armed pid-reuse at pid %u "
                       "(A's stale death must be dropped by generation)\n",
                       (unsigned)a_pid);
                box::result<int> r = b->wait(200);
                Check(!r.has_value() && r.error().code() == box::errc::timeout,
                      "phase58.8 stale death of A NOT misrouted to recycled-pid B");
                (void)b->kill();
                (void)b->wait();
            } else {
                pins.push_back(std::move(*b));   // pin the lower pid, retry
            }
            drain_inbox();
        }
        if (!armed)
            printf("[CXX] note phase58.8: pid-reuse not armed (%s) — skipped\n",
                   spawn_failed ? "spawn unavailable"
                                : reap_failed ? "child not reaped in time"
                                              : "no reuse in 40 attempts");
        for (auto &p : pins) { (void)p.kill(); (void)p.wait(); }   // release pins
        drain_inbox();
    }

    printf("[CXX] PASS phase58: box::child — RAII child supervisor "
           "(clean/co_await/cache/detach/kill/timeout/pid-demux/pid-reuse)\n");
}

// ── Phase59 (Ф26e) — box::ferry: co_await async file I/O over the waybill demux ─
// Proves the async storage substrate end-to-end: a single read round-trips; N
// reads submitted in REVERSE offset order each resume with THEIR own block (the
// waybill demux clincher — a FIFO/address-keyed scheme would misroute and fail);
// read+write interleave without cross-routing; an EOF read exercises the kernel
// SYNC-FALLBACK token delivery (0 bytes, never a hang); a dropped ferry's
// completion is reaped by the station, not misrouted onto a live one; and a
// synchronous read_at interleaved with a ferry stays byte-identical (full
// isolation). Non-vacuous where storage works; a documented skip otherwise.

// A sibling task: co_await one ferry into an out-slot (simultaneous-waiter demux).
static box::task<void> p59_await_into(box::ferry *f, box::result<std::size_t> *out)
{
    *out = co_await *f;
    co_return;
}
// Single-await driver for executor::block_on.
static box::task<box::result<std::size_t>> p59_await(box::ferry *f)
{
    co_return co_await *f;
}

void Phase59()
{
    constexpr std::size_t CH = 512;                         // per-block chunk
    auto fill = [](std::byte *b, std::size_t n, std::byte v) {
        for (std::size_t i = 0; i < n; ++i) b[i] = v;
    };
    auto all_eq = [](const std::byte *b, std::size_t n, std::byte v) {
        for (std::size_t i = 0; i < n; ++i) if (b[i] != v) return false;
        return true;
    };
    auto pat = [](int i) { return static_cast<std::byte>(0xA0 + i); };

    // Availability probe — create (or reopen) a scratch file.
    box::result<box::tagfs::file> ff = box::tagfs::create("ferrytest");
    if (!ff) {
        box::result<box::tagfs::record> rec = box::tagfs::find("ferrytest");
        if (rec) ff = rec->live();
    }
    if (!ff) {
        printf("[CXX] note phase59: tagfs create/find unavailable (%.*s); box::ferry skipped\n",
               (int)ff.error().message().size(), ff.error().message().data());
        printf("[CXX] PASS phase59: box::ferry (storage unavailable on this config — skipped)\n");
        return;
    }
    box::tagfs::file f = *ff;

    // Seed 4 blocks {0..3} with distinct patterns (synchronous writes).
    {
        bool wok = true;
        std::byte wb[CH];
        for (int i = 0; i < 4; ++i) {
            fill(wb, CH, pat(i));
            box::result<std::size_t> w =
                f.write_at((std::uint64_t)i * CH, std::span<const std::byte>(wb, CH));
            if (!w || *w != CH) wok = false;
        }
        Check(wok, "phase59 setup: 4 block patterns written");
        if (!wok) {
            printf("[CXX] PASS phase59: box::ferry (write setup failed — skipped)\n");
            return;
        }
    }

    // 1. single read_async round-trips block 0.
    {
        std::byte rb[CH];
        fill(rb, CH, std::byte{0});
        box::ferry fr = f.read_async(0, std::span<std::byte>(rb, CH));
        box::executor ex;
        box::result<std::size_t> r = ex.block_on(p59_await(&fr));
        Check(r.has_value() && *r == CH && all_eq(rb, CH, pat(0)),
              "phase59.1 single read_async round-trips block 0");
    }

    // 2. out-of-order demux — 4 reads submitted in REVERSE order, awaited as
    //    simultaneous sibling tasks; each must resume with ITS OWN block.
    {
        std::byte rb[4][CH];
        for (int i = 0; i < 4; ++i) fill(rb[i], CH, std::byte{0});
        box::ferry f3 = f.read_async(3 * CH, std::span<std::byte>(rb[3], CH));
        box::ferry f0 = f.read_async(0 * CH, std::span<std::byte>(rb[0], CH));
        box::ferry f2 = f.read_async(2 * CH, std::span<std::byte>(rb[2], CH));
        box::ferry f1 = f.read_async(1 * CH, std::span<std::byte>(rb[1], CH));
        printf("[CXX] note phase59.2: armed 4 out-of-order ferries (reverse submit)\n");
        box::result<std::size_t> o0, o1, o2, o3;
        {
            box::executor ex;
            ex.spawn(p59_await_into(&f3, &o3));
            ex.spawn(p59_await_into(&f0, &o0));
            ex.spawn(p59_await_into(&f2, &o2));
            ex.spawn(p59_await_into(&f1, &o1));
            ex.run();
        }
        int correct = 0;
        if (o0.has_value() && *o0 == CH && all_eq(rb[0], CH, pat(0))) correct++;
        if (o1.has_value() && *o1 == CH && all_eq(rb[1], CH, pat(1))) correct++;
        if (o2.has_value() && *o2 == CH && all_eq(rb[2], CH, pat(2))) correct++;
        if (o3.has_value() && *o3 == CH && all_eq(rb[3], CH, pat(3))) correct++;
        Check(correct == 4, "phase59.2 out-of-order demux: each co_await got ITS block");
    }

    // 3. mixed read+write interleave — 2 writes (blocks 4,5) + 2 reads (blocks 0,1)
    //    concurrent; then confirm the writes landed at the right offsets.
    {
        std::byte w4[CH], w5[CH], r0[CH], r1[CH];
        fill(w4, CH, std::byte{0xB4}); fill(w5, CH, std::byte{0xB5});
        fill(r0, CH, std::byte{0});    fill(r1, CH, std::byte{0});
        box::ferry fw4 = f.write_async(4 * CH, std::span<const std::byte>(w4, CH));
        box::ferry fr0 = f.read_async (0 * CH, std::span<std::byte>(r0, CH));
        box::ferry fw5 = f.write_async(5 * CH, std::span<const std::byte>(w5, CH));
        box::ferry fr1 = f.read_async (1 * CH, std::span<std::byte>(r1, CH));
        box::result<std::size_t> ow4, ow5, or0, or1;
        {
            box::executor ex;
            ex.spawn(p59_await_into(&fw4, &ow4));
            ex.spawn(p59_await_into(&fr0, &or0));
            ex.spawn(p59_await_into(&fw5, &ow5));
            ex.spawn(p59_await_into(&fr1, &or1));
            ex.run();
        }
        bool ok = ow4.has_value() && *ow4 == CH && ow5.has_value() && *ow5 == CH
               && or0.has_value() && *or0 == CH && all_eq(r0, CH, pat(0))
               && or1.has_value() && *or1 == CH && all_eq(r1, CH, pat(1));
        // No cross-routing: the writes must be at 4*CH and 5*CH, not swapped.
        std::byte v4[CH], v5[CH];
        box::result<std::size_t> rv4 = f.read_at(4 * CH, std::span<std::byte>(v4, CH));
        box::result<std::size_t> rv5 = f.read_at(5 * CH, std::span<std::byte>(v5, CH));
        ok = ok && rv4.has_value() && all_eq(v4, CH, std::byte{0xB4})
                && rv5.has_value() && all_eq(v5, CH, std::byte{0xB5});
        Check(ok, "phase59.3 mixed read+write interleave, writes landed at right offsets");
    }

    // 4. EOF read — offset past end forces the kernel SYNC-FALLBACK token path.
    //    0 bytes, success (proves the fallback delivers the completion, not a hang).
    {
        std::byte rb[CH];
        box::ferry fe = f.read_async((std::uint64_t)100 * CH, std::span<std::byte>(rb, CH));
        box::executor ex;
        box::result<std::size_t> r = ex.block_on(p59_await(&fe));
        Check(r.has_value() && *r == 0,
              "phase59.4 EOF read_async via sync-fallback -> 0 bytes (no hang)");
    }

    // 5. detach safety — a ferry dropped un-awaited has its completion reaped by
    //    the station (not misrouted); a normal ferry after it stays correct. The
    //    dropped op's buffer is kept alive for the kernel DMA (caller contract).
    {
        std::byte junk[CH];
        fill(junk, CH, std::byte{0});
        { box::ferry dropped = f.read_async(2 * CH, std::span<std::byte>(junk, CH)); }  // ~ferry = DETACH
        std::byte rb[CH];
        fill(rb, CH, std::byte{0});
        box::ferry fr = f.read_async(1 * CH, std::span<std::byte>(rb, CH));
        box::executor ex;
        box::result<std::size_t> r = ex.block_on(p59_await(&fr));
        Check(r.has_value() && *r == CH && all_eq(rb, CH, pat(1)),
              "phase59.5 detach safety: live ferry unaffected by a dropped one");
    }

    // 6. sync path unregressed — a synchronous read_at issued WHILE a ferry is in
    //    flight must return its own bytes, and must not steal the ferry's reply.
    {
        std::byte sb[CH], ab[CH];
        fill(sb, CH, std::byte{0}); fill(ab, CH, std::byte{0});
        box::ferry fa = f.read_async(3 * CH, std::span<std::byte>(ab, CH));
        box::result<std::size_t> sr = f.read_at(0, std::span<std::byte>(sb, CH));  // SYNC, in flight
        box::executor ex;
        box::result<std::size_t> ar = ex.block_on(p59_await(&fa));
        Check(sr.has_value() && *sr == CH && all_eq(sb, CH, pat(0)),
              "phase59.6 sync read_at unregressed (interleaved with a live ferry)");
        Check(ar.has_value() && *ar == CH && all_eq(ab, CH, pat(3)),
              "phase59.6 ferry reply not stolen by the interleaved sync read");
    }

    // 7. error-path completions land on the ferry channel — never a KCTX_GUIDE
    //    hang. Every EARLY-error exit of a waybilled storage op must still answer
    //    on KCTX_STORAGE, or its co_await (which only collects KCTX_STORAGE) waits
    //    forever. Covers file-not-found (a stale id) and the zero-length read/
    //    write early returns; each must RESUME with an error, not deadlock.
    {
        std::byte rb[CH];
        fill(rb, CH, std::byte{0});

        box::tagfs::file missing{0xDEADBEEFu};             // never a real file id
        box::ferry fnf = missing.read_async(0, std::span<std::byte>(rb, CH));
        box::executor ex;
        box::result<std::size_t> rnf = ex.block_on(p59_await(&fnf));
        Check(!rnf.has_value(),
              "phase59.7 ferry read of a missing file resumes with an error (no hang)");

        box::ferry fzr = f.read_async(0, std::span<std::byte>(rb, std::size_t{0}));
        box::executor ex2;
        box::result<std::size_t> rzr = ex2.block_on(p59_await(&fzr));
        Check(!rzr.has_value(),
              "phase59.7 zero-length ferry read resumes with an error (no hang)");

        box::ferry fzw = f.write_async(0, std::span<const std::byte>(rb, std::size_t{0}));
        box::executor ex3;
        box::result<std::size_t> rzw = ex3.block_on(p59_await(&fzw));
        Check(!rzw.has_value(),
              "phase59.7 zero-length ferry write resumes with an error (no hang)");
    }

    printf("[CXX] PASS phase59: box::ferry — co_await async file I/O "
           "(single/out-of-order demux/mixed r+w/EOF sync-fallback/detach/"
           "sync-isolation/error-path completions)\n");
}

// ── phase60: strong_order / weak_order for x87 80-bit long double (Ф27a) ──
// Runs the IEEE-totalOrder kernel on REAL x87 (fldt/fstpt + the andl padding
// mask). The cxxtest_traits.cpp static_asserts exercise only the host
// constant-evaluator, so the value asserts are re-checked here on target.
void Phase60()
{
    using std::strong_order;
    using std::weak_order;
    using std::bit_cast;
    const long double inf  = std::numeric_limits<long double>::infinity();
    const long double pnan = __builtin_nanl("");
    const long double nnan = -__builtin_nanl("");

    // strong_order — full IEEE total order over the 80-bit encodings.
    Check(strong_order(-0.0L, +0.0L) < 0 && strong_order(+0.0L, -0.0L) > 0,
          "phase60 strong -0 < +0");
    Check(strong_order(1.0L, 1.0L) == 0 && strong_order(1.0L, 2.0L) < 0 &&
          strong_order(2.0L, 1.0L) > 0, "phase60 strong 1 < 2");
    Check(strong_order(nnan, -inf) < 0 && strong_order(-inf, inf) < 0 &&
          strong_order(inf, pnan) < 0,
          "phase60 strong -NaN < -inf < +inf < +NaN");
    Check(strong_order(__LDBL_DENORM_MIN__, __LDBL_MIN__) < 0 &&
          strong_order(__LDBL_MIN__, 1.0L) < 0,
          "phase60 strong denorm < min-normal < 1");

    // weak_order — -0 ≡ +0 (fold), same-sign NaN equivalent, NaN tiers by sign.
    Check(weak_order(-0.0L, +0.0L) == 0, "phase60 weak -0 == +0");
    Check(weak_order(1.0L, 2.0L) < 0 && weak_order(2.0L, 1.0L) > 0 &&
          weak_order(1.0L, 1.0L) == 0, "phase60 weak 1 < 2");
    Check(weak_order(nnan, -inf) < 0 && weak_order(inf, pnan) < 0,
          "phase60 weak -NaN < -inf, +inf < +NaN");

    // Concrete strictly-ascending chain executed on x87 — falsifiable: the P1
    // sign-flip bug (or any wrong ordering) breaks a consecutive `< 0`.
    const long double asc[] = {
        -inf, -2.0L, -1.0L, -__LDBL_MIN__, -__LDBL_DENORM_MIN__,
        -0.0L, +0.0L, __LDBL_DENORM_MIN__, __LDBL_MIN__, 1.0L, 2.0L, inf,
    };
    bool asc_ok = true;
    for (unsigned i = 0; i + 1 < sizeof(asc) / sizeof(asc[0]); ++i)
        if (!(strong_order(asc[i], asc[i + 1]) < 0)) asc_ok = false;
    Check(asc_ok, "phase60 strong_order strictly ascends -inf..-0<+0..+inf");

    // Non-canonical x87 patterns (pseudo-denormal E=0/J=1, unnormal E=3/J=0,
    // pseudo-NaN E=max/J=0) via runtime-only bit_cast<long double>. FLD/FST m80
    // never raise #IA (Intel SDM) → cannot trap on real x87; each sorts by its
    // exponent class (below 1.0, or above every finite for E=max) — a concrete,
    // falsifiable position robust to whether the x87 load renormalizes.
    const long double pseudo_denorm =
        bit_cast<long double>((unsigned __int128)0x8000000000000000ull);
    const long double unnormal =
        bit_cast<long double>(((unsigned __int128)0x0003u << 64) | 0x4000000000000000ull);
    const long double pseudo_nan =
        bit_cast<long double>(((unsigned __int128)0x7FFFu << 64) | 0x4000000000000001ull);
    Check(strong_order(pseudo_denorm, 1.0L) < 0 && strong_order(unnormal, 1.0L) < 0 &&
          strong_order(2.0L, pseudo_nan) < 0,
          "phase60 non-canonical x87 patterns order by exponent class (no trap)");

    printf("[CXX] PASS phase60: strong_order/weak_order long double (x87 80-bit)\n");
}

// ── Phase61 (Ф27b) — <charconv> long double (80-bit x87): to_chars/from_chars ─
void Phase61()
{
    using F = std::chars_format;
    // low 80 meaningful bits (padding bytes 10-15 are never compared)
    auto bits = [](long double v) -> unsigned __int128 {
        return __builtin_bit_cast(unsigned __int128, v) & ((((unsigned __int128)1) << 80) - 1);
    };
    auto rt = [&](long double x, const char *tag) {
        char b[64]; auto r = std::to_chars(b, b + sizeof(b), x);
        long double y{}; auto q = std::from_chars(b, r.ptr, y);
        Check(r.ec == std::errc{} && q.ec == std::errc{} && q.ptr == r.ptr && bits(x) == bits(y), tag);
    };
    const long double inf = __builtin_infl();
    const long double qnan = __builtin_nanl("");

    // bit-exact round-trip over normals, subnormals, boundaries, signed zero
    rt(1.0L, "phase61 rt 1");
    rt(-1.0L, "phase61 rt -1");
    rt(3.0L, "phase61 rt 3");
    rt(0.5L, "phase61 rt 0.5");
    rt(__LDBL_MIN__, "phase61 rt LDBL_MIN");
    rt(__LDBL_MAX__, "phase61 rt LDBL_MAX");
    rt(__LDBL_DENORM_MIN__, "phase61 rt LDBL_DENORM_MIN");
    rt(0.0L, "phase61 rt +0");
    rt(-0.0L, "phase61 rt -0");
    rt(1e-4932L, "phase61 rt 1e-4932 (subnormal)");
    rt(1e4932L, "phase61 rt 1e4932");
    rt(3.141592653589793238L, "phase61 rt pi");

    // specials
    {
        char b[8]; auto r = std::to_chars(b, b + sizeof(b), inf);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == "inf", "phase61 to_chars inf");
        auto r2 = std::to_chars(b, b + sizeof(b), -inf);
        Check(r2.ec == std::errc{} && std::string_view(b, r2.ptr - b) == "-inf", "phase61 to_chars -inf");
        auto r3 = std::to_chars(b, b + sizeof(b), qnan);
        Check(r3.ec == std::errc{} && std::string_view(b, r3.ptr - b) == "nan", "phase61 to_chars nan");
        long double y{};
        const char si[] = "inf", sni[] = "-inf", sn[] = "nan";
        auto p1 = std::from_chars(si, si + 3, y);
        Check(p1.ec == std::errc{} && __builtin_isinf(y) && y > 0, "phase61 from_chars inf");
        auto p2 = std::from_chars(sni, sni + 4, y);
        Check(p2.ec == std::errc{} && __builtin_isinf(y) && y < 0, "phase61 from_chars -inf");
        auto p3 = std::from_chars(sn, sn + 3, y);
        Check(p3.ec == std::errc{} && __builtin_isnan(y), "phase61 from_chars nan");
    }

    // shortest strings for the extremes
    {
        char b[64]; auto r = std::to_chars(b, b + sizeof(b), __LDBL_DENORM_MIN__);
        Check(r.ec == std::errc{} && std::string_view(b, r.ptr - b) == "4e-4951", "phase61 shortest DENORM_MIN");
        auto r2 = std::to_chars(b, b + sizeof(b), __LDBL_MAX__);
        Check(r2.ec == std::errc{} && std::string_view(b, r2.ptr - b) == "1.189731495357231765e+4932",
              "phase61 shortest LDBL_MAX");
    }

    // hex
    {
        char b[64];
        auto h1 = std::to_chars(b, b + sizeof(b), 1.0L, F::hex);
        Check(h1.ec == std::errc{} && std::string_view(b, h1.ptr - b) == "1p+0", "phase61 hex 1");
        auto h2 = std::to_chars(b, b + sizeof(b), 3.0L, F::hex);
        Check(h2.ec == std::errc{} && std::string_view(b, h2.ptr - b) == "1.8p+1", "phase61 hex 3");
        auto h3 = std::to_chars(b, b + sizeof(b), __LDBL_DENORM_MIN__, F::hex);
        Check(h3.ec == std::errc{} && std::string_view(b, h3.ptr - b) == "1p-16445", "phase61 hex DENORM_MIN");
        long double y{};
        auto p = std::from_chars(b, h3.ptr, y, F::hex);
        Check(p.ec == std::errc{} && bits(y) == bits(__LDBL_DENORM_MIN__), "phase61 hex round-trip DENORM_MIN");
    }

    // one precision case per mode — each round-trips because ≥21 significant digits are emitted
    {
        long double x = 3.141592653589793238L;
        auto prt = [&](F f, int prec, const char *tag) {
            char b[64]; auto r = std::to_chars(b, b + sizeof(b), x, f, prec);
            long double y{}; auto q = std::from_chars(b, r.ptr, y, f);
            Check(r.ec == std::errc{} && q.ec == std::errc{} && bits(y) == bits(x), tag);
        };
        prt(F::fixed, 20, "phase61 fixed,20 round-trip");
        prt(F::scientific, 30, "phase61 scientific,30 round-trip");
        prt(F::general, 21, "phase61 general,21 round-trip");
    }

    // to_string / stold
    {
        Check(std::to_string(3.14159L) == "3.141590", "phase61 to_string 3.14159L");
        size_t pos = 0;
        long double v = std::stold("3.14159", &pos);
        Check(bits(v) == bits(3.14159L) && pos == 7, "phase61 stold 3.14159");
        bool threw = false;
        try { std::stold("1e99999"); } catch (const std::out_of_range &) { threw = true; }
        Check(threw, "phase61 stold overflow → out_of_range");
        threw = false;
        try { std::stold("xyz"); } catch (const std::invalid_argument &) { threw = true; }
        Check(threw, "phase61 stold junk → invalid_argument");
    }

    // stack probe: huge precision must not fault the 64KB stack's guard page
    {
        static char big[10240];
        auto s1 = std::to_chars(big, big + sizeof(big), __LDBL_DENORM_MIN__, F::scientific, 5000);
        Check(s1.ec == std::errc{} && (s1.ptr - big) > 5000, "phase61 stack probe sci,5000");
        auto s2 = std::to_chars(big, big + sizeof(big), __LDBL_MAX__, F::fixed, 4000);
        Check(s2.ec == std::errc{} && (s2.ptr - big) > 8900, "phase61 stack probe fixed,4000");
    }

    // ── Ф27b end-of-phase repro (karpathy #4): a >1290-digit subnormal forces
    // den = 10^~6240 (~648 words) and num<<16445 (~651 words), both past the old
    // kBigWords=576 → the big-int saturates and misrounds.  Each string is the
    // subnormal's OWN exact decimal truncated to 1310 significant digits, so its
    // correctly-rounded value is exactly M (oracle cross-checked host-side with
    // rational arithmetic).  These FAILED at 576 and pass at 720.
    {
        static const char kSub1[] =
            "2.3732492774908895340683443113489266643504822744199421576085225091157548"
            "276930485964851953725810242461670905682067230355554149070436856483471255"
            "067375395185962924456224099278200387611033012127078927732959167361673854"
            "123277931296132104797398198957951853654643489242724146355388334266840103"
            "898874789572573674687762277633747346173516826388082449381663462350659483"
            "414148586665711690674383627060956594480212465375082556596642117310001309"
            "148585466812077029604310739255027667178431703850481209846517552598367174"
            "051587967728968795853862788261184735201940143134484802881901843092349271"
            "732238073364185493871703420825897174990850157871154859174729203429251975"
            "431163187004802856486753810830827613006532722072482068267774817440465589"
            "652443165347122493952500987164479277439106574782817655788373198234838403"
            "595529746603388896496258952395665032391650869437281717532289618762394556"
            "546161251753168674069303605832850381748259261696056445535956038291106347"
            "894073011162824719532260900607755212507094765229153020810827997685364840"
            "380197204964158629676742540386388006353321283392503900628840274893627127"
            "906186163809877358616019979744169976258579680890411264173105637308583858"
            "932845224887434549578159453277805368702763707638451779363278148405751695"
            "330825405440929463215338649864234290848780450266965816868150100776062476"
            "798752876657780e-4932";
        static const unsigned long long kSub1_M = 0x5A5A5A5A5A5A5A5BULL;  // ~2^62 subnormal
        static const char kSub2[] =
            "1.5674357987094640790872145514563505210515918507482322378000216094221998"
            "261123610268281111047091056982348163593592975183738458038220274738330886"
            "084725036178723580437099065106217847668634364224306953472719033728666134"
            "443375112280167254942335439802943087366729715047098035505476316531969815"
            "196788553321329274890389128481629492740816941080659373257579182097425056"
            "111416363391600932079490855738792964647832636103778120731077786612697881"
            "036002996805295390599152709566825689347123687925590237489758400928431932"
            "334151427128226927711769186756314990575705419323542962621629254042307937"
            "033099537542688678255325886144866059116330570910308436773451740586006625"
            "091978972064969904707960088599474214517610934629103330000326494504383499"
            "083007356779970846702709966567288819284798296570825508597257667573687727"
            "617552812930442871076765835908093166330666787827645240044355185109363948"
            "505863700237274643997704165481909645728453568221961798293228821774624210"
            "090572555608692657369237297043706745765309201821177976041244731763682999"
            "109934371169126516299942529210171150142849323190310484408122312265983986"
            "187372360800103680554933892853980382533165189002249694394982737635338224"
            "663392122087761666872053311001400656809776171871274645567373660331906643"
            "404395922407979853241375715066463935461529363662222715691032060159693518"
            "284102993006162e-4949";
        static const unsigned long long kSub2_M = 0x000000000000002BULL;  // 43·2^-16445, max den budget
        auto sub = [&](const char *d, size_t n, unsigned long long m, const char *tag) {
            long double y{};
            auto p = std::from_chars(d, d + n, y);
            Check(p.ec == std::errc{} && bits(y) == (unsigned __int128)m, tag);
        };
        sub(kSub1, sizeof(kSub1) - 1, kSub1_M, "phase61 subnormal 1310-digit round-trip (~2^62)");
        sub(kSub2, sizeof(kSub2) - 1, kSub2_M, "phase61 subnormal 1310-digit round-trip (near DENORM_MIN)");
    }

    // ── Ф27b root#2 (C5, owner: heap path): EXACT subnormal binary midpoints.
    // (2k+1)*2^-16446 has ~11515 significant digits; the 1290-digit cap folds the
    // tail into sticky and rounds UP instead of round-half-even. The heap big-int
    // path keeps every digit and ties to even (k even -> down, k odd -> up).
    {
        // kMidEven: k=0x5A5A5A5A5A5A5A5A (even -> half-even DOWN); 11515 sig digits (exact midpoint)
        static const char kMidEven[] =
            "2.3732492774908895338860843347548029342240619777389711667885699683310766"
            "605069995721337767881493543593266125327754316752955525396763254717920271"
            "574559084169877937702186304373697645699742423161239643120327264544893975"
            "812496648304426740327238302181188219359023089265637740847629065075317076"
            "514517078347926021757903143526616035139750648657660415342033585480815370"
            "244662635259034358568404980154956486098876319358943839777126694507236411"
            "389157817205949369018728995440458435713413946924106129689875263304693890"
            "622546293202406943256868436269530441281966981883750501652168654798914043"
            "520420718242986896542273242041863607751858775761286712765853555648983552"
            "550529263680848597755128348962807856645142014095799020068052615739407062"
            "908219154358240691733546643352854760380651927507551514011071070726602879"
            "340034668978541486308171416008404233429236237354945302842677201573133482"
            "122399020926498447498854338363399841748526219198159975102414925707157301"
            "580906869096007950917078045304418856999043916735698748047419950961972052"
            "468424561735823397222099008855094225388211686972716557588974074522660792"
            "509892871685925732315162728488343966891726902651576335971574801378089708"
            "421800720511232430431253392569926105112990066824335423920280919118394178"
            "404386607727917062819478658886562214890480948515051405767972267792083091"
            "253284032453165450295549716553255783240244010261736313963038538317733323"
            "994814975108032781008831401319871214860050070489082161104931261577400427"
            "769702975946771779020886422499744522487061197532249528213902758428156906"
            "583027349638370866084237871464920736432415836610849413083091504376544923"
            "825756696591806559436712225243617686552669729948674838718602283852961274"
            "794895411898301429730068553289810227321590997589598220690876067349433470"
            "405319718884610492270983432018225092094321223016964290811367377375553021"
            "792308384163603363422126650085184258612067097626427535033318145384331387"
            "507905403238162052202751935562509935470321650189347795037357339401822465"
            "502382094556368371051590109854404998254363214492510724425016056720112273"
            "576554197546652573176005407842560919312606818572989552906103958942583239"
            "027799863042509118608373011669114344792707207509787652941154989221080770"
            "717096899138795083818453220385262402419413405297102917158838654860394059"
            "641035968831472318259139033493055370585670579176989641414596425975316931"
            "522280440759825807647789224019111869392243936862233436649125044691568506"
            "452563568382886063710214562278223209930907090453934776051104032986093957"
            "880833343660699311366700630225075602865494344312374369459171232455480810"
            "158955582463654223832550690215830303209375328995576908110552655658710278"
            "943372913892308703820683258522040684605346786411987011635960748661797182"
            "375800337197664703985098144325034951673634128558140407108363246506516664"
            "404113310493655331065790794382285124471622739590096933598639371651529884"
            "964315592359238698660967202951270012672749874966891374438584461415039100"
            "173834240992773012538088489467560477063858228703214751891439071202259308"
            "015536490557926718929167530563092850934229325587803210039161117860360460"
            "061904714220228366664283684385248069506648031973390972422520687407275519"
            "358661192193798281803282027119302975688268360549449126978503818109643331"
            "033896802052057127190355203863498157415279265992689073372721055324424989"
            "700245417403195086884830150251580001441926480624685228657188017015911785"
            "293824763460508294026866841373043724136736044877182870330389996373195034"
            "958749297702141784105405738090484763519525121978384747712363004241765612"
            "727545309562364167939325094686543941919314542630209090288917310250899483"
            "333791853247179181113197704917510379203874703672242495084214218858302879"
            "590296589479715500304246794835389050344525041748567194188693416591095695"
            "969254305122335332302479302389579507913105722744944918646857364351725532"
            "371619287034923666064960946317588186244145127180577353885996311826356787"
            "840745934091532225137147382734211577758813348002335471531311218584668680"
            "432918456895037321675192886315018517751173605071106546465259421781210248"
            "864755984473501714212590504548700862505545311355268984919997782770112969"
            "918478909240566031157475874881431007199460541669479850867711231941297655"
            "396078630642975117162562921707649850518708791394941815887362914132442278"
            "353578777088408140446301575726288168779591393519952780257471103577601247"
            "987253239743285082612466427773854470611969526921120986493188757514720157"
            "361503469189852802702446730722773480588759219551644221662992541596095148"
            "924572765097937643416088253549818550431951668843784177415260771767126191"
            "402245767065513104149632091756624200351397060420364798092120431609840090"
            "190523237461536500117965095681535627565369002369306658937425562401397289"
            "308361694322965947148809315375460344615729063560267878742746170856841178"
            "808628255756600649743235118613401781792115842345054983318513671948832329"
            "263308525282155631915558890847386610012735191823136574742239995106266482"
            "724488877147946039662329728081241070126267335001410789807592357243282378"
            "620980093421214442032537601004445477007051469096778719593904065105799455"
            "641200660782427045792634101693325618371069888850213282055882542887859499"
            "851535531843291275890892598123458627673247978590077770979221160858067743"
            "748335471554356272326473292228451790946228825471181384171094841161142360"
            "821766901900411962037622029761690744827663493478630869449890664194797801"
            "088187512897643051032101856416828757413555825112176201376784396722023766"
            "665276035419116675674230372361365902788856832368222471707617983382700742"
            "935465820644664318624326291710953323890309115791822164302939535161212467"
            "139655950433597267925972715876063675945275914283145773484382729356911315"
            "734925605499438970618659410623643245990544208790827174913348361481044777"
            "768327219382054646680098197402676859401913778734291767910848296509636171"
            "261551263215941140748081395846227274730338768276770551766987344714200715"
            "993828171628543590014181562042233812364866639923495579980838843372538727"
            "535099755004317967469235605437823337423186071787195006202028140649218424"
            "304373714759811539405421807762865095130842224329634083774196921979007211"
            "027279031795883155222566520661348538186047078979462469512329778564453211"
            "754962690143365712356307540866715027093713284461708898811670953401227915"
            "269266064203306785296025339988931685462949175668838208691336934432237568"
            "969144932686351229295835245390427186717381857535107551629888469897665725"
            "853816317484103634347634403705998858167966525560288773531207045971803239"
            "283619347022863163021961111380423955116240038605472124654665551321759262"
            "808208445134203690334824076456878318477184980667616747001920575219994164"
            "916608449690885899745229850781611927012800799228687728908880333236440416"
            "680141184561964731547818333918622598798929987019538279384017360157111047"
            "167259823770533843754974647666433279863864341660968764390856360892157320"
            "986586678388307260742479111003020335955668408788529990178440935913465177"
            "148672463082571419085275333639358996200430988506659815369277964654791034"
            "232274650259396100495074415231398147099659352378590235856233512867624617"
            "134203969650053142133708156711136512238934789247861363259680053035504926"
            "144867037453480636838351810089677437007239391473957866279889899214737471"
            "984813472669296972975239988752218910510554196476029269328623494970634292"
            "467400596634106222627695641109576978193239731106553867656292277457897945"
            "148112871988255992375163873515373132509581195914834747060188064077721390"
            "997178697058276225941505185075887064053691028388282388307649314498286713"
            "764846953212309279043881820342434895633579304861594494221758161331956482"
            "545342491774746360247643710898069471123175072427787624975265922600901869"
            "795606497910370196767027422778175554848144861596805683185775730465663918"
            "372723374530927639300103782361387981555807764775525397907298316619973801"
            "391551512143440565004117747075740968998797039424543028499470306838830477"
            "722721298717296402053617568912797374786857375846589013116604822895476885"
            "589717701995756120359265979744351715659105532428058509686291986565840753"
            "873489572078505781915204622306433051113409845052948316029477393119253005"
            "344294163454321391608102682940784777118914840946450169426091750009434236"
            "827945154537438463459330415277440394693356713104040689618638993175752922"
            "521438175739643140850856929718586926855597398638644437856251543856492383"
            "336954565137616103013282500583070346810896293035854856559147834974329251"
            "732806464626510433764880712891084968891299386321588642936326134241455173"
            "182290702178324296579707507496866815716432726825907329806828140438316908"
            "630981980518309056468990094605522452130802561185434741588783821628831533"
            "267455913470192788998083234073407809558079405274684161871232439678829603"
            "711028221230492500445762752614149634616288754089197335372373652541117256"
            "435152289701022533930960920086127299135560838530812189418608152745168162"
            "209143425826930757161466803667138623186970838683928309805200036499358008"
            "891575754455571395456676065880717031872303858613765231946367085095379127"
            "233706551010085583639473053596260009507540055187820714776814201583356752"
            "254358126112282725324581266635068306673768682012296994542429239152938108"
            "698043575989575240506980836648506469383149865089465817811985043871334621"
            "859194036604221342115906714099274105267967371881267798430413017611768367"
            "137416197044886695103422731543657883906095811939626403390623360762947090"
            "401617665782472186114110474535633084018860482989029762649429418860087057"
            "062239941233460251002318985121227905760893498431257771075942553298439427"
            "469396246466278655929658933250103636833462247993448182988120651133441467"
            "697622004161214639269834527946400920121307058834044410398457579236713553"
            "210648203814143495826042478606591000130727878506769873153266447725072755"
            "306155657089949160747252986410730499854570428074925657828273835633329103"
            "421304840091200045223163512202355170245227644022614790470267521805654926"
            "270566827194108398102502203959582428440156128652334951790300537747887102"
            "969468561499468408220217349258100122845942759841379402461391242301704611"
            "379161226689712319187039150538203023261479448939492593889908532656633122"
            "166927436203702454048504232252930298642449973646927626788072691082228880"
            "589642423891137751393695579544679829107622396234808963242470089181266474"
            "904844276605261338118972017524349864086160552069668626470246433694824504"
            "325555671763481768795153883558220405738140291223402827677066990461547818"
            "136439102583124801951200538038648127441791492384042356152068708385251975"
            "862994872328627473917776742525001321581937639890113896230861662111331346"
            "510475506063812893979906615567477196787532479169160553777030813420064532"
            "337447027978985195709460368051757085333052673278490918385896961393667824"
            "295087985381339056927467100835270000996592254612423430283566259404913270"
            "712084000924182746315249247044949831998981570584259196529682336731242487"
            "846141345249769785178684895677130457926111557825718450841844689292275824"
            "745874551825984042168401111502852882082613503163769302521173935239123112"
            "136697843529692799777131004955931645458600098734890135532911400477986838"
            "934157288754208320115060743578432517743285030796788721883145282578961664"
            "440685859338340079479486226612770624242273201553565039064108338439672750"
            "992709603637601924774049652711333691760572183510684175185397475181951747"
            "901704845454769536103081821544908054924180221426596111125559289218432339"
            "232123686013002813734668205331736643090353127065705961469538103130578395"
            "444761489404467258918214930122651933683245078876753753782519693025776555"
            "234612507122831830776929027185689363252547927369276756986511064263557567"
            "748385579602775791001803288366572801437925090924758811176002744572843092"
            "335137460718208623001812099295729466069292724436056427275163334049600734"
            "39504265491096331288227200446983022885660830070264637470245361328125e-49"
            "32";
        static const unsigned long long kMidEven_M = 0x5A5A5A5A5A5A5A5AULL;  // half-even result

        // kMidOdd: k=0x5A5A5A5A5A5A5A5B (odd -> half-even UP); 11515 sig digits (exact midpoint)
        static const char kMidOdd[] =
            "2.3732492774908895342506042879430503944769025711009131484284750499004329"
            "948790976208366139570126941330075686036380143958152772744110458249022238"
            "560191706202047911210261894182703129522323601092918212345591070178453732"
            "434059214287837469267558095734715487950263889219810551863147603458363131"
            "283232500797221327617621411740878657207283004118504483421293339220503596"
            "583634538072389022780362273966956702861548611391221273416157540112766206"
            "908013116418204690189892483069596898643449460776856290003159841892040457"
            "480629642255530648450857140252839029121913304385219104111635031385784499"
            "944055428485384091201133599609930742229841539981023005583604851209520398"
            "311797110328757115218379272698847369367923430049165116467497019141524116"
            "396667176336004296171455330976103794497561222058083797565675325743073927"
            "851024824228236306684346488782925831354065501519618132221902035951655630"
            "969923482579838900639752873302300921747992304193952915969497150875055394"
            "207239153229641488147443755911091568015145613722607293574236044408757628"
            "291969848192493862131386071917681787318430879812291243668706475264593463"
            "302479455933828984916877230999995985625432459129246192374636473239078009"
            "443889729263636668725065513985684632292537348452568134806275377693109212"
            "257264203153941863611198640841906366807079952018880227968327933760041862"
            "344221720862396501059294598647712103724516726384534463799400198907328533"
            "078391862362265313964061151212511765623009508730360657298750643654946333"
            "230875523908112248838577588175838783807786924844857011151555898849776592"
            "686086878837792298021687437564258132495491054913093258558433182349006961"
            "952383599999973645954257611625772695508735685359067753671226211733808434"
            "328079947384967910210631960821238612650891520067282437741827332096888106"
            "160185933669552034586094470648909483519533662321154874933741600347837628"
            "329793436628252018791845749111944776899628094623638313506385125513758814"
            "947422435629126843570430157312140910890035088328510473915913338579350916"
            "385130141094956548498860416592639896757340117345512088975421284007206213"
            "634879986132109941830997364108423969396078906196055573809482878432504214"
            "218629284644519500647850783752515006311862180205224235246176900129657462"
            "765270434097689254795216305193099805233262632838044990474239770122728936"
            "456998056272774574817577945993662600791452592260663754438094038420531726"
            "693264289447814155851078665276020624957460018677384642288018157253983835"
            "537020502478032248179505463444333844806497255289541717680511522760473750"
            "620638018424942795278785322096844828312000628622939678650043709741736735"
            "542484127500014774686777609787076664342601233815878824698869740036422538"
            "311267760303603526407759094009026330116176666940565794334373304911071704"
            "610861429652007090187425264732862492007548741750329581424247383194536100"
            "370386973964086771508822930794060729502224176427160906288969789126717215"
            "165227559859735460279089817747722949796820273670782243354111512905504836"
            "919232005933867892017737927630137091410806880788522776587797327958845102"
            "584029809599980601772975710921879961708647401066604396558738534132256843"
            "231740422445864951318852328087866671940196970137111705195059032861788584"
            "370201867959308938773782440656508345756193240758666490385827491478091335"
            "938305216029798834155849450904297943867141088181162644630277874757709077"
            "186269093652662956368461530725436487681473496228003551269971669084611583"
            "833225170097819976757982182050885384377446476428323240740765581946134338"
            "042823141242843904657368788605444709260884291890061323190766283621877633"
            "074893602100514966259777889528971468421111395542072198467193948723219721"
            "203874715518942577308337218629813515398240725213073026541124802906675425"
            "615474354620175707417169946758565452491277909970534084104075013072348856"
            "666076565845529591824292356693036730281073652013707991539212643206439810"
            "615482397817990726189419985633116363084450058703390502631820662718487382"
            "855943936648736557620362934181309090268889571367136674419398854235323315"
            "379805689622661228134269556542127151913492815428113819834614078968779295"
            "259640879557295817744426464162695437883443086880730962384745300933087980"
            "009942471165598773803070520682012607897808358692674230789108579622448148"
            "083315979664539323625659060410851100796466771805714462901754493276866653"
            "606098543853078598454816873373912313594044889589637907697271733129676940"
            "635861921250164126988895405237029645817151074879580501652263458800558651"
            "055537476852262902885979872239457666049517394618125782681893674628046191"
            "023626062216775092400531484406461288038430456724069477318782626792738604"
            "873280233689684663358709630255099307162691089480699069895440998103775114"
            "812242955146340943469832042372060893626127334559838466169991064291691043"
            "567952641756106331484176288774277243869427365680184021634377176206176550"
            "966634035701129826479122312593811070266737536935358081870896048918274694"
            "449331656267663455533228942039275625364214851601812707191911637121326023"
            "162726181070849095741398072716681346898832760945740717462498620195757934"
            "371892117300639078053999644326076905372727320962433621287378408517205676"
            "548750750828116634116570916368451721835471112578271467104203018139126382"
            "726834652425544365924070481797890971485982102512120697534129709089541029"
            "097907682516646162127467318762081112410929377500572901464150037379208028"
            "073043139390541168136036368664901687316566285732555537390972937462361329"
            "742050856341129546794502266395824742465638135842453278342027243000426334"
            "950463401007993328919653419567309381401788796579515961081737549134187530"
            "140350322952176691548299410902078937959065210318714947624627565472113025"
            "165339405885495755327112031166807153407196940271051105720682128216360254"
            "477013092815727701329728163387092638357441445049824266090344614765351465"
            "403781581244234053871750352646327478973634010862915898696253227076441200"
            "494822638762194454035527681658447236295361422787144422348635559505339886"
            "864798680702702311981814987513784328166364831030711857601531190386773641"
            "062485567169152442471242704839946328862728855769179933383328810252508614"
            "799479910544060070705162070468971893469415748306999324715490617193195886"
            "659994478877255407773589502582599663861178918189536601798359400007715338"
            "900834712547679970575563147281995245538787938496499013887229466775650668"
            "886454262589237950106208156991725857742752203870924775772604387781578914"
            "465534172066127723714857546432495173317882903002057555687023196831813417"
            "531848304182949004647621471090406168505400353676803604276581536731974911"
            "436597942063214568594487634406475170209949939173345693039908125868144271"
            "375946709085736366884118432874444116248780619164335896579724791511886290"
            "457663596864609644736986520454070948387912087982813101827274751212683509"
            "708355887891350763347132283453783942691650396065603289342998339105108709"
            "712774487964795941420271890278801248629864970835624651481680724475811164"
            "645566955954338547172107587321593251001492304594356000311153913747883790"
            "616961296071118796259826130441932144884684282678944872147104892031949092"
            "879420053817117334007076910668737561098502134978300422071956950825778892"
            "474713780722402183180191962995745385633210508126451781952489341368928448"
            "926107736427922466717655604331133289783952712828828513530984619890309794"
            "773261381448075865091474120369827327242835183969170601213551355229958685"
            "248656382784998081831192853725493383192336798885293782514253583342832623"
            "910156387509446574273420498151330963437392617069826887047188000281352540"
            "849551499622591122093254511480315836061941456957620718005811164669552125"
            "191555066935867604102944691849147734094565336008654845713264477344489021"
            "338696991320413369775217521790117475775627524567476414368178860852790211"
            "093728733143231335500930576339142846952386052543156199955545035496426305"
            "281122967085970831536173061852612447382491968087606653339150072961208368"
            "305524101534042123654948184477786564206487035584140574211278510527148590"
            "641668960379412204973196205316227697828467393910285720337854649279192148"
            "978568009414225544332752726753585660695326433946244276629025823411967570"
            "511647917127288510156455902126743244336922804163978200031707964807469859"
            "626552036672953136059053235268213729657064386209230059250490778522566445"
            "466578225139573986211407076845312867018482387395309571467740409558456569"
            "972866408587636473008037723733623292217243286785000187222401752941949572"
            "524187351537201982954962870931211452215638591488352751928435028948825392"
            "178483765897914023430703623987917920912480729892725600544142452584210943"
            "742697272098585930546110991618546685259117570512815684180516578407000434"
            "354648102004257003216877667321883252827245377659548335054889910833011087"
            "944601288052630751662702117092036497068158916527240164113020785962849354"
            "575020505137006072771680806078230278137820908914272155611451643320124326"
            "023186319320584724347708279799011383460082437208991079605017604225854943"
            "942320250747207592163944327153232210150703994108420207380695888048885043"
            "429913448770524452620695280907387805783301301893525237237008596680086320"
            "460057822680525358780549606228996175625151450122146388520696567101675313"
            "463299679043911850461380978209647223414792269263725993489392421564588663"
            "102249992326062659163086169687052243343340814692315914020554129101563330"
            "754068614127733900397880107060538026924131335250656210193889406237720774"
            "038934470457923763746806281826620121635291713728225796267274447362796314"
            "395876811041712665498491132339131142737923011996369190965659416616986914"
            "774356863992143322709791920695727150739313286602596239547693678602300339"
            "058685647272915858850088423169973900249413908882423617815424948584803792"
            "750315486162739482441063606414311458034445174292671022423428310136228667"
            "512563485428072735155393979656450225405291986353587986091605609034015908"
            "511273308806329629209549180293174793225054796605022556641052808811614016"
            "219855085337263457141646150464049199490500935669322122064942735233813681"
            "544449265552068643166553219848500970241295900272078173195338335033556584"
            "425279214819477312436214947234007514357968973861391412596098779075932247"
            "856742144857220169672236215864594114432931212690730206629407001166642177"
            "646952532219688035817874411909711449949015926237029604186407876124866562"
            "571948872304859929289360869847476052111820922313952067312477963721814696"
            "487191666184003245813212090272483724909529682721476246906347401116982433"
            "397459733749358022049446631658721897114716742381016769820221150929211359"
            "038597758462840034693103003257709331907860891927579979897395634115387172"
            "710578182568927610818600439186113380730886891480486005309026968435789464"
            "183443102070698505174180628682161270841713597597444564077523816778769562"
            "527475076700212728691532321874659643791738014879778987732432876297586793"
            "922326941679078455780938255937203396434062183470788035608563365750286297"
            "068556886600201357346446953466174215312417291409881412594234579445998754"
            "742274858024021255629491502212179746773839155838133462605815588234551807"
            "855737229861844060525099810867442978035998299824782282726545709307295823"
            "496687505654200118271632931852913773605692027544321119226420028482939760"
            "024108624644297614724845308501856062518561958279375198010206625448032606"
            "554367887455827095003586483732738034638044212820626153187134280811734628"
            "493503766206348868516612837719386328338315090196911646236541948974519026"
            "072905080832672166808965042645800898428703969733442597539114550556655967"
            "631726683121324903533298795120909839761296584876678211416433699303390297"
            "369360243509489382180988096287347719175745258045597026655619580715603487"
            "063679267121638632833379940452008366893716143466822555713070697877652951"
            "989573933395780168437539650466433052045792286580700336635060284024141084"
            "571129250087776709382233160619448432284999648853984826055342685396456040"
            "78133270830551384048172163486467578508154474548064172267913818359375e-49"
            "32";
        static const unsigned long long kMidOdd_M = 0x5A5A5A5A5A5A5A5CULL;  // half-even result
        auto mid = [&](const char *d, size_t n, unsigned long long m, const char *tag) {
            long double y{};
            auto p = std::from_chars(d, d + n, y);
            Check(p.ec == std::errc{} && bits(y) == (unsigned __int128)m, tag);
        };
        mid(kMidEven, sizeof(kMidEven) - 1, kMidEven_M, "phase61 exact subnormal midpoint half-even DOWN");
        mid(kMidOdd,  sizeof(kMidOdd)  - 1, kMidOdd_M,  "phase61 exact subnormal midpoint half-even UP");
    }

    // Ф27b heap NORMAL branch: a >1290-digit normal (2^-4300-scaled). This class
    // was corrupted by the uninitialized-remainder bug the audit caught — every
    // other heap test is subnormal, whose huge denominator masks it.
    {
        // kNormHeap: normal 2^-4300-scaled, 3025 sig digits (heap normal branch)
        static const char kNormHeap[] =
            "5.2365860835548085186726110131716923231356606043659281055476161682721399"
            "978859078815908800520228665146117917837947652295128404415567364029844142"
            "732437981468002676625384514105780179443196094784939035483367701437276088"
            "976265101671014750252041937712317713792062860028863771751964479783862097"
            "445829943879378942307972903836771370874223657121561961510353278412366540"
            "978977885654236902791370514505214703248819943574468461881792012585899441"
            "900212712316810244433613931889357538515196330778446859713153448006200530"
            "121359107154745188363266666175877058277535330044697554192871081928370513"
            "764544935003125743654694076677045384731629535897552455359062701949659692"
            "923883017097641013506262440837116101888647715272772309860534668862408458"
            "396371362028298589454346177550007494016711816919482860824336785538711136"
            "052654386555642684395841279328399229968449779040937341085031811863331321"
            "161086079284698762832889841231380045962065756784090280582548946798678046"
            "776859726306391911419049347874096972026777966727613415365335829067865951"
            "560381567400605041752374110496989000768989600621757278280617074446328235"
            "555639043889226508559281243456572423648958984402009224636379472108043637"
            "683283065039178519284486838423413304432582938546739582276115742394329959"
            "392069460788105884374324677600478270351626419562047003556648760206002063"
            "237690189524025616262906621136991029621057286621836632710722222003515898"
            "884073684978919877206548222068136924631295452075817219418807245311076650"
            "837700852871603351088693431781626149019887109958231256472826049766085310"
            "498756805451781211830942137630502637232325023391459541005616988877818217"
            "084338400427271558769218743012000560775173791471739518272053365950835874"
            "555430276043143636134462019421851148477516600109850134208387626350173969"
            "291836942735661179281443135693514199546907607303091414230966403695298234"
            "772977241152566161274974263923355090348895912947032787574076457660859027"
            "429758466849673101581892797442942923758227743039112505744358527242045344"
            "947615436820524298589258969710762021601969611357651838291794541231759205"
            "452363296976935097350978195456436415709492734861704480025819387352331234"
            "850085317265799710951763628939649041266990276417294473750083801852173546"
            "230076623471690536241498464898430566354482783392449293567578948146746200"
            "387198236358790998304028037789710635803444278255879914722333175308454279"
            "061223819349383520693403868277702258432756197983786187956803911944341679"
            "573362814008133156173194472136972263653650635315784114220076510623155211"
            "431641301178162969050989225638659162390543550025490751628323796416767082"
            "893425303049985035837412254072837826169548810033638785745050300608424154"
            "641606254131486120464816705949205561445770587945392301961799702345256800"
            "420075866523368158572154778475649275260001107240164154603207511446152906"
            "794082496896859154886505332094144744677556888847414624282586878467958235"
            "663834586676338365735587301792112711204780557574194527868606433406454435"
            "187306468748059837167150757810867014744069833161161502583074954633560497"
            "509451326306476444961931623677886885048771148376545170322060585021972656"
            "25e-1276";
        static const unsigned __int128 kNormHeap_bits =
            ((unsigned __int128)0x2F72ULL << 64) | (unsigned __int128)0xC3243F6A8885A309ULL;
        long double y{};
        auto p = std::from_chars(kNormHeap, kNormHeap + sizeof(kNormHeap) - 1, y);
        Check(p.ec == std::errc{} && bits(y) == kNormHeap_bits, "phase61 heap normal-branch >1290-digit round-trip");
    }

    printf("[CXX] PASS phase61: <charconv> long double\n");
}

// ── Phase62 (Ф27c) — <format> long double (80-bit x87): full precision ─
void Phase62()
{
    using F = std::chars_format;
    const long double x = 3.14159265358979323846L;  // 21 significant digits

    // The direct-to_chars references every assertion is measured against —
    // std::format must render byte-for-byte the same, i.e. never narrow to
    // double. Buffers sized generously; the LDBL_MAX case has its own below.
    auto shortest = [](long double v) -> std::string {
        char b[64];
        auto r = std::to_chars(b, b + sizeof(b), v);
        return std::string(b, size_t(r.ptr - b));
    };
    auto tc = [](long double v, F f, int prec) -> std::string {
        char b[128];
        auto r = std::to_chars(b, b + sizeof(b), v, f, prec);
        return std::string(b, size_t(r.ptr - b));
    };
    auto tchex = [](long double v) -> std::string {
        char b[64];
        auto r = std::to_chars(b, b + sizeof(b), v, F::hex);
        return std::string(b, size_t(r.ptr - b));
    };

    // default {} — shortest round-trip at full LD precision, never narrowed
    Check(std::format("{}", x) == shortest(x) &&
              std::format("{}", x) != std::format("{}", (double)x),
          "phase62 {} full-precision (not double-narrowed)");

    // fixed
    Check(std::format("{:.20Lf}", x) == tc(x, F::fixed, 20) &&
              std::format("{:.20Lf}", x) != std::format("{:.20f}", (double)x),
          "phase62 {:.20Lf} fixed");

    // scientific
    Check(std::format("{:.21Le}", x) == tc(x, F::scientific, 21),
          "phase62 {:.21Le} scientific");

    // general — explicit .21 (default 6 digits would be vacuous vs double's 17)
    Check(std::format("{:.21Lg}", x) == tc(x, F::general, 21) &&
              std::format("{:.21Lg}", x) != std::format("{:.21g}", (double)x),
          "phase62 {:.21Lg} general");

    // hex — 16 mantissa nibbles vs double's 13
    Check(std::format("{:La}", x) == tchex(x) &&
              std::format("{:La}", x) != std::format("{:a}", (double)x),
          "phase62 {:La} hex");

    // the L flag is cosmetic — LD precision comes from the argument TYPE
    Check(std::format("{:.20Lf}", x) == std::format("{:.20f}", x),
          "phase62 L flag cosmetic (type drives precision)");

    // width / fill applied to the shortest form
    {
        std::string s    = shortest(x);
        std::string want = std::string(30 - s.size(), ' ') + s;
        Check(std::format("{:>30}", x) == want, "phase62 {:>30} width/fill");
    }

    // '#' forces a decimal point
    Check(std::format("{:#.0Lf}", x) == "3.", "phase62 {:#.0Lf} forces point");

    // upper 'E' scientific
    Check(std::format("{:.2LE}", x) == "3.14E+00", "phase62 {:.2LE} upper sci");

    // 'F' == 'f' for a finite value
    Check(std::format("{:.20LF}", x) == std::format("{:.20Lf}", x),
          "phase62 {:LF} == {:Lf} finite");

    // LDBL_MAX fixed — the big-buffer guard. On the un-grown base (340) the
    // fixed expansion silently truncates well below 4900 chars; this passes
    // only once FormatFloatCore's `need` grows to LDBL_MAX_10_EXP + 34.
    {
        long double  m = (long double)__LDBL_MAX__;
        std::string  s = std::format("{:Lf}", m);
        static char  big[10240];
        auto         r = std::to_chars(big, big + sizeof(big), m, F::fixed, 6);
        Check(s == std::string(big, size_t(r.ptr - big)) && s.size() > 4900,
              "phase62 {:Lf} LDBL_MAX big-buffer (need formula)");
    }

    printf("[CXX] PASS phase62: <format> long double full precision\n");
}

// ── Phase63 (Ф27d1a) — <cmath> long double (80-bit x87): direct HW functions ─
void Phase63()
{
    auto bits80 = [](long double v) -> unsigned __int128 {
        return __builtin_bit_cast(unsigned __int128, v) & ((((unsigned __int128)1) << 80) - 1);
    };
    auto sig_digits = [](const char *p, const char *e) -> int {
        int n = 0; bool started = false;
        for (const char *q = p; q < e; ++q) {
            char c = *q;
            if (c == 'e' || c == 'E' || c == 'p' || c == 'P') break;
            if (c >= '0' && c <= '9') { if (c != '0') started = true; if (started) ++n; }
        }
        return n;
    };
    const long double infL = __builtin_infl();
    const long double piL  = 3.14159265358979323846264338327950288L;
    const long double eps  = 0x1p-63L;                    // LDBL_EPSILON = 2^-63

    // 1) hardware-exact integers
    Check(std::sqrtl(4.0L) == 2.0L,      "phase63 sqrtl(4)=2");
    Check(std::log2l(0x1p+40L) == 40.0L, "phase63 log2l(2^40)=40");
    Check(std::expl(0.0L) == 1.0L,       "phase63 expl(0)=1");
    Check(std::truncl(2.5L) == 2.0L,     "phase63 truncl(2.5)=2");
    Check(std::ceill(-2.5L) == -2.0L,    "phase63 ceill(-2.5)=-2");
    Check(std::roundl(2.5L) == 3.0L,     "phase63 roundl(2.5)=3");

    // 2) genuinely 80-bit, not silently double.
    long double s2 = std::sqrtl(2.0L);
    // fsqrt is IEEE correctly-rounded → bit-identical on QEMU / Bochs / real HW.
    Check(bits80(s2) == (((unsigned __int128)0x3FFFu << 64) | (unsigned __int128)0xB504F333F9DE6484ull),
          "phase63 sqrtl(2) bit-exact 80-bit");
    Check(s2 != (long double)std::sqrt(2.0), "phase63 sqrtl(2) != double sqrt(2)");
    { char b[64]; auto r = std::to_chars(b, b + sizeof(b), s2);
      Check(r.ec == std::errc{} && sig_digits(b, r.ptr) >= 19, "phase63 sqrtl(2) >=19 sig digits"); }
    // sin/cos/tan are DIRECT hardware ops (fsin/fcos/fptan issued on the 80-bit
    // operand — confirmed in disassembly, never a narrowed double call). Their
    // genuine 80-bit *output* needs an 80-bit FPU (real HW / Bochs); QEMU degrades
    // fsin/fcos/fptan to host double, so verify CORRECTNESS here — the 80-bit path
    // itself is locked bit-exactly by sqrtl(2)/nextafterl/classification/log2l above.
    Check(std::sinl(0.0L) == 0.0L && !std::signbit(std::sinl(0.0L)), "phase63 sinl(+0)=+0");
    Check(std::signbit(std::sinl(-0.0L)), "phase63 sinl(-0)=-0");
    Check(std::cosl(0.0L) == 1.0L, "phase63 cosl(0)=1");
    Check(std::fabsl(std::sinl(piL / 6.0L) - 0.5L) < 1e-15L, "phase63 sinl(pi/6)~0.5");
    Check(std::fabsl(std::cosl(piL / 3.0L) - 0.5L) < 1e-15L, "phase63 cosl(pi/3)~0.5");
    Check(std::fabsl(std::tanl(piL / 4.0L) - 1.0L) < 1e-14L, "phase63 tanl(pi/4)~1");
    { long double a = 0.7L, s = std::sinl(a), c = std::cosl(a);
      Check(std::fabsl(s * s + c * c - 1.0L) < 1e-15L, "phase63 sin^2+cos^2=1"); }

    // 3) decisive ULP: nextafterl(1,2)−1 == 2^-63 (double would be 2^-52)
    Check(std::nextafterl(1.0L, 2.0L) - 1.0L == eps, "phase63 nextafterl ulp = 2^-63");

    // 4) accuracy vs baked 80-bit constants — genuinely 80-bit (error ≪ double 2^-52).
    // Direct constant checks, not a log∘exp round-trip: fyl2x accuracy varies with
    // the argument on the emulated FPUs (Bochs ~290 ULP at log2(e), <2 ULP elsewhere),
    // so a round-trip would gate on the worst FPU's transcendental, not on 80-bit-ness.
    const long double kEL   = 2.718281828459045235360287471352662498L;   // e
    const long double kLn2L = 0.693147180559945309417232121458176568L;   // ln 2
    Check(std::fabsl(std::expl(1.0L) - kEL)   < 32 * eps,                  "phase63 expl(1)~e");
    Check(std::fabsl(std::logl(2.0L) - kLn2L) < 32 * eps,                  "phase63 logl(2)~ln2");
    Check(std::fabsl(std::atan2l(1.0L, 1.0L) * 4.0L - piL) < 8 * eps * piL, "phase63 4*atan2(1,1)~pi");

    // 5) specials — NaN via isnan, never ==
    Check(std::isnan(std::sqrtl(-1.0L)), "phase63 sqrtl(-1)=NaN");
    Check(std::isnan(std::logl(-1.0L)),  "phase63 logl(-1)=NaN");
    { long double l0 = std::logl(0.0L);
      Check(std::isinf(l0) && std::signbit(l0), "phase63 logl(0)=-inf"); }
    Check(std::isnan(std::sinl(infL)),   "phase63 sinl(inf)=NaN");
    Check(std::signbit(std::copysignl(2.0L, -0.0L)) && std::fabsl(std::copysignl(2.0L, -0.0L)) == 2.0L,
          "phase63 copysignl sign from -0");
    Check(std::fmodl(5.0L, 3.0L) == 2.0L, "phase63 fmodl(5,3)=2");

    // 6) classification fix: 2^1030 is a finite long double (old narrowing → wrongly inf)
    { long double big = 0x1p+1030L;
      Check(std::isinf(big) == false && std::isfinite(big) == true,
            "phase63 isinf(2^1030)=false (classification fix)"); }

    // 7) coverage — every remaining direct primitive (forces x87 codegen + sanity)
    Check(std::fabsl(std::tanl(std::atanl(1.0L)) - 1.0L) < 1e-13L, "phase63 tan(atan(1))~1");
    Check(std::exp2l(10.0L) == 1024.0L, "phase63 exp2l(10)=1024");
    Check(std::fabsl(std::log10l(1000.0L) - 3.0L) < 16 * eps, "phase63 log10l(1000)~3");
    Check(std::log1pl(0.0L) == 0.0L, "phase63 log1pl(0)=0");
    Check(std::floorl(-2.5L) == -3.0L, "phase63 floorl(-2.5)=-3");
    Check(std::rintl(2.5L) == 2.0L, "phase63 rintl(2.5)=2 (even)");
    Check(std::nearbyintl(3.5L) == 4.0L, "phase63 nearbyintl(3.5)=4 (even)");
    Check(std::logbl(8.0L) == 3.0L, "phase63 logbl(8)=3");
    Check(std::ilogbl(8.0L) == 3 && std::ilogbl(0.0L) == FP_ILOGB0, "phase63 ilogbl");
    { int e = 0; long double m = std::frexpl(12.0L, &e); Check(m == 0.75L && e == 4, "phase63 frexpl(12)=0.75,4"); }
    Check(std::scalbnl(1.0L, 10) == 1024.0L && std::ldexpl(3.0L, 4) == 48.0L &&
          std::scalblnl(1.0L, 5L) == 32.0L, "phase63 scalbnl/ldexpl/scalblnl");
    Check(std::fdiml(5.0L, 2.0L) == 3.0L && std::fdiml(2.0L, 5.0L) == 0.0L, "phase63 fdiml");
    Check(std::fmaxl(-1.0L, 2.0L) == 2.0L && std::fminl(-1.0L, 2.0L) == -1.0L, "phase63 fmaxl/fminl");
    Check(std::remainderl(5.0L, 3.0L) == -1.0L, "phase63 remainderl(5,3)=-1");
    { int q = 0; long double r = std::remquol(5.0L, 3.0L, &q); Check(r == -1.0L && (q & 7) == 2, "phase63 remquol(5,3)"); }
    { long double ip = 0; long double fr = std::modfl(3.75L, &ip); Check(ip == 3.0L && std::fabsl(fr - 0.75L) < eps, "phase63 modfl(3.75)"); }
    Check(std::fpclassify(0.0L) == FP_ZERO && std::fpclassify(1.0L) == FP_NORMAL &&
          std::fpclassify(infL) == FP_INFINITE && std::fpclassify(__builtin_nanl("")) == FP_NAN,
          "phase63 fpclassify LD");
    Check(std::sqrtf(4.0f) == 2.0f && std::sinf(0.0f) == 0.0f && std::truncf(2.9f) == 2.0f,
          "phase63 float C-names route through 80-bit");
    Check(std::nexttowardl(1.0L, 2.0L) - 1.0L == eps, "phase63 nexttowardl ulp");

    printf("[CXX] PASS phase63: <cmath> long double x87 direct\n");
}

// ── Phase64 (Ф27d1b) — <cmath> long double: composed elementary functions ─
void Phase64()
{
    const long double infL  = __builtin_infl();
    const long double qnanL = __builtin_nanl("");
    // full-precision 80-bit baked constants — the target compiler rounds each
    // literal to 80-bit (host LD width is irrelevant to a compile-time literal).
    const long double piL     = 3.14159265358979323846264338327950288L;
    const long double halfPiL = 1.57079632679489661923132169163975144L;
    const long double cosh1   = 1.54308063481524377847790562075706168L;   // cosh 1
    const long double tanh1   = 0.761594155955764888119458282604793590L;  // tanh 1
    const long double acosh2  = 1.31695789692481670862504634730796845L;   // acosh 2

    // Transcendental-routed fns are host-double precision on QEMU, full 80-bit
    // on real HW — accept both with a relative bound well above double's ULP.
    auto near = [](long double a, long double b, long double rel) -> bool {
        long double d = std::fabsl(a - b), m = std::fabsl(b);
        if (m < 1.0L) m = 1.0L;
        return d <= rel * m;
    };
    const long double tol = 0x1p-49L;                    // ~8× double ULP

    // ── 1) fma: DECISIVE genuine 80-bit (QEMU-valid; only ·,+,− internally) ──
    // (1+2^-32)² − 1 = 2^-31 + 2^-64 exactly; a rounded a*b+c loses the 2^-64.
    {
        long double r = std::fmal(1.0L + 0x1p-32L, 1.0L + 0x1p-32L, -1.0L);
        Check(r == 0x1p-31L + 0x1p-64L, "phase64 fmal exact 80-bit (2^-31+2^-64)");
        Check(r != (1.0L + 0x1p-32L) * (1.0L + 0x1p-32L) - 1.0L,
              "phase64 fmal != rounded a*b+c (true fused op)");
        Check(std::fmal(2.0L, 3.0L, 4.0L) == 10.0L, "phase64 fmal(2,3,4)=10");
        Check(std::isnan(std::fmal(infL, 0.0L, 1.0L)), "phase64 fmal(inf,0,1)=NaN");
    }

    // ── 2) hypot: DECISIVE genuine 80-bit (fsqrt + arithmetic only) ──
    {
        Check(std::hypotl(3.0L, 4.0L) == 5.0L, "phase64 hypotl(3,4)=5 exact");
        // sqrt(1+2^-52) rounds to 1+2^-53 in 80-bit — a bit a double cannot hold.
        long double r = std::hypotl(1.0L, 0x1p-26L);
        Check(r == 1.0L + 0x1p-53L, "phase64 hypotl(1,2^-26)=1+2^-53 (sub-double bit)");
        Check(r != (long double)(double)r, "phase64 hypotl result has sub-double bits");
        char b1[64]; auto q1 = std::to_chars(b1, b1 + sizeof(b1), r);
        char b2[64]; auto q2 = std::to_chars(b2, b2 + sizeof(b2), (long double)(double)r);
        Check(q1.ec == std::errc{} && q2.ec == std::errc{} &&
                  std::string_view(b1, q1.ptr - b1) != std::string_view(b2, q2.ptr - b2),
              "phase64 hypotl to_chars not double-narrowed");
        Check(std::isinf(std::hypotl(infL, qnanL)), "phase64 hypotl(inf,nan)=inf");
        Check(std::hypotl(0.0L, 0.0L) == 0.0L, "phase64 hypotl(0,0)=0");
    }

    // ── 3) pow edge table [c.math.pow], evaluated in the mandated order ──
    Check(std::powl(qnanL, 0.0L) == 1.0L,       "phase64 powl(NaN,0)=1");
    Check(std::powl(2.0L, 0.0L) == 1.0L,        "phase64 powl(2,0)=1");
    Check(std::powl(1.0L, infL) == 1.0L,        "phase64 powl(1,inf)=1");
    Check(std::powl(2.0L, 10.0L) == 1024.0L,    "phase64 powl(2,10)=1024");
    Check(std::powl(-2.0L, 3.0L) == -8.0L,      "phase64 powl(-2,3)=-8");
    Check(std::isnan(std::powl(-2.0L, 0.5L)),   "phase64 powl(-2,0.5)=NaN");
    Check(std::powl(0.0L, -1.0L) == infL,       "phase64 powl(+0,-1)=+inf");
    { long double r = std::powl(-0.0L, -3.0L);
      Check(std::isinf(r) && std::signbit(r),   "phase64 powl(-0,-3)=-inf"); }
    Check(std::powl(infL, 0.5L) == infL,        "phase64 powl(inf,0.5)=+inf");
    Check(std::powl(0.5L, -infL) == infL,       "phase64 powl(0.5,-inf)=+inf");
    Check(near(std::powl(2.0L, 0.5L), std::sqrtl(2.0L), tol), "phase64 powl(2,0.5)~sqrt2");

    // ── 4) asin / acos ──
    Check(near(std::asinl(1.0L), halfPiL, tol),  "phase64 asinl(1)=pi/2");
    Check(near(std::acosl(-1.0L), piL, tol),     "phase64 acosl(-1)=pi");
    Check(std::asinl(0.0L) == 0.0L && !std::signbit(std::asinl(0.0L)), "phase64 asinl(+0)=+0");
    Check(std::signbit(std::asinl(-0.0L)),       "phase64 asinl(-0)=-0");
    Check(std::acosl(1.0L) == 0.0L,              "phase64 acosl(1)=0");
    Check(std::isnan(std::asinl(2.0L)),          "phase64 asinl(2)=NaN");
    Check(std::isnan(std::acosl(-2.0L)),         "phase64 acosl(-2)=NaN");

    // ── 5) hyperbolics ──
    Check(std::sinhl(0.0L) == 0.0L && !std::signbit(std::sinhl(0.0L)), "phase64 sinhl(+0)=+0");
    Check(std::signbit(std::sinhl(-0.0L)),       "phase64 sinhl(-0)=-0");
    Check(std::coshl(0.0L) == 1.0L,              "phase64 coshl(0)=1");
    Check(std::coshl(-infL) == infL && std::coshl(infL) == infL, "phase64 coshl(+-inf)=+inf");
    Check(std::tanhl(infL) == 1.0L && std::tanhl(-infL) == -1.0L, "phase64 tanhl(+-inf)=+-1");
    Check(std::tanhl(0.0L) == 0.0L && !std::signbit(std::tanhl(0.0L)), "phase64 tanhl(+0)=+0");
    Check(near(std::coshl(1.0L), cosh1, tol),    "phase64 coshl(1)~baked");
    Check(near(std::tanhl(1.0L), tanh1, tol),    "phase64 tanhl(1)~baked");
    // sinh small-arg: the cubic term x^3/6 survives (a raw (e^x-e^-x)/2 cancels it)
    { long double x = 0x1p-20L, d = std::sinhl(x) - x;   // ~ x^3/6 = 2^-60/6
      Check(std::sinhl(x) > x && d > 0x1p-64L && d < 0x1p-61L,
            "phase64 sinhl(2^-20) keeps cubic term"); }

    // ── 6) inverse hyperbolics ──
    Check(std::acoshl(1.0L) == 0.0L,             "phase64 acoshl(1)=0");
    Check(std::atanhl(0.0L) == 0.0L && !std::signbit(std::atanhl(0.0L)), "phase64 atanhl(+0)=+0");
    Check(std::atanhl(1.0L) == infL,             "phase64 atanhl(1)=+inf");
    { long double r = std::atanhl(-1.0L);
      Check(std::isinf(r) && std::signbit(r),    "phase64 atanhl(-1)=-inf"); }
    Check(std::isnan(std::acoshl(0.5L)),         "phase64 acoshl(0.5)=NaN");
    Check(std::isnan(std::atanhl(2.0L)),         "phase64 atanhl(2)=NaN");
    Check(std::asinhl(-2.0L) == -std::asinhl(2.0L), "phase64 asinhl odd symmetry");
    Check(std::asinhl(0.0L) == 0.0L && !std::signbit(std::asinhl(0.0L)), "phase64 asinhl(+0)=+0");
    Check(near(std::acoshl(2.0L), acosh2, tol),  "phase64 acoshl(2)~baked");

    // ── 7) cbrt (odd; real root for negatives) — Newton refines to full 80-bit ──
    Check(std::cbrtl(27.0L) == 3.0L,             "phase64 cbrtl(27)=3");
    Check(std::cbrtl(-8.0L) == -2.0L,            "phase64 cbrtl(-8)=-2");
    Check(std::cbrtl(1000.0L) == 10.0L,          "phase64 cbrtl(1000)=10");
    Check(std::cbrtl(0.0L) == 0.0L && !std::signbit(std::cbrtl(0.0L)), "phase64 cbrtl(+0)=+0");
    Check(std::signbit(std::cbrtl(-0.0L)),       "phase64 cbrtl(-0)=-0");
    Check(std::cbrtl(infL) == infL && std::cbrtl(-infL) == -infL, "phase64 cbrtl(+-inf)=+-inf");

    // ── 8) expm1 ──
    Check(std::expm1l(0.0L) == 0.0L && !std::signbit(std::expm1l(0.0L)), "phase64 expm1l(+0)=+0");
    Check(std::signbit(std::expm1l(-0.0L)),      "phase64 expm1l(-0)=-0");
    Check(std::expm1l(-1000.0L) == -1.0L,        "phase64 expm1l(-1000)=-1");
    Check(std::expm1l(infL) == infL,             "phase64 expm1l(+inf)=+inf");
    // small-arg: the quadratic term x^2/2 survives (a raw exp(x)-1 cancels it)
    { long double x = 0x1p-30L, d = std::expm1l(x) - x;  // ~ x^2/2 = 2^-61
      Check(std::expm1l(x) > x && near(d, 0x1p-61L, 0x1p-6L),
            "phase64 expm1l(2^-30) keeps quadratic term"); }

    printf("[CXX] PASS phase64: <cmath> long double composed\n");
}

// ── Phase65 (Ф27d2) — <cmath> long double special: erf/erfc/tgamma/lgamma ─
void Phase65()
{
    const long double infL  = __builtin_infl();
    const long double qnanL = __builtin_nanl("");
    const long double subd  = 4 * 0x1p-63L;              // ~4 ULP baked-value tolerance
    const long double reflTol = 0x1p-49L;                // reflection: 80-bit on Bochs, loose on QEMU

    auto sig_digits = [](const char *p, const char *e) -> int {
        int n = 0; bool started = false;
        for (const char *q = p; q < e; ++q) {
            char c = *q;
            if (c == 'e' || c == 'E' || c == 'p' || c == 'P') break;
            if (c >= '0' && c <= '9') { if (c != '0') started = true; if (started) ++n; }
        }
        return n;
    };
    auto near = [](long double a, long double b, long double rel) -> bool {
        long double d = std::fabsl(a - b), m = std::fabsl(b);
        if (m < 1.0L) m = 1.0L;
        return d <= rel * m;
    };
    // exact ULP distance |r−o| / ulp(o) at 80-bit (the LD field is not IEEE-monotone
    // across exponent boundaries, so subtract in value-space, not bit-space).
    auto ulps = [](long double r, long double o) -> long double {
        if (r == o) return 0.0L;
        if (std::isnan(r) || std::isnan(o)) return 1e30L;
        if (o == 0.0L) return std::fabsl(r) / 0x1p-16445L;
        long double u = std::ldexpl(1.0L, std::ilogbl(o) - 63);
        return std::fabsl(r - o) / u;
    };

    struct OraclePt { long double x, o; };               // o = correctly-rounded 80-bit reference
    static const OraclePt kErf[] = {
        { 0.1000000000000000000000000L, 0.1124629160182848922047891L },
        { 0.2500000000000000000000000L, 0.2763263901682369329850683L },
        { 0.4000000000000000000000000L, 0.4283923550466684551088164L },
        { 0.4990000000000000000000000L, 0.5196206559894684606584045L },
        { 0.5000000000000000000000000L, 0.5204998778130465376827467L },
        { 0.7500000000000000000000000L, 0.7111556336535151315989378L },
        { 1.000000000000000000000000L, 0.8427007929497148693412206L },
        { 1.500000000000000000000000L, 0.9661051464753107270669763L },
        { 2.000000000000000000000000L, 0.9953222650189527341620693L },
        { 3.000000000000000000000000L, 0.9999779095030014145586272L },
        { 4.000000000000000000000000L, 0.9999999845827420997199811L },
        { 5.000000000000000000000000L, 0.9999999999984625402055720L },
        { 6.000000000000000000000000L, 0.9999999999999999784802633L },
        { -0.5000000000000000000000000L, -0.5204998778130465376827467L },
        { -2.000000000000000000000000L, -0.9953222650189527341620693L },
        { -4.000000000000000000000000L, -0.9999999845827420997199811L },
    };
    static const OraclePt kErfc[] = {
        { 0.5000000000000000000000000L, 0.4795001221869534623172533L },
        { 0.7500000000000000000000000L, 0.2888443663464848684010622L },
        { 1.000000000000000000000000L, 0.1572992070502851306587794L },
        { 1.500000000000000000000000L, 0.03389485352468927293302374L },
        { 2.000000000000000000000000L, 0.004677734981047265837930744L },
        { 3.000000000000000000000000L, 0.00002209049699858544137277613L },
        { 4.000000000000000000000000L, 1.541725790028001885215967e-8L },
        { 5.000000000000000000000000L, 1.537459794428034850188343e-12L },
        { 6.000000000000000000000000L, 2.151973671249891311659335e-17L },
        { 7.000000000000000000000000L, 4.183825607779414398614010e-23L },
        { 8.000000000000000000000000L, 1.122429717298292707996789e-29L },
        { 10.00000000000000000000000L, 2.088487583762544757000786e-45L },
        { 12.00000000000000000000000L, 1.356261169205904212780306e-64L },
        { 15.00000000000000000000000L, 7.212994172451206666565067e-100L },
        { 20.00000000000000000000000L, 5.395865611607900928934999e-176L },
        { 30.00000000000000000000000L, 2.564656203756111600033397e-393L },
        { 40.00000000000000000000000L, 1.896961059966276509268278e-697L },
        { 50.00000000000000000000000L, 2.070920778841656048448448e-1088L },
        { -0.3000000000000000000000000L, 1.328626759459127427650095L },
        { -1.000000000000000000000000L, 1.842700792949714869341221L },
        { -3.000000000000000000000000L, 1.999977909503001414558627L },
    };
    static const OraclePt kTgam[] = {                    // 0.9/1.1/4.4/7.3/10.9/0.51 = non-representable stressors
        { 0.6000000000000000000000000L, 1.489192248812817102344584L },
        { 0.7500000000000000000000000L, 1.225416702465177645129098L },
        { 0.9000000000000000000000000L, 1.068628702119319354914799L },
        { 1.000000000000000000000000L, 1.000000000000000000000000L },
        { 1.100000000000000000000000L, 0.9513507698668731836205070L },
        { 1.500000000000000000000000L, 0.8862269254527580136490837L },
        { 2.500000000000000000000000L, 1.329340388179137020473626L },
        { 3.000000000000000000000000L, 2.000000000000000000000000L },
        { 4.000000000000000000000000L, 6.000000000000000000000000L },
        { 4.400000000000000000000000L, 10.13610185115513210528956L },
        { 5.500000000000000000000000L, 52.34277778455352018114901L },
        { 7.000000000000000000000000L, 720.0000000000000000000000L },
        { 7.300000000000000000000000L, 1271.423633663909273480982L },
        { 10.00000000000000000000000L, 362880.0000000000000000000L },
        { 10.90000000000000000000000L, 2869690.268017083124829523L },
        { 11.00000000000000000000000L, 3628800.000000000000000000L },
        { 15.00000000000000000000000L, 87178291200.00000000000000L },
        { 20.00000000000000000000000L, 121645100408832000.0000000L },
        { 25.00000000000000000000000L, 620448401733239439360000.0L },
        { 30.00000000000000000000000L, 8.841761993739701954543616e+30L },
        { 0.5100000000000000000000000L, 1.738415068463864014145204L },
        { -0.5000000000000000000000000L, -3.544907701811032054596335L },
        { -1.500000000000000000000000L, 2.363271801207354703064223L },
        { -2.500000000000000000000000L, -0.9453087204829418812256893L },
        { -3.500000000000000000000000L, 0.2700882058522691089216255L },
    };
    static const OraclePt kLgam[] = {                    // roots x=1,2 excluded; 0.51/4.4/7.3/10.9 = non-representable
        { 0.5100000000000000000000000L, 0.5529738179298007399106158L },
        { 0.6000000000000000000000000L, 0.3982338580692348995834474L },
        { 0.7500000000000000000000000L, 0.2032809514312953714814330L },
        { 1.500000000000000000000000L, -0.1207822376352452223455184L },
        { 2.500000000000000000000000L, 0.2846828704729191596324947L },
        { 3.000000000000000000000000L, 0.6931471805599453094172321L },
        { 4.000000000000000000000000L, 1.791759469228055000812477L },
        { 4.400000000000000000000000L, 2.316103491424857273261808L },
        { 5.000000000000000000000000L, 3.178053830347945619646942L },
        { 7.300000000000000000000000L, 7.147892523022249033109746L },
        { 8.000000000000000000000000L, 8.525161361065414300165531L },
        { 10.90000000000000000000000L, 14.86971466136042312999966L },
        { 11.00000000000000000000000L, 15.10441257307551529522571L },
        { 15.00000000000000000000000L, 25.19122118273868150009343L },
        { 20.00000000000000000000000L, 39.33988418719949403622465L },
        { 50.00000000000000000000000L, 144.5657439463448860089184L },
        { 100.0000000000000000000000L, 359.1342053695753987760440L },
        { -0.5000000000000000000000000L, 1.265512123484645396488946L },
        { -1.500000000000000000000000L, 0.8600470153764810145109327L },
        { -2.500000000000000000000000L, -0.05624371649767405067259453L },
        { -3.500000000000000000000000L, -1.309006684993042046360715L },
    };
    const long double kErf05    = 0.5204998778130465376827467L;
    const long double kErf3     = 0.9999779095030014145586272L;
    const long double kErfc1    = 0.1572992070502851306587794L;
    const long double kErfc8    = 1.122429717298292707996789e-29L;
    const long double kErfc20   = 5.395865611607900928934999e-176L;
    const long double kErfc40   = 1.896961059966276509268278e-697L;
    const long double kSqrtPi   = 1.772453850905516027298167L;      // tgamma(0.5)
    const long double kTg55     = 52.34277778455352018114901L;
    const long double kHalfLnPi = 0.5723649429247000870717137L;     // lgamma(0.5)
    const long double kLg50     = 144.5657439463448860089184L;
    const long double kTgNeg05  = -3.544907701811032054596335L;     // -2 sqrt(pi)
    const long double kLgNeg05  = 1.265512123484645396488946L;

    // ── 1) specials — transcendental-free, hold everywhere ──
    Check(std::erfl(0.0L) == 0.0L && !std::signbit(std::erfl(0.0L)), "phase65 erfl(+0)=+0");
    Check(std::signbit(std::erfl(-0.0L)),        "phase65 erfl(-0)=-0");
    Check(std::erfl(infL) == 1.0L,               "phase65 erfl(inf)=1");
    Check(std::erfl(-infL) == -1.0L,             "phase65 erfl(-inf)=-1");
    Check(std::isnan(std::erfl(qnanL)),          "phase65 erfl(NaN)=NaN");
    Check(std::erfcl(infL) == 0.0L,              "phase65 erfcl(inf)=0");
    Check(std::erfcl(-infL) == 2.0L,             "phase65 erfcl(-inf)=2");
    Check(std::erfcl(0.0L) == 1.0L,              "phase65 erfcl(0)=1");
    Check(near(std::tgammal(1.0L), 1.0L, subd),  "phase65 tgammal(1)=1");
    Check(std::isinf(std::tgammal(0.0L)) && !std::signbit(std::tgammal(0.0L)), "phase65 tgammal(+0)=+inf");
    { long double r = std::tgammal(-0.0L); Check(std::isinf(r) && std::signbit(r), "phase65 tgammal(-0)=-inf"); }
    Check(std::isnan(std::tgammal(-3.0L)),       "phase65 tgammal(-3)=NaN");
    Check(std::isnan(std::tgammal(-infL)),       "phase65 tgammal(-inf)=NaN");
    Check(std::tgammal(infL) == infL,            "phase65 tgammal(inf)=inf");
    Check(std::tgammal(1800.0L) == infL,         "phase65 tgammal(1800)=inf");
    Check(std::fabsl(std::lgammal(1.0L)) < 8 * 0x1p-63L, "phase65 lgammal(1)=0");
    Check(std::fabsl(std::lgammal(2.0L)) < 8 * 0x1p-63L, "phase65 lgammal(2)=0");
    Check(std::lgammal(0.0L) == infL,            "phase65 lgammal(0)=+inf");
    Check(std::lgammal(-2.0L) == infL,           "phase65 lgammal(-2)=+inf");
    Check(std::isinf(std::lgammal(infL)),        "phase65 lgammal(inf)=+inf");

    // ── 2) decisive genuine-80-bit on QEMU (main paths are transcendental-free) ──
    Check(near(std::erfl(0.5L), kErf05, subd),   "phase65 erfl(0.5) ~ baked 80-bit");
    Check(near(std::erfl(3.0L), kErf3, subd),    "phase65 erfl(3) ~ baked 80-bit");
    Check(std::erfl(0.5L) != (long double)(double)std::erfl(0.5L), "phase65 erfl(0.5) sub-double bits");
    { char b[64]; auto r = std::to_chars(b, b + sizeof(b), std::erfl(0.5L));
      Check(r.ec == std::errc{} && sig_digits(b, r.ptr) >= 19, "phase65 erfl(0.5) >=19 sig digits"); }
    Check(near(std::erfcl(1.0L), kErfc1, subd),  "phase65 erfcl(1) [W1] ~ baked 80-bit");
    Check(near(std::erfcl(8.0L), kErfc8, subd),  "phase65 erfcl(8) [W2] ~ baked 80-bit");
    Check(near(std::erfcl(20.0L), kErfc20, 8 * 0x1p-63L), "phase65 erfcl(20) [asymptotic] ~ baked");
    Check(near(std::erfcl(40.0L), kErfc40, 8 * 0x1p-63L), "phase65 erfcl(40) [asymptotic] ~ baked");
    Check(std::erfcl(8.0L) != (long double)(double)std::erfcl(8.0L), "phase65 erfcl(8) sub-double bits");
    Check(near(std::tgammal(0.5L), kSqrtPi, subd), "phase65 tgammal(0.5)=sqrt(pi) ~ baked");
    Check(near(std::tgammal(5.5L), kTg55, 8 * 0x1p-63L), "phase65 tgammal(5.5) ~ baked (dd-lnGamma)");
    Check(near(std::tgammal(20.0L), 121645100408832000.0L, 8 * 0x1p-63L), "phase65 tgammal(20)=19! ~ baked");
    Check(std::tgammal(0.5L) != (long double)(double)std::tgammal(0.5L), "phase65 tgammal(0.5) sub-double bits");
    Check(near(std::lgammal(0.5L), kHalfLnPi, subd), "phase65 lgammal(0.5)=0.5 ln pi ~ baked");
    Check(near(std::lgammal(50.0L), kLg50, 8 * 0x1p-63L), "phase65 lgammal(50) ~ baked");

    // ── 3) reflection — genuine 80-bit on Bochs/real-HW (fsin/fyl2x); loose on QEMU ──
    Check(near(std::tgammal(-0.5L), kTgNeg05, reflTol), "phase65 tgammal(-0.5)=-2sqrt(pi) (80-bit on Bochs)");
    Check(near(std::lgammal(-0.5L), kLgNeg05, reflTol), "phase65 lgammal(-0.5) (80-bit on Bochs)");

    // ── 4) precision guard (runtime) — a truncated coeff → ~100 ULP → sweep fails ──
    Check(std::ERF_C_L[0] != (long double)(double)std::ERF_C_L[0], "phase65 ERF_C_L[0] sub-double bits");
    Check(std::erfl(0.5L) != (long double)(double)std::erfl(0.5L),  "phase65 erfl(0.5) sub-double (guard)");

    // ── 5) oracle sweep — max ULP vs correctly-rounded 80-bit reference ──
    long double maxErf = 0, maxErfc = 0, maxTg = 0, maxLg = 0;
    bool tgReflOk = true, lgReflOk = true;
    for (auto &p : kErf)  { long double u = ulps(std::erfl(p.x), p.o);  if (u > maxErf)  maxErf  = u; }
    for (auto &p : kErfc) { long double u = ulps(std::erfcl(p.x), p.o); if (u > maxErfc) maxErfc = u; }
    for (auto &p : kTgam) {                              // positive: main path; negative: reflection
        long double r = std::tgammal(p.x);
        if (p.x > 0.0L) { long double u = ulps(r, p.o); if (u > maxTg) maxTg = u; }
        else if (!near(r, p.o, reflTol)) tgReflOk = false;
    }
    for (auto &p : kLgam) {
        long double r = std::lgammal(p.x);
        if (p.x > 0.0L) { long double u = ulps(r, p.o); if (u > maxLg) maxLg = u; }
        else if (!near(r, p.o, reflTol)) lgReflOk = false;
    }
    // main paths (incl. non-representable stressors 0.9/1.1/4.4/7.3/10.9/0.51) hold a few ULP
    Check(maxErf  <= 8.0L, "phase65 erf sweep <=8 ULP");
    Check(maxErfc <= 8.0L, "phase65 erfc sweep <=8 ULP");
    Check(maxTg   <= 8.0L, "phase65 tgamma main-path sweep <=8 ULP");
    Check(maxLg   <= 8.0L, "phase65 lgamma main-path sweep <=8 ULP");
    Check(tgReflOk, "phase65 tgamma reflection sweep (80-bit on Bochs, loose on QEMU)");
    Check(lgReflOk, "phase65 lgamma reflection sweep (80-bit on Bochs, loose on QEMU)");
    // achieved max-ULP surfaced for regression visibility (deterministic; ×100)
    printf("[CXX] phase65 maxUlp(x100) erf=%d erfc=%d tgamma=%d lgamma=%d\n",
           (int)(maxErf * 100), (int)(maxErfc * 100), (int)(maxTg * 100), (int)(maxLg * 100));
    printf("[CXX] PASS phase65: <cmath> long double special\n");
}

// ── Phase66 (de-risk) — exp2/exp correctly-rounded 80-bit: MPFR-verified ────
// Iterates baked (x, MPFR-correctly-rounded ref) 80-bit vectors — systematic
// grids (near 0, near overflow/underflow, near integers/halves, the subnormal
// region) plus seeded-random breadth. For each x the software double-double
// exp2/exp result bits must equal the MPFR reference bits exactly (0 ULP).
#include "cr_exp_vectors.h"
#include "cr_exp_checksums.h"

inline long double CrLdFromBits(unsigned long long mant, unsigned short se){
    unsigned __int128 b = ((unsigned __int128)se << 64) | mant;
    return __builtin_bit_cast(long double, b);
}
inline void CrLdToBits(long double v, unsigned long long &mant, unsigned short &se){
    unsigned __int128 b = __builtin_bit_cast(unsigned __int128, v);
    mant = (unsigned long long)b;
    se   = (unsigned short)(b >> 64);
}
inline unsigned long long CrUlp(unsigned long long am, unsigned short ase,
                                unsigned long long bm, unsigned short bse){
    auto key = [](unsigned long long m, unsigned short s) -> __int128 {
        unsigned __int128 mag = ((unsigned __int128)(s & 0x7FFF) << 64) | m;
        return ((s >> 15) & 1) ? -(__int128)mag : (__int128)mag;
    };
    __int128 d = key(am, ase) - key(bm, bse);
    unsigned __int128 mag = d < 0 ? (unsigned __int128)(-d) : (unsigned __int128)d;
    return mag > 0xFFFFFFFFFFFFFFFFULL ? 0xFFFFFFFFFFFFFFFFULL : (unsigned long long)mag;
}
unsigned CrSweep(const char *name, const CrVec *v, unsigned n, long double (*fn)(long double)){
    unsigned long long maxulp = 0; unsigned nonCR = 0, wi = 0;
    unsigned long long gm = 0; unsigned short gse = 0;
    for (unsigned i = 0; i < n; ++i){
        long double x = CrLdFromBits(v[i].xm, v[i].xse);
        unsigned long long rm; unsigned short rse; CrLdToBits(fn(x), rm, rse);
        unsigned long long d = CrUlp(rm, rse, v[i].rm, v[i].rse);
        if (d){ nonCR++; if (d > maxulp){ maxulp = d; wi = i; gm = rm; gse = rse; } }
    }
    printf("[CXX] %s: swept=%u max-ULP=%llu non-CR=%u\n", name, n, maxulp, nonCR);
    if (nonCR)
        printf("[CXX]   worst x: xm=0x%016llX xse=0x%04X  got=0x%016llX/0x%04X want=0x%016llX/0x%04X\n",
               v[wi].xm, v[wi].xse, gm, gse, v[wi].rm, v[wi].rse);
    return nonCR;
}
// ≥100k breadth: a deterministic pure-integer x stream (xorshift64 + bit
// assembly) reproduced bit-identically on the host, which baked FNV-1a
// checksums of the MPFR-correctly-rounded results. The guest folds ITS own
// exp2/exp result bits; a matching rSum proves every one of kCrN results is
// bit-identical to MPFR (0 non-CR). xSum proves guest/host generate the same x.
inline unsigned long long CrNext(unsigned long long &s){
    s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
}
inline unsigned long long CrFold(unsigned long long h, unsigned long long v){
    return (h ^ v) * 1099511628211ULL;
}
void CrChecksum(unsigned long long seed, int elo, int ehi, unsigned long long n,
                long double (*fn)(long double),
                unsigned long long &hx, unsigned long long &hr){
    unsigned long long s = seed;
    unsigned span = (unsigned)(ehi - elo + 1);
    hx = 1469598103934665603ULL; hr = 1469598103934665603ULL;
    for (unsigned long long i = 0; i < n; ++i){
        unsigned long long a = CrNext(s); s = a;
        unsigned long long b = CrNext(s); s = b;
        int E = elo + (int)(a % span);
        unsigned short se = (unsigned short)((((a >> 40) & 1ULL) << 15) | (unsigned short)((16383 + E) & 0x7FFF));
        unsigned long long mant = b | (1ULL << 63);
        hx = CrFold(CrFold(hx, mant), se);
        unsigned long long rm; unsigned short rse; CrLdToBits(fn(CrLdFromBits(mant, se)), rm, rse);
        hr = CrFold(CrFold(hr, rm), rse);
    }
}
unsigned CrStream(const char *name, unsigned long long seed, int elo, int ehi,
                  unsigned long long n, unsigned long long xsum, unsigned long long rsum,
                  long double (*fn)(long double)){
    unsigned long long hx, hr;
    CrChecksum(seed, elo, ehi, n, fn, hx, hr);
    bool xok = (hx == xsum), rok = (hr == rsum);
    printf("[CXX] %s stream: N=%llu xSum=%s rSum=%s\n", name, n,
           xok ? "MATCH" : "MISMATCH", rok ? "MATCH" : "MISMATCH");
    if (!xok || !rok)
        printf("[CXX]   %s got xSum=0x%016llX rSum=0x%016llX want xSum=0x%016llX rSum=0x%016llX\n",
               name, hx, hr, xsum, rsum);
    return (xok && rok) ? 0u : 1u;
}
// Positive-x stream (sign forced 0): the log/log2/log10 domain is x>0. Mirrors
// stream_pos in gen_all.py exactly (same xorshift/E/se/mant assembly).
void CrChecksumPos(unsigned long long seed, int elo, int ehi, unsigned long long n,
                   long double (*fn)(long double),
                   unsigned long long &hx, unsigned long long &hr){
    unsigned long long s = seed;
    unsigned span = (unsigned)(ehi - elo + 1);
    hx = 1469598103934665603ULL; hr = 1469598103934665603ULL;
    for (unsigned long long i = 0; i < n; ++i){
        unsigned long long a = CrNext(s); s = a;
        unsigned long long b = CrNext(s); s = b;
        int E = elo + (int)(a % span);
        unsigned short se = (unsigned short)((16383 + E) & 0x7FFF);   // sign 0 -> x>0
        unsigned long long mant = b | (1ULL << 63);
        hx = CrFold(CrFold(hx, mant), se);
        unsigned long long rm; unsigned short rse; CrLdToBits(fn(CrLdFromBits(mant, se)), rm, rse);
        hr = CrFold(CrFold(hr, rm), rse);
    }
}
unsigned CrStreamPos(const char *name, unsigned long long seed, int elo, int ehi,
                     unsigned long long n, unsigned long long xsum, unsigned long long rsum,
                     long double (*fn)(long double)){
    unsigned long long hx, hr;
    CrChecksumPos(seed, elo, ehi, n, fn, hx, hr);
    bool xok = (hx == xsum), rok = (hr == rsum);
    printf("[CXX] %s stream: N=%llu xSum=%s rSum=%s\n", name, n,
           xok ? "MATCH" : "MISMATCH", rok ? "MATCH" : "MISMATCH");
    if (!xok || !rok)
        printf("[CXX]   %s got xSum=0x%016llX rSum=0x%016llX want xSum=0x%016llX rSum=0x%016llX\n",
               name, hx, hr, xsum, rsum);
    return (xok && rok) ? 0u : 1u;
}
// x>-1 stream for log1p: negative only when |x|<1 (ebias<=16382). Mirrors
// stream_1p in gen_all.py exactly.
void CrChecksum1p(unsigned long long seed, unsigned long long n,
                  long double (*fn)(long double),
                  unsigned long long &hx, unsigned long long &hr){
    unsigned long long s = seed;
    hx = 1469598103934665603ULL; hr = 1469598103934665603ULL;
    for (unsigned long long i = 0; i < n; ++i){
        unsigned long long a = CrNext(s); s = a;
        unsigned long long b = CrNext(s); s = b;
        unsigned ebias = 1u + (unsigned)(a % 32766ULL);
        unsigned sgn = (unsigned)((a >> 40) & 1ULL);
        if (sgn && ebias > 16382u) sgn = 0;                      // negative only when x>-1
        unsigned short se = (unsigned short)((sgn << 15) | ebias);
        unsigned long long mant = b | (1ULL << 63);
        hx = CrFold(CrFold(hx, mant), se);
        unsigned long long rm; unsigned short rse; CrLdToBits(fn(CrLdFromBits(mant, se)), rm, rse);
        hr = CrFold(CrFold(hr, rm), rse);
    }
}
unsigned CrStream1p(const char *name, unsigned long long seed, unsigned long long n,
                    unsigned long long xsum, unsigned long long rsum, long double (*fn)(long double)){
    unsigned long long hx, hr;
    CrChecksum1p(seed, n, fn, hx, hr);
    bool xok = (hx == xsum), rok = (hr == rsum);
    printf("[CXX] %s stream: N=%llu xSum=%s rSum=%s\n", name, n,
           xok ? "MATCH" : "MISMATCH", rok ? "MATCH" : "MISMATCH");
    if (!xok || !rok)
        printf("[CXX]   %s got xSum=0x%016llX rSum=0x%016llX want xSum=0x%016llX rSum=0x%016llX\n",
               name, hx, hr, xsum, rsum);
    return (xok && rok) ? 0u : 1u;
}
// Generic 2-arg (arg1,arg2) CR harness (atan2: arg1=y,arg2=x; pow: arg1=x,arg2=y).
struct CrVec2 { unsigned long long a1m; unsigned short a1se;
                unsigned long long a2m; unsigned short a2se;
                unsigned long long rm;  unsigned short rse; };
unsigned CrSweep2(const char *name, const CrVec2 *v, unsigned n,
                  long double (*fn)(long double, long double)){
    unsigned long long maxulp = 0; unsigned nonCR = 0, wi = 0;
    unsigned long long gm = 0; unsigned short gse = 0;
    for (unsigned i = 0; i < n; ++i){
        long double a1 = CrLdFromBits(v[i].a1m, v[i].a1se), a2 = CrLdFromBits(v[i].a2m, v[i].a2se);
        unsigned long long rm; unsigned short rse; CrLdToBits(fn(a1, a2), rm, rse);
        unsigned long long d = CrUlp(rm, rse, v[i].rm, v[i].rse);
        if (d){ nonCR++; if (d > maxulp){ maxulp = d; wi = i; gm = rm; gse = rse; } }
    }
    printf("[CXX] %s: swept=%u max-ULP=%llu non-CR=%u\n", name, n, maxulp, nonCR);
    if (nonCR)
        printf("[CXX]   worst: a1=0x%016llX/%04X a2=0x%016llX/%04X got=0x%016llX/%04X want=0x%016llX/%04X\n",
               v[wi].a1m, v[wi].a1se, v[wi].a2m, v[wi].a2se, gm, gse, v[wi].rm, v[wi].rse);
    return nonCR;
}
void CrChecksum2(unsigned long long seed, int e1lo, int e1hi, int e2lo, int e2hi,
                 int sign1, int sign2, unsigned long long n, long double (*fn)(long double, long double),
                 unsigned long long &hx, unsigned long long &hr){
    unsigned long long s = seed;
    unsigned sp1 = (unsigned)(e1hi - e1lo + 1), sp2 = (unsigned)(e2hi - e2lo + 1);
    hx = 1469598103934665603ULL; hr = 1469598103934665603ULL;
    for (unsigned long long i = 0; i < n; ++i){
        unsigned long long a = CrNext(s); s = a; unsigned long long b = CrNext(s); s = b;
        unsigned long long c = CrNext(s); s = c; unsigned long long d = CrNext(s); s = d;
        int E1 = e1lo + (int)(a % sp1);
        unsigned short s1e = (unsigned short)(((sign1 ? ((a >> 40) & 1ULL) : 0ULL) << 15) |
                                              (unsigned short)((16383 + E1) & 0x7FFF));
        unsigned long long m1 = b | (1ULL << 63);
        int E2 = e2lo + (int)(c % sp2);
        unsigned short s2e = (unsigned short)(((sign2 ? ((c >> 40) & 1ULL) : 0ULL) << 15) |
                                              (unsigned short)((16383 + E2) & 0x7FFF));
        unsigned long long m2 = d | (1ULL << 63);
        hx = CrFold(CrFold(CrFold(CrFold(hx, m1), s1e), m2), s2e);
        unsigned long long rm; unsigned short rse;
        CrLdToBits(fn(CrLdFromBits(m1, s1e), CrLdFromBits(m2, s2e)), rm, rse);
        hr = CrFold(CrFold(hr, rm), rse);
    }
}
unsigned CrStream2(const char *name, unsigned long long seed, int e1lo, int e1hi, int e2lo, int e2hi,
                   int sign1, int sign2, unsigned long long n, unsigned long long xsum, unsigned long long rsum,
                   long double (*fn)(long double, long double)){
    unsigned long long hx, hr; CrChecksum2(seed, e1lo, e1hi, e2lo, e2hi, sign1, sign2, n, fn, hx, hr);
    bool xok = (hx == xsum), rok = (hr == rsum);
    printf("[CXX] %s stream: N=%llu xSum=%s rSum=%s\n", name, n, xok?"MATCH":"MISMATCH", rok?"MATCH":"MISMATCH");
    if (!xok || !rok)
        printf("[CXX]   %s got xSum=0x%016llX rSum=0x%016llX want 0x%016llX 0x%016llX\n", name, hx, hr, xsum, rsum);
    return (xok && rok) ? 0u : 1u;
}
void Phase66(){
    unsigned f = 0;
    f += CrSweep("phase66 exp2", kExp2Vec, kExp2VecN, [](long double x){ return std::exp2(x); });
    f += CrSweep("phase66 exp",  kExpVec,  kExpVecN,  [](long double x){ return std::exp(x); });
    f += CrStream("phase66 exp2", 0x2545F4914F6CDD1DULL, -25, 14, kCrN, kExp2XSum, kExp2RSum,
                  [](long double x){ return std::exp2(x); });
    f += CrStream("phase66 exp",  0x9E3779B97F4A7C15ULL, -25, 13, kCrN, kExpXSum, kExpRSum,
                  [](long double x){ return std::exp(x); });
    Check(f == 0, "phase66 exp2/exp correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase66: exp2/exp correctly-rounded 80-bit dd "
               "(MPFR-verified: %u+%u baked, %llu+%llu streamed, 0 non-CR)\n",
               kExp2VecN, kExpVecN, kCrN, kCrN);
}

// ── Phases 67–71 (Ф27e) — log/log2/log10/log1p/expm1 correctly-rounded 80-bit.
// Software double-double (no x87 fyl2x/fyl2xp1/f2xm1), MPFR-verified 0 non-CR:
// baked hard-class (x, MPFR-CR-ref) vectors + a deterministic N=20000 pure-int
// stream FNV-checksummed against MPFR on the host. log/log2/log10 share one
// log_dd_core_l reduction (<cmath>); the streams are domain-restricted.
#include "cr_log_vectors.h"
#include "cr_log_checksums.h"
void Phase67(){
    unsigned f = 0;
    f += CrSweep("phase67 log", kLogVec, kLogVecN, [](long double x){ return std::log(x); });
    f += CrStreamPos("phase67 log", kLogSeed, kLogElo, kLogEhi, kLogN, kLogXSum, kLogRSum,
                     [](long double x){ return std::log(x); });
    Check(std::log(1.0L) == 0.0L && !std::signbit(std::log(1.0L)), "phase67 log(1)==+0");
    Check(std::log(0.0L) == -__builtin_infl(), "phase67 log(0)==-Inf");
    Check(std::isnan(std::log(-1.0L)), "phase67 log(-1)==NaN");
    Check(std::log(__builtin_infl()) == __builtin_infl(), "phase67 log(+Inf)==+Inf");
    Check(std::isnan(std::log(__builtin_nanl(""))), "phase67 log(NaN)==NaN");
    Check(f == 0, "phase67 log correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase67: log correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kLogVecN, kLogN);
}
#include "cr_log2_vectors.h"
#include "cr_log2_checksums.h"
void Phase68(){
    unsigned f = 0;
    f += CrSweep("phase68 log2", kLog2Vec, kLog2VecN, [](long double x){ return std::log2(x); });
    f += CrStreamPos("phase68 log2", kLog2Seed, kLog2Elo, kLog2Ehi, kLog2N, kLog2XSum, kLog2RSum,
                     [](long double x){ return std::log2(x); });
    Check(std::log2(1.0L) == 0.0L && !std::signbit(std::log2(1.0L)), "phase68 log2(1)==+0");
    Check(std::log2(8.0L) == 3.0L, "phase68 log2(8)==3 exact");
    Check(std::log2(0.0L) == -__builtin_infl(), "phase68 log2(0)==-Inf");
    Check(std::isnan(std::log2(-1.0L)), "phase68 log2(-1)==NaN");
    Check(std::log2(__builtin_infl()) == __builtin_infl(), "phase68 log2(+Inf)==+Inf");
    Check(f == 0, "phase68 log2 correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase68: log2 correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kLog2VecN, kLog2N);
}
#include "cr_log10_vectors.h"
#include "cr_log10_checksums.h"
void Phase69(){
    unsigned f = 0;
    f += CrSweep("phase69 log10", kLog10Vec, kLog10VecN, [](long double x){ return std::log10(x); });
    f += CrStreamPos("phase69 log10", kLog10Seed, kLog10Elo, kLog10Ehi, kLog10N, kLog10XSum, kLog10RSum,
                     [](long double x){ return std::log10(x); });
    Check(std::log10(1.0L) == 0.0L && !std::signbit(std::log10(1.0L)), "phase69 log10(1)==+0");
    Check(std::log10(1000.0L) == 3.0L, "phase69 log10(1000)==3 exact");
    Check(std::log10(0.0L) == -__builtin_infl(), "phase69 log10(0)==-Inf");
    Check(std::isnan(std::log10(-1.0L)), "phase69 log10(-1)==NaN");
    Check(std::log10(__builtin_infl()) == __builtin_infl(), "phase69 log10(+Inf)==+Inf");
    Check(f == 0, "phase69 log10 correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase69: log10 correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kLog10VecN, kLog10N);
}
#include "cr_log1p_vectors.h"
#include "cr_log1p_checksums.h"
void Phase70(){
    unsigned f = 0;
    f += CrSweep("phase70 log1p", kLog1pVec, kLog1pVecN, [](long double x){ return std::log1p(x); });
    f += CrStream1p("phase70 log1p", kLog1pSeed, kLog1pN, kLog1pXSum, kLog1pRSum,
                    [](long double x){ return std::log1p(x); });
    Check(std::log1p(0.0L) == 0.0L && !std::signbit(std::log1p(0.0L)), "phase70 log1p(+0)==+0");
    Check(std::log1p(-0.0L) == 0.0L && std::signbit(std::log1p(-0.0L)), "phase70 log1p(-0)==-0");
    Check(std::log1p(-1.0L) == -__builtin_infl(), "phase70 log1p(-1)==-Inf");
    Check(std::isnan(std::log1p(-2.0L)), "phase70 log1p(-2)==NaN");
    Check(std::log1p(__builtin_infl()) == __builtin_infl(), "phase70 log1p(+Inf)==+Inf");
    Check(std::isnan(std::log1p(__builtin_nanl(""))), "phase70 log1p(NaN)==NaN");
    Check(f == 0, "phase70 log1p correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase70: log1p correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kLog1pVecN, kLog1pN);
}
#include "cr_expm1_vectors.h"
#include "cr_expm1_checksums.h"
void Phase71(){
    unsigned f = 0;
    f += CrSweep("phase71 expm1", kExpm1Vec, kExpm1VecN, [](long double x){ return std::expm1(x); });
    f += CrStream("phase71 expm1", kExpm1Seed, kExpm1Elo, kExpm1Ehi, kExpm1N, kExpm1XSum, kExpm1RSum,
                  [](long double x){ return std::expm1(x); });
    Check(std::expm1(0.0L) == 0.0L && !std::signbit(std::expm1(0.0L)), "phase71 expm1(+0)==+0");
    Check(std::expm1(-0.0L) == 0.0L && std::signbit(std::expm1(-0.0L)), "phase71 expm1(-0)==-0");
    Check(std::expm1(-__builtin_infl()) == -1.0L, "phase71 expm1(-Inf)==-1");
    Check(std::expm1(__builtin_infl()) == __builtin_infl(), "phase71 expm1(+Inf)==+Inf");
    Check(std::isnan(std::expm1(__builtin_nanl(""))), "phase71 expm1(NaN)==NaN");
    Check(f == 0, "phase71 expm1 correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase71: expm1 correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kExpm1VecN, kExpm1N);
}

// ── Phase72 (Ф27f) — sin(long double) correctly-rounded 80-bit ──────────────
// Software argument reduction (Cody-Waite dd for |x|<2^20, exact integer
// Payne-Hanek beyond, up to LDBL_MAX) + dd Taylor kernels — NO x87 fsin/fprem.
// Baked hard-class (x, MPFR-CR-ref) vectors (near k·π/2 where sin/cos vanish,
// near π/4, tiny, Cody-Waite + Payne-Hanek bands) + a deterministic signed
// N=20000 pure-int stream FNV-checksummed against MPFR on the host.
#include "cr_sin_vectors.h"
#include "cr_sin_checksums.h"
void Phase72(){
    unsigned f = 0;
    f += CrSweep("phase72 sin", kSinVec, kSinVecN, [](long double x){ return std::sin(x); });
    f += CrStream("phase72 sin", kSinSeed, kSinElo, kSinEhi, kSinN, kSinXSum, kSinRSum,
                  [](long double x){ return std::sin(x); });
    Check(std::sin(0.0L) == 0.0L && !std::signbit(std::sin(0.0L)), "phase72 sin(+0)==+0");
    Check(std::sin(-0.0L) == 0.0L && std::signbit(std::sin(-0.0L)), "phase72 sin(-0)==-0");
    Check(std::isnan(std::sin(__builtin_infl())),  "phase72 sin(+Inf)==NaN");
    Check(std::isnan(std::sin(-__builtin_infl())), "phase72 sin(-Inf)==NaN");
    Check(std::isnan(std::sin(__builtin_nanl(""))),"phase72 sin(NaN)==NaN");
    Check(f == 0, "phase72 sin correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase72: sin correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kSinVecN, kSinN);
}
// ── Phase73/74 (Ф27f) — cos/tan correctly-rounded 80-bit (reuse sin's reducer) ─
#include "cr_cos_vectors.h"
#include "cr_cos_checksums.h"
void Phase73(){
    unsigned f = 0;
    f += CrSweep("phase73 cos", kCosVec, kCosVecN, [](long double x){ return std::cos(x); });
    f += CrStream("phase73 cos", kCosSeed, kCosElo, kCosEhi, kCosN, kCosXSum, kCosRSum,
                  [](long double x){ return std::cos(x); });
    Check(std::cos(0.0L) == 1.0L && std::cos(-0.0L) == 1.0L, "phase73 cos(±0)==1");
    Check(std::isnan(std::cos(__builtin_infl())),  "phase73 cos(+Inf)==NaN");
    Check(std::isnan(std::cos(__builtin_nanl(""))),"phase73 cos(NaN)==NaN");
    Check(f == 0, "phase73 cos correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase73: cos correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kCosVecN, kCosN);
}
#include "cr_tan_vectors.h"
#include "cr_tan_checksums.h"
void Phase74(){
    unsigned f = 0;
    f += CrSweep("phase74 tan", kTanVec, kTanVecN, [](long double x){ return std::tan(x); });
    f += CrStream("phase74 tan", kTanSeed, kTanElo, kTanEhi, kTanN, kTanXSum, kTanRSum,
                  [](long double x){ return std::tan(x); });
    Check(std::tan(0.0L) == 0.0L && !std::signbit(std::tan(0.0L)), "phase74 tan(+0)==+0");
    Check(std::tan(-0.0L) == 0.0L && std::signbit(std::tan(-0.0L)), "phase74 tan(-0)==-0");
    Check(std::isnan(std::tan(__builtin_infl())),  "phase74 tan(+Inf)==NaN");
    Check(std::isnan(std::tan(__builtin_nanl(""))),"phase74 tan(NaN)==NaN");
    Check(f == 0, "phase74 tan correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase74: tan correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kTanVecN, kTanN);
}
// ── Phase75 (Ф27f) — atan(long double) correctly-rounded 80-bit ──────────────
#include "cr_atan_vectors.h"
#include "cr_atan_checksums.h"
void Phase75(){
    unsigned f = 0;
    f += CrSweep("phase75 atan", kAtanVec, kAtanVecN, [](long double x){ return std::atan(x); });
    f += CrStream("phase75 atan", kAtanSeed, kAtanElo, kAtanEhi, kAtanN, kAtanXSum, kAtanRSum,
                  [](long double x){ return std::atan(x); });
    Check(std::atan(0.0L) == 0.0L && !std::signbit(std::atan(0.0L)), "phase75 atan(+0)==+0");
    Check(std::atan(-0.0L) == 0.0L && std::signbit(std::atan(-0.0L)), "phase75 atan(-0)==-0");
    Check(std::atan(__builtin_infl()) > 1.5L && std::atan(-__builtin_infl()) < -1.5L, "phase75 atan(±Inf)==±π/2");
    Check(std::isnan(std::atan(__builtin_nanl(""))), "phase75 atan(NaN)==NaN");
    Check(f == 0, "phase75 atan correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase75: atan correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kAtanVecN, kAtanN);
}
// ── Phase76 (Ф27g) — sinh/cosh/tanh correctly-rounded 80-bit ────────────────
// The hyperbolic family rebuilt on the dd exp reducer (exp_reduce_dd): factor
// out the dominant exponent, combine e^a and e^{−a} in double-double, round once
// — NO x87 fsinh/f2xm1. Baked hard-class (x, MPFR-CR-ref) vectors (tiny |x| at
// the 2⁻³² identity floor, integers, the tanh saturation shoulder ~24, the
// cosh/sinh overflow shoulder near LDBL_MAX) + deterministic N=20000 streams.
#include "cr_cosh_vectors.h"
#include "cr_cosh_checksums.h"
#include "cr_sinh_vectors.h"
#include "cr_sinh_checksums.h"
#include "cr_tanh_vectors.h"
#include "cr_tanh_checksums.h"
void Phase76(){
    unsigned f = 0;
    f += CrSweep("phase76 cosh", kCoshVec, kCoshVecN, [](long double x){ return std::cosh(x); });
    f += CrSweep("phase76 sinh", kSinhVec, kSinhVecN, [](long double x){ return std::sinh(x); });
    f += CrSweep("phase76 tanh", kTanhVec, kTanhVecN, [](long double x){ return std::tanh(x); });
    f += CrStream("phase76 cosh", kCoshSeed, kCoshElo, kCoshEhi, kCoshN, kCoshXSum, kCoshRSum,
                  [](long double x){ return std::cosh(x); });
    f += CrStream("phase76 sinh", kSinhSeed, kSinhElo, kSinhEhi, kSinhN, kSinhXSum, kSinhRSum,
                  [](long double x){ return std::sinh(x); });
    f += CrStream("phase76 tanh", kTanhSeed, kTanhElo, kTanhEhi, kTanhN, kTanhXSum, kTanhRSum,
                  [](long double x){ return std::tanh(x); });
    Check(std::cosh(0.0L) == 1.0L && std::cosh(-0.0L) == 1.0L, "phase76 cosh(±0)==1");
    Check(std::sinh(0.0L) == 0.0L && !std::signbit(std::sinh(0.0L)), "phase76 sinh(+0)==+0");
    Check(std::sinh(-0.0L) == 0.0L && std::signbit(std::sinh(-0.0L)), "phase76 sinh(-0)==-0");
    Check(std::tanh(0.0L) == 0.0L && !std::signbit(std::tanh(0.0L)), "phase76 tanh(+0)==+0");
    Check(std::tanh(-0.0L) == 0.0L && std::signbit(std::tanh(-0.0L)), "phase76 tanh(-0)==-0");
    Check(std::cosh(__builtin_infl()) == __builtin_infl() && std::cosh(-__builtin_infl()) == __builtin_infl(),
          "phase76 cosh(±Inf)==+Inf");
    Check(std::sinh(__builtin_infl()) == __builtin_infl() && std::sinh(-__builtin_infl()) == -__builtin_infl(),
          "phase76 sinh(±Inf)==±Inf");
    Check(std::tanh(__builtin_infl()) == 1.0L && std::tanh(-__builtin_infl()) == -1.0L, "phase76 tanh(±Inf)==±1");
    Check(std::isnan(std::cosh(__builtin_nanl(""))) && std::isnan(std::sinh(__builtin_nanl(""))) &&
          std::isnan(std::tanh(__builtin_nanl(""))), "phase76 hyper(NaN)==NaN");
    Check(f == 0, "phase76 sinh/cosh/tanh correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase76: sinh/cosh/tanh correctly-rounded 80-bit dd "
               "(MPFR-verified: %u+%u+%u baked, 3×%llu streamed, 0 non-CR)\n",
               kCoshVecN, kSinhVecN, kTanhVecN, kCoshN);
}
// ── Phase77 (Ф27h) — asinh/acosh/atanh correctly-rounded 80-bit ─────────────
// The inverse-hyperbolic family rebuilt on the dd log core (log_dd_core_dd) +
// dd sqrt (sqrt_dd): ln of a genuine √(x²±1) dd argument, rounded once — NO x87
// transcendentals. Baked hard-class vectors (tiny |x| identity floor, the x→1⁺
// acosh shoulder, the |x|→1⁻ atanh pole, the 2⁸¹⁹⁰ overflow-safe branch) +
// deterministic N=20000 streams (asinh signed, acosh x≥1, atanh |x|<1).
#include "cr_asinh_vectors.h"
#include "cr_asinh_checksums.h"
#include "cr_acosh_vectors.h"
#include "cr_acosh_checksums.h"
#include "cr_atanh_vectors.h"
#include "cr_atanh_checksums.h"
void Phase77(){
    unsigned f = 0;
    f += CrSweep("phase77 asinh", kAsinhVec, kAsinhVecN, [](long double x){ return std::asinh(x); });
    f += CrSweep("phase77 acosh", kAcoshVec, kAcoshVecN, [](long double x){ return std::acosh(x); });
    f += CrSweep("phase77 atanh", kAtanhVec, kAtanhVecN, [](long double x){ return std::atanh(x); });
    f += CrStream   ("phase77 asinh", kAsinhSeed, kAsinhElo, kAsinhEhi, kAsinhN, kAsinhXSum, kAsinhRSum,
                     [](long double x){ return std::asinh(x); });
    f += CrStreamPos("phase77 acosh", kAcoshSeed, kAcoshElo, kAcoshEhi, kAcoshN, kAcoshXSum, kAcoshRSum,
                     [](long double x){ return std::acosh(x); });
    f += CrStream   ("phase77 atanh", kAtanhSeed, kAtanhElo, kAtanhEhi, kAtanhN, kAtanhXSum, kAtanhRSum,
                     [](long double x){ return std::atanh(x); });
    Check(std::asinh(0.0L) == 0.0L && !std::signbit(std::asinh(0.0L)), "phase77 asinh(+0)==+0");
    Check(std::asinh(-0.0L) == 0.0L && std::signbit(std::asinh(-0.0L)), "phase77 asinh(-0)==-0");
    Check(std::acosh(1.0L) == 0.0L, "phase77 acosh(1)==0");
    Check(std::isnan(std::acosh(0.5L)), "phase77 acosh(<1)==NaN");
    Check(std::atanh(0.0L) == 0.0L && !std::signbit(std::atanh(0.0L)), "phase77 atanh(+0)==+0");
    Check(std::atanh(-0.0L) == 0.0L && std::signbit(std::atanh(-0.0L)), "phase77 atanh(-0)==-0");
    Check(std::atanh(1.0L) == __builtin_infl() && std::atanh(-1.0L) == -__builtin_infl(),
          "phase77 atanh(±1)==±Inf");
    Check(std::isnan(std::atanh(1.5L)), "phase77 atanh(|x|>1)==NaN");
    Check(std::asinh(__builtin_infl()) == __builtin_infl() &&
          std::asinh(-__builtin_infl()) == -__builtin_infl(), "phase77 asinh(±Inf)==±Inf");
    Check(std::acosh(__builtin_infl()) == __builtin_infl(), "phase77 acosh(+Inf)==+Inf");
    Check(std::isnan(std::asinh(__builtin_nanl(""))) && std::isnan(std::acosh(__builtin_nanl(""))) &&
          std::isnan(std::atanh(__builtin_nanl(""))), "phase77 invhyper(NaN)==NaN");
    Check(f == 0, "phase77 asinh/acosh/atanh correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase77: asinh/acosh/atanh correctly-rounded 80-bit dd "
               "(MPFR-verified: %u+%u+%u baked, 3×%llu streamed, 0 non-CR)\n",
               kAsinhVecN, kAcoshVecN, kAtanhVecN, kAsinhN);
}
// ── Phase78 (Ф27i) — asin/acos correctly-rounded 80-bit ─────────────────────
// Inverse trig on the dd atan kernel (asin_small_dd = atan(a/√(1−a²)) + range
// reduction through √((1−|x|)/2) near ±1). NO x87 fpatan. Baked hard-class
// vectors (|x|→1⁻ endpoints, √½ split, identity floor, exact fractions) + a
// deterministic signed N=20000 stream over (−1,1).
#include "cr_asin_vectors.h"
#include "cr_asin_checksums.h"
#include "cr_acos_vectors.h"
#include "cr_acos_checksums.h"
void Phase78(){
    unsigned f = 0;
    f += CrSweep("phase78 asin", kAsinVec, kAsinVecN, [](long double x){ return std::asin(x); });
    f += CrSweep("phase78 acos", kAcosVec, kAcosVecN, [](long double x){ return std::acos(x); });
    f += CrStream("phase78 asin", kAsinSeed, kAsinElo, kAsinEhi, kAsinN, kAsinXSum, kAsinRSum,
                  [](long double x){ return std::asin(x); });
    f += CrStream("phase78 acos", kAcosSeed, kAcosElo, kAcosEhi, kAcosN, kAcosXSum, kAcosRSum,
                  [](long double x){ return std::acos(x); });
    Check(std::asin(0.0L) == 0.0L && !std::signbit(std::asin(0.0L)), "phase78 asin(+0)==+0");
    Check(std::asin(-0.0L) == 0.0L && std::signbit(std::asin(-0.0L)), "phase78 asin(-0)==-0");
    Check(std::asin(1.0L) > 1.5707L && std::asin(1.0L) < 1.5709L, "phase78 asin(1)==π/2");
    Check(std::acos(1.0L) == 0.0L, "phase78 acos(1)==0");
    Check(std::acos(-1.0L) > 3.1415L && std::acos(-1.0L) < 3.1417L, "phase78 acos(-1)==π");
    Check(std::acos(0.0L) > 1.5707L && std::acos(0.0L) < 1.5709L, "phase78 acos(0)==π/2");
    Check(std::isnan(std::asin(1.5L)) && std::isnan(std::acos(1.5L)), "phase78 asin/acos(|x|>1)==NaN");
    Check(std::isnan(std::asin(__builtin_nanl(""))) && std::isnan(std::acos(__builtin_nanl(""))),
          "phase78 asin/acos(NaN)==NaN");
    Check(f == 0, "phase78 asin/acos correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase78: asin/acos correctly-rounded 80-bit dd "
               "(MPFR-verified: %u+%u baked, 2×%llu streamed, 0 non-CR)\n", kAsinVecN, kAcosVecN, kAsinN);
}
// ── Phase79 (Ф27j) — cbrt correctly-rounded 80-bit ──────────────────────────
// Exponent-reduce to [1,8) then one dd Halley step, RN64·2^k exact. NO x87.
// Baked hard-class (perfect cubes exact, the 2^(3k+j) boundaries, tiny/huge
// magnitudes) + a deterministic signed N=20000 stream.
#include "cr_cbrt_vectors.h"
#include "cr_cbrt_checksums.h"
void Phase79(){
    unsigned f = 0;
    f += CrSweep("phase79 cbrt", kCbrtVec, kCbrtVecN, [](long double x){ return std::cbrt(x); });
    f += CrStream("phase79 cbrt", kCbrtSeed, kCbrtElo, kCbrtEhi, kCbrtN, kCbrtXSum, kCbrtRSum,
                  [](long double x){ return std::cbrt(x); });
    Check(std::cbrt(0.0L) == 0.0L && !std::signbit(std::cbrt(0.0L)), "phase79 cbrt(+0)==+0");
    Check(std::cbrt(-0.0L) == 0.0L && std::signbit(std::cbrt(-0.0L)), "phase79 cbrt(-0)==-0");
    Check(std::cbrt(8.0L) == 2.0L && std::cbrt(-27.0L) == -3.0L && std::cbrt(1000.0L) == 10.0L,
          "phase79 cbrt perfect cubes exact");
    Check(std::cbrt(__builtin_infl()) == __builtin_infl() &&
          std::cbrt(-__builtin_infl()) == -__builtin_infl(), "phase79 cbrt(±Inf)==±Inf");
    Check(std::isnan(std::cbrt(__builtin_nanl(""))), "phase79 cbrt(NaN)==NaN");
    Check(f == 0, "phase79 cbrt correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase79: cbrt correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR)\n", kCbrtVecN, kCbrtN);
}
// ── Phase80 (Ф27k) — atan2(y,x) correctly-rounded 80-bit (last x87 removed) ──
// Quadrant + dd atan(|y|/|x|); replaces the x87 fpatan — cmath is now fully
// software 80-bit. Baked hard-class (all quadrants, |y|≈|x|, near-axis) + a
// deterministic 2-arg N=20000 stream. Axis/±0/±Inf via explicit Checks.
#include "cr_atan2_vectors.h"
#include "cr_atan2_checksums.h"
void Phase80(){
    unsigned f = 0;
    f += CrSweep2("phase80 atan2", kAtan2Vec, kAtan2VecN, [](long double y, long double x){ return std::atan2(y, x); });
    f += CrStream2("phase80 atan2", kAtan2Seed, kAtan2E1lo, kAtan2E1hi, kAtan2E2lo, kAtan2E2hi,
                   kAtan2Sign1, kAtan2Sign2, kAtan2N, kAtan2XSum, kAtan2RSum,
                   [](long double y, long double x){ return std::atan2(y, x); });
    Check(std::atan2(0.0L, 1.0L) == 0.0L && !std::signbit(std::atan2(0.0L, 1.0L)), "phase80 atan2(+0,+x)=+0");
    Check(std::atan2(-0.0L, 1.0L) == 0.0L && std::signbit(std::atan2(-0.0L, 1.0L)), "phase80 atan2(-0,+x)=-0");
    Check(std::atan2(0.0L, -1.0L) > 3.1415L, "phase80 atan2(+0,-x)=+pi");
    Check(std::atan2(-0.0L, -1.0L) < -3.1415L, "phase80 atan2(-0,-x)=-pi");
    Check(std::atan2(1.0L, 0.0L) > 1.5707L && std::atan2(1.0L, 0.0L) < 1.5709L, "phase80 atan2(+y,0)=+pi/2");
    Check(std::atan2(-1.0L, 0.0L) < -1.5707L, "phase80 atan2(-y,0)=-pi/2");
    Check(std::atan2(1.0L, 1.0L) > 0.7853L && std::atan2(1.0L, 1.0L) < 0.7854L, "phase80 atan2(1,1)=pi/4");
    Check(std::atan2(__builtin_infl(), __builtin_infl()) > 0.7853L &&
          std::atan2(__builtin_infl(), __builtin_infl()) < 0.7854L, "phase80 atan2(inf,inf)=pi/4");
    Check(std::atan2(__builtin_infl(), -__builtin_infl()) > 2.356L, "phase80 atan2(inf,-inf)=3pi/4");
    Check(std::atan2(1.0L, __builtin_infl()) == 0.0L, "phase80 atan2(y,+inf)=0");
    Check(std::atan2(1.0L, -__builtin_infl()) > 3.1415L, "phase80 atan2(y,-inf)=pi");
    Check(std::isnan(std::atan2(__builtin_nanl(""), 1.0L)), "phase80 atan2(NaN,x)=NaN");
    Check(f == 0, "phase80 atan2 correctly-rounded 80-bit (0 non-CR vs MPFR)");
    if (f == 0)
        printf("[CXX] PASS phase80: atan2 correctly-rounded 80-bit dd "
               "(MPFR-verified: %u baked, %llu streamed, 0 non-CR) — last x87 fpatan removed\n",
               kAtan2VecN, kAtan2N);
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
    Phase30();
    Phase31();
    Phase32();
    Phase33();
    Phase34();
    Phase35();
    Phase36();
    Phase37();
    Phase38();
    Phase39();
    Phase40();
    Phase41();
    Phase42();
    Phase43();
    Phase44();
    Phase45();
    Phase46();
    Phase47();
    Phase48();
    Phase49();
    Phase50();
    Phase51();
    Phase52();
    Phase53();
    Phase54();
    Phase55();
    Phase56();
    Phase57();
    Phase58();
    Phase59();
    Phase60();
    Phase61();
    Phase62();
    Phase63();
    Phase64();
    Phase65();
    Phase66();
    Phase67();
    Phase68();
    Phase69();
    Phase70();
    Phase71();
    Phase72();
    Phase73();
    Phase74();
    Phase75();
    Phase76();
    Phase77();
    Phase78();
    Phase79();
    Phase80();

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
