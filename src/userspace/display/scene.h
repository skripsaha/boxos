#ifndef BOXOS_DISPLAY_SCENE_H
#define BOXOS_DISPLAY_SCENE_H

#include <cstdint>

#include "easel.h"

// The scene the loader left on the glass (boxos_scene.h, boxos_screen.h
// SCREEN_LEFT_SCENE), played on from that very frame while the machine gets
// ready, and then gone into the console: its enter act to its end, its wait
// act round and round, and its leave act once the machine waits for the
// person and enter is over — or gone at once when the person presses a key,
// the glass is turned over, or a program opens an area to show something
// (not one shown always, a clock in the corner). A line said in error does not
// take it away: the machine goes on, and the line is in the console, which is
// kept under the scene with every area and drawn whole when it goes. Each frame is drawn where it changed,
// the pixels of the brush the loader drew with (boxos_scene_play.h), onto the
// easel.

// The scene the claim says the loader left, read by its fid and its first
// frame made — the pixels the glass shows already. False: none is left, or it
// will not read (said), and the console is drawn from the start.
bool SceneBegin(Easel &On) noexcept;

// Whether the scene is on the glass.
bool SceneOn() noexcept;

// Its frame when one is due, the acts moved on — leave begun once Ready, the
// machine waiting for the person, and enter over; when leave is over the
// scene is gone. True when anything moved.
bool SceneStep(bool Ready) noexcept;

// The scene gone at once.
void SceneEnd() noexcept;

// The moment its next frame is due, in uptime ms; 0 when it is not on.
std::uint64_t SceneDue() noexcept;

#endif
