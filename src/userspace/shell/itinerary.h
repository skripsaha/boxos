#ifndef ITINERARY_H
#define ITINERARY_H

#include "box/defs.h"
#include "box/types.h"
#include "box/form.h"

/* Run the Itinerary in `file`: its Form filled from `line` (the words after
 * its name, as typed), asking the person what it must, then its steps one
 * after another. `who` is the name the person used. True when every step
 * went through; anything wrong was said. */
bool ItineraryRun(file_id_t file, const char *who, const char *line);

/* The Form an Itinerary takes, from its first lines. 0, or a negative
 * -error_t (-ERR_INVALID_ARGUMENT: lines that are no Form). */
int ItineraryForm(file_id_t file, BoxForm *form);

/* Show a lookup the names of the running Itinerary's fields; none when no
 * Itinerary runs. */
void ItineraryShow(BoxFormLookup *lookup);

#endif /* ITINERARY_H */
