/* fill.h — a program's Form filled from the typed line: what the program is
 * started with.
 *
 * BoxLib's filler sorts the words by what each one is and says when it needs
 * the person: a name that means several files, a field left empty, `all` with
 * no Use Context. The hub only asks those questions and hands the answers
 * back. Every program goes through here, with a Form or without one: a program
 * with none still gets the arrow. What is filled is started elsewhere
 * (launch.h); here nothing starts. */
#ifndef HUB_FILL_H
#define HUB_FILL_H

#include <optional>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/types.h"

#include "held.h"

namespace hub {

struct Hub;

namespace fill {

// A Form filled: the program and the Luggage it is started with, and where
// what it gives back goes.
struct Filled {
    file_id_t   Program  = 0;
    std::string Luggage;            // word 0 the name the person used, then its words as filled
    file_id_t   Screen   = 0;       // the file its screen goes into (->); 0: the person sees it
    bool        Souvenir = false;   // its Souvenir was asked for, to be kept as `them`
};

// The hub's view for a filler: the volume, `them`, and the names of the
// running Itinerary's fields.
void Lookup(const Hub &Self, BoxFormLookup &Out);

// Form filled from Line over Lookup — which outlives the filler — asking the
// person only what the filler cannot settle: the filler, done, or nothing
// when the line could not be filled, and why was said.
HeldFiller Form(const BoxForm &Declared, BoxFormLookup &Lookup, std::string_view Who, std::string_view Line);

// Program's Form filled from Line — the words after its name, as typed. Who
// is the name the person used: word 0 of the Luggage and what every sentence
// begins with. Nothing when it could not be filled, and why was said.
std::optional<Filled> FromLine(const Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line);

// Program's Form filled from `it`, the one file Thing, asking nobody: the
// person's own `them` is neither read nor changed.
std::optional<Filled> OnThing(file_id_t Program, std::string_view Who, file_id_t Thing);

}  // namespace fill
}  // namespace hub

#endif
