// boxcxx — box::flat_hash_set  (an open-addressed, robin-hood key set)
//
// The plain-table counterpart to std::unordered_set, over the SAME engine as
// box::flat_hash_map: a set is that engine with the value array removed, which
// is why the two containers cannot drift apart in probe behaviour, growth
// policy or exception doctrine — there is one implementation of each.
//
//     box::flat_hash_set<int> seen;
//     if (seen.insert(id).second) visit(id);
//
//     box::tagged_resource res("net:seen");
//     box::pmr::flat_hash_set<int> tagged(&res);   // one tagged allocation
//
// Members follow std::unordered_set, minus the bucket family and the node
// handle family a plain table does not have (see <box/cxx/flat_hash_map.h>),
// plus slot_count() / max_distance() / fallible().
//
// ‼ NOTHING IS STABLE: any insert or erase relocates elements, so every
// iterator, pointer and reference is invalidated by any mutation. Unlike the
// map's, this container's iterator hands back a genuine const Key& (there is
// no second half to pair it with), so it is a full forward iterator with a
// real reference type.
//
// iterator and const_iterator are the SAME type — a key is immutable while it
// is in the table — which is why erase is declared once, exactly as
// std::flat_set does it.
#ifndef BOXCXX_BOX_FLAT_HASH_SET_H
#define BOXCXX_BOX_FLAT_HASH_SET_H

#include <memory_resource>

#include "box/cxx/flat_hash_table.h"

namespace box {

template <class Key, class Hash = std::hash<Key>, class KeyEq = std::equal_to<Key>,
          class Alloc = std::allocator<Key>>
class flat_hash_set : public __flat_hash::Table<Key, void, Hash, KeyEq, Alloc> {
    using Base = __flat_hash::Table<Key, void, Hash, KeyEq, Alloc>;

public:
    flat_hash_set() = default;
    using Base::Base;
    using Base::operator=;
};

template <class Key, class Hash, class KeyEq, class Alloc>
bool operator==(const flat_hash_set<Key, Hash, KeyEq, Alloc> &a,
                const flat_hash_set<Key, Hash, KeyEq, Alloc> &b)
{
    return a.KeyedEqual(b);
}

template <class Key, class Hash, class KeyEq, class Alloc>
void swap(flat_hash_set<Key, Hash, KeyEq, Alloc> &a,
          flat_hash_set<Key, Hash, KeyEq, Alloc> &b) noexcept(noexcept(a.swap(b)))
{
    a.swap(b);
}

// pred is applied EXACTLY ONCE per element and receives const Key& — the
// engine's scan starts at an empty slot so no element can be relocated past
// the cursor.
template <class Key, class Hash, class KeyEq, class Alloc, class Pred>
typename flat_hash_set<Key, Hash, KeyEq, Alloc>::size_type
erase_if(flat_hash_set<Key, Hash, KeyEq, Alloc> &s, Pred pred)
{
    return s.EraseIf(pred);
}

// ── deduction guides ────────────────────────────────────────────────────────

template <class It, std::NotAllocatorLike Hash = std::hash<std::iter_value_t<It>>,
          std::NotAllocatorLike KeyEq = std::equal_to<std::iter_value_t<It>>,
          class Alloc = std::allocator<std::iter_value_t<It>>>
flat_hash_set(It, It, std::size_t = 0, Hash = Hash(), KeyEq = KeyEq(), Alloc = Alloc())
    -> flat_hash_set<std::iter_value_t<It>, Hash, KeyEq, Alloc>;

template <class It, std::AllocatorLike Alloc>
flat_hash_set(It, It, std::size_t, Alloc)
    -> flat_hash_set<std::iter_value_t<It>, std::hash<std::iter_value_t<It>>,
                     std::equal_to<std::iter_value_t<It>>, Alloc>;

template <class It, class Hash, std::AllocatorLike Alloc>
flat_hash_set(It, It, std::size_t, Hash, Alloc)
    -> flat_hash_set<std::iter_value_t<It>, Hash, std::equal_to<std::iter_value_t<It>>, Alloc>;

template <std::ranges::input_range R,
          std::NotAllocatorLike Hash = std::hash<std::ranges::range_value_t<R>>,
          std::NotAllocatorLike KeyEq = std::equal_to<std::ranges::range_value_t<R>>,
          class Alloc = std::allocator<std::ranges::range_value_t<R>>>
flat_hash_set(std::from_range_t, R &&, std::size_t = 0, Hash = Hash(), KeyEq = KeyEq(),
              Alloc = Alloc()) -> flat_hash_set<std::ranges::range_value_t<R>, Hash, KeyEq, Alloc>;

template <std::ranges::input_range R, std::AllocatorLike Alloc>
flat_hash_set(std::from_range_t, R &&, std::size_t, Alloc)
    -> flat_hash_set<std::ranges::range_value_t<R>,
                     std::hash<std::ranges::range_value_t<R>>,
                     std::equal_to<std::ranges::range_value_t<R>>, Alloc>;

template <std::ranges::input_range R, class Hash, std::AllocatorLike Alloc>
flat_hash_set(std::from_range_t, R &&, std::size_t, Hash, Alloc)
    -> flat_hash_set<std::ranges::range_value_t<R>, Hash,
                     std::equal_to<std::ranges::range_value_t<R>>, Alloc>;

template <class Key, std::NotAllocatorLike Hash = std::hash<Key>,
          std::NotAllocatorLike KeyEq = std::equal_to<Key>,
          class Alloc = std::allocator<Key>>
flat_hash_set(std::initializer_list<Key>, std::size_t = 0, Hash = Hash(), KeyEq = KeyEq(),
              Alloc = Alloc()) -> flat_hash_set<Key, Hash, KeyEq, Alloc>;

template <class Key, std::AllocatorLike Alloc>
flat_hash_set(std::initializer_list<Key>, std::size_t, Alloc)
    -> flat_hash_set<Key, std::hash<Key>, std::equal_to<Key>, Alloc>;

template <class Key, class Hash, std::AllocatorLike Alloc>
flat_hash_set(std::initializer_list<Key>, std::size_t, Hash, Alloc)
    -> flat_hash_set<Key, Hash, std::equal_to<Key>, Alloc>;

namespace pmr {

template <class Key, class Hash = std::hash<Key>, class KeyEq = std::equal_to<Key>>
using flat_hash_set =
    box::flat_hash_set<Key, Hash, KeyEq, std::pmr::polymorphic_allocator<Key>>;

}  // namespace pmr

}  // namespace box

#endif  // BOXCXX_BOX_FLAT_HASH_SET_H
