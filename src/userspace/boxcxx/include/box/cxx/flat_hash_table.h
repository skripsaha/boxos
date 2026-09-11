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

#include "box/cxx/error.h"
#include "box/cxx/heap.h"

namespace box {

namespace __flat_hash {

struct NoValue {};

template <bool HasValue, class KeyRef, class ValRef>
struct RefOf {
    using type = std::pair<KeyRef, ValRef>;
};
template <class KeyRef, class ValRef>
struct RefOf<false, KeyRef, ValRef> {
    using type = KeyRef;
};

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
    using type = std::pair<Key, Mapped>;
};
template <class Key, class Mapped>
struct ValueOf<false, Key, Mapped> {
    using type = Key;
};

template <class KeyPtr, class ValPtr, class KeyRef, class ValRef, class Tag>
class Tandem {
    static constexpr bool kHasValue = !std::is_void_v<ValPtr>;
    using ValStore = std::conditional_t<kHasValue, ValPtr, NoValue>;

public:
    using value_type =
        typename ValueOf<kHasValue, std::remove_cv_t<std::remove_pointer_t<KeyPtr>>,
                         std::remove_cv_t<std::remove_pointer_t<ValStore>>>::type;
    using difference_type = std::ptrdiff_t;
    using reference       = typename RefOf<kHasValue, KeyRef, ValRef>::type;
    using iterator_concept = std::forward_iterator_tag;

    Tandem() = default;

    Tandem(const std::uint32_t *ctl, const std::uint32_t *ctl_end, KeyPtr key, ValStore val)
        : ctl_(ctl), ctl_end_(ctl_end), key_(key), val_(val)
    {
        Settle();
    }

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

    using RvalRef = typename RefOf<kHasValue, typename RvalOf<KeyRef>::type,
                                   typename RvalOf<ValRef>::type>::type;
    friend RvalRef iter_move(const Tandem &c)
    {
        if constexpr (kHasValue) return RvalRef(std::move(*c.key_), std::move(*c.val_));
        else return std::move(*c.key_);
    }

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

template <std::size_t A>
struct alignas(A) Chunk {
    unsigned char raw[A];
};

template <class Key, class Mapped, class Hash, class KeyEq, class Alloc>
class Table {
    static constexpr bool kMap = !std::is_void_v<Mapped>;

    using MappedStore = std::conditional_t<kMap, Mapped *, NoValue>;
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


    iterator       begin() noexcept { return MakeIter(0); }
    iterator       end() noexcept { return MakeIter(cap_); }
    const_iterator begin() const noexcept { return MakeConstIter(0); }
    const_iterator end() const noexcept { return MakeConstIter(cap_); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }


    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    size_type          size() const noexcept { return size_; }
    size_type          max_size() const noexcept
    {
        size_type by_ctl   = kMaxSlots - (kMaxSlots >> 3);
        size_type chunks   = std::allocator_traits<ChunkAlloc>::max_size(ChunkAlloc(alloc_));
        size_type slots    = (chunks / SlotBytes()) * kAlign;
        size_type by_alloc = slots - (slots >> 3);
        return by_ctl < by_alloc ? by_ctl : by_alloc;
    }


    size_type slot_count() const noexcept { return cap_; }
    float     load_factor() const noexcept
    {
        return cap_ ? static_cast<float>(size_) / static_cast<float>(cap_) : 0.0f;
    }
    static constexpr float max_load_factor() noexcept { return 0.875f; }

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
        value_type v(static_cast<Args &&>(args)...);
        if constexpr (kMap) return Wrap(EmplaceKey(std::move(v.first), std::move(v.second)));
        else return Wrap(EmplaceKey(std::move(v)));
    }
    template <class... Args>
    iterator emplace_hint(const_iterator, Args &&...args)
    {
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
        return MakeIter(s);
    }
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


    hasher    hash_function() const { return hash_; }
    key_equal key_eq() const { return eq_; }

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


    static constexpr size_type npos = static_cast<size_type>(-1);

    template <class Pred>
    size_type EraseIf(Pred &&pred)
    {
        if (cap_ == 0 || size_ == 0) return 0;
        size_type origin = 0;
        while (origin < cap_ && ctl_[origin] != 0) ++origin;
        if (origin == cap_) origin = 0;

        size_type removed = 0;
        for (size_type n = 0; n < cap_; ++n) {
            size_type s = (origin + n) & mask_;
            if (ctl_[s] == 0) continue;
            if (!ApplyPred(pred, s)) continue;
            EraseAt(s);
            ++removed;
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


    static std::uint32_t CtlOf(std::size_t h) noexcept
    {
        return static_cast<std::uint32_t>(h) | kTaken;
    }
    size_type IdealOf(std::uint32_t c) const noexcept { return static_cast<size_type>(c) & mask_; }
    size_type DistanceOf(size_type s) const noexcept { return (s - IdealOf(ctl_[s])) & mask_; }


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

    static constexpr bool kShiftIsNothrow =
        std::is_nothrow_move_constructible_v<Key> && std::is_nothrow_move_assignable_v<Key> &&
        (!kMap || (std::is_nothrow_move_constructible_v<MappedT> &&
                   std::is_nothrow_move_assignable_v<MappedT>));

    using ShiftGuard = RestoreGuard<!kShiftIsNothrow, Table>;


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
            BuildSlot(s, static_cast<KArg &&>(karg), static_cast<MArgs &&>(margs)...);
            ctl_[s] = c;
            return s;
        }

        size_type e = s;
        while (ctl_[e] != 0) e = (e + 1) & mask_;
        BuildSlot(e, static_cast<KArg &&>(karg), static_cast<MArgs &&>(margs)...);

        ShiftGuard guard(*this);
        guard.Arm();
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


    template <class KArg, class... MArgs>
    std::pair<size_type, bool> EmplaceKey(KArg &&karg, MArgs &&...margs)
    {
        const std::size_t h = hash_(karg);
        if (cap_) {
            size_type s = FindSlot(karg, h);
            if (s != npos) return {s, false};
        }
        if (size_ + 1 > Limit()) {
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
            Key     held(static_cast<KArg &&>(karg));
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
        std::swap(ctl_, fresh.ctl_);
        std::swap(keys_, fresh.keys_);
        if constexpr (kMap) std::swap(vals_, fresh.vals_);
        std::swap(cap_, fresh.cap_);
        std::swap(mask_, fresh.mask_);
        std::swap(size_, fresh.size_);
    }


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
                value_type v(static_cast<decltype(e) &&>(e));
                EmplaceKey(std::move(v.first), std::move(v.second));
            } else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(e)>, Key>) {
                EmplaceKey(static_cast<decltype(e) &&>(e));
            } else {
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

}
}

#endif