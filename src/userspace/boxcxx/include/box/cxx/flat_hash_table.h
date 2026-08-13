// boxcxx — box::__flat_hash::Table  (the robin-hood engine behind
//   box::flat_hash_map and box::flat_hash_set)
//
// INTERNAL. Include <box/cxx/flat_hash_map.h> or <box/cxx/flat_hash_set.h>;
// nothing here is a supported spelling on its own.
//
// std::unordered_map in this tree is a separate-chaining table: one heap block
// per element, node pointers stable across rehash, a real bucket API. That is
// what the standard mandates and it is the right container when references
// must outlive an insert. It is also 37.11 bytes and one malloc per element
// (measured, N=10000, <int,int>) -- on bare metal that is 10 000 trips through
// the boxlib heap lock and 10 000 block headers to hold 80 KB of payload.
//
// This engine is the other trade: ONE allocation, no per-element header, keys
// packed densely enough that a probe stays in cache -- paid for by giving up
// reference stability. Nothing here is stable across a mutation, and that is
// stated first because it is the only thing a reader must not miss.
//
// ── layout: three parallel arrays, one block (SoA) ──────────────────────────
//
//   ctl [cap]   uint32_t   occupancy + fingerprint + ideal-slot source
//   keys[cap]   Key        the dense key block -- a probe touches ONLY this
//   vals[cap]   Mapped     the map's values; the set has no array at all
//
// A probe reads ctl[] and keys[]: two cache lines regardless of sizeof(Mapped).
// The array-of-structs alternative pulls a value into cache on every probe
// step, so a map with a 64-byte value would miss on each one. Storing the key
// NON-const is what lets robin-hood relocate an element with a plain move; the
// union trick that buys libc++ and abseil an honest pair<const Key,T>& (an
// inactive-member read plus a const_cast on every relocation) is formally UB
// and is not taken here. The cost of that honesty is that *it is a PROXY --
// exactly like std::flat_map's Yoke, whose machinery this tree already ships.
//
// ── ctl: one 32-bit word doing three jobs ───────────────────────────────────
//
//   ctl[s] == 0                        slot s is empty
//   ctl[s] == (uint32_t(h) | kTaken)   slot s holds a key whose hash is h
//   Ideal(s)    = ctl[s] & mask        (mask < 2^31, so bit 31 is outside it)
//   Distance(s) = (s - Ideal(s)) & mask
//
// * occupancy -- bit 31 is always set on a live slot, so 0 is unambiguous;
// * fingerprint -- comparing the whole word rejects a foreign key before
//   KeyEq is ever called (a false match is 1 in 2^31, which is what makes a
//   string-keyed probe cheap);
// * ideal slot -- h & mask uses only the low 32 bits and mask < 2^31, so
//   ctl[s] & mask EQUALS h & mask exactly, not approximately;
// * DISTANCE IS DERIVED, NEVER STORED, SO IT CANNOT OVERFLOW. Textbook
//   robin-hood keeps the probe distance in a byte and dies at 255 collisions
//   -- and growing does not help, because with a degenerate hash the distance
//   does not shrink. Here the distance is a subtraction that is < cap by
//   construction, so there is no cliff to document and none to test.
// * a rehash never calls Hash again: the new ideal slot comes out of ctl.
//
// The one price is the ceiling: cap <= 2^31 slots, which max_size() reports.
//
// ── insertion: build at the run's end, then rotate the run right ────────────
//
// The textbook robin-hood insert swaps the carried element with each poorer
// occupant and walks on. This engine walks the SAME span -- both stop at the
// run's first empty slot -- but instead of a chain of swaps it BUILDS THE NEW
// ELEMENT AT THAT EMPTY SLOT FIRST and then rotates the run one place right,
// which drops the newcomer into the seat it earned. Two things follow from
// that order, and neither is available to the swap chain:
//
//   * an argument that names an element of THIS table -- m[m.begin()->first],
//     m.try_emplace(k, m.at(j)) -- is read before anything has moved, so it
//     cannot be read through a moved-from object;
//   * a throwing element constructor runs before the first relocation, so the
//     table is left exactly as it was: the strong guarantee, on the crowded
//     path as well as the free-seat one.
//
// It is a valid robin-hood table afterwards. The invariant a probe relies on
// is: for consecutive occupied slots, Distance(s+1) <= Distance(s) + 1. It
// gives the early exit its licence -- if the occupant of s is closer to home
// than we are, our key cannot be further along, because on its way there it
// would have passed s with a greater distance and taken the seat. Rotating the
// run right adds exactly 1 to every distance inside it, so the relation holds
// within the block; at the head, the new element's distance d satisfies
// d <= Distance(s-1) + 1 (we walked past s-1 without stopping) and the element
// pushed to s+1 had Distance(s) < d, hence Distance(s) + 1 <= d. Both edges
// hold, so the whole table still satisfies it.
//
// Growth is the one place an argument still has to be materialised up front:
// there the relocation happens before any seat exists. That costs one move,
// amortised over a whole doubling.
//
// ── erasure: backward shift, no tombstones ──────────────────────────────────
//
// Erasing pulls the rest of the run one slot left, so every distance in it
// drops by 1 and the chain stays contiguous. No tombstone is ever written,
// which is why a long insert/erase cycle does not degrade this table the way
// it degrades a deletion-marker one.
//
// ‼ One consequence has to be said out loud: a run may cross the end of the
// array, and an element pulled across that seam moves LATER in slot order. A
// hand-written "erase while iterating" loop can therefore see such an element
// twice. box::erase_if does not -- it starts its scan at an empty slot, and
// since erasure only empties slots, no run can ever cross that seam, so every
// element is visited exactly once. Bulk removal goes through erase_if.
//
// ── exceptions: the tree's doctrine, not a static_assert ────────────────────
//
// A move that throws in the middle of a shift leaves a hole that cannot be
// repaired -- undoing it is another move, and a second throw during unwinding
// is terminate. The answer is the one <flat_set>/<flat_map> already ship
// ([flat.set.overview]/6): the invariant is restored by emptying, and the
// exception propagates. Requiring nothrow-movable elements with a
// static_assert would reject valid code instead, which this epic has already
// judged the wrong trade once. For nothrow-movable Key and Mapped -- every
// scalar, std::string, every well-behaved type -- the guard is compiled out
// entirely and costs nothing.
//
// Growth moves with move_if_noexcept, so a throwing-move-but-copyable element
// keeps the strong guarantee, exactly as <vector> does here.
#ifndef BOXCXX_BOX_FLAT_HASH_TABLE_H
#define BOXCXX_BOX_FLAT_HASH_TABLE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "box/cxx/error.h"  // box::result / box::status / box::error / box::errc
#include "box/cxx/heap.h"   // box::heap::last_error — the real cause behind bad_alloc

