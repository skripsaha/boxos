/* lavalamp — the screen is a lava lamp: blobs of wax warm in the pool at the
 * bottom, rise, cool under the cap and sink, flowing into one another and
 * apart, glowing, lit as round things are, behind the curved glass of the
 * lamp. Drawn in the screen's own pixels (painter::gui); in the console's
 * cells (painter::tui) when the settings say tui on, or the screen is a text
 * page.
 *
 *   lavalamp                 the lamp as the settings make it
 *   lavalamp ocean 12        a palette and how many blobs, to begin with
 *   lavalamp calm            a lamp kept before, lit again
 *   lavalamp -> calm         the lamp kept in calm when you leave it, and given
 *                            back: `them` is the file after
 *
 * While it flows: enter opens the panel that tunes this lamp — the palette,
 * the wax's, the rim's and the liquid's colours, how many blobs, their size,
 * the flow, the glow; left and right go through the palettes, up and down
 * set the flow; esc leaves. settings lavalamp sets the lamp it begins as.
 *
 * What it is made of: composition (what a lamp is: palettes, colours,
 * numbers, its file's text), look (the settings), lamp (the wax's motion),
 * field (the wax over a grid), shades (the colours), tuning (the keys and
 * the panel's rows), pixels and panel (the lamp in pixels), cells (the lamp
 * in cells). This part reads the line, lights the lamp and keeps it. */

#include <charconv>
#include <cstdlib>
#include <expected>
#include <format>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/print.h"
#include "painter/gui/area.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/stage.h"

#include "cells.h"
#include "composition.h"
#include "look.h"
#include "pixels.h"

BOX_FORM("takes optional thing \"a lamp kept before, to light it again\"\n"
         "takes optional words \"how it begins: a palette (lava, magma, ocean, lime, violet, sunset) and how many blobs, 2 to 24\"\n"
         "takes optional into \"the file to keep the lamp in when you leave it\"\n"
         "gives things\n");

