/* rules_page.cpp — the rules of the game and the keys, in three pages of words. */

#include <algorithm>
#include <array>
#include <format>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "hud.h"
#include "keys.h"
#include "pages.h"
#include "sea.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

struct Section {
    const char *Heading;
    const char *Words;
};

struct Leaflet {
    const char                 *Title;
    std::array<Section, 3>      Sections;
};

constexpr std::array<Leaflet, 3> Leaves = {{
    {"THE GAME",
     {{{"The waters", "Each captain commands a secret fleet on a square grid of waters. The two fleets are laid out of each other's sight, and the captains take turns to fire at a cell of the other's waters, calling it by its column and row: D7."},
       {"The answer", "A shot is a miss, when it falls in the water; a hit, when it strikes a ship; or sunk, when it strikes the last cell of a ship that was hit all along its length. The captain is told which, and a sunk ship is shown whole."},
       {"The win", "Whoever sinks every ship of the other's fleet first wins the battle. Wins, shots and accuracy are written in the hall of records under the captain's name."}}}},
    {"THE FLEET",
     {{{"The ships", "A ship is a straight line of cells, across or down. The classic fleet is one of four cells, two of three, three of two and four of one; the setup offers a skirmish, an armada and a duel besides, on grids of 6 to 12 cells."},
       {"Not touching", "By the classic rule ships may not touch, not even corner to corner. The cells next to a ship put down are marked as forbidden while you lay the fleet, and when a ship sinks the cells round it are marked as empty, since no ship can lie there: they cannot be fired at."},
       {"Laying it", "Each captain lays the fleet in secret: move a ship with the arrows, turn it, put it down, take it back, or have the whole fleet laid at random."}}}},
    {"THE PLAY",
     {{{"Shots", "By the classic rule a hit earns another shot, and a captain shoots until one falls in the water. Without that rule a captain has one shot a turn. In a salvo a captain fires as many shots in a turn as there are ships afloat, and a hit earns nothing extra."},
       {"One screen", "The screen is one. When a turn is over it goes dark and asks to be passed to the other captain, who presses enter to begin the turn: nobody sees the other's fleet. Against the computer there is no passing, and the computer shoots where its hunt says."},
       {"Keys", "Arrows aim and move, enter or space fires and puts a ship down, r turns it, u takes it back, a lays the fleet at random, c clears it, esc leaves. The keys a person can choose are in settings seabattle keys."}}}},
}};

class RulesPage final : public Page {
public:
    explicit RulesPage(Context &Shared) : Page(Shared) {}

    std::uint32_t FrameMs() const override { return 200; }

    void Key(const tui::KeyEvent &Pressed) override
    {
        const auto Count = static_cast<int>(Leaves.size());
        if (Pressed.IsExtended()) {
            if (Pressed.Scancode == tui::KeyCodes::Left || Pressed.Scancode == tui::KeyCodes::PageUp) Leaf = (Leaf + Count - 1) % Count;
            else if (Pressed.Scancode == tui::KeyCodes::Right || Pressed.Scancode == tui::KeyCodes::PageDown) Leaf = (Leaf + 1) % Count;
        } else if (Pressed.Ascii == tui::KeyCodes::Tab || Pressed.Ascii == ' ') {
            Leaf = (Leaf + 1) % Count;
        } else if (Pressed.Ascii == tui::KeyCodes::Escape || Pressed.Ascii == tui::KeyCodes::Enter) {
            Ctx.Go(PageId::Title);
        }
    }

protected:
    void Pixels(Canvas &Out) override { PaintBackdrop(Out, Ctx.Chosen.Sea, Ctx.Text(), Ctx.Now); }

    void Words(tui::Frame &Out) override
    {
        const Look &Shown = Ctx.Chosen;
        const core::Size Size = Out.Size();
        PaintBar(Out, Shown, "SEA BATTLE", "the rules");
        PaintHints(Out, Shown, {{"\xE2\x86\x94", "page", 1}, {"esc", "back", 0, true}}, std::format("page {} of {}", Leaf + 1, Leaves.size()));
        const std::int32_t Wide = std::min(Size.Width - 6, 90);
        const int          Gap = Size.Height > 32 ? 2 : 1;
        std::int32_t       Lines = 1;
        for (const Leaflet &Each : Leaves) {
            std::int32_t Needed = 1;
            for (const Section &Part : Each.Sections)
                Needed += 1 + static_cast<std::int32_t>(Wrapped(Part.Words, static_cast<std::size_t>(Wide - 8)).size()) + Gap;
            Lines = std::max(Lines, Needed);
        }
        const std::int32_t High = std::min(Size.Height - 4, Lines + 2);
        const core::Rect Inside = PaintPanel(Out, Shown, tui::Centred(Size, Wide, High), Leaves[static_cast<std::size_t>(Leaf)].Title, "left and right turn the page");
        const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
        std::int32_t Row = Inside.Y + 1;
        for (const Section &Part : Leaves[static_cast<std::size_t>(Leaf)].Sections) {
            tui::PutText(Out, core::Point{Inside.X + 1, Row++}, Part.Heading, Inside.Width - 2, Shown.Title, Paper);
            for (const std::string &Line : Wrapped(Part.Words, static_cast<std::size_t>(Inside.Width - 4))) {
                if (Row >= Inside.Y + Inside.Height) return;
                tui::PutText(Out, core::Point{Inside.X + 3, Row++}, Line, Inside.Width - 4, Shown.Text, Paper);
            }
            Row += Gap;
        }
    }

private:
    int Leaf = 0;
};

}  // namespace

std::unique_ptr<Page> MakeRules(Context &Shared)
{
    return std::make_unique<RulesPage>(Shared);
}

}  // namespace seabattle
