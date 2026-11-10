/* prompt.cpp — the prompt laid out and said in its colours. */

#include "prompt.h"

#include <algorithm>
#include <string>

#include "box/display.h"
#include "box/print.h"

#include "hub.h"
#include "say.h"
#include "tags.h"

namespace hub {

namespace {

// How much of a line the tags may take: half the console, so the line typed
// always has the other half. Asked of the display once; a console nobody
// draws has no width to ask, and the old line printers' 80 stand in for it.
std::size_t TagsRoom()
{
    static std::size_t Room = 0;
    if (Room == 0) {
        std::uint32_t Cols = 0, Rows = 0;
        Room = display_size(&Cols, &Rows) == 0 && Cols >= 32 ? Cols / 2 : 40;
    }
    return Room;
}

// The tags that fit, whole, and how many more there are: "code,cpp +3".
std::string Fitted(std::string_view Context)
{
    const std::vector<std::string_view> Tags = tags::Split(Context);
    std::string                         Shown;
    std::size_t                         Count = 0;
    for (std::string_view Tag : Tags) {
        if (Count > 0 && Shown.size() + 1 + Tag.size() > TagsRoom()) break;
        if (Count > 0) Shown += ',';
        Shown += Tag;
        Count++;
    }
    if (Count < Tags.size()) Shown += " +" + std::to_string(Tags.size() - Count);
    return Shown;
}

}  // namespace

void ShowPrompt(const Hub &Self, std::string_view Context)
{
    const Look &Shown = Self.Shown;
    if (Shown.Context && !Context.empty()) SayIn(Shown.ContextColor, "[{}] ", Fitted(Context));
    if (Shown.Them && Self.Given.Known() && !Self.Given.Files().empty())
        SayIn(Shown.ThemColor, "{} ", Self.Given.Files().size());
    // "+2", and "+2!" while one of them asks for the keyboard.
    if (Shown.Behind == BehindShown::Prompt) {
        const std::vector<BehindOne> Living = Self.Started.Living();
        const bool Asks = std::any_of(Living.begin(), Living.end(), [](const BehindOne &One) { return One.Asks; });
        if (!Living.empty()) SayIn(Shown.BehindColor, "+{}{} ", Living.size(), Asks ? "!" : "");
    }
    SayIn(Shown.SignColor, "{} ", Shown.Sign);
    io_flush();
}

}  // namespace hub
