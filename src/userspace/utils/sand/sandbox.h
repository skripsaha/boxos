/* sandbox.h — the world, the brush and what the person does with them: the
 * cursor, what is poured, the keys, and time turned into steps. Nothing here
 * knows the screen; paint.cpp draws what is kept here. */
#ifndef SAND_SANDBOX_H
#define SAND_SANDBOX_H

#include <cstdint>
#include <string_view>

#include "look.h"
#include "painter/tui/stage.h"
#include "scenes.h"
#include "world.h"

namespace sand {

enum class Sky : std::uint8_t { Clear, Rain, Sandstorm };

class Sandbox {
public:
    Sandbox(const Scene &Begin, std::uint64_t Seed);

    // The field is Wide by High pixels; the cursor starts in the middle.
    void Open(int Wide, int High, std::uint64_t NowUs);
    // False to leave.
    bool OnKey(const painter::tui::KeyEvent &Key);
    // The steps the time since the last call owes.
    void Advance(std::uint64_t NowUs);
    // The settings read again: what the person changed takes hold, the rest is kept.
    void Reread();
    // What is poured now: air for the eraser.
    Kind Material() const;

    Look  Shown;
    World Field;
    int   CursorX  = 0;
    int   CursorY  = 0;
    int   Brush    = 3;
    int   Speed    = 60;
    int   Chosen   = 0;   // index into Pourable, or EraserAt
    bool  Pours    = false;
    bool  Paused   = false;
    bool  Helping  = false;
    bool  Upward   = false;
    Sky   Weather  = Sky::Clear;

private:
    void Move(int Dx, int Dy, std::uint64_t NowUs);
    void Pour();
    void Tune();
    void Choose(int Index);
    void Restart();
    void StepOnce();

    const Scene  &Begun;
    std::uint64_t Last      = 0;
    std::uint64_t Owed      = 0;
    std::uint64_t MovedAt   = 0;
    int           MovedWay  = -1;
    int           Run       = 0;
};

}  // namespace sand

#endif
