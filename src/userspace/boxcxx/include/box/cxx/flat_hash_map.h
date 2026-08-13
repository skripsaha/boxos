// boxcxx — box::flat_hash_map  (an open-addressed, robin-hood key→value map)
//
// The plain-table counterpart to std::unordered_map. Same vocabulary of
// members, minus the two families a plain table does not physically have —
// buckets (bucket_count / bucket / bucket_size / local_iterator) and node
// handles (extract / insert(node_type) / merge, which the standard specifies
// in terms of extract) — plus a slot vocabulary that replaces them and a
// box::result face for the one failure a caller can act on.
//
//     box::flat_hash_map<int, Mesh> m;
//     m.try_emplace(id, mesh);
//     if (auto *e = m.find(id); e != m.end()) draw(e->second);
//
//     box::tagged_resource res("render:index");        // whole map accounted
//     box::pmr::flat_hash_map<int, Mesh> tagged(&res); // under one heap tag
//
//     if (auto r = m.fallible().reserve(4096); !r)     // the REAL cause,
//         box::println("{}", r.error());               // not bad_alloc
//
// ‼ TWO THINGS THAT DIFFER FROM unordered_map AND CANNOT BE MISSED:
//
//   1. NOTHING IS STABLE. Any insert, erase or rehash relocates elements, so
//      every iterator, pointer and reference into the map is invalidated by
//      any mutation. unordered_map keeps node addresses stable and that is a
//      real guarantee to give up; the return is one allocation instead of one
//      per element, and a probe that stays in cache.
//
//   2. value_type is pair<Key, T>, NOT pair<const Key, T>. A relocating table
//      has to move its keys, and pair<const Key,T> cannot be moved from. This
//      is the same conclusion std::flat_map reaches for the same reason, and
//      it is why *it hands back a PROXY pair<const Key&, T&>:
//
//          for (auto [k, v] : m) v += 1;     // ok — v is T&
//          for (auto &&[k, v] : m) v += 1;   // ok
//          for (auto &[k, v] : m) ...        // ill-formed: *it is a prvalue
//
//      The key half of the proxy is const, so a key can never be mutated
//      through an iterator and the table's invariant cannot be broken from
//      the outside.
//
// Bulk removal goes through box::erase_if, which is exactly-once even though
// erasure relocates (see the engine banner for why a hand-written erase-while-
// iterating loop is not).
#ifndef BOXCXX_BOX_FLAT_HASH_MAP_H
#define BOXCXX_BOX_FLAT_HASH_MAP_H

#include <memory_resource>

#include "box/cxx/flat_hash_table.h"

namespace box {

namespace __flat_hash {

// Key/mapped extraction for the deduction guides — same shape as the
// __ht::Deduct* family <unordered_map> uses for its own guides.
template <class It>
using DeductKey = std::remove_const_t<typename std::iter_value_t<It>::first_type>;
template <class It>
using DeductMapped = typename std::iter_value_t<It>::second_type;
template <class R>
using DeductRangeKey =
    std::remove_const_t<typename std::ranges::range_value_t<R>::first_type>;
template <class R>
using DeductRangeMapped = typename std::ranges::range_value_t<R>::second_type;

}  // namespace __flat_hash

template <class Key, class T, class Hash = std::hash<Key>,
          class KeyEq = std::equal_to<Key>, class Alloc = std::allocator<std::pair<Key, T>>>
class flat_hash_map : public __flat_hash::Table<Key, T, Hash, KeyEq, Alloc> {
    using Base = __flat_hash::Table<Key, T, Hash, KeyEq, Alloc>;

public:
    using mapped_type = T;

