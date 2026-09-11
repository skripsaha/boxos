#ifndef BOXCXX_BOX_USE_H
#define BOXCXX_BOX_USE_H

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "box/use.h"
#include "box/cxx/error.h"

namespace box {

struct use {
    static result<bool> set(std::string_view list)
    {
        std::string owned(list);
        bool        kept = false;
        int         rc   = ::use_set(owned.c_str(), &kept);
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return kept;
    }

    static result<bool> set(std::initializer_list<std::string_view> tags)
    {
        std::string list;
        for (std::string_view t : tags) {
            if (t.empty()) continue;
            if (!list.empty()) list.push_back(',');
            list.append(t.data(), t.size());
        }
        return set(list);
    }

    static result<bool> clear()
    {
        bool kept = false;
        int  rc   = ::use_clear(&kept);
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return kept;
    }

    static std::vector<std::string> current()
    {
        std::vector<std::string> out;
        std::size_t              need = 0;
        int                      n    = ::use_get(nullptr, 0, &need);
        if (n < 0 || need == 0) return out;

        std::string list(need, '\0');
        n = ::use_get(list.data(), list.size(), nullptr);
        if (n <= 0) return out;
        list.resize(std::char_traits<char>::length(list.c_str()));

        out.reserve(static_cast<std::size_t>(n));
        std::size_t start = 0;
        for (;;) {
            std::size_t comma = list.find(',', start);
            out.emplace_back(list, start, comma == std::string::npos ? std::string::npos : comma - start);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

    static bool active()
    {
        std::size_t need = 0;
        return ::use_get(nullptr, 0, &need) >= 0 && need > 1;
    }
};

}

#endif