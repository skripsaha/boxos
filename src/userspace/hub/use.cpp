/* use — say what the person at this machine is doing, in tags.
 *
 *   use code cpp      the Use Context is now {code, cpp}: files are seen and
 *                     created inside it, a program wearing both tags pays a
 *                     third of the scheduler's Fare and one wearing either half
 *   use +draft        draft is added to it
 *   use -cpp          cpp is taken out of it
 *   use +draft -cpp   both, in one word: nobody ever sees one done and not
 *                     the other
 *   use               the context is cleared
 *
 * Plain words say the whole context; + and - change the one that stands. The
 * two do not mix in one line — which of them was meant would be a guess.
 *
 * The volume remembers it: what is said here is written to the mounted volume
 * and taken up again at the next boot with that volume. When the volume
 * cannot remember (no volume, no ground on it for the context, a medium that
 * will not take the write) the context is set all the same and the person is
 * told. The context is the kernel's, one per machine; the hub only speaks for
 * the person, and its prompt reads the context from the kernel before every
 * line. */

#include <optional>
#include <string>

#include "box/error.h"
#include "box/print.h"
#include "box/use.h"

#include "builtins.h"
#include "context.h"
#include "say.h"
#include "words.h"

namespace hub {

namespace {

enum class Asks { Whole, Change, Unreadable };

// What the line asks: the whole context, a change — or it cannot be read,
// and why was said.
Asks ReadLine(const Words &Typed)
{
    bool Plain = false, Signed = false;
    for (std::size_t I = 1; I < Typed.Count(); I++) {
        const std::string_view Word = Typed[I];
        if (Word.front() != '+' && Word.front() != '-') {
            Plain = true;
            continue;
        }
        if (Word.size() == 1) {
            print("use: a sign wants a tag right after it: use +draft, use -cpp\n");
            return Asks::Unreadable;
        }
        Signed = true;
    }
    if (Plain && Signed) {
        print("use: say the whole context, or change it with + and -, not both at once: "
              "use code cpp, or use +draft -cpp\n");
        return Asks::Unreadable;
    }
    return Signed ? Asks::Change : Asks::Whole;
}

// The words that begin with Sign — '+' or '-', the sign left off — or, with
// no sign, the words that begin with neither, comma-joined.
std::string Joined(const Words &Typed, char Sign)
{
    std::string List;
    for (std::size_t I = 1; I < Typed.Count(); I++) {
        std::string_view Word     = Typed[I];
        const bool       SignWord = Word.front() == '+' || Word.front() == '-';
        if (Sign ? Word.front() != Sign : SignWord) continue;
        if (Sign) Word.remove_prefix(1);
        if (!List.empty()) List += ',';
        List += Word;
    }
    return List;
}

}  // namespace

bool Use(Hub &, const Words &Typed, std::string_view)
{
    bool Remembered = false, Unchanged = false;
    int  Rc         = 0;
    if (Typed.Count() == 1) {
        Rc = use_clear(&Remembered);
    } else {
        const Asks Asked = ReadLine(Typed);
        if (Asked == Asks::Unreadable) return false;
        if (Asked == Asks::Whole) {
            Rc = use_set(Joined(Typed, 0).c_str(), &Remembered);
        } else {
            // The context before and after, as the kernel spells it: a change
            // that changed nothing is said so.
            const std::optional<std::string> Before = context::Whole();
            Rc = use_change(Joined(Typed, '+').c_str(), Joined(Typed, '-').c_str(), &Remembered);
            const std::optional<std::string> After = Rc == 0 && Before ? context::Whole() : std::nullopt;
            Unchanged = After && *Before == *After;
        }
    }

    if (Rc < 0) {
        const error_t Why = box_errno_of(Rc);
        if (Why == ERR_ACCESS_DENIED) print("use: only the hub and system programs may set the Use Context\n");
        else if (Why == ERR_INVALID_ARGUMENT || Why == ERR_TAGFS_TAG_TOO_LONG)
            print("use: a tag is longer than a tag may be\n");
        else Say("use: refused (error {})\n", static_cast<int>(Why));
        return false;
    }
    // The volume remembers the context across the night; when it cannot, the
    // person hears it now rather than at the next boot.
    if (Typed.Count() == 1)
        print(Remembered ? "Context cleared\n" : "Context cleared; this volume still remembers the old one\n");
    else if (Unchanged) print("Context unchanged\n");
    else print(Remembered ? "Context set\n" : "Context set; this volume will not remember it\n");
    return true;
}

}  // namespace hub
