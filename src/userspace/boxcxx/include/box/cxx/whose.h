// boxcxx — box::tagfs::whose  (whose tags a call speaks)
//
// Every call that speaks tags says whose first (box/file.h, TagsOf). These are
// the C++ spellings of the same words, kept apart from box/cxx/tagfs.h so that
// a header that only looks a name up — <fstream>, the C stdio — names them
// without the rest:
//
//   my_tags          the program's own words for its own files, which no other
//                    program meets; every program started from a file has them
//   person_tags      the person's labels: seen when the program's box asks
//                    `see-tags`, changed when it asks `change-tags`
//   program_tags(p)  another program's own, seen when it shows them to this one
//   all_tags         every tag this program may see, each with its owner — for
//                    a file's tags, the volume's tags and a name looked up
//
// A call that names no tag — a name looked up alone — asks nothing, whichever
// it says.
#ifndef BOXCXX_BOX_WHOSE_H
#define BOXCXX_BOX_WHOSE_H

#include "box/file.h"

namespace box::tagfs {

using whose = ::TagsOf;
inline constexpr whose my_tags     = MY_TAGS;
inline constexpr whose person_tags = PERSON_TAGS;
inline constexpr whose all_tags    = ALL_TAGS;
inline constexpr whose program_tags(file_id_t program) noexcept { return PROGRAM_TAGS(program); }

// Whose a tag is, as an answer says it: person_owns, or the file of the
// program whose own it is.
inline constexpr file_id_t person_owns = PERSON_OWNS;

// show_tags_to / hide_tags_from: every program at once.
inline constexpr file_id_t every_program = EVERY_PROGRAM;

}  // namespace box::tagfs

#endif  // BOXCXX_BOX_WHOSE_H
