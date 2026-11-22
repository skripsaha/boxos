#ifndef KEEP_H
#define KEEP_H

/* keep — a program's small file on the volume: a record, a list.
 *
 * A program's own is found by its own words — its tags, which no other
 * program meets — on the whole volume, and made wearing them: the Use Context
 * is the person's words, and whatever the person has said neither hides the
 * file nor stamps it. A file programs keep together — the gardener writes the
 * plants the garden shows — is found instead by what it begins with
 * (Beginning), which the volume keeps on every file's card, and made wearing
 * no tag: nobody's words are asked for. Of the files that begin so, the one
 * called by the name, the last made. What is written is settled on the medium
 * before the call returns. A refusal is a sentence for the person. */

#include <optional>
#include <string>
#include <string_view>

struct Keep {
    const char *Name;                /* "tetris.record" */
    const char *Tags;                /* "tetris,record": the program's own, every one of them, to be found by */
    const char *Beginning = nullptr; /* or what a file programs share begins with: "garden 1\n" */
};

/* The file's text; empty when there is no file yet. */
std::optional<std::string> KeepRead(const Keep &File, std::string &Text);

/* The file's text replaced by `Text`, the file made when there is none. */
std::optional<std::string> KeepWrite(const Keep &File, std::string_view Text);

/* The Use Context as the kernel holds it, comma-joined: "code,cpp"; "" for
 * none, and when the kernel would not say. */
std::string KeepContext();

#endif
