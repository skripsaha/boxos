/* held.h — what boxlib lends the hub, given back when the hub lets go of it. */
#ifndef HUB_HELD_H
#define HUB_HELD_H

#include <memory>

#include "box/form.h"

namespace hub {

// A Form read from a program or an Itinerary, released with its holder.
class HeldForm {
public:
    HeldForm() = default;
    HeldForm(const HeldForm &) = delete;
    HeldForm &operator=(const HeldForm &) = delete;
    ~HeldForm() { Let(); }

    BoxForm       &operator*() noexcept { return Value; }
    const BoxForm &operator*() const noexcept { return Value; }
    const BoxForm *operator->() const noexcept { return &Value; }
    BoxForm       *Take() noexcept { Let(); Held = true; return &Value; }  // for a reader to fill
    void           Drop() noexcept { Held = false; }                       // the reader refused: nothing held
    void           Let() noexcept
    {
        if (Held) form_release(&Value);
        Held = false;
    }

private:
    BoxForm Value{};
    bool    Held = false;
};

struct FillerEnd {
    void operator()(BoxFormFiller *Filler) const noexcept { form_fill_end(Filler); }
};
using HeldFiller = std::unique_ptr<BoxFormFiller, FillerEnd>;

}  // namespace hub

#endif
