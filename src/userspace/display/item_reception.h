#ifndef BOXOS_DISPLAY_ITEM_RECEPTION_H
#define BOXOS_DISPLAY_ITEM_RECEPTION_H

// An area's ring of changes read (box/items.h): every step written whole
// applied to its tree, in order — a step the program is still writing waits
// for its end — each change refused told back, and a ring that breaks the
// language, or whose cursors lie, said broken: the area is closed and its
// program told. The ring is the program's memory and nothing in it is
// trusted: each record's head is read once, and every length is held to the
// ring before a byte past the head is read.
//
// And the events the display tells back, put in the area's second ring.

#include <cstdint>
#include <functional>
#include <span>

#include "box/error.h"
#include "box/items.h"
#include "item_tree.h"

namespace items {

struct Reading {
    std::uint64_t Step    = 0;      // the last step applied
    std::uint32_t Bytes   = 0;      // the ring's bytes taken
    std::uint32_t Records = 0;
    bool          Stepped = false;  // a whole step was applied
    bool          Broken  = false;  // the ring breaks the language
};

struct Listener {
    // A change refused: its item, what it was, why.
    std::function<void(std::uint32_t Item, std::uint16_t Op, std::uint16_t Property, error_t Reason)> Refused;
    // A block's pixels changed: the block, where, and a canvas's page.
    std::function<void(std::uint32_t Block, const ItemBlockChanged &Where)> BlockChanged;
    // An animation said: OK, or why not. None takes no animation.
    std::function<error_t(std::uint32_t Item, const ItemAnimation &How, std::uint16_t Property,
                          std::span<const std::uint8_t> Value)>
        Animate;
    // A property said plainly or unsaid — or, as 0, the item gone: what
    // animates it is cut.
    std::function<void(std::uint32_t Item, std::uint16_t Property)> Cut;
    // The focus asked to an item, ITEM_AREA for none.
    std::function<void(std::uint32_t Item)> Focus;
};

// Every whole step of the ring after Head's Read applied to Items; Read moved
// past the last.
Reading ReadSteps(ItemAreaHeader &Head, std::span<const std::uint8_t> Ring, Tree &Items, const Listener &Tell);

// An event added to the area's events: false when the ring has no room (the
// head's Lost counts it).
bool AddEvent(ItemAreaHeader &Head, std::span<std::uint8_t> Events, std::uint16_t Kind, std::uint32_t Item,
              const void *Payload, std::uint32_t Bytes) noexcept;

}  // namespace items

#endif
