#ifndef BOXOS_DISPLAY_LANES_H
#define BOXOS_DISPLAY_LANES_H

#include <cstdint>

#include "box/core/result.h"
#include "box/display.h"
#include "boxos_kctx.h"

// Console lanes — every printing strand's own stream of ConsoleRun frames,
// merged by the console's one order (box/display.h), and the ear: which lane
// hears the keyboard.

// A frame, met in the console's order, and the lane it came on (numbered from
// 1; 0 is never a lane); and the point where the frames met so far are to be
// drawn (the keys are sipped there, inside a long render).
using FrameSink  = void (*)(const ConsoleRun &Run, std::uint32_t Lane);
using FrameFlush = void (*)();

// What became of a lane, as FrameSink numbers it, that the console keeps
// something of: it went behind while its strand read (front.h), came forward
// to read again, or closed — its number free for another.
enum class LaneTurn { Behind, Forward, Closed };
using LaneTold = void (*)(std::uint32_t Lane, LaneTurn Turn);

// The keyboard's tag claimed, the console's order made, and whom a lane's turn
// is told. False: no keyboard.
bool LanesStart(LaneTold Told) noexcept;

// Every key the keyboard said taken — the two chords told (front.h), the rest
// kept in order — and the kept ones said again on the lane that listens. True
// when any moved.
bool LanesKeys() noexcept;

// A death: grants the dead never attached are revoked; attached lanes drain.
void LanesDeath(std::uint32_t Pid, std::uint32_t Generation) noexcept;

// Frames to the sink in the console's order — none numbered past Ceiling —
// then lanes that are over freed. True when anything moved.
bool LanesStep(FrameSink Sink, FrameFlush Flush, std::uint64_t Ceiling) noexcept;

// True when the lanes of (Pid, Generation) hold no frame numbered Mark or
// less: everything that strand said up to Mark has been met.
bool LanesDrawnTo(std::uint32_t Pid, std::uint32_t Generation, std::uint64_t Mark) noexcept;

// DISP_CMD_LANE, asked of this display (Asked, kind KCTX_ASK) by the strand
// From names: answered here — the lane's tag, or an honest refusal.
void LanesGrant(const Result &Asked, const AskFrom &From) noexcept;

// DISP_CMD_LISTEN. A strand of a program behind (front.h) that asks to listen
// waits for the program to come forward.
void LanesListen(std::uint32_t Requester, std::uint32_t Generation, bool Listening) noexcept;

// Each lane held again to the rule — it hears while its strand reads and its
// program is not behind (front.h): after a program came forward, went behind
// or died.
void LanesRecheck() noexcept;

// A notice said on each lane of the program that reads now (box/display.h
// ConsoleNotice): true when a strand of it holds its lane's tag to hear it.
bool LanesNotice(std::uint32_t ProgramPid, std::uint32_t ProgramGeneration, std::uint8_t Kind) noexcept;

// The lane that hears the keyboard now, as FrameSink numbers it; 0 for none.
std::uint32_t LanesTyping() noexcept;

// The frames met up to Order are said into the log ring: the console's order
// page says so (CONSOLE_ORDER_SAID), and a writer waiting to leave is rung.
void LanesSaid(std::uint64_t Order) noexcept;

// Before sleeping: this pid hung on every lane, so a writer rings; after, the
// bells taken in again.
void LanesBellsHang(std::uint32_t Me) noexcept;
void LanesBellsTake() noexcept;

#endif
