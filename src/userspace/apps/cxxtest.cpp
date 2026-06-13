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

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <iterator>
#include <list>
#include <map>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>
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