namespace {

using namespace lavalamp;

constexpr std::int32_t LeastCols = 20;
constexpr std::int32_t LeastRows = 8;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

std::string Why(std::int64_t Code)
{
    return std::string(box::error(static_cast<::error_t>(Code < 0 ? -Code : Code)).message());
}

/* A file's whole text; a sentence when it would not be read. */
std::expected<std::string, std::string> ReadText(file_id_t File)
{
    FileMetadata Metadata;
    if (const int Code = FileMetadataRead(File, &Metadata); Code != 0) return std::unexpected("it could not be looked at: " + Why(Code));
    const std::uint64_t Size = Metadata.Size;
    FileMetadataRelease(&Metadata);
    std::string Text(static_cast<std::size_t>(std::min<std::uint64_t>(Size, 64 * 1024)), '\0');
    std::size_t Have = 0;
    while (Have < Text.size()) {
        const std::int64_t Got = fread(File, Have, Text.data() + Have, Text.size() - Have);
        if (Got < 0) return std::unexpected("it would not be read: " + Why(Got));
        if (Got == 0) break;
        Have += static_cast<std::size_t>(Got);
    }
    Text.resize(Have);
    return Text;
}

/* The line's words over the lamp: a palette's name, a number of blobs. */
bool TakeWords(const char *const *Words, std::size_t Count, Composition &Lamp)
{
    for (std::size_t At = 0; At < Count; At++) {
        const std::string_view Word = Words[At];
        if (const std::optional<int> Which = PaletteNamed(Word)) {
            Lamp.Take(*Which);
            continue;
        }
        int                     Blobs = 0;
        const std::from_chars_result Read = std::from_chars(Word.data(), Word.data() + Word.size(), Blobs);
        if (Read.ec == std::errc() && Read.ptr == Word.data() + Word.size()) {
            if (Blobs < limits::LeastBlobs || Blobs > limits::MostBlobs) {
                Say(std::format("lavalamp: {} blobs? A lamp holds {} to {}.", Word, limits::LeastBlobs, limits::MostBlobs));
                return false;
            }
            Lamp.Blobs = Blobs;
            continue;
        }
        Say(std::format("lavalamp: what is {}? It takes a palette — lava, magma, ocean, lime, violet, sunset — and how many blobs.", Word));
        return false;
    }
    return true;
}

/* Where the lamp goes when it is kept: the file of that name when it is a
 * lamp kept before, 0 for one to be made. A sentence when the name is taken
 * by something else, or by more than one file. */
std::expected<file_id_t, std::string> KeepingPlace(const std::string &Name)
{
    file_id_t Found[2];
    const int Count = find_file_by_name(Name.c_str(), nullptr, Found, nullptr, 2);
    if (Count < 0) return std::unexpected(std::format("could not look for {}: {}", Name, Why(Count)));
    if (Count == 0) return file_id_t{0};
    if (Count > 1) return std::unexpected(std::format("{} files are called {} here: give the lamp another name", Count, Name));
    const std::expected<std::string, std::string> Text = ReadText(Found[0]);
    if (!Text) return std::unexpected(std::format("{} is here and {}", Name, Text.error()));
    if (!IsLamp(*Text)) return std::unexpected(std::format("{} is here and is not a lamp: give the lamp another name", Name));
    return Found[0];
}

/* The lamp written into its file — made in the Use Context when there is
 * none — settled on the medium, and given back. */
std::optional<std::string> Keep(const std::string &Name, file_id_t File, const Composition &Lamp)
{
    if (File == 0) {
        const std::int64_t Made = create(Name.c_str(), nullptr);
        if (Made <= 0) return std::format("{} was not made: {}", Name, Why(Made));
        File = static_cast<file_id_t>(Made);
    }
    const std::string Text = Written(Lamp);
    if (const int Code = file_span(File, Text.size(), STORAGE_SPAN_GROW); Code != 0) return std::format("{} would not grow: {}", Name, Why(Code));
    for (std::size_t Done = 0; Done < Text.size();) {
        const std::int64_t Wrote = fwrite(File, Done, Text.data() + Done, Text.size() - Done);
        if (Wrote <= 0) return std::format("{} would not take the lamp: {}", Name, Why(Wrote));
        Done += static_cast<std::size_t>(Wrote);
    }
    if (const int Code = file_span(File, Text.size(), 0); Code != 0) return std::format("{} would not be cut to the lamp: {}", Name, Why(Code));
    if (const int Code = anchor(File); Code != 0) return std::format("{} was written but not settled: {}", Name, Why(Code));
    (void)souvenir_things(&File, 1);
    return std::nullopt;
}

/* The lamp lit, in pixels or in cells, until the person leaves it; none when
 * it could not be lit. */
std::optional<Composition> Light(const Look &Shown, const Composition &Lamp)
{
    if (!Shown.Cells)
        if (std::expected<painter::gui::Area, box::error> Area = painter::gui::Area::Open(painter::gui::Placement::Whole);
            Area && Area->On() == painter::gui::Glass::Pixels)
            if (std::expected<painter::tui::Keyboard, painter::tui::StageError> Keys = painter::tui::Keyboard::Open())
                return PixelLamp(std::move(*Area), std::move(*Keys), Lamp).Run();
    std::expected<painter::tui::Stage, painter::tui::StageError> Opened = painter::tui::Stage::Open(LeastCols, LeastRows);
    if (!Opened) {
        Say(Opened.error().Describe("lavalamp"));
        return std::nullopt;
    }
    bool              Closed = true;
    const Composition Left   = CellLamp(std::move(*Opened), Lamp).Run(Closed);
    if (!Closed) Say("lavalamp: the screen was not given back as it should have been");
    return Left;
}

int Run()
{
    const file_id_t   *Files     = nullptr;
    std::size_t        FileCount = 0;
    const char *const *Words     = nullptr;
    std::size_t        WordCount = 0;
    const char *const *Into      = nullptr;
    std::size_t        IntoCount = 0;
    if (form_things(0, &Files, &FileCount) != 0 || form_words(1, &Words, &WordCount) != 0 || form_words(2, &Into, &IntoCount) != 0) return 1;
    if (form_screen_file() != 0) {
        Say("lavalamp is shown on the glass: name a file after -> and the lamp is kept there");
        return 1;
    }

    const Look  Shown = ReadLook();
    Composition Lamp  = Shown.Lamp;
    if (FileCount > 0) {
        const std::expected<std::string, std::string> Text = ReadText(Files[0]);
        const std::expected<Composition, std::string> Kept =
            Text ? Read(*Text) : std::expected<Composition, std::string>(std::unexpected(Text.error()));
        if (!Kept) {
            Say("lavalamp: that file cannot be lit: " + Kept.error());
            return 1;
        }
        Lamp = *Kept;
    }
    if (!TakeWords(Words, WordCount, Lamp)) return 1;

    /* The keeping place is settled before the lamp is lit: a name that cannot
     * take it is said now, not after the person has tuned it for an hour. */
    const std::string                     KeepName = IntoCount > 0 ? std::string(Into[0]) : std::string();
    std::expected<file_id_t, std::string> Place    = file_id_t{0};
    if (!KeepName.empty()) {
        Place = KeepingPlace(KeepName);
        if (!Place) {
            Say("lavalamp: " + Place.error());
            return 1;
        }
    }

    const std::optional<Composition> Left = Light(Shown, Lamp);
    if (!Left) return 1;
    if (KeepName.empty()) return 0;
    if (const std::optional<std::string> Refused = Keep(KeepName, *Place, *Left)) {
        Say("lavalamp: the lamp was not kept: " + *Refused);
        return 1;
    }
    Say(std::format("lavalamp: the lamp is kept in {}", KeepName));
    return 0;
}

}  // namespace

int main()
{
    try {
        return Run();
    } catch (const std::bad_alloc &) {
        Say("lavalamp: there is no memory left for a lamp the size of this screen");
        return 1;
    }
}
