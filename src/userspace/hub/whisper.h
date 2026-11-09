/* whisper.h — what the line at the prompt could go on to be.
 *
 * The editor lays it dim past the caret; Tab takes it a word at a time,
 * Shift+Tab all of it (box/print.h, LineWhisper). What is whispered, the first
 * that has anything:
 *
 *   - `them` — `it` when that is one file — on an empty word where the command
 *     still needs files and the last command gave some: what was just given
 *     is the likeliest thing to go on with;
 *   - the newest line typed — in this Use Context first, when the person set
 *     the hub so — that begins with the line;
 *   - a name the word being typed can be: a command's for the first word, and
 *     after it what the command takes — files where its Form takes things and
 *     after `->`, tags where it takes words. The Form is sorted by BoxLib's own
 *     filler over what the line can see, so a word is taken here as it will be
 *     at Enter, and only what the command still takes is offered — on an empty
 *     word, only what it still needs. Of several names, the one typed most
 *     lately wins, then the shortest. An empty word gets a name only when it
 *     is the one typed most lately, or the only one.
 *
 * Tab with nothing whispered says what the command still needs: the fields of
 * its Form the line has not filled, or what a built-in takes. Up and Down walk
 * the history. One Whisper for each line: made before it is read, gone after.
 * The line is read as typing.h reads it. */
#ifndef HUB_WHISPER_H
#define HUB_WHISPER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "box/print.h"

#include "held.h"
#include "typing.h"
#include "view.h"

namespace hub {

struct Hub;

class Whisper {
public:
    // For a line begun in the Use Context Context ("" for none).
    Whisper(Hub &Self, std::string Context);
    Whisper(const Whisper &) = delete;
    Whisper &operator=(const Whisper &) = delete;

    // What the editor is handed.
    LineWhisper *Hook() noexcept { return &Hooked.Line; }

private:
    // What the word being typed can be.
    struct Offer {
        bool Commands   = false;   // a command's name
        bool Programs   = false;   // a program's file
        bool Files      = false;   // a file's name
        bool Tags       = false;   // a tag
        bool Everywhere = false;   // the files and tags of the whole volume, not of the Use Context
        bool Them       = false;   // `them` or `it` can stand here
        bool Several    = false;   // and several files, not only one
        bool Signed     = false;   // a + or - may come before it

        // Widened by what a field of a Form takes: files or tags.
        void Take(std::uint8_t Kind) noexcept;
        // The shelves its names stand on.
        std::vector<View::Shelf> Shelves() const;
    };
    // The editor's whisperer, and the way back to this one: the editor hands
    // its whisperer back, and it begins with what the editor reads.
    struct Hooking {
        LineWhisper Line;
        Whisper    *Owner;
    };
    struct Recent {
        std::string_view Word;   // into the history, while its count stays
        std::size_t      Back;   // the line it was typed in, 0 the newest
    };
    enum class Sorted { Done, Refused, Failed };

    static char *ContinueHook(LineWhisper *Line, const char *Typed, std::uint32_t Length);
    static char *NeedsHook(LineWhisper *Line, const char *Typed, std::uint32_t Length);
    static char *RecallHook(LineWhisper *Line, std::uint32_t Back);

    void        Fresh();
    std::string Continue(std::string_view Line);
    std::string Needs(std::string_view Line);
    std::string Recall(std::uint32_t Back);

    bool        HoldForm(std::string_view Name);
    Sorted      SortForm(std::string_view Words, std::vector<bool> &Filled, std::string *Refusal);
    void        OfferProgram(const Typing &Line, std::string_view Name, Offer &Out);
    Offer       MakeOffer(const Typing &Line);
    bool        Named(const Offer &Offered, std::string_view Word);
    void        ReadRecent();
    bool        Latest(const Offer &Offered, std::string_view Typed, bool Quoted, std::string_view &Out);
    bool        Shortest(const Offer &Offered, std::string_view Typed, bool Quoted, std::string_view &Out,
                         std::size_t &Kinds);
    bool        Choose(const Offer &Offered, std::string_view Typed, bool Quoted, std::string_view &Out);
    std::string FromThem(const Offer &Offered) const;
    std::string FromHistory(std::string_view Line) const;
    std::string FromNames(const Typing &Line, const Offer &Offered);
    std::string FormNeeds(std::string_view Name, std::string_view Words);

    Hooking                  Hooked;
    Hub                     &Self;
    std::string              Context;
    View                     Seen;
    std::string              FormOf;          // the command whose Form was asked for
    bool                     FormAsked = false;
    HeldForm                 Form;
    std::vector<Recent>      Words;           // every word of the history, each once, in byte order
    bool                     WordsRead = false;
    std::vector<std::size_t> Walk;            // Up's order, into the history
    std::size_t              SeenLines = static_cast<std::size_t>(-1);
};

}  // namespace hub

#endif
