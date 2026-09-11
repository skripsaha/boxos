#ifndef BOXCXX_BOX_FLAT_HASH_MAP_H
#define BOXCXX_BOX_FLAT_HASH_MAP_H

#include <memory_resource>

#include "box/cxx/flat_hash_table.h"

namespace box {

namespace __flat_hash {

template <class It>
using DeductKey = std::remove_const_t<typename std::iter_value_t<It>::first_type>;
template <class It>
using DeductMapped = typename std::iter_value_t<It>::second_type;
template <class R>
using DeductRangeKey =
    std::remove_const_t<typename std::ranges::range_value_t<R>::first_type>;
template <class R>
using DeductRangeMapped = typename std::ranges::range_value_t<R>::second_type;

}

template <class Key, class T, class Hash = std::hash<Key>,
          class KeyEq = std::equal_to<Key>, class Alloc = std::allocator<std::pair<Key, T>>>
class flat_hash_map : public __flat_hash::Table<Key, T, Hash, KeyEq, Alloc> {
    using Base = __flat_hash::Table<Key, T, Hash, KeyEq, Alloc>;

public:
    using mapped_type = T;

    flat_hash_map() = default;
    using Base::Base;
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

template <class Key, class T, class Hash, class KeyEq, class Alloc, class Pred>
typename flat_hash_map<Key, T, Hash, KeyEq, Alloc>::size_type
erase_if(flat_hash_map<Key, T, Hash, KeyEq, Alloc> &m, Pred pred)
{
    return m.EraseIf(pred);
}


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

namespace pmr {

template <class Key, class T, class Hash = std::hash<Key>, class KeyEq = std::equal_to<Key>>
using flat_hash_map =
    box::flat_hash_map<Key, T, Hash, KeyEq, std::pmr::polymorphic_allocator<std::pair<Key, T>>>;

}

}

#endif