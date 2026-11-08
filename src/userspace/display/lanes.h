#ifndef BOXOS_DISPLAY_LANES_H
#define BOXOS_DISPLAY_LANES_H

#include <cstdint>

#include "box/core/result.h"
#include "box/display.h"

// Console lanes — every printing strand's own stream of ConsoleRun frames,
// merged by the console's one order (box/display.h), and the ear: which lane
// hears the keyboard.

// A frame, met in the console's order; and the point where the frames met so
// far are to be drawn (the keys are sipped there, inside a long render).
using FrameSink  = void (*)(const ConsoleRun &Run);
using FrameFlush = void (*)();

// The keyboard's tag claimed, the console's order made. False: no keyboard.
bool LanesStart() noexcept;

// Keys said again on the lane that listens. True when any moved.
bool LanesKeys() noexcept;

// A death: grants the dead never attached are revoked; attached lanes drain.
void LanesDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept;

// Frames to the sink in the console's order — none numbered past Ceiling —
// then lanes that are over freed. True when anything moved.
bool LanesStep(FrameSink Sink, FrameFlush Flush, std::uint64_t Ceiling) noexcept;

// True when the lanes of (Pid, Generation) hold no frame numbered Mark or
// less: everything that strand said up to Mark has been met.
bool LanesDrawnTo(std::uint32_t Pid, std::uint32_t Generation, std::uint64_t Mark) noexcept;

// DISP_CMD_LANE, asked of this display (Asked, kind KCTX_ASK) by its sender
// in Generation: answered here — the lane's tag, or an honest refusal.
void LanesGrant(const Result &Asked, std::uint32_t Generation) noexcept;

// DISP_CMD_LISTEN.
void LanesListen(std::uint32_t Requester, std::uint32_t Generation, bool Listening) noexcept;

// The frames met up to Order are said into the log ring: the console's order
// page says so (CONSOLE_ORDER_SAID), and a writer waiting to leave is rung.
void LanesSaid(std::uint64_t Order) noexcept;

// Before sleeping: this pid hung on every lane, so a writer rings; after, the
// bells taken in again.
void LanesBellsHang(std::uint32_t Me) noexcept;
void LanesBellsTake() noexcept;

#endif
