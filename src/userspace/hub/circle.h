/* circle.h — the programs that live on their own behind the hub, and the
 * round they come back to the screen in.
 *
 * A program the person let go (the key let-go, look.h), or one that lives on
 * by itself from its start (box/life.h stays), lives behind. The key next
 * brings them to the screen one at a time, in the order the person chose,
 * and after the last the hub's own prompt again: a circle, the hub one place
 * on it. A round of presses keeps the order it began with, so the next one
 * is always the one the person expects — however each one's turn on the
 * screen changes what comes first in the next round. One that asks for the
 * keyboard comes first: whoever waits for the person is never last. Those
 * let go in the Use Context the person is in come next, when they set it so.
 *
 * Only what is known of them is kept here; jobs.h talks to the display and
 * the kernel. */
#ifndef HUB_CIRCLE_H
#define HUB_CIRCLE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "look.h"

namespace hub {

// A program, by its main strand in one incarnation.
struct Which {
    std::uint32_t Pid = 0;
    std::uint32_t Generation = 0;

    bool operator==(const Which &) const = default;
};

// One that lives behind.
struct BehindOne {
    std::string   Who;                  // the name the person used
    Which         Program;
    std::string   Context;              // the Use Context it went behind in
    std::uint64_t Away = 0;             // when it went behind, in the circle's count
    std::uint64_t Shown = 0;            // when it was last on the screen; 0: never
    bool          Serves = false;       // it serves others: on the circle only while it asks
    bool          Asks = false;         // it asked for the keyboard since it was last on the screen
    bool          AskedToEnd = false;   // the key stop was pressed over it once
};

class Circle {
public:
    // Gone behind now — counted from here.
    void Add(BehindOne One);
    // Ended: true when it lived behind.
    bool Remove(const Which &Program);
    BehindOne *Find(const Which &Program);
    // It asked for the keyboard: true when it had not asked before.
    bool Asks(const Which &Program);
    // The place on the circle after `Now`, the program on the screen — none
    // for the hub's own prompt: the program to bring, or none when the hub
    // comes next. The one brought is counted as shown from here.
    std::optional<BehindOne> Next(const std::optional<Which> &Now, const Look &Shown, std::string_view Context);
    // A round of presses is over: the next press orders them afresh.
    void Forget() { Round.clear(); }
    // Those on the circle, as the line at the top of the screen names them.
    std::vector<BehindOne> OnCircle() const;
    // Every one behind, those that serve too: a display that came is told of each.
    const std::vector<BehindOne> &All() const { return Behind; }

private:
    std::vector<Which> Ordered(const Look &Shown, std::string_view Context) const;

    std::vector<BehindOne> Behind;
    std::vector<Which>     Round;    // the order this round of presses keeps
    std::uint64_t          Clock = 0;
};

}  // namespace hub

#endif
