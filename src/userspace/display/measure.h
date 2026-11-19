#ifndef BOXOS_DISPLAY_MEASURE_H
#define BOXOS_DISPLAY_MEASURE_H

// Where the display's time goes, in a build made to measure it (make
// MEASURE=on): every part of its work clocked by the processor's own counter,
// each part's time its own — a part within another is taken out of the outer
// one — and once a second a few lines in the kernel's log of how that second
// went: the frames, the longest of them, the pixels composed and put on the
// glass, and the milliseconds of every part. At start, what each of the
// engine's ways costs a pixel on this machine, on every way of working rows
// it has (painter/core/cpu_path.h), and how fast the glass takes pixels. In
// any other build none of it is there, and nothing is clocked.

#include <cstdint>

class Easel;

enum class Part : std::uint8_t {
    Idle,       // asleep, waiting for something to happen
    Other,      // awake, in none of the parts below
    Rotate,     // the keyboard, lanes, the ring, questions and messages heard
    Intake,     // a canvas's page taken in
    Animate,    // animations brought to the frame's moment
    Console,    // what the console changed known, and where it moved
    Take,       // the areas' marks taken, what is to be composed again found
    Prepare,    // an item's drawing made again
    Compose,    // tiles composed, besides the drawing of their items below
    Kept,       // a kept picture laid
    Straight,   // an item drawn straight
    Turned,     // a kept picture laid through a turn or a scale
    Faded,      // two looks drawn apart and one faded toward the other
    Layered,    // a group's own picture made and laid
    Page,       // a canvas's page laid
    Put,        // a composed tile written into the mirror, dithered
    Lay,        // the console's cells written into the mirror straight
    Flush,      // the mirror onto the glass
    Count
};

#if defined(BOX_MEASURE)

// While one lives, the display's time is Into's.
class Clocked {
public:
    explicit Clocked(Part Into) noexcept;
    ~Clocked();
    Clocked(const Clocked &)            = delete;
    Clocked &operator=(const Clocked &) = delete;

private:
    Part Outer;
};

// The processor's counter now.
std::uint64_t MeasureTicks() noexcept;
// A frame composed from Began (MeasureTicks), Pixels of it composed.
void MeasureFrame(std::uint64_t Began, std::uint64_t Pixels) noexcept;
// Pixels put onto the glass.
void MeasureFlushed(std::uint64_t Pixels) noexcept;
// A key taken, the kernel having said it at Said (the counter's, as the
// Touch carries it); a frame on the glass since the keys taken before it.
void MeasureKey(std::uint64_t Said) noexcept;
void MeasureGlassed() noexcept;
// The alarm came, set for Moment (µs of uptime): how late.
void MeasureAlarm(std::uint64_t Moment) noexcept;
// The second's lines, once a second has gone.
void MeasureSecond() noexcept;
// What each of the engine's ways costs a pixel here, and the glass's speed —
// the mirror put on it whole again, the picture it shows unchanged.
void MeasureEngine(Easel &Glass) noexcept;

#else

class Clocked {
public:
    explicit Clocked(Part) noexcept {}
};

inline std::uint64_t MeasureTicks() noexcept { return 0; }
inline void          MeasureFrame(std::uint64_t, std::uint64_t) noexcept {}
inline void          MeasureFlushed(std::uint64_t) noexcept {}
inline void          MeasureKey(std::uint64_t) noexcept {}
inline void          MeasureGlassed() noexcept {}
inline void          MeasureAlarm(std::uint64_t) noexcept {}
inline void          MeasureSecond() noexcept {}
inline void          MeasureEngine(Easel &) noexcept {}

#endif

#endif