    flat_hash_map() = default;
    using Base::Base;
    // Brings operator=(initializer_list); the implicitly declared copy and
    // move assignments of this class are exact matches and keep winning for
    // same-type assignment.
    using Base::operator=;
};

template <class Key, class T, class Hash, class KeyEq, class Alloc>
bool operator==(const flat_hash_map<Key, T, Hash, KeyEq, Alloc> &a,
                const flat_hash_map<Key, T, Hash, KeyEq, Alloc> &b)
{
    return a.KeyedEqual(b);
}

template <class Key, class T, class Hash, class KeyEq, class Alloc>
void swap(flat_hash_map<Key, T, Hash, KeyEq, Alloc> &a,
          flat_hash_map<Key, T, Hash, KeyEq, Alloc> &b) noexcept(noexcept(a.swap(b)))
{
    a.swap(b);
}

// The sanctioned bulk removal. pred is applied EXACTLY ONCE per element, and
// it receives the map's proxy reference pair<const Key&, T&> — the same shape
// std::flat_map's erase_if hands over, and the reason the predicate must take
// auto&& or the proxy type rather than pair<const Key,T>&.
template <class Key, class T, class Hash, class KeyEq, class Alloc, class Pred>
typename flat_hash_map<Key, T, Hash, KeyEq, Alloc>::size_type
erase_if(flat_hash_map<Key, T, Hash, KeyEq, Alloc> &m, Pred pred)
{
    return m.EraseIf(pred);
}

// ── deduction guides ────────────────────────────────────────────────────────
// Iterator-pair, from_range_t and initializer_list forms, each with the
// allocator-extended variants. The trailing functor slots are NotAllocatorLike
// so an allocator argument lands in the allocator guide and never deduces a
// hasher — the disambiguation <unordered_map>'s guides already document.

template <class It, std::NotAllocatorLike Hash = std::hash<__flat_hash::DeductKey<It>>,
          std::NotAllocatorLike KeyEq = std::equal_to<__flat_hash::DeductKey<It>>,
          class Alloc = std::allocator<
              std::pair<__flat_hash::DeductKey<It>, __flat_hash::DeductMapped<It>>>>
flat_hash_map(It, It, std::size_t = 0, Hash = Hash(), KeyEq = KeyEq(), Alloc = Alloc())
    -> flat_hash_map<__flat_hash::DeductKey<It>, __flat_hash::DeductMapped<It>, Hash, KeyEq,
                     Alloc>;

template <class It, std::AllocatorLike Alloc>
flat_hash_map(It, It, std::size_t, Alloc)
    -> flat_hash_map<__flat_hash::DeductKey<It>, __flat_hash::DeductMapped<It>,
                     std::hash<__flat_hash::DeductKey<It>>,
                     std::equal_to<__flat_hash::DeductKey<It>>, Alloc>;

template <class It, class Hash, std::AllocatorLike Alloc>
flat_hash_map(It, It, std::size_t, Hash, Alloc)
    -> flat_hash_map<__flat_hash::DeductKey<It>, __flat_hash::DeductMapped<It>, Hash,
                     std::equal_to<__flat_hash::DeductKey<It>>, Alloc>;

template <std::ranges::input_range R,
          std::NotAllocatorLike Hash = std::hash<__flat_hash::DeductRangeKey<R>>,
          std::NotAllocatorLike KeyEq = std::equal_to<__flat_hash::DeductRangeKey<R>>,
          class Alloc = std::allocator<std::pair<__flat_hash::DeductRangeKey<R>,
                                                 __flat_hash::DeductRangeMapped<R>>>>
flat_hash_map(std::from_range_t, R &&, std::size_t = 0, Hash = Hash(), KeyEq = KeyEq(),
              Alloc = Alloc())
    -> flat_hash_map<__flat_hash::DeductRangeKey<R>, __flat_hash::DeductRangeMapped<R>, Hash,
                     KeyEq, Alloc>;

template <std::ranges::input_range R, std::AllocatorLike Alloc>
flat_hash_map(std::from_range_t, R &&, std::size_t, Alloc)
    -> flat_hash_map<__flat_hash::DeductRangeKey<R>, __flat_hash::DeductRangeMapped<R>,
                     std::hash<__flat_hash::DeductRangeKey<R>>,
                     std::equal_to<__flat_hash::DeductRangeKey<R>>, Alloc>;

template <std::ranges::input_range R, class Hash, std::AllocatorLike Alloc>
flat_hash_map(std::from_range_t, R &&, std::size_t, Hash, Alloc)
    -> flat_hash_map<__flat_hash::DeductRangeKey<R>, __flat_hash::DeductRangeMapped<R>, Hash,
                     std::equal_to<__flat_hash::DeductRangeKey<R>>, Alloc>;

template <class Key, class T, std::NotAllocatorLike Hash = std::hash<Key>,
          std::NotAllocatorLike KeyEq = std::equal_to<Key>,
          class Alloc = std::allocator<std::pair<Key, T>>>
flat_hash_map(std::initializer_list<std::pair<Key, T>>, std::size_t = 0, Hash = Hash(),
              KeyEq = KeyEq(), Alloc = Alloc()) -> flat_hash_map<Key, T, Hash, KeyEq, Alloc>;

template <class Key, class T, std::AllocatorLike Alloc>
flat_hash_map(std::initializer_list<std::pair<Key, T>>, std::size_t, Alloc)
    -> flat_hash_map<Key, T, std::hash<Key>, std::equal_to<Key>, Alloc>;

template <class Key, class T, class Hash, std::AllocatorLike Alloc>
flat_hash_map(std::initializer_list<std::pair<Key, T>>, std::size_t, Hash, Alloc)
    -> flat_hash_map<Key, T, Hash, std::equal_to<Key>, Alloc>;

// ── box::pmr — the whole map on one memory_resource ─────────────────────────
// Bind it to a box::tagged_resource and every byte the map owns is accounted
// under one BoxOS heap tag; the single-block layout means that is ONE tagged
// allocation, not one per element.
namespace pmr {

template <class Key, class T, class Hash = std::hash<Key>, class KeyEq = std::equal_to<Key>>
using flat_hash_map =
    box::flat_hash_map<Key, T, Hash, KeyEq, std::pmr::polymorphic_allocator<std::pair<Key, T>>>;

}  // namespace pmr

}  // namespace box

#endif  // BOXCXX_BOX_FLAT_HASH_MAP_H
