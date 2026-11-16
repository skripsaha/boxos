/* bar.cpp — the one line at the bottom: what the forest is now, and the keys.
 * Each piece is laid whole or not at all, in the order it is needed, so a
 * narrow glass loses the last piece and never half a word; how to leave is
 * kept room for before the others are laid. */

#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "painter/tui/text.h"
#include "show.h"

namespace forest {

namespace {

namespace tui = painter::tui;

class Line {
public:
    Line(tui::Frame &On, const Look &Chosen)
        : Glass(On), Cols(On.Size().Width), Row(On.Size().Height - 1), Paper(Chosen.BarPaper), Ink(Chosen.BarInk),
          KeyInk(Chosen.BarKey), Quiet(Mixed(Chosen.BarInk, Chosen.BarPaper, 110))
    {
        for (int Col = 0; Col < Cols; Col++) Glass.Put({Col, Row}, tui::Cell{' ', Ink, Paper});
    }

    bool Fits(std::size_t Length) const { return Col + static_cast<int>(Length) < Cols; }

    void Write(std::string_view Text, Color With)
    {
        for (std::size_t Byte = 0; Byte < Text.size() && Col < Cols;) {
            const tui::Glyph Shape = tui::GlyphAt(Text, Byte);
            Glass.Put({Col++, Row}, tui::Cell{Shape.Character, With, Paper});
            Byte += Shape.Length;
        }
    }

    void Gap(int Width) { Col += Width; }

    int  Col = 1;
    Color Bright() const { return Ink; }
    Color Keys() const { return KeyInk; }
    Color Dim() const { return Quiet; }

private:
    tui::Frame &Glass;
    int         Cols, Row;
    Color       Paper, Ink, KeyInk, Quiet;
};

struct Key {
    std::string Keys;
    const char *What;
};

std::string Spelled(const KeyChord &Chord)
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Chord, Spelling, sizeof(Spelling));
    return Spelling;
}

std::size_t Width(const Key &Each) { return 2 + tui::CellsOf(Each.Keys) + 1 + std::strlen(Each.What); }

}  // namespace

void PaintBar(tui::Frame &Glass, const Look &Chosen, const Saying &Say)
{
    Line Bar(Glass, Chosen);
    Bar.Write("forest", Bar.Bright());

    std::string Clock = std::format("{:02}:{:02}", Say.Hour, Say.Minute);
    std::string Flow  = Say.Real ? "real" : Say.Paused ? "paused" : std::format("x{}", Say.Speed);
    struct Piece {
        std::string Text;
        bool        Bright;
    };
    const Piece State[] = {{SeasonName(Say.Of), true}, {WeatherName(Say.Is), true}, {Clock, false}, {Flow, false}};
    for (const Piece &Each : State) {
        if (!Bar.Fits(Each.Text.size() + 1)) return;
        Bar.Gap(1);
        Bar.Write(Each.Text, Each.Bright ? Bar.Bright() : Bar.Dim());
    }

    const std::vector<Key> Keys = {{"\u2190\u2192", "time"},
                                   {"\u2191\u2193", "speed"},
                                   {Spelled(Chosen.Keys[KeySeason]), "season"},
                                   {Spelled(Chosen.Keys[KeyWeather]), "weather"},
                                   {Spelled(Chosen.Keys[KeyTime]), "clock"},
                                   {Spelled(Chosen.Keys[KeyPause]), "pause"},
                                   {Spelled(Chosen.Keys[KeyHelp]), "keys"}};
    const Key Leave{"esc", "leave"};

    Bar.Gap(1);
    bool Full = false;
    for (const Key &Each : Keys) {
        Full = Full || !Bar.Fits(Width(Each) + Width(Leave));
        if (Full) continue;
        Bar.Gap(2);
        Bar.Write(Each.Keys, Bar.Keys());
        Bar.Gap(1);
        Bar.Write(Each.What, Bar.Dim());
    }
    if (!Bar.Fits(Width(Leave))) return;
    Bar.Gap(2);
    Bar.Write(Leave.Keys, Bar.Keys());
    Bar.Gap(1);
    Bar.Write(Leave.What, Bar.Dim());
    if (Full) return;

    std::string Count = std::format("{} living", Say.Living);
    if (Bar.Fits(Count.size() + 3)) {
        Bar.Gap(3);
        Bar.Write(Count, Bar.Dim());
    }
}

}  // namespace forest
