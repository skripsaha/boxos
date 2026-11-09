/* tags.cpp — a comma-joined list of tags walked one tag at a time. */

#include "tags.h"

namespace hub::tags {

namespace {

// The tag at the front of Rest, taken off it with its comma; false when Rest
// is spent.
bool Next(std::string_view &Rest, std::string_view &Tag) noexcept
{
    if (Rest.empty()) return false;
    const std::size_t Comma = Rest.find(',');
    Tag = Rest.substr(0, Comma);
    Rest.remove_prefix(Comma == std::string_view::npos ? Rest.size() : Comma + 1);
    return true;
}

}  // namespace

std::vector<std::string_view> Split(std::string_view List)
{
    std::vector<std::string_view> Found;
    for (std::string_view Tag; Next(List, Tag);) Found.push_back(Tag);
    return Found;
}

std::size_t Count(std::string_view List) noexcept
{
    std::size_t Found = 0;
    for (std::string_view Tag; Next(List, Tag);) Found++;
    return Found;
}

bool Holds(std::string_view List, std::string_view Tag) noexcept
{
    for (std::string_view One; Next(List, One);)
        if (One == Tag) return true;
    return false;
}

bool Wears(std::string_view List, std::string_view Key) noexcept
{
    for (std::string_view One; Next(List, One);)
        if (One.substr(0, One.find(':')) == Key) return true;
    return false;
}

bool Same(std::string_view Left, std::string_view Right) noexcept
{
    if (Count(Left) != Count(Right)) return false;
    for (std::string_view One; Next(Left, One);)
        if (!Holds(Right, One)) return false;
    return true;
}

}  // namespace hub::tags
