#ifndef BOXOS_DISPLAY_EFFECTS_H
#define BOXOS_DISPLAY_EFFECTS_H

#include <cstdint>
#include <span>
#include <vector>

#include "box/display.h"
#include "box/error.h"
#include "boxos_kctx.h"
#include "easel.h"

// The effect over the whole glass (box/display.h): at most one, laid by any
// program for a while. It is kept here and applied at present time to the
// picture the layers composed — the composed cells stay as they were, so the
// glass is the picture itself again the moment the effect ends. Time is the
// ClockBoard's: the effect is judged where it is stepped and drawn at the
// phase of its last frame, so the display need be awake only when a frame or
// an end is due (EffectsDue).

// How many frames a second an animated effect is drawn at: the screen's to
// say, as a framebuffer takes more than a text page can show.
void EffectsFrameRate(std::uint32_t PerSecond) noexcept;

// DISP_CMD_EFFECT asked by the program From names: the effect laid in place of
// any other, or why not.
error_t EffectsLay(const AskFrom &From, const DisplayEffectRequest &Request) noexcept;

// DISP_CMD_EFFECT_LIFT: the effect lifted if the asker laid it or is a Keeper.
// Lifted says whether one was; ERR_ACCESS_DENIED when another's stays.
error_t EffectsLift(const AskFrom &From, bool Keeper, bool &Lifted) noexcept;

// The program (Pid, Generation) ended: its effect with it. True when it had one.
bool EffectsDrop(std::uint32_t Pid, std::uint32_t Generation) noexcept;

// Whether an effect lies over the glass.
bool EffectsOn() noexcept;

// The effect's clock: a frame drawn when one is due, the effect ended when its
// time is up. True when the picture changed.
bool EffectsStep() noexcept;

// When the effect next needs a step, in milliseconds of the ClockBoard's
// uptime; 0 when none lies over the glass.
std::uint64_t EffectsDue() noexcept;

// `Composed` — Cols x Rows cells and the caret — as the effect shows it, into
// `Glass`; Ink and Paper are the screen's own colours, which a colour no
// program chose is drawn in. The caret goes where its cell goes.
void EffectsApply(std::span<const GlassCell> Composed, std::vector<GlassCell> &Glass, Caret &At,
                  std::uint32_t Cols, std::uint32_t Rows, std::uint32_t Ink, std::uint32_t Paper);

#endif
