/* grimoire.cpp — the book and its file. */

#include "grimoire.h"

#include <charconv>
#include <format>
#include <optional>
#include <string_view>

#include "keep.h"

namespace wizard {

namespace {

const Keep BookFile{"wizard.grimoire", "wizard,grimoire"};

// The lines of Text, each as the word it begins with and what follows.
template <class Each>
void ForLines(std::string_view Text, Each Do)
{
    while (!Text.empty()) {
        const std::size_t End = Text.find('\n');
        const std::string_view Line = Text.substr(0, End);
        const std::size_t Gap = Line.find(' ');
        if (Gap != std::string_view::npos) Do(Line.substr(0, Gap), Line.substr(Gap + 1));
        Text = End == std::string_view::npos ? std::string_view() : Text.substr(End + 1);
    }
}

std::uint32_t Number(std::string_view Text)
{
    std::uint32_t Value = 0;
    std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    return Value;
}

}  // namespace

Grimoire::Grimoire()
{
    std::string Text;
    if (const std::optional<std::string> Refusal = KeepRead(BookFile, Text)) {
        Said = *Refusal;
        Writable = false;
        return;
    }
    ForLines(Text, [&](std::string_view Word, std::string_view Rest) {
        if (Word == "known") {
            for (std::size_t At = 0; At < Recipes().size(); At++)
                if (Recipes()[At].Word == Rest) Found.set(At);
        } else if (Word == "drunk") {
            Brewed = Number(Rest);
        } else if (Word == "cast") {
            Spelled = Number(Rest);
        }
    });
}

bool Grimoire::Learn(std::size_t Recipe)
{
    if (Found.test(Recipe)) return false;
    Found.set(Recipe);
    Save();
    return true;
}

void Grimoire::CountDrunk()
{
    Brewed++;
    Save();
}

void Grimoire::CountCast()
{
    Spelled++;
    Save();
}

void Grimoire::Save()
{
    if (!Writable) return;
    std::string Text = std::format("drunk {}\ncast {}\n", Brewed, Spelled);
    for (std::size_t At = 0; At < Recipes().size(); At++)
        if (Found.test(At)) Text += std::format("known {}\n", Recipes()[At].Word);
    if (const std::optional<std::string> Refusal = KeepWrite(BookFile, Text)) Said = *Refusal;
    else Said.clear();
}

}  // namespace wizard
