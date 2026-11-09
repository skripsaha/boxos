/* them.h — `them` and `it`: the files the last command that gives files gave
 * back, kept for as long as the hub runs. */
#ifndef HUB_THEM_H
#define HUB_THEM_H

#include <cstddef>
#include <span>
#include <vector>

#include "box/form.h"
#include "box/types.h"

namespace hub {

class Them {
public:
    // What was given, in place of what was held.
    void Keep(std::vector<file_id_t> Files);

    // A file kept in place of another: a program made again by its Recipe
    // takes its own place, so `them` holds the new one where it held the old.
    void Replace(file_id_t Old, file_id_t Made);

    // Taken aside — `them` is nothing given after this — and put back: what
    // an Itinerary does with the person's own `them` while it runs.
    struct Saved {
        std::vector<file_id_t> Files;
        bool                   Known = false;
    };
    Saved Save();
    void  Restore(Saved Back);

    // A filler's lookup told the files kept; valid while they stay kept.
    void Show(BoxFormLookup &Lookup) const noexcept;

    std::span<const file_id_t> Files() const noexcept { return Held; }
    // False until a command has given files.
    bool Known() const noexcept { return Given; }

private:
    std::vector<file_id_t> Held;
    bool                   Given = false;
};

}  // namespace hub

#endif
