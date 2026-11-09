/* itinerary.h — an Itinerary: steps of the hub kept in a file, run by name.
 *
 * A text file that wears `itinerary`. Its first lines are its Form — `takes`
 * and `gives`, as a program's — and every line after them is a step, run as
 * if it were typed. The person starts it as any program, by name: the words
 * are sorted into its Form, asking what must be asked, and the steps run one
 * after another.
 *
 *     takes things files "the files to tidy"
 *     takes words label "the tag they get"
 *     gives things
 *     tag files label
 *     show files
 *
 * A field's NAME is, in every step, what the field was given (box/form.h,
 * BoxFormGiven). `them` begins as the files of its first things field and
 * goes on as the steps give; when the Form gives things, `them` at the end is
 * what the Itinerary gave. The person's own `them` is neither read nor
 * changed inside it. A step that does not go through stops it: the rest is
 * not done. It has no conditions, so one that would run itself again —
 * directly or through another — would never end, and is not started.
 *
 * Steps print to the screen, one program after another: an arrow on the
 * Itinerary itself is for a Form that takes into, whose name the steps use. */
#ifndef HUB_ITINERARY_H
#define HUB_ITINERARY_H

#include <string_view>
#include <vector>

#include "box/form.h"
#include "box/types.h"

#include "held.h"

namespace hub {

struct Hub;

// The Itineraries running, the innermost last.
class Trips {
public:
    struct Trip {
        file_id_t                 File;
        std::vector<BoxFormGiven> Given;   // into the trip's filler, which outlives the trip
    };

    // A filler's lookup told the innermost trip's fields; none when no trip runs.
    void Show(BoxFormLookup &Lookup) const noexcept;
    bool Runs(file_id_t File) const noexcept;
    void Begin(Trip Started) { Running.push_back(std::move(Started)); }
    void End() noexcept { Running.pop_back(); }

private:
    std::vector<Trip> Running;
};

namespace itinerary {

// The Itinerary in File run: its Form filled from Line (the words after its
// name, as typed), asking what it must, then its steps one after another. Who
// is the name the person used. True when every step went through.
bool Run(Hub &Self, file_id_t File, std::string_view Who, std::string_view Line);

// The Form an Itinerary takes, from its first lines: 0, or a negative -error_t
// (-ERR_INVALID_ARGUMENT: lines that are no Form).
int FormOf(file_id_t File, HeldForm &Out);

}  // namespace itinerary
}  // namespace hub

#endif
