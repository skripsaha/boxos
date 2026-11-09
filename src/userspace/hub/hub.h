/* hub.h — what one hub holds for as long as it runs.
 *
 * The hub is where everything meets: the line the person types, the programs
 * on the volume, the Use Context, the files the last command gave. Every part
 * of it is handed this one record. */
#ifndef HUB_HUB_H
#define HUB_HUB_H

#include "history.h"
#include "itinerary.h"
#include "jobs.h"
#include "look.h"
#include "them.h"

namespace hub {

struct Hub {
    Look    Shown;          // hub.settings, as last read
    Them    Given;          // `them` and `it`
    History Lines;          // what was typed, here and at every other hub
    Jobs    Started;        // the programs this hub started
    Trips   Travelling;     // the Itineraries running, the innermost last
    bool    Going = true;   // false once `exit` was said
};

}  // namespace hub

#endif
