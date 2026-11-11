/* tones.h — the colours of the scene at an hour: the person's own by day, the
 * sky and the hills warmed and darkened toward dusk when the man is lost, and
 * brightened when he is freed. */
#ifndef HANGMAN_TONES_H
#define HANGMAN_TONES_H

#include "look.h"
#include "numbers.h"

namespace hangman {

struct Tones {
    Color SkyTop;
    Color SkyLow;
    Color Far;        // the mountains
    Color Hill;
    Color HillDeep;
    Color Sun;
    Color Cloud;
    Color Leaf;
    Color Trunk;
    Color Timber;     // the gallows
    Color Figure;

    friend bool operator==(const Tones &, const Tones &) = default;
};

// Dusk and Joy are each 0 (the day as set) to 1 (all the way).
Tones TonesFor(const Look &Shown, float Dusk, float Joy);

}  // namespace hangman

#endif
