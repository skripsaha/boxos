// boxcxx — box::use  (the Use Context: what the person at this machine is doing)
//
// The idiomatic C++ face of box/use.h. The Use Context is ONE per machine, kept
// by the kernel and spoken for by the shell (`use code cpp`) or a program with
// system authority; every station of the system reads it — the scheduler runs
// programs wearing every context tag in its context tier, TagFS narrows queries
// to it and stamps created files with it (box::tagfs::query / create, with the
// _everywhere spellings for the whole volume).
//
//   box::use::set("code,cpp")            — say what the user is doing (system authority)
//   box::use::set({"code", "cpp"})       — the same, from a list
//   box::use::current()                  — the tags, freshly read from the kernel (anyone)
//   box::use::active()                   — is a context set at all
//   box::use::clear()                    — the user is doing nothing in particular (system authority)
//
// The volume remembers the context: set and clear write it to the mounted
// volume, and a machine booting with that volume takes it up again. Their
// value says whether the volume now holds it — false with no volume up, a
// medium that will not take the write, or a context longer than the volume's
// record holds. The context is set either way.
//
// This is a box:: extension, not part of std. A program without system
// authority gets box::errc::access_denied from set / clear — the context is
// the user's, not a program's.
#ifndef BOXCXX_BOX_USE_H
#define BOXCXX_BOX_USE_H

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "box/use.h"
#include "box/cxx/error.h"  // box::status / box::error

namespace box {

struct use {
    // Replace the context with a comma-separated list. Empty clears. The
    // value: does the volume now remember it.
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

    // The context as tags, in the kernel's order; empty when none is set (or
    // when the kernel could not be asked — a failed ask reads as no context,
    // which is what a caller that adapts to the user wants to see).
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

}  // namespace box

#endif  // BOXCXX_BOX_USE_H
