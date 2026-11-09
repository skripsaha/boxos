/* tags.h — a list of tags as the kernel and the volume spell one: the tags
 * joined by commas, "code,cpp", "" for none. A tag is bare or carries a value
 * after a colon, "utility" or "log:file". */
#ifndef HUB_TAGS_H
#define HUB_TAGS_H

#include <cstddef>
#include <string_view>
#include <vector>

namespace hub::tags {

// The tags, in the order the list says them.
std::vector<std::string_view> Split(std::string_view List);

std::size_t Count(std::string_view List) noexcept;

// The list holds Tag, spelt as it is.
bool Holds(std::string_view List, std::string_view Tag) noexcept;

// The list holds a tag whose key is Key: Key bare, or with any value.
bool Wears(std::string_view List, std::string_view Key) noexcept;

// The two lists hold the same tags, in whatever order each says them.
bool Same(std::string_view Left, std::string_view Right) noexcept;

}  // namespace hub::tags

#endif
