#ifndef BOXCXX_BOX_LUGGAGE_H
#define BOXCXX_BOX_LUGGAGE_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "box/luggage.h"

namespace box {

struct luggage {
    static std::string_view line() noexcept
    {
        Luggage l = ::luggage();
        return l.bytes ? std::string_view(l.bytes, l.length) : std::string_view{};
    }

    static std::size_t size() noexcept { return ::luggage_word_count(); }
    static bool        empty() noexcept { return size() == 0; }

    static std::string_view word(std::size_t i) noexcept
    {
        if (i > UINT32_MAX) return {};
        const char *w = ::luggage_word(static_cast<std::uint32_t>(i));
        return w ? std::string_view(w) : std::string_view{};
    }

    static std::string_view tail(std::size_t from) noexcept
    {
        if (from > UINT32_MAX) return {};
        return std::string_view(::luggage_tail(static_cast<std::uint32_t>(from)));
    }

    static std::vector<std::string_view> words()
    {
        std::vector<std::string_view> out;
        std::size_t                   n = size();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(word(i));
        return out;
    }
};

}

#endif