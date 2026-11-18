#ifndef BOXOS_DISPLAY_INPUT_H
#define BOXOS_DISPLAY_INPUT_H

// The keyboard's way to the things programs show (box/items.h, KEYS): the
// display's Input part. A key goes first to the area the keyboard is in — the
// topmost seen area of the program in front, not one shown always, that holds
// a thing the keyboard reaches — and only what is not used there goes on to
// the program's lane. Tab and Shift+Tab go from one thing to the next in the
// order the program made them, the arrows to the nearest the way they point,
// Enter or Space presses what is reached, and a field reached is typed into by
// the display itself — its caret moved, its text edited, the program told the
// text as it is now; Escape leaves it. A chord shows a short label on every
// thing reached; typing one reaches it, and presses it if it is no field.
//
// How a thing looks reached or pressed is drawn over everything by the
// display, on an area of its own it speaks the items' language to — a ring
// round what is reached that glides to the next, a flash where one is
// pressed, a field's caret blinking, the labels — so the program is never
// woken for it. On a text page, where none of that is drawn, what is reached
// is shown by the page.

#include <cstdint>
#include <functional>

#include "compositor.h"
#include "kb_event.h"
#include "key_chord.h"

// What an area is told: an event, as areas.cpp tells it.
using InputTeller = std::function<void(std::uint16_t Kind, std::uint32_t Item, const void *Payload, std::uint32_t Bytes)>;

// The area the keyboard is in, and how it is told.
struct InputArea {
    items::AreaState *State = nullptr;
    InputTeller       Tell;
};

// The glass: on a framebuffer the display's own area is made over Screen, in
// points Scale pixels each, drawn in Look; on a text page there is none.
void InputStart(painter::core::Rect Screen, float Scale, bool Text, const items::Theme &Look);

// The chord that shows the labels.
void InputPoint(KeyChord Chord) noexcept;

// A key: true when the display used it, and it goes no further.
bool InputKey(const kb_event_t &Key) noexcept;

// Whether the keyboard reaches anything in Area.
bool InputReaches(const items::AreaState &Area) noexcept;

// The program asked the focus to Item of its area (box/items.h ITEM_OP_FOCUS),
// ITEM_AREA for none.
void InputFocusAsked(items::AreaState &Area, std::uint32_t Item, const InputTeller &Tell) noexcept;

// An area going: nothing of the display's own is left drawn for it.
void InputAreaGone(const items::AreaState &Area) noexcept;

// The display's own area brought to Now: the ring, the caret, the labels where
// what they are drawn for lies, and their animations played.
void InputFrame(std::uint64_t Now) noexcept;
// When the display's own area is next wanted, of the clock's microseconds; 0
// for never.
std::uint64_t InputDue() noexcept;
// The display's own area, to compose over every other; none on a text page.
items::AreaState *InputOverlay() noexcept;

#endif
