// show.h — the sky on the glass: time kept by a fixed step whatever the frame
// rate, the person's keys and the settings read again as they change. The
// bodies are sky.h's, the picture paint.h's and the words panel.h's.
#ifndef ORBITS_SHOW_H
#define ORBITS_SHOW_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "box/cxx/settings.h"
#include "painter/core/geometry.h"
#include "painter/tui/stage.h"

#include "look.h"
#include "paint.h"
#include "panel.h"
#include "sky.h"

namespace orbits {

class Show {
public:
    // First is the sky to begin with when the person named one.
    Show(painter::tui::Stage Opened, std::optional<std::size_t> First);

    // Until the person leaves; false when the screen stopped taking frames.
    bool Run();
    void Close();

    std::size_t Merged() const { return Bodies.Merged; }
    std::size_t Escaped() const { return Bodies.Escaped; }
    std::size_t Count() const { return Bodies.Bodies.size(); }

private:
    void     Begin(std::size_t Which);
    void     Retarget();
    void     Apply(const Look &Fresh);
    void     Simulate(double Seconds);
    void     TakeCollisions();
    void     Throw();
    void     Say(std::string Words);
    bool     OnKey(const painter::tui::KeyEvent &Key);
    bool     OnChord(const painter::tui::KeyEvent &Key);
    void     OnExtended(const painter::tui::KeyEvent &Key);
    void     OnCharacter(char Typed);
    const Aiming &Foresee();
    Readout  Reading();
    void     Paint();

    painter::tui::Stage    Screen;
    Look                   Shown;
    box::settings::watch   Watch;
    Sky                    Bodies;
    Canvas                 Picture;
    painter::core::Size    Fitted;
    std::size_t            Preset   = 0;
    Vec                    Launcher;
    double                 Angle    = 0.0;
    double                 Power    = 0.0;
    std::size_t            MassAt   = 0;
    std::uint32_t          Thrown   = 0;
    double                 Speed    = 1.0;
    int                    SinceTrail = 0;
    double                 Owed     = 0.0;   // time that has passed and is not yet stepped
    bool                   Paused   = false;
    bool                   Helping  = false;
    Aiming                 Foreseen;
    std::uint64_t          ForeseenAt = 0;
    bool                   Stale    = true;   // a key or a setting may have changed the path
    std::string            Note;
    std::uint64_t          NoteEnds = 0;
    std::uint64_t          Began    = 0;
};

}  // namespace orbits

#endif  // ORBITS_SHOW_H
