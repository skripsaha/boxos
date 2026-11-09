/* held.h — what boxlib lends the hub, given back when the hub lets go of it. */
#ifndef HUB_HELD_H
#define HUB_HELD_H

#include <memory>

#include "box/form.h"
#include "box/memory.h"
#include "box/recipe.h"

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
    bool           Holds() const noexcept { return Held; }
    void           Let() noexcept
    {
        if (Held) form_release(&Value);
        Held = false;
    }

private:
    BoxForm Value{};
    bool    Held = false;
};

// A program's Recipe, read from its file, released with its holder.
class HeldRecipe {
public:
    HeldRecipe() = default;
    HeldRecipe(const HeldRecipe &) = delete;
    HeldRecipe &operator=(const HeldRecipe &) = delete;
    ~HeldRecipe()
    {
        if (Held) recipe_release(&Value);
    }

    // Read from Program: 0, or a negative -error_t and nothing held.
    int Read(file_id_t Program) noexcept
    {
        const int Rc = recipe_of_file(Program, &Value);
        Held = Rc == 0;
        return Rc;
    }
    const BoxRecipe &operator*() const noexcept { return Value; }
    const BoxRecipe *operator->() const noexcept { return &Value; }

private:
    BoxRecipe Value{};
    bool      Held = false;
};

struct FillerEnd {
    void operator()(BoxFormFiller *Filler) const noexcept { form_fill_end(Filler); }
};
using HeldFiller = std::unique_ptr<BoxFormFiller, FillerEnd>;

// What boxlib handed over on its heap, freed with its holder.
struct HeapFree {
    void operator()(void *Bytes) const noexcept { free(Bytes); }
};
template <typename Value>
using HeldHeap = std::unique_ptr<Value, HeapFree>;

}  // namespace hub

#endif
