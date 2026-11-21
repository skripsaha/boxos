/* composition.cpp — the palettes, the colours spelled and typed, and a lamp
 * as the text of its file. See composition.h. */

#include "composition.h"

#include <charconv>
#include <format>

#include "box/color.h"

namespace lavalamp {

namespace {

constexpr std::string_view FirstLine = "lavalamp";

std::string_view Trimmed(std::string_view Text)
{
    while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\r')) Text.remove_prefix(1);
    while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t' || Text.back() == '\r')) Text.remove_suffix(1);
    return Text;
}

std::optional<int> Whole(std::string_view Text, int Least, int Most)
{
    int                          Value = 0;
    const std::from_chars_result Got   = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
    if (Got.ec != std::errc() || Got.ptr != Text.data() + Text.size() || Value < Least || Value > Most) return std::nullopt;
    return Value;
}

/* One line of a kept lamp into the composition; a sentence when it is wrong. */
std::optional<std::string> TakeLine(std::string_view Name, std::string_view Value, Composition &Into)
{
    const auto Number = [&](int &Field, int Least, int Most) -> std::optional<std::string> {
        const std::optional<int> Got = Whole(Value, Least, Most);
        if (!Got) return std::format("{} wants a whole number from {} to {}, not {}", Name, Least, Most, Value);
        Field = *Got;
        return std::nullopt;
    };
    const auto Paint = [&](Color &Field) -> std::optional<std::string> {
        const std::optional<Color> Got = ColorTyped(Value);
        if (!Got) return std::format("{} wants a colour such as #FF3A6B, not {}", Name, Value);
        Field = *Got;
        return std::nullopt;
    };
    if (Name == "palette") {
        if (Value == "own") {
            Into.Palette = Own;
            return std::nullopt;
        }
        const std::optional<int> Which = PaletteNamed(Value);
        if (!Which) return std::format("there is no palette {}", Value);
        Into.Take(*Which);
        return std::nullopt;
    }
    if (Name == "wax") return Paint(Into.Wax);
    if (Name == "rim") return Paint(Into.Rim);
    if (Name == "glass") return Paint(Into.Glass);
    if (Name == "blobs") return Number(Into.Blobs, limits::LeastBlobs, limits::MostBlobs);
    if (Name == "size") return Number(Into.Size, limits::LeastSize, limits::MostSize);
    if (Name == "flow") return Number(Into.Flow, limits::LeastFlow, limits::MostFlow);
    if (Name == "glow") return Number(Into.Glow, limits::LeastGlow, limits::MostGlow);
    return std::format("a lamp has no {}", Name);
}

}  // namespace

void Composition::Take(int Which)
{
    Palette = Which;
    Wax     = Palettes[static_cast<std::size_t>(Which)].Wax;
    Rim     = Palettes[static_cast<std::size_t>(Which)].Rim;
    Glass   = Palettes[static_cast<std::size_t>(Which)].Glass;
}

std::string Composition::PaletteName() const
{
    return Palette == Own ? "own" : Palettes[static_cast<std::size_t>(Palette)].Name;
}

std::optional<int> PaletteNamed(std::string_view Name)
{
    for (std::size_t At = 0; At < Palettes.size(); At++)
        if (Name == Palettes[At].Name) return static_cast<int>(At);
    return std::nullopt;
}

std::string Spelled(Color Of)
{
    char Out[8];
    color_format(COLOR_RGB(Of.Red(), Of.Green(), Of.Blue()), Out);
    return Out;
}

std::optional<Color> ColorTyped(std::string_view Text)
{
    const std::string Word(Trimmed(Text));
    ::Color           Got = 0;
    if (!color_parse(Word.c_str(), &Got)) return std::nullopt;
    return Color::FromRaw(Got);
}

std::string Written(const Composition &Lamp)
{
    /* The colours are written whatever the palette: a palette's colours may
     * change in a later lamp, and the file says the lamp as it was seen. */
    return std::format("{}\npalette {}\nwax {}\nrim {}\nglass {}\nblobs {}\nsize {}\nflow {}\nglow {}\n", FirstLine, Lamp.PaletteName(),
                       Spelled(Lamp.Wax), Spelled(Lamp.Rim), Spelled(Lamp.Glass), Lamp.Blobs, Lamp.Size, Lamp.Flow, Lamp.Glow);
}

bool IsLamp(std::string_view Text)
{
    const std::size_t End = Text.find('\n');
    return Trimmed(Text.substr(0, End)) == FirstLine;
}

std::expected<Composition, std::string> Read(std::string_view Text)
{
    if (!IsLamp(Text)) return std::unexpected(std::string("it is not a lamp: its first line is not lavalamp"));
    Composition Lamp;
    int         Number = 1;
    /* The colours after the palette's: a lamp of its own colours says them
     * after the palette's name, and a palette's lamp says the palette's. */
    std::optional<int> Named;
    for (std::size_t At = Text.find('\n'); At != std::string_view::npos && At < Text.size();) {
        const std::size_t Next = Text.find('\n', At + 1);
        const std::string_view Line = Trimmed(Text.substr(At + 1, Next == std::string_view::npos ? std::string_view::npos : Next - At - 1));
        At = Next;
        Number++;
        if (Line.empty()) continue;
        const std::size_t Gap = Line.find_first_of(" \t");
        if (Gap == std::string_view::npos) return std::unexpected(std::format("line {} says {} and nothing of it", Number, Line));
        const std::string_view Name = Line.substr(0, Gap);
        if (std::optional<std::string> Wrong = TakeLine(Name, Trimmed(Line.substr(Gap)), Lamp))
            return std::unexpected(std::format("line {}: {}", Number, *Wrong));
        if (Name == "palette") Named = Lamp.Palette;
    }
    /* Colours that are not the palette's make the lamp its own. */
    if (Named && *Named != Own) {
        const Palette &Of = Palettes[static_cast<std::size_t>(*Named)];
        if (!(Lamp.Wax == Of.Wax && Lamp.Rim == Of.Rim && Lamp.Glass == Of.Glass)) Lamp.Palette = Own;
    }
    return Lamp;
}

}  // namespace lavalamp