namespace box {

namespace __flat_hash {

// The set instantiates the cursor with no value channel; this is the stand-in
// for the pointer it does not carry.
struct NoValue {};

template <bool HasValue, class KeyRef, class ValRef>
struct RefOf {
    using type = std::pair<KeyRef, ValRef>;
};
template <class KeyRef, class ValRef>
struct RefOf<false, KeyRef, ValRef> {
    using type = KeyRef;
};

// void&& is not a type, and the set's value channel IS void, so the rvalue
// form of a reference type has to be taken through a specialization rather
// than spelled remove_reference_t<R>&& in a template argument list — that one
// is formed eagerly and hard-errors before any conditional can discard it.
template <class R>
struct RvalOf {
    using type = std::remove_reference_t<R> &&;
};
template <>
struct RvalOf<void> {
    using type = void;
};

template <bool IsMap, class Key, class Mapped>
struct ValueOf {
    // pair<Key, Mapped>, NOT pair<const Key, Mapped>. The const form cannot be
    // moved from, so it is useless as a value_type for a container that stores
    // its keys unqualified — std::flat_map reaches the same conclusion for the
    // same reason ([flat.map.overview]). This is the one place where the shape
    // follows flat_map rather than unordered_map, and it follows from the
    // layout, not from taste.
    using type = std::pair<Key, Mapped>;
};
template <class Key, class Mapped>
struct ValueOf<false, Key, Mapped> {
    using type = Key;
};

// ── Tandem ──────────────────────────────────────────────────────────────────
// One harness, two in step: a position in this table names a key AND its
// value, and the cursor has to carry both. It also steps over empty slots, so
// it walks the ctl array and stops only on a live one.
//
// Tag is a phantom parameter, never touched in the body: the cursor's shape
// depends on neither Hash nor KeyEq, so without it two tables differing only
// in hasher would share an iterator type and one's iterator would pass where
// the other's is expected. Table passes itself.
//
// The set instantiates this with ValPtr = void — the second trace unhooked —
// and then reference is KeyRef, a real reference, not a proxy pair.
template <class KeyPtr, class ValPtr, class KeyRef, class ValRef, class Tag>
class Tandem {
    static constexpr bool kHasValue = !std::is_void_v<ValPtr>;
    using ValStore = std::conditional_t<kHasValue, ValPtr, NoValue>;

public:
    // remove_pointer_t leaves a non-pointer alone, so the set's NoValue stand-in
    // passes through untouched and ValueOf<false,...> discards it anyway.
    using value_type =
        typename ValueOf<kHasValue, std::remove_cv_t<std::remove_pointer_t<KeyPtr>>,
                         std::remove_cv_t<std::remove_pointer_t<ValStore>>>::type;
    using difference_type = std::ptrdiff_t;
    using reference       = typename RefOf<kHasValue, KeyRef, ValRef>::type;
    // iterator_concept only, on purpose: declaring iterator_category by hand
    // would skip [iterator.traits]/3.2's synthesis, which derives the right
    // answer from the fact that the map's reference is a prvalue pair and the
    // set's is a genuine reference.
    using iterator_concept = std::forward_iterator_tag;

    Tandem() = default;

    // Always settles on a live slot (or on the end), so begin(), find() and
    // end() all build through the same door and no caller can forget to skip.
    Tandem(const std::uint32_t *ctl, const std::uint32_t *ctl_end, KeyPtr key, ValStore val)
        : ctl_(ctl), ctl_end_(ctl_end), key_(key), val_(val)
    {
        Settle();
    }

    // iterator converts to const_iterator; const_iterator never converts back.
    // Both directions fall out of ONE constraint — whether the source's own
    // component pointers convert — rather than a hand-written exclusion.
    template <class OKeyPtr, class OValPtr, class OKeyRef, class OValRef>
        requires std::is_convertible_v<OKeyPtr, KeyPtr> &&
                 (!kHasValue || std::is_convertible_v<OValPtr, ValPtr>)
    Tandem(const Tandem<OKeyPtr, OValPtr, OKeyRef, OValRef, Tag> &other)
        : ctl_(other.ctl_), ctl_end_(other.ctl_end_), key_(other.key_)
    {
        if constexpr (kHasValue) val_ = other.val_;
    }

    reference operator*() const
    {
        if constexpr (kHasValue) return reference(*key_, *val_);
        else return *key_;
    }

    // *it is a prvalue pair for the map, so it->second needs the classic
    // arrow-proxy: materialise the pair, hand back a pointer into it. Tandem
    // declares no `pointer` member, so [iterator.traits]/3.2.1 picks this up as
    // the synthesized pointer type.
    class ArrowProxy {
    public:
        explicit ArrowProxy(reference r) : ref(std::move(r)) {}
        reference *operator->() { return std::addressof(ref); }

    private:
        reference ref;
    };
    std::conditional_t<kHasValue, ArrowProxy, KeyPtr> operator->() const
    {
        if constexpr (kHasValue) return ArrowProxy(**this);
        else return key_;
    }

    Tandem &operator++()
    {
        Step();
        Settle();
        return *this;
    }
    Tandem operator++(int)
    {
        Tandem t = *this;
        ++*this;
        return t;
    }

    friend bool operator==(const Tandem &a, const Tandem &b) { return a.ctl_ == b.ctl_; }

    // Built from the REFERENCE types, not the value types: the key half is
    // const Key&, whose rvalue form is const Key&& (nothing to steal), not
    // Key&&. Handing this to the public iterator is a deliberate departure
    // from both oracle libraries' flat_map cursors, whose default prvalue
    // iter_move makes ranges::move copy the values instead of moving them.
    using RvalRef = typename RefOf<kHasValue, typename RvalOf<KeyRef>::type,
                                   typename RvalOf<ValRef>::type>::type;
    friend RvalRef iter_move(const Tandem &c)
    {
        if constexpr (kHasValue) return RvalRef(std::move(*c.key_), std::move(*c.val_));
        else return std::move(*c.key_);
    }

    // The slot this cursor sits on, relative to the table's ctl base. The
    // engine's only way back from an iterator to an index.
    std::size_t Slot(const std::uint32_t *base) const
    {
        return static_cast<std::size_t>(ctl_ - base);
    }

private:
    template <class, class, class, class, class>
    friend class Tandem;
    template <class, class, class, class, class>
    friend class Table;

    void Step()
    {
        ++ctl_;
        ++key_;
        if constexpr (kHasValue) ++val_;
    }
    void Settle()
    {
        while (ctl_ != ctl_end_ && *ctl_ == 0) Step();
    }

    const std::uint32_t                 *ctl_     = nullptr;
    const std::uint32_t                 *ctl_end_ = nullptr;
    KeyPtr                               key_     = nullptr;
    [[no_unique_address]] ValStore       val_{};
};

// ── the restore guard ───────────────────────────────────────────────────────
// Restores the invariant the only way it can be restored once a relocation has
// been interrupted: by emptying. Armed explicitly, immediately before the first
// move, so a throw that happens BEFORE anything moved leaves the table alone.
//
// Active = false is the whole point of the parameter: when no relocation in a
// given table can throw, the guard is an empty, trivially destructible object
// with no members and no destructor body, so "the guard costs nothing for a
// nothrow-movable element" is a fact the compiler enforces rather than a claim
// in a comment — and the test asserts exactly that with is_empty_v /
// is_trivially_destructible_v.
template <bool Active, class T>
class RestoreGuard {
public:
    explicit RestoreGuard(T &) noexcept {}
    void Arm() noexcept {}
    void Done() noexcept {}

    RestoreGuard(const RestoreGuard &)            = delete;
    RestoreGuard &operator=(const RestoreGuard &) = delete;
};

template <class T>
class RestoreGuard<true, T> {
public:
    explicit RestoreGuard(T &t) noexcept : t_(&t) {}
    ~RestoreGuard()
    {
        if (t_ && armed_) t_->clear();
    }
    void Arm() noexcept { armed_ = true; }
    void Done() noexcept { t_ = nullptr; }

