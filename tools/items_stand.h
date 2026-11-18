#ifndef ITEMS_STAND_H
#define ITEMS_STAND_H

/* items stand — what its parts share (tools/items_stand.cpp). */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "compositor.h"
#include "item_reception.h"
#include "painter/gui/changes.h"
#include "typecase/fonts.h"

namespace stand {

struct Tally {
    int Held   = 0;
    int Failed = 0;
    void Check(bool Holds, const std::string &What);
};

std::uint32_t Random();

/* An area as the display keeps one, its memory a program's ring and events
 * on the host: changes written as a program writes them, read as the display
 * reads them. */
struct Bench {
    explicit Bench(items::ProgramState &Program, std::uint32_t RingBytes = ITEM_RING_BYTES,
                   const items::Motion &Wanted = items::AsItComes);
    // What waits in the program's changes, into the ring; then every whole
    // step read into the tree at Clock. Refusals are counted.
    items::Reading Step(painter::gui::Changes &Changes);
    // The area's animations brought to Now, each end kept in Ends.
    void Advance(std::uint64_t Now, const items::Theme &Look);

    std::vector<std::uint8_t>      Memory;
    ItemAreaHeader                *Head = nullptr;
    std::uint32_t                  RingBytes;
    items::AreaState               Area;
    std::uint64_t                  Steps   = 0;
    std::vector<error_t>           Refusals;
    std::vector<std::uint32_t>     RefusedItems;
    std::uint64_t                  Clock = 1;   // microseconds: when a step is read
    std::vector<ItemAnimationEnd>  Ends;
};

/* A program's sources on the host: the tree's fonts. */
items::ProgramState HostProgram(const items::Theme &Look, float Scale);

// The tree's own fonts, as the display's side here lays texts in them — and,
// as the system's, a program's measuring (items_stand_gui.cpp).
std::shared_ptr<const typecase::Fonts> TreeFonts();
const typecase::Fonts                 &TreeBook();

void CheckWire(Tally &Count);
void CheckCompose(Tally &Count);
void CheckCells(Tally &Count);
void CheckGui(Tally &Count);
void CheckMotion(Tally &Count);
bool WriteSheet(const char *Out);

}  // namespace stand

#endif
