#ifndef ITEMS_STAND_H
#define ITEMS_STAND_H

/* items stand — what its parts share (tools/items_stand.cpp). */

#include <cstdint>
#include <string>
#include <vector>

#include "compositor.h"
#include "item_reception.h"
#include "painter/gui/changes.h"

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
    explicit Bench(items::ProgramState &Program, std::uint32_t RingBytes = ITEM_RING_BYTES);
    // What waits in the program's changes, into the ring; then every whole
    // step read into the tree. Refusals are counted.
    items::Reading Step(painter::gui::Changes &Changes);

    std::vector<std::uint8_t>      Memory;
    ItemAreaHeader                *Head = nullptr;
    std::uint32_t                  RingBytes;
    items::AreaState               Area;
    std::uint64_t                  Steps   = 0;
    std::vector<error_t>           Refusals;
    std::vector<std::uint32_t>     RefusedItems;
};

/* A program's sources on the host: the tree's fonts. */
items::ProgramState HostProgram(const items::Theme &Look, float Scale);

void CheckWire(Tally &Count);
void CheckCompose(Tally &Count);
bool WriteSheet(const char *Out);

}  // namespace stand

#endif
