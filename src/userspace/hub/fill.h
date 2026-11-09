/* fill.h — a program's Form filled from the typed line, then the program started.
 *
 * BoxLib's filler sorts the words by what each one is and says when it needs
 * the person: a name that means several files, a field left empty, `all` with
 * no Use Context. The hub only asks those questions and hands the answers
 * back. Every program goes through here, with a Form or without one: a program
 * with none still gets the arrow. */
#ifndef HUB_FILL_H
#define HUB_FILL_H

#include <string_view>

#include "box/form.h"
#include "box/types.h"

#include "held.h"

namespace hub {

struct Hub;

namespace fill {

// The hub's view for a filler: the volume, `them`, and the names of the
// running Itinerary's fields.
void Lookup(const Hub &Self, BoxFormLookup &Out);

// Form filled from Line over Lookup — which outlives the filler — asking the
// person only what the filler cannot settle: the filler, done, or nothing
// when the line could not be filled, and why was said.
HeldFiller Form(const BoxForm &Declared, BoxFormLookup &Lookup, std::string_view Who, std::string_view Line);

// Program's Form filled from Line — the words after its name, as typed — and
// the program started. Who is the name the person used: word 0 of the
// program's Luggage and what every sentence begins with. A program made here
// whose files changed since is made again first (remake.h). Stands by until
// it is gone and keeps what it gave as `them`. True when it started and
// ended with 0.
bool AndRun(Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line);

// Program started on the one file Thing — its Form filled from `it`, that
// file, asking nobody — and stood by until gone. True when it ended with 0;
// Given is then the first file it gave back, 0 for none. The person's `them`
// is neither read nor changed.
bool AndRunOn(Hub &Self, file_id_t Program, std::string_view Who, file_id_t Thing, file_id_t &Given);

}  // namespace fill
}  // namespace hub

#endif
