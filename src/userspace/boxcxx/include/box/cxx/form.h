// boxcxx — box::form  (what the program takes and what it gives)
//
// The idiomatic C++ face of box/form.h. A program declares its Form once, in
// its image, with BOX_FORM — the same macro, usable from C++ as it stands:
//
//   BOX_FORM("takes things \"the files to tag\"\n"
//            "takes words \"the tags they get\"\n"
//            "gives things\n");
//
// The shell reads it, sorts the words the person typed into the fields and
// starts the program with the fields filled. The program never parses its line:
//
//   box::form::things(0)    — field 0's files, as ids      (thing, things)
//   box::form::words(1)     — field 1's words              (words, name, text, into)
//   box::form::text(1)      — field 1 as one text, words joined by single spaces
//   box::form::souvenir(v)  — files given back to the one who started the program;
//                             the shell keeps them as `them` for the next command
//
// A program started by someone other than the shell has its fields filled by
// BoxLib from the line at the first ask, asking nobody. When the line cannot
// be filled (a field left empty, a name that means more than one file, no
// Form) BoxLib has already said why on the screen, and every ask answers
// box::errc::invalid_argument — the program ends. The spans and views live as
// long as the program.
//
// This is a box:: extension, not part of std.
#ifndef BOXCXX_BOX_FORM_H
#define BOXCXX_BOX_FORM_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "box/form.h"
#include "box/cxx/error.h"  // box::result / box::status

namespace box {

struct form {
    static result<std::span<const file_id_t>> things(std::uint32_t field) noexcept
    {
        const file_id_t *ids   = nullptr;
        std::size_t      count = 0;
        int              rc    = ::form_things(field, &ids, &count);
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return std::span<const file_id_t>(ids, count);
    }

    static result<std::span<const char *const>> words(std::uint32_t field) noexcept
    {
        const char *const *list  = nullptr;
        std::size_t        count = 0;
        int                rc    = ::form_words(field, &list, &count);
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return std::span<const char *const>(list, count);
    }

    static result<std::string_view> text(std::uint32_t field) noexcept
    {
        const char *joined = ::form_text(field);
        if (!joined) return std::unexpected(error{ERR_INVALID_ARGUMENT});
        return std::string_view(joined);
    }

    static status souvenir(std::span<const file_id_t> ids) noexcept
    {
        return _detail::from_status(::souvenir_things(ids.data(), ids.size()));
    }
};

}  // namespace box

#endif  // BOXCXX_BOX_FORM_H
