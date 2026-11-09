#ifndef BOXOS_DISPLAY_FRONT_H
#define BOXOS_DISPLAY_FRONT_H

#include <cstdint>

#include "box/core/result.h"
#include "box/display.h"
#include "box/keyboard.h"
#include "boxos_kctx.h"

// The program in front of the person and the ones behind (box/display.h):
// what keepers ask, the two chords told to the keeper of the program in
// front, and which programs hear no key.

// The news tag, and the ear for programs being started. False: neither.
bool FrontStart() noexcept;

// DISP_CMD_FRONT, _BACK and _ASK_END, asked of this display (Asked) by the
// keeper From names: answered here, a refusal too.
void FrontAsked(const Result &Asked, const AskFrom &From, const DisplayFrontRequest &Request) noexcept;

// A key the keyboard said: true when it is one of the two chords — told to the
// keeper of the program in front, and taken, never typed.
bool FrontChord(const kb_event_t &Key) noexcept;

// Whether the program lives behind — let go itself, or started, however far
// down, from one that was: none of its strands hears a key.
bool FrontBehind(std::uint32_t ProgramPid, std::uint32_t ProgramGeneration) noexcept;

// A strand of a program behind asks for the keyboard: the keeper of the
// program it lives behind with is told, once until that one comes forward.
void FrontWants(std::uint32_t ProgramPid, std::uint32_t ProgramGeneration) noexcept;

// A death: what the dead kept or was kept as let go, and the programs it
// started counted as started by the one that started it — those of one let
// go staying behind, kept by its keeper.
void FrontDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept;

// The programs started since, each with the program it was started from.
// True when any was heard.
bool FrontSpawnStep() noexcept;

#endif