    RestoreGuard(const RestoreGuard &)            = delete;
    RestoreGuard &operator=(const RestoreGuard &) = delete;

private:
    T   *t_;
    bool armed_ = false;
};

// ── one block for three arrays ──────────────────────────────────────────────
// Rebinding the user's allocator to a type whose alignment is the strictest of
// the three is what keeps the single-block layout allocator-correct: both
// std::allocator and std::pmr::polymorphic_allocator derive the alignment they
// request from the value type, so an over-aligned Key or Mapped is honoured
// without this engine ever calling ::operator new behind the allocator's back.
template <std::size_t A>
struct alignas(A) Chunk {
    unsigned char raw[A];
};

// ── Table ───────────────────────────────────────────────────────────────────
// Mapped = void makes this a set: the values array, and every member that
// names a mapped value, disappear.
template <class Key, class Mapped, class Hash, class KeyEq, class Alloc>
class Table {
    static constexpr bool kMap = !std::is_void_v<Mapped>;

    using MappedStore = std::conditional_t<kMap, Mapped *, NoValue>;
    // The engine's own storage type for the mapped half; naming Mapped
    // directly in a member declaration would hard-error for the set.
    using MappedT = std::conditional_t<kMap, Mapped, NoValue>;

    static constexpr std::size_t kAlign = []() constexpr {
        std::size_t a = alignof(std::uint32_t);
        if (alignof(Key) > a) a = alignof(Key);
        if constexpr (kMap)
            if (alignof(Mapped) > a) a = alignof(Mapped);
        return a;
    }();
    using ChunkT     = Chunk<kAlign>;
    using ChunkAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<ChunkT>;
    using KeyAlloc   = typename std::allocator_traits<Alloc>::template rebind_alloc<Key>;
    using MappedAlloc =
        typename std::allocator_traits<Alloc>::template rebind_alloc<MappedT>;
    using AllocTraits = std::allocator_traits<Alloc>;

    static constexpr std::uint32_t kTaken = 0x8000'0000u;
    // cap <= 2^31 keeps bit 31 clear in every mask, which is what makes
    // `ctl & mask` an exact stand-in for `hash & mask`.
    static constexpr std::size_t kMaxSlots = std::size_t{1} << 31;

public:
    using key_type        = Key;
    using value_type      = typename ValueOf<kMap, Key, MappedT>::type;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hasher          = Hash;
    using key_equal       = KeyEq;
    using allocator_type  = Alloc;

    // MappedT, not Mapped: `Mapped &` is formed eagerly here and void& is not a
    // type, so the set would hard-error on a branch conditional_t discards.
    using iterator = Tandem<const Key *, std::conditional_t<kMap, MappedT *, void>, const Key &,
                            std::conditional_t<kMap, MappedT &, void>, Table>;
    using const_iterator =
        Tandem<const Key *, std::conditional_t<kMap, const MappedT *, void>, const Key &,
               std::conditional_t<kMap, const MappedT &, void>, Table>;
    using reference       = typename iterator::reference;
    using const_reference = typename const_iterator::reference;
    using pointer         = value_type *;
    using const_pointer   = const value_type *;

    // ── lifecycle ───────────────────────────────────────────────────────────

    Table() = default;

    explicit Table(size_type slots, const Hash &h = Hash(), const KeyEq &eq = KeyEq(),
                   const Alloc &a = Alloc())
        : hash_(h), eq_(eq), alloc_(a)
    {
        if (slots) Allocate(RoundSlots(slots));
    }
    Table(size_type slots, const Alloc &a) : Table(slots, Hash(), KeyEq(), a) {}
    Table(size_type slots, const Hash &h, const Alloc &a) : Table(slots, h, KeyEq(), a) {}
    explicit Table(const Alloc &a) : alloc_(a) {}

    template <class It>
    Table(It first, It last, size_type slots = 0, const Hash &h = Hash(),
          const KeyEq &eq = KeyEq(), const Alloc &a = Alloc())
        : Table(slots, h, eq, a)
    {
        Fill(first, last);
    }
    template <class It>
    Table(It first, It last, size_type slots, const Alloc &a)
        : Table(first, last, slots, Hash(), KeyEq(), a)
    {
    }
    template <class It>
    Table(It first, It last, size_type slots, const Hash &h, const Alloc &a)
        : Table(first, last, slots, h, KeyEq(), a)
    {
    }

    template <std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>>
    Table(std::from_range_t, R &&rg, size_type slots = 0, const Hash &h = Hash(),
          const KeyEq &eq = KeyEq(), const Alloc &a = Alloc())
        : Table(slots, h, eq, a)
    {
        FillRange(static_cast<R &&>(rg));
    }
    template <std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>>
    Table(std::from_range_t, R &&rg, size_type slots, const Alloc &a)
        : Table(std::from_range, static_cast<R &&>(rg), slots, Hash(), KeyEq(), a)
    {
    }
    template <std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>>
    Table(std::from_range_t, R &&rg, size_type slots, const Hash &h, const Alloc &a)
        : Table(std::from_range, static_cast<R &&>(rg), slots, h, KeyEq(), a)
    {
    }

    Table(std::initializer_list<value_type> il, size_type slots = 0, const Hash &h = Hash(),
          const KeyEq &eq = KeyEq(), const Alloc &a = Alloc())
        : Table(il.begin(), il.end(), slots, h, eq, a)
    {
    }
    Table(std::initializer_list<value_type> il, size_type slots, const Alloc &a)
        : Table(il, slots, Hash(), KeyEq(), a)
    {
    }
    Table(std::initializer_list<value_type> il, size_type slots, const Hash &h, const Alloc &a)
        : Table(il, slots, h, KeyEq(), a)
    {
    }

    Table(const Table &other)
        : hash_(other.hash_), eq_(other.eq_),
          alloc_(AllocTraits::select_on_container_copy_construction(other.alloc_))
    {
        CopyFrom(other);
    }
    Table(const Table &other, const Alloc &a) : hash_(other.hash_), eq_(other.eq_), alloc_(a)
    {
        CopyFrom(other);
    }
    Table(Table &&other) noexcept
        : hash_(std::move(other.hash_)), eq_(std::move(other.eq_)),
          alloc_(std::move(other.alloc_))
    {
        AdoptFrom(other);
    }
    Table(Table &&other, const Alloc &a)
        : hash_(std::move(other.hash_)), eq_(std::move(other.eq_)), alloc_(a)
    {
        if (AllocTraits::is_always_equal::value || alloc_ == other.alloc_) AdoptFrom(other);
        else MoveElementsFrom(other);
    }

    ~Table()
    {
        DestroyAll();
        Release();
    }

    Table &operator=(const Table &other)
    {
        if (this == &other) return *this;
        DestroyAll();
        Release();
        if constexpr (AllocTraits::propagate_on_container_copy_assignment::value)
            alloc_ = other.alloc_;
        hash_ = other.hash_;
        eq_   = other.eq_;
        CopyFrom(other);
        return *this;
    }

    Table &operator=(Table &&other) noexcept(AllocTraits::is_always_equal::value &&
                                             std::is_nothrow_move_assignable_v<Hash> &&
                                             std::is_nothrow_move_assignable_v<KeyEq>)
    {
        if (this == &other) return *this;
        constexpr bool pocma = AllocTraits::propagate_on_container_move_assignment::value;
        if (pocma || AllocTraits::is_always_equal::value || alloc_ == other.alloc_) {
            DestroyAll();
            Release();
            if constexpr (pocma) alloc_ = std::move(other.alloc_);
            hash_ = std::move(other.hash_);
            eq_   = std::move(other.eq_);
            AdoptFrom(other);
            return *this;
        }
        // Unequal allocators without propagation: taking other's block would
        // mean freeing it later through an allocator that never owned it.
        clear();
        hash_ = std::move(other.hash_);
        eq_   = std::move(other.eq_);
        MoveElementsFrom(other);
        return *this;
    }

