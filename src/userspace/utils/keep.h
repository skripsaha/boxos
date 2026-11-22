#ifndef KEEP_H
#define KEEP_H

/* keep — a program's own small file on the volume: a record, a list.
 *
 * Found by the program's own words — its tags, which no other program meets —
 * on the whole volume, and made wearing them: the Use Context is the person's
 * words, and whatever the person has said neither hides the file nor stamps
 * it. What is written is settled on the medium before the call returns. A
 * refusal is a sentence for the person. */

#include <optional>
#include <string>
#include <string_view>

struct Keep {
    const char *Name;    /* "tetris.record" */
    const char *Tags;    /* "tetris,record": every one of them, to be found by */
};

/* The file's text; empty when there is no file yet. */
std::optional<std::string> KeepRead(const Keep &File, std::string &Text);

/* The file's text replaced by `Text`, the file made when there is none. */
std::optional<std::string> KeepWrite(const Keep &File, std::string_view Text);

/* The Use Context as the kernel holds it, comma-joined: "code,cpp"; "" for
 * none, and when the kernel would not say. */
std::string KeepContext();

#endif
