/* grimoire.h — the book of what he has found: which of the potions that are
 * more than their parts have been brewed, and how many he has drunk and cast.
 * It is kept in a file of its own (keep.h), written as soon as there is
 * something new in it. */
#ifndef WIZARD_GRIMOIRE_H
#define WIZARD_GRIMOIRE_H

#include <bitset>
#include <cstdint>
#include <string>

#include "lore.h"

namespace wizard {

class Grimoire {
public:
    // The book as the volume keeps it; empty when there is none yet.
    Grimoire();

    bool Knows(std::size_t Recipe) const { return Found.test(Recipe); }
    std::size_t KnownCount() const { return Found.count(); }
    // A potion found for the first time: written down. True when it was new.
    bool Learn(std::size_t Recipe);
    void CountDrunk();
    void CountCast();

    std::uint32_t Drunk() const { return Brewed; }
    std::uint32_t Cast() const { return Spelled; }
    // Why the book could not be read or written, as a sentence; empty when it could.
    const std::string &Trouble() const { return Said; }

private:
    void Save();

    std::bitset<RecipeCount> Found;
    std::uint32_t            Brewed = 0;
    std::uint32_t            Spelled = 0;
    std::string              Said;
    bool                     Writable = true;
};

}  // namespace wizard

#endif