    Table &operator=(std::initializer_list<value_type> il)
    {
        clear();
        Fill(il.begin(), il.end());
        return *this;
    }

    allocator_type get_allocator() const noexcept { return alloc_; }

    // ── iterators ───────────────────────────────────────────────────────────

    iterator       begin() noexcept { return MakeIter(0); }
    iterator       end() noexcept { return MakeIter(cap_); }
    const_iterator begin() const noexcept { return MakeConstIter(0); }
    const_iterator end() const noexcept { return MakeConstIter(cap_); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    // ── capacity ────────────────────────────────────────────────────────────

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    size_type          size() const noexcept { return size_; }
    size_type          max_size() const noexcept
    {
        // Two ceilings, both expressed in ELEMENTS: the ctl word's spare bit
        // caps the slot count at 2^31, and one allocation caps it at whatever
        // the allocator will hand over. The division comes before the multiply
        // so the byte figure never overflows on the way.
        size_type by_ctl   = kMaxSlots - (kMaxSlots >> 3);
        size_type chunks   = std::allocator_traits<ChunkAlloc>::max_size(ChunkAlloc(alloc_));
        size_type slots    = (chunks / SlotBytes()) * kAlign;
        size_type by_alloc = slots - (slots >> 3);
        return by_ctl < by_alloc ? by_ctl : by_alloc;
    }

    // ── hash policy (the slot vocabulary that replaces buckets) ─────────────

    size_type slot_count() const noexcept { return cap_; }
    float     load_factor() const noexcept
    {
        return cap_ ? static_cast<float>(size_) / static_cast<float>(cap_) : 0.0f;
    }
    // Fixed at 7/8 and deliberately not settable: past it a plain table's probe
    // lengths grow without bound, and there is no bucket list to absorb it.
    static constexpr float max_load_factor() noexcept { return 0.875f; }

    // The worst probe distance in the table right now — the honest health
    // metric of a plain table, and the one number that tells a caller whether
    // its hasher is doing its job. O(slot_count).
    size_type max_distance() const noexcept
    {
        size_type worst = 0;
        for (size_type s = 0; s < cap_; ++s)
            if (ctl_[s]) {
                size_type d = DistanceOf(s);
                if (d > worst) worst = d;
            }
        return worst;
    }

    // How many slots a lookup for k inspects before it answers — the cost of
    // that one lookup, hit or miss. Together with max_distance() this is what
    // tells a caller its hasher is failing, instead of leaving it to guess from
    // a stopwatch. Runs the SAME walk find() runs.
    size_type probe_distance(const key_type &k) const
    {
        size_type visited = 0;
        FindSlotImpl<true>(k, hash_(k), visited);
        return visited;
    }
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq>
    size_type probe_distance(const K &k) const
    {
        size_type visited = 0;
        FindSlotImpl<true>(k, hash_(k), visited);
        return visited;
    }

    void rehash(size_type slots)
    {
        size_type need = SlotsForSize(size_);
        size_type want = RoundSlots(slots < need ? need : slots);
        if (want == cap_) return;
        RehashTo(want);
    }
    void reserve(size_type n)
    {
        if (n > max_size()) throw std::length_error("box::flat_hash: reserve past max_size");
        size_type want = SlotsForSize(n);
        if (want > cap_) RehashTo(want);
    }

    // ── lookup ──────────────────────────────────────────────────────────────

    iterator       find(const key_type &k) { return MakeIterAt(FindSlot(k, hash_(k))); }
    const_iterator find(const key_type &k) const { return MakeConstIterAt(FindSlot(k, hash_(k))); }
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq>
    iterator find(const K &k)
    {
        return MakeIterAt(FindSlot(k, hash_(k)));
    }
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq>
    const_iterator find(const K &k) const
    {
        return MakeConstIterAt(FindSlot(k, hash_(k)));
    }

    size_type count(const key_type &k) const { return FindSlot(k, hash_(k)) == npos ? 0 : 1; }
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq>
    size_type count(const K &k) const
    {
        return FindSlot(k, hash_(k)) == npos ? 0 : 1;
    }

    bool contains(const key_type &k) const { return FindSlot(k, hash_(k)) != npos; }
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq>
    bool contains(const K &k) const
    {
        return FindSlot(k, hash_(k)) != npos;
    }

    std::pair<iterator, iterator> equal_range(const key_type &k) { return RangeOf(find(k), end()); }
    std::pair<const_iterator, const_iterator> equal_range(const key_type &k) const
    {
        return RangeOf(find(k), end());
    }
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq>
    std::pair<iterator, iterator> equal_range(const K &k)
    {
        return RangeOf(find(k), end());
    }
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq>
    std::pair<const_iterator, const_iterator> equal_range(const K &k) const
    {
        return RangeOf(find(k), end());
    }

    // ── map-only element access ─────────────────────────────────────────────

    MappedT &at(const key_type &k)
        requires kMap
    {
        size_type s = FindSlot(k, hash_(k));
        if (s == npos) throw std::out_of_range("box::flat_hash_map::at: no such key");
        return vals_[s];
    }
    const MappedT &at(const key_type &k) const
        requires kMap
    {
        size_type s = FindSlot(k, hash_(k));
        if (s == npos) throw std::out_of_range("box::flat_hash_map::at: no such key");
        return vals_[s];
    }
    // The slot lands in a local FIRST. In `vals_[EmplaceKey(k).first]` the
    // built-in subscript sequences vals_ before the call ([expr.sub]), so the
    // values pointer would be loaded before an insertion that grows the table
    // frees the very block it points into — a use-after-free that a plain build
    // does not notice because the freed block is still mapped.
    MappedT &operator[](const key_type &k)
        requires kMap
    {
        size_type s = EmplaceKey(k).first;
        return vals_[s];
    }
    MappedT &operator[](key_type &&k)
        requires kMap
    {
        size_type s = EmplaceKey(std::move(k)).first;
        return vals_[s];
    }

    // ── modifiers ───────────────────────────────────────────────────────────

    std::pair<iterator, bool> insert(const value_type &v)
    {
        if constexpr (kMap) return Wrap(EmplaceKey(v.first, v.second));
        else return Wrap(EmplaceKey(v));
    }
    std::pair<iterator, bool> insert(value_type &&v)
    {
        if constexpr (kMap) return Wrap(EmplaceKey(std::move(v.first), std::move(v.second)));
        else return Wrap(EmplaceKey(std::move(v)));
    }
    // The map's catch-all: anything a value_type can be built from — including
    // pair<const Key, Mapped>, which is what a std::map or an unordered_map
    // hands out.
    template <class P>
        requires kMap && std::is_constructible_v<value_type, P &&>
    std::pair<iterator, bool> insert(P &&p)
    {
        value_type v(static_cast<P &&>(p));
        return Wrap(EmplaceKey(std::move(v.first), std::move(v.second)));
    }
    iterator insert(const_iterator, const value_type &v) { return insert(v).first; }
    iterator insert(const_iterator, value_type &&v) { return insert(std::move(v)).first; }
    template <class P>
        requires kMap && std::is_constructible_v<value_type, P &&>
    iterator insert(const_iterator, P &&p)
    {
        return insert(static_cast<P &&>(p)).first;
    }
    template <class It>
    void insert(It first, It last)
    {
        Fill(first, last);
    }
    void insert(std::initializer_list<value_type> il) { Fill(il.begin(), il.end()); }
    template <std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>>
    void insert_range(R &&rg)
    {
        FillRange(static_cast<R &&>(rg));
    }

    template <class... Args>
    std::pair<iterator, bool> emplace(Args &&...args)
    {
        // [unord.map.modifiers]: the element is constructed first — the key is
        // not knowable before it exists. try_emplace is the member that avoids
        // this when the caller does know the key.
        value_type v(static_cast<Args &&>(args)...);
        if constexpr (kMap) return Wrap(EmplaceKey(std::move(v.first), std::move(v.second)));
        else return Wrap(EmplaceKey(std::move(v)));
    }
    template <class... Args>
    iterator emplace_hint(const_iterator, Args &&...args)
    {
        // A plain table has no position to take advice about; the hint is
        // accepted for source compatibility and ignored, as it must be.
        return emplace(static_cast<Args &&>(args)...).first;
    }

    template <class... Args>
        requires kMap
    std::pair<iterator, bool> try_emplace(const key_type &k, Args &&...args)
    {
        return Wrap(EmplaceKey(k, static_cast<Args &&>(args)...));
    }
    template <class... Args>
        requires kMap
    std::pair<iterator, bool> try_emplace(key_type &&k, Args &&...args)
    {
        return Wrap(EmplaceKey(std::move(k), static_cast<Args &&>(args)...));
    }
    template <class... Args>
        requires kMap
    iterator try_emplace(const_iterator, const key_type &k, Args &&...args)
    {
        return try_emplace(k, static_cast<Args &&>(args)...).first;
    }
    template <class... Args>
        requires kMap
    iterator try_emplace(const_iterator, key_type &&k, Args &&...args)
    {
        return try_emplace(std::move(k), static_cast<Args &&>(args)...).first;
    }

    template <class M>
        requires kMap
    std::pair<iterator, bool> insert_or_assign(const key_type &k, M &&v)
    {
        return AssignOrEmplace(k, static_cast<M &&>(v));
    }
    template <class M>
        requires kMap
    std::pair<iterator, bool> insert_or_assign(key_type &&k, M &&v)
    {
        return AssignOrEmplace(std::move(k), static_cast<M &&>(v));
    }
    template <class M>
        requires kMap
    iterator insert_or_assign(const_iterator, const key_type &k, M &&v)
    {
        return insert_or_assign(k, static_cast<M &&>(v)).first;
    }
    template <class M>
        requires kMap
    iterator insert_or_assign(const_iterator, key_type &&k, M &&v)
    {
        return insert_or_assign(std::move(k), static_cast<M &&>(v)).first;
    }

    iterator erase(const_iterator pos)
    {
        size_type s = pos.Slot(ctl_);
        EraseAt(s);
        return MakeIter(s);  // whatever the shift pulled into this slot
    }
    // The map's iterator and const_iterator are distinct types, so both need a
    // declaration; the set's are the same type and one would redeclare the
    // other, which is why this overload is gated.
    iterator erase(iterator pos)
        requires kMap
    {
        size_type s = pos.Slot(ctl_);
        EraseAt(s);
        return MakeIter(s);
    }
    size_type erase(const key_type &k)
    {
        size_type s = FindSlot(k, hash_(k));
        if (s == npos) return 0;
        EraseAt(s);
        return 1;
    }
    // Heterogeneous erase, with the two exclusions [associative.reqmts] spells
    // out: a K that converts to an iterator must not steal the iterator
    // overload. That guard is load-bearing here in a way it is not for a
    // node-based container — nothing stops a caller's K from being convertible.
    template <class K>
        requires std::__detail::TransparentHashEq<Hash, KeyEq> &&
                 (!std::is_convertible_v<K &&, iterator>) &&
                 (!std::is_convertible_v<K &&, const_iterator>)
    size_type erase(K &&k)
    {
        size_type s = FindSlot(k, hash_(k));
        if (s == npos) return 0;
        EraseAt(s);
        return 1;
    }

    void clear() noexcept
    {
        DestroyAll();
        for (size_type s = 0; s < cap_; ++s) ctl_[s] = 0;
        size_ = 0;
    }

    void swap(Table &other) noexcept(AllocTraits::is_always_equal::value &&
                                     std::is_nothrow_swappable_v<Hash> &&
                                     std::is_nothrow_swappable_v<KeyEq>)
    {
        using std::swap;
        if constexpr (AllocTraits::propagate_on_container_swap::value) swap(alloc_, other.alloc_);
        swap(ctl_, other.ctl_);
        swap(keys_, other.keys_);
        if constexpr (kMap) swap(vals_, other.vals_);
        swap(cap_, other.cap_);
        swap(mask_, other.mask_);
        swap(size_, other.size_);
        swap(hash_, other.hash_);
        swap(eq_, other.eq_);
    }

    // ── observers ───────────────────────────────────────────────────────────

    hasher    hash_function() const { return hash_; }
    key_equal key_eq() const { return eq_; }

    // ── the BoxOS half: box::result instead of a thrown bad_alloc ───────────
    // Every member below can fail for exactly one reason a caller can act on —
    // the heap said no. The std-shaped members above answer that with
    // std::bad_alloc, which carries nothing; these answer with the REAL kernel
    // cause off this strand's own heap cell (box::heap::last_error), so a
    // caller can tell "heap exhausted" from "region poisoned" from "bad
    // argument". A throwing element constructor is NOT an expected failure and
    // propagates untouched — this facet converts allocation failure, not bugs.
    class fallible_facet {
    public:
        explicit fallible_facet(Table &t) noexcept : t_(t) {}

        box::status reserve(size_type n) const
        {
            return Guard([&] { t_.reserve(n); });
        }
        box::status rehash(size_type slots) const
        {
            return Guard([&] { t_.rehash(slots); });
        }
        box::result<std::pair<iterator, bool>> insert(const value_type &v) const
        {
            return Guard([&] { return t_.insert(v); });
        }
        box::result<std::pair<iterator, bool>> insert(value_type &&v) const
        {
            return Guard([&] { return t_.insert(std::move(v)); });
        }
        template <class... Args>
        box::result<std::pair<iterator, bool>> emplace(Args &&...args) const
        {
            return Guard([&] { return t_.emplace(static_cast<Args &&>(args)...); });
        }
        template <class... Args>
            requires kMap
        box::result<std::pair<iterator, bool>> try_emplace(const key_type &k, Args &&...args) const
        {
            return Guard([&] { return t_.try_emplace(k, static_cast<Args &&>(args)...); });
        }
        template <class... Args>
            requires kMap
        box::result<std::pair<iterator, bool>> try_emplace(key_type &&k, Args &&...args) const
        {
            return Guard(
                [&] { return t_.try_emplace(std::move(k), static_cast<Args &&>(args)...); });
        }
        template <class M>
            requires kMap
        box::result<std::pair<iterator, bool>> insert_or_assign(const key_type &k, M &&v) const
        {
            return Guard([&] { return t_.insert_or_assign(k, static_cast<M &&>(v)); });
        }
        template <class M>
            requires kMap
        box::result<std::pair<iterator, bool>> insert_or_assign(key_type &&k, M &&v) const
        {
            return Guard(
                [&] { return t_.insert_or_assign(std::move(k), static_cast<M &&>(v)); });
        }

    private:
        // One conversion point, so every fallible member reports a cause the
        // same way. bad_alloc carries no cause, so the cause is read back from
        // this strand's own heap cell; a sibling strand's success cannot mask
        // it. If that cell is clear (an allocator that is not the boxlib heap),
        // no_memory is the honest fallback — it is what the throw meant.
        template <class Fn>
        static auto Guard(Fn &&fn) -> std::conditional_t<
            std::is_void_v<decltype(fn())>, box::status, box::result<decltype(fn())>>
        {
            try {
                if constexpr (std::is_void_v<decltype(fn())>) {
                    fn();
                    return {};
                } else {
                    return fn();
                }
            } catch (const std::bad_alloc &) {
                box::error e = box::heap::last_error();
                return std::unexpected(e ? e : box::error{box::errc::no_memory});
            } catch (const std::length_error &) {
                return std::unexpected(box::error{box::errc::invalid_argument});
            }
        }

        Table &t_;
    };

    fallible_facet fallible() noexcept { return fallible_facet(*this); }

    // ── engine internals reached by the free functions ──────────────────────

    static constexpr size_type npos = static_cast<size_type>(-1);

    // Slot-order scan that starts at an EMPTY slot. Erasure only ever empties
    // slots, so no run can cross the point we started from, and therefore no
    // element can be relocated past the cursor: every element is visited
    // exactly once even while the scan erases. This is what makes erase_if's
    // "the predicate is applied exactly once per element" true for a table
    // whose runs wrap around the end of the array.
    template <class Pred>
    size_type EraseIf(Pred &&pred)
    {
        if (cap_ == 0 || size_ == 0) return 0;
        size_type origin = 0;
        while (origin < cap_ && ctl_[origin] != 0) ++origin;
        // The load ceiling guarantees an empty slot exists; if the table were
        // somehow full, falling back to 0 still terminates, it merely loses the
        // exactly-once property, so assert the guarantee instead of pretending.
        if (origin == cap_) origin = 0;

        size_type removed = 0;
        for (size_type n = 0; n < cap_; ++n) {
            size_type s = (origin + n) & mask_;
            if (ctl_[s] == 0) continue;
            if (!ApplyPred(pred, s)) continue;
            EraseAt(s);
            ++removed;
            // A live element may have been pulled into s by the shift; re-test
            // this slot before moving on.
            if (ctl_[s] != 0) --n;
        }
        return removed;
    }

    bool KeyedEqual(const Table &other) const
    {
        if (size_ != other.size_) return false;
        for (size_type s = 0; s < cap_; ++s) {
            if (!ctl_[s]) continue;
            size_type o = other.FindSlot(keys_[s], other.hash_(keys_[s]));
            if (o == other.npos) return false;
            if constexpr (kMap)
                if (!(vals_[s] == other.vals_[o])) return false;
        }
        return true;
    }

private:
    template <class, class, class, class, class>
    friend class Table;

    // ── geometry ────────────────────────────────────────────────────────────

    static constexpr size_type SlotBytes()
    {
        size_type b = sizeof(std::uint32_t) + sizeof(Key);
        if constexpr (kMap) b += sizeof(Mapped);
        return b;
    }
    static size_type RoundSlots(size_type want)
    {
        size_type n = 8;
        while (n < want) n <<= 1;
        return n;
    }
    // Smallest power-of-two slot count whose 7/8 ceiling still holds n.
    static size_type SlotsForSize(size_type n)
    {
        size_type c = 8;
        while (c - (c >> 3) < n) c <<= 1;
        return c;
    }
    size_type Limit() const noexcept { return cap_ - (cap_ >> 3); }

    static constexpr std::size_t AlignUp(std::size_t v)
    {
        return (v + (kAlign - 1)) & ~(kAlign - 1);
    }

    void Allocate(size_type slots)
    {
        std::size_t ctl_bytes = AlignUp(slots * sizeof(std::uint32_t));
        std::size_t key_bytes = AlignUp(slots * sizeof(Key));
        std::size_t total     = ctl_bytes + key_bytes;
        if constexpr (kMap) total += AlignUp(slots * sizeof(Mapped));

        ChunkAlloc      ca(alloc_);
        std::size_t     chunks = (total + kAlign - 1) / kAlign;
        unsigned char  *raw =
            reinterpret_cast<unsigned char *>(std::allocator_traits<ChunkAlloc>::allocate(ca, chunks));
        ctl_  = reinterpret_cast<std::uint32_t *>(raw);
        keys_ = reinterpret_cast<Key *>(raw + ctl_bytes);
        if constexpr (kMap) vals_ = reinterpret_cast<Mapped *>(raw + ctl_bytes + key_bytes);
        cap_  = slots;
        mask_ = slots - 1;
        for (size_type s = 0; s < slots; ++s) ctl_[s] = 0;
    }

    void Release() noexcept
    {
        if (!ctl_) return;
        std::size_t ctl_bytes = AlignUp(cap_ * sizeof(std::uint32_t));
        std::size_t key_bytes = AlignUp(cap_ * sizeof(Key));
        std::size_t total     = ctl_bytes + key_bytes;
        if constexpr (kMap) total += AlignUp(cap_ * sizeof(Mapped));
        ChunkAlloc ca(alloc_);
        std::allocator_traits<ChunkAlloc>::deallocate(
            ca, reinterpret_cast<ChunkT *>(ctl_), (total + kAlign - 1) / kAlign);
        ctl_  = nullptr;
        keys_ = nullptr;
        if constexpr (kMap) vals_ = nullptr;
        cap_  = 0;
        mask_ = 0;
    }

    // ── ctl arithmetic ──────────────────────────────────────────────────────

    static std::uint32_t CtlOf(std::size_t h) noexcept
    {
        return static_cast<std::uint32_t>(h) | kTaken;
    }
    size_type IdealOf(std::uint32_t c) const noexcept { return static_cast<size_type>(c) & mask_; }
    size_type DistanceOf(size_type s) const noexcept { return (s - IdealOf(ctl_[s])) & mask_; }

    // ── element construction / destruction ──────────────────────────────────

    template <class... Args>
    void BuildKey(size_type s, Args &&...args)
    {
        KeyAlloc ka(alloc_);
        std::allocator_traits<KeyAlloc>::construct(ka, keys_ + s, static_cast<Args &&>(args)...);
    }
    template <class... Args>
    void BuildMapped(size_type s, Args &&...args)
        requires kMap
    {
        MappedAlloc ma(alloc_);
        std::allocator_traits<MappedAlloc>::construct(ma, vals_ + s,
                                                      static_cast<Args &&>(args)...);
    }
    void DestroySlot(size_type s) noexcept
    {
        KeyAlloc ka(alloc_);
        std::allocator_traits<KeyAlloc>::destroy(ka, keys_ + s);
        if constexpr (kMap) {
            MappedAlloc ma(alloc_);
            std::allocator_traits<MappedAlloc>::destroy(ma, vals_ + s);
        }
    }
    void DestroyAll() noexcept
    {
        if (!ctl_) return;
        for (size_type s = 0; s < cap_; ++s)
            if (ctl_[s]) DestroySlot(s);
    }

    // Whether a relocation inside a shift can throw. When it cannot — every
    // scalar, std::string, every well-behaved type — the restore guard below
    // is not merely unused, it is not compiled.
    static constexpr bool kShiftIsNothrow =
        std::is_nothrow_move_constructible_v<Key> && std::is_nothrow_move_assignable_v<Key> &&
        (!kMap || (std::is_nothrow_move_constructible_v<MappedT> &&
                   std::is_nothrow_move_assignable_v<MappedT>));

    // Restores the invariant the only way it can be restored once a shift has
    // been interrupted: by emptying. Armed explicitly, immediately before the
    // first relocation, so a throw that happens BEFORE anything moved leaves
    // the table untouched.
    //
    using ShiftGuard = RestoreGuard<!kShiftIsNothrow, Table>;

    // ── the three primitives ────────────────────────────────────────────────

    // The one walk. probe_distance() runs it with Count = true, so the two
    // properties this engine claims for its lookups — the robin-hood early
    // exit, and the fingerprint rejecting a foreign key before KeyEq is called
    // — are observable from outside instead of merely asserted in a comment.
    // With Count = false there is no counter and no branch: the visited-slot
    // tally is compiled away entirely.
    template <bool Count, class K>
    size_type FindSlotImpl(const K &k, std::size_t h, size_type &visited) const
    {
        if (cap_ == 0) return npos;
        const std::uint32_t c = CtlOf(h);
        size_type           s = h & mask_;
        size_type           d = 0;
        for (;;) {
            if constexpr (Count) ++visited;
            const std::uint32_t cs = ctl_[s];
            if (cs == 0) return npos;
            if (((s - (static_cast<size_type>(cs) & mask_)) & mask_) < d) return npos;
            if (cs == c && eq_(keys_[s], k)) return s;
            s = (s + 1) & mask_;
            ++d;
        }
    }
    template <class K>
    size_type FindSlot(const K &k, std::size_t h) const
    {
        size_type ignored = 0;
        return FindSlotImpl<false>(k, h, ignored);
    }

    // Builds one slot from the caller's arguments, key first. If the mapped
    // half throws the key is destroyed again, so a half-built slot never
    // survives the call.
    template <class KArg, class... MArgs>
    void BuildSlot(size_type s, KArg &&karg, MArgs &&...margs)
    {
        BuildKey(s, static_cast<KArg &&>(karg));
        if constexpr (kMap) {
            try {
                BuildMapped(s, static_cast<MArgs &&>(margs)...);
            } catch (...) {
                KeyAlloc ka(alloc_);
                std::allocator_traits<KeyAlloc>::destroy(ka, keys_ + s);
                throw;
            }
        }
    }

    // THE placement primitive — every insertion and every rehash goes through
    // this one function, so the map and the set, and the fresh table and the
    // live one, cannot drift apart. The ctl word is supplied rather than the
    // hash, which is what lets a rehash place an element without asking the
    // hasher anything.
    //
    // Returns the slot. Does not touch size_.
    template <class KArg, class... MArgs>
    size_type PlaceWithCtl(std::uint32_t c, KArg &&karg, MArgs &&...margs)
    {
        size_type s = IdealOf(c);
        size_type d = 0;
        while (ctl_[s] != 0 && DistanceOf(s) >= d) {
            s = (s + 1) & mask_;
            ++d;
        }
        if (ctl_[s] == 0) {
            // The free seat: nothing else moves, so a throwing element
            // constructor leaves the table exactly as it was — the strong
            // guarantee, for free.
            BuildSlot(s, static_cast<KArg &&>(karg), static_cast<MArgs &&>(margs)...);
            ctl_[s] = c;
            return s;
        }

        // The seat is taken. Build the newcomer at the run's first FREE slot
        // and only then rotate the run right by one. Two things follow from
        // that order and neither is free otherwise: an argument that names an
        // element of THIS table (m[m.begin()->first], m.try_emplace(k, m.at(j)))
        // is read before anything moves, and a throwing element constructor
        // still leaves the table exactly as it was.
        size_type e = s;
        while (ctl_[e] != 0) e = (e + 1) & mask_;
        BuildSlot(e, static_cast<KArg &&>(karg), static_cast<MArgs &&>(margs)...);

        ShiftGuard guard(*this);
        guard.Arm();
        // Register the newcomer before moving out of it: from here on every
        // slot in [s, e] carries a live ctl word, so an interrupted rotation
        // leaves clear() destroying each element exactly once.
        ctl_[e] = c;
        if constexpr (kMap) {
            Key     held(std::move(keys_[e]));
            MappedT heldv(std::move(vals_[e]));
            RotateRight(s, e);
            keys_[s] = std::move(held);
            vals_[s] = std::move(heldv);
        } else {
            Key held(std::move(keys_[e]));
            RotateRight(s, e);
            keys_[s] = std::move(held);
        }
        ctl_[s] = c;
        guard.Done();
        return s;
    }

    // Moves [s, e) one slot right by assignment; slot e already holds a live
    // (moved-from) object, which is why nothing here constructs.
    void RotateRight(size_type s, size_type e)
    {
        size_type dst = e;
        size_type src = (e - 1) & mask_;
        for (;;) {
            keys_[dst] = std::move(keys_[src]);
            if constexpr (kMap) vals_[dst] = std::move(vals_[src]);
            ctl_[dst] = ctl_[src];
            if (src == s) break;
            dst = src;
            src = (src - 1) & mask_;
        }
    }

    void EraseAt(size_type s)
    {
        ShiftGuard guard(*this);
        size_type  n = (s + 1) & mask_;
        while (ctl_[n] != 0 && DistanceOf(n) != 0) {
            guard.Arm();
            keys_[s] = std::move(keys_[n]);
            if constexpr (kMap) vals_[s] = std::move(vals_[n]);
            ctl_[s] = ctl_[n];
            s       = n;
            n       = (n + 1) & mask_;
        }
        DestroySlot(s);
        ctl_[s] = 0;
        --size_;
        guard.Done();
    }

    // ── insert front-ends ───────────────────────────────────────────────────

    // The single door every insertion goes through: hash once, look once, grow
    // only when the key is genuinely new (a duplicate must never rehash), then
    // place. Returns {slot, inserted}; on a duplicate the arguments are left
    // untouched, which is what makes m.try_emplace(k, std::move(x)) safe to
    // retry.
    template <class KArg, class... MArgs>
    std::pair<size_type, bool> EmplaceKey(KArg &&karg, MArgs &&...margs)
    {
        const std::size_t h = hash_(karg);
        if (cap_) {
            size_type s = FindSlot(karg, h);
            if (s != npos) return {s, false};
        }
        if (size_ + 1 > Limit()) {
            // Growth relocates EVERY element, so an argument that names one of
            // them (m[m.begin()->first]) would be read through a moved-from
            // object afterwards. Materialise first — this costs one move, on
            // the growth path only, which already moves the whole table.
            Key held(static_cast<KArg &&>(karg));
            if constexpr (kMap) {
                MappedT heldv(static_cast<MArgs &&>(margs)...);
                RehashTo(cap_ ? cap_ * 2 : 8);
                size_type s = PlaceWithCtl(CtlOf(h), std::move(held), std::move(heldv));
                ++size_;
                return {s, true};
            } else {
                RehashTo(cap_ ? cap_ * 2 : 8);
                size_type s = PlaceWithCtl(CtlOf(h), std::move(held));
                ++size_;
                return {s, true};
            }
        }
        size_type s =
            PlaceWithCtl(CtlOf(h), static_cast<KArg &&>(karg), static_cast<MArgs &&>(margs)...);
        ++size_;
        return {s, true};
    }

    template <class KArg, class M>
        requires kMap
    std::pair<iterator, bool> AssignOrEmplace(KArg &&karg, M &&v)
    {
        const std::size_t h = hash_(karg);
        if (cap_) {
            size_type s = FindSlot(karg, h);
            if (s != npos) {
                vals_[s] = static_cast<M &&>(v);
                return {MakeIterAt(s), false};
            }
        }
        if (size_ + 1 > Limit()) {
            Key     held(static_cast<KArg &&>(karg));  // see EmplaceKey: growth relocates
            MappedT heldv(static_cast<M &&>(v));
            RehashTo(cap_ ? cap_ * 2 : 8);
            size_type s = PlaceWithCtl(CtlOf(h), std::move(held), std::move(heldv));
            ++size_;
            return {MakeIterAt(s), true};
        }
        size_type s = PlaceWithCtl(CtlOf(h), static_cast<KArg &&>(karg), static_cast<M &&>(v));
        ++size_;
        return {MakeIterAt(s), true};
    }

    std::pair<iterator, bool> Wrap(std::pair<size_type, bool> r)
    {
        return {MakeIterAt(r.first), r.second};
    }

    // ── growth ──────────────────────────────────────────────────────────────

    // Builds a fresh table, moves everything across, then takes its storage.
    // If a relocation throws, the fresh table dies with the elements it had
    // taken and THIS table keeps its structure — move_if_noexcept is what makes
    // that the strong guarantee for every copyable element.
    void RehashTo(size_type slots)
    {
        Table fresh(0, hash_, eq_, alloc_);
        fresh.Allocate(slots);
        for (size_type s = 0; s < cap_; ++s) {
            if (!ctl_[s]) continue;
            const std::uint32_t c = ctl_[s];
            if constexpr (kMap)
                fresh.PlaceWithCtl(c, std::move_if_noexcept(keys_[s]),
                                   std::move_if_noexcept(vals_[s]));
            else fresh.PlaceWithCtl(c, std::move_if_noexcept(keys_[s]));
            ++fresh.size_;
        }
        // Only the storage moves; hasher, key_eq and allocator stay ours, and
        // fresh's allocator is a copy of ours, so it frees our old block
        // legitimately when it dies.
        std::swap(ctl_, fresh.ctl_);
        std::swap(keys_, fresh.keys_);
        if constexpr (kMap) std::swap(vals_, fresh.vals_);
        std::swap(cap_, fresh.cap_);
        std::swap(mask_, fresh.mask_);
        std::swap(size_, fresh.size_);
    }

    // ── bulk fills ──────────────────────────────────────────────────────────

    template <class It>
    void Fill(It first, It last)
    {
        for (; first != last; ++first) insert(*first);
    }
    template <class R>
    void FillRange(R &&rg)
    {
        if constexpr (std::ranges::sized_range<R>) reserve(size_ + std::ranges::size(rg));
        for (auto &&e : rg) {
            if constexpr (kMap) {
                // A map has to materialise: the key is not knowable until the
                // pair exists, which is what unordered_map's insert_range does
                // too (it goes through emplace).
                value_type v(static_cast<decltype(e) &&>(e));
                EmplaceKey(std::move(v.first), std::move(v.second));
            } else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(e)>, Key>) {
                // A set does NOT have to. Building a Key first would move the
                // range's element even when the key turns out to be a duplicate
                // and nothing is inserted — the same divergence between two
                // halves of one class that insert(Key&&) already avoids.
                EmplaceKey(static_cast<decltype(e) &&>(e));
            } else {
                // A range of something else (const char* into a string set):
                // the hasher only speaks Key, so one must be built.
                EmplaceKey(Key(static_cast<decltype(e) &&>(e)));
            }
        }
    }
    void CopyFrom(const Table &other)
    {
        if (other.size_ == 0) return;
        Allocate(SlotsForSize(other.size_));
        try {
            for (size_type s = 0; s < other.cap_; ++s) {
                if (!other.ctl_[s]) continue;
                if constexpr (kMap)
                    PlaceWithCtl(other.ctl_[s], other.keys_[s], other.vals_[s]);
                else PlaceWithCtl(other.ctl_[s], other.keys_[s]);
                ++size_;
            }
        } catch (...) {
            // From a CONSTRUCTOR caller ~Table does not run, so a throwing copy
            // mid-loop would leak the block and everything built so far.
            DestroyAll();
            for (size_type s = 0; s < cap_; ++s) ctl_[s] = 0;
            size_ = 0;
            Release();
            throw;
        }
    }
    void MoveElementsFrom(Table &other)
    {
        if (other.size_ == 0) return;
        if (cap_ == 0) Allocate(SlotsForSize(other.size_));
        try {
            for (size_type s = 0; s < other.cap_; ++s) {
                if (!other.ctl_[s]) continue;
                if constexpr (kMap)
                    EmplaceKey(std::move(other.keys_[s]), std::move(other.vals_[s]));
                else EmplaceKey(std::move(other.keys_[s]));
            }
        } catch (...) {
            DestroyAll();
            for (size_type s = 0; s < cap_; ++s) ctl_[s] = 0;
            size_ = 0;
            Release();
            throw;
        }
        other.clear();
    }
    void AdoptFrom(Table &other) noexcept
    {
        ctl_  = other.ctl_;
        keys_ = other.keys_;
        if constexpr (kMap) vals_ = other.vals_;
        cap_        = other.cap_;
        mask_       = other.mask_;
        size_       = other.size_;
        other.ctl_  = nullptr;
        other.keys_ = nullptr;
        if constexpr (kMap) other.vals_ = nullptr;
        other.cap_  = 0;
        other.mask_ = 0;
        other.size_ = 0;
    }

