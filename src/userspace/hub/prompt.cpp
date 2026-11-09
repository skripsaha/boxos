/* prompt.cpp — the prompt laid out and said in its colours. */

#include "prompt.h"

#include <string>

#include "box/color.h"
#include "box/display.h"
#include "box/print.h"

#include "context.h"
#include "hub.h"

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

void Say(std::string_view Text, Color Ink)
{
    set_color(Ink);
    print_bytes(Text.data(), Text.size());
}

// The tags that fit, whole, and how many more there are: "code,cpp +3".
std::string Fitted(std::string_view Tags)
{
    const std::size_t Total = context::Count(Tags);
    std::string       Shown;
    std::size_t       Count = 0;
    while (!Tags.empty()) {
        const std::size_t      Comma = Tags.find(',');
        const std::string_view Tag   = Tags.substr(0, Comma);
        if (Count > 0 && Shown.size() + 1 + Tag.size() > TagsRoom()) break;
        if (Count > 0) Shown += ',';
        Shown += Tag;
        Count++;
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    if (Count < Total) Shown += " +" + std::to_string(Total - Count);
    return Shown;
}

}  // namespace

void ShowPrompt(const Hub &Self, std::string_view Context)
{
    const Look &Shown = Self.Shown;
    if (Shown.Context && !Context.empty()) {
        Say("[", Shown.ContextColor);
        Say(Fitted(Context), Shown.ContextColor);
        Say("] ", Shown.ContextColor);
    }
    if (Shown.Them && Self.Given.Known() && !Self.Given.Files().empty())
        Say(std::to_string(Self.Given.Files().size()) + " ", Shown.ThemColor);
    Say(Shown.Sign, Shown.SignColor);
    Say(" ", Shown.SignColor);
    set_color(COLOR_DEFAULT);
    io_flush();
}

}  // namespace hub
