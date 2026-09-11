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

template <class Key, class Hash, class KeyEq, class Alloc, class Pred>
typename flat_hash_set<Key, Hash, KeyEq, Alloc>::size_type
erase_if(flat_hash_set<Key, Hash, KeyEq, Alloc> &s, Pred pred)
{
    return s.EraseIf(pred);
}


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

}

}

#endif