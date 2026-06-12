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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
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
