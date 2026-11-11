/* wizard — an old bearded wizard in his stone tower, drawn in the glass's own
 * half-block pixels. He casts a spell on request — a charm chosen by chance,
 * with words nobody has said before — and mixes potions: choose a thing from
 * the shelf, drop it in the pot, drink the brew with enter, and what the mix
 * is decides what the glass does for a while: it sways, ripples, shakes, melts,
 * glitches, snows, glows in rainbows. Every one of them is laid over the whole
 * screen by the display (box::glass), which ends it at its time, when it is
 * lifted, and when the wizard leaves. What he says is in a bubble by his head;
 * the potions he has found are in his grimoire, kept in a file of its own.
 *
 *   wizard                   the tower: spells and potions by the keys
 *   wizard giddy             that charm cast at once (the keys: ctrl+? in the tower)
 *   wizard toadstool cobweb  those two mixed and drunk at once
 *
 * What it is made of: lore (the charms, the things in the pot, the potions),
 * look (the settings), scene, room and cauldron (where things stand, the room,
 * the pot), figure (the wizard), speech (his bubble), specks, icons, grimoire
 * and book, tower (the keys and what moves of itself) and paint (all of it as a
 * picture). Only the pictures and paint know the screen; only tower asks the
 * display.
 */

#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "tower.h"

BOX_FORM("takes optional words \"a charm to cast at once (giddy, mirror, upside, ghost, grey, ember, frost, starfall, moonlit, fever, "
         "pond, tremor, pixie, prism, drip, hex, lantern, yore, blizzard, heartbeat, carnival, nightmare, tide, avalanche), or things to mix and "
         "drink (mandrake, toadstool, feather, moonstone, scale, eye, stardust, tail, fern, bone, berry, shard, moss, beetle, cobweb, parchment)\"\n");

namespace {

// The least glass the tower is drawn for: the wizard's picture, the words above
// it and the shelf below.
constexpr std::int32_t LeastCols = 80;
constexpr std::int32_t LeastRows = 25;

// Words for the person, before the glass is taken or after it is given back.
void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}  // namespace

int main()
{
    try {
        const char *const *Heard = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Heard, &Count) != 0) return 1;
        std::vector<std::string> Words;
        for (std::size_t At = 0; At < Count; At++) Words.emplace_back(Heard[At]);

        if (const auto Wrong = wizard::Tower::Misunderstood(Words)) {
            Say("wizard: " + *Wrong);
            return 1;
        }
        if (form_screen_file() != 0) {
            Say("wizard is shown on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("wizard"));
            return 1;
        }
        wizard::Tower Tower(std::move(*Opened), std::move(Words));
        Tower.Run();
        return 0;
    } catch (const std::bad_alloc &) {
        Say("wizard: there is no memory left for the tower");
        return 1;
    }
}