    // ── iterator construction ───────────────────────────────────────────────

    iterator MakeIter(size_type s) noexcept
    {
        if constexpr (kMap) return iterator(ctl_ + s, ctl_ + cap_, keys_ + s, vals_ + s);
        else return iterator(ctl_ + s, ctl_ + cap_, keys_ + s, NoValue{});
    }
    const_iterator MakeConstIter(size_type s) const noexcept
    {
        if constexpr (kMap) return const_iterator(ctl_ + s, ctl_ + cap_, keys_ + s, vals_ + s);
        else return const_iterator(ctl_ + s, ctl_ + cap_, keys_ + s, NoValue{});
    }
    iterator       MakeIterAt(size_type s) noexcept { return MakeIter(s == npos ? cap_ : s); }
    const_iterator MakeConstIterAt(size_type s) const noexcept
    {
        return MakeConstIter(s == npos ? cap_ : s);
    }
    // The end-of-range has to be compared against THIS table's end(), not
    // against a default-constructed cursor: a not-found find() hands back
    // end(), whose ctl pointer is one past the control array and is nothing
    // like a value-initialised null one. Incrementing it walked off the end of
    // the ctl array — and because all three arrays share ONE block, that walk
    // lands in the key array rather than in a redzone, so it reads live memory
    // and hands the caller a NON-EMPTY range for an absent key. A sanitizer
    // cannot see it; only comparing against the right sentinel can.
    template <class It>
    static std::pair<It, It> RangeOf(It it, It last)
    {
        if (it == last) return {it, it};
        It next = it;
        ++next;
        return {it, next};
    }

    template <class Pred>
    bool ApplyPred(Pred &pred, size_type s)
    {
        if constexpr (kMap) return pred(reference(keys_[s], vals_[s]));
        else return pred(static_cast<const Key &>(keys_[s]));
    }

    std::uint32_t                     *ctl_  = nullptr;
    Key                               *keys_ = nullptr;
    [[no_unique_address]] MappedStore  vals_{};
    size_type                          cap_  = 0;
    size_type                          mask_ = 0;
    size_type                          size_ = 0;
    [[no_unique_address]] Hash         hash_{};
    [[no_unique_address]] KeyEq        eq_{};
    [[no_unique_address]] Alloc        alloc_{};
};

}  // namespace __flat_hash
}  // namespace box

#endif  // BOXCXX_BOX_FLAT_HASH_TABLE_H
