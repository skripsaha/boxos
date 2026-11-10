#ifndef KEY_CHORD_H
#define KEY_CHORD_H

/*
 * A chord: a key held with ctrl, alt or both — "ctrl+alt+q", "alt+tab",
 * "ctrl+shift+left". Read from a person's spelling (a setting of the kind
 * key, settings_reader.h), said back the same way, and matched against what
 * the keyboard says (kb_event.h): one reading for every program that lets a
 * person choose its keys.
 *
 * The names, apart by '+', in any case: ctrl, alt and shift, each once and in
 * any order, then the key. A key that types a character is named by the
 * character it types alone — a letter, a digit, or one of ` - = [ ] \ ; ' ,
 * . / — and a letter matches whichever case shift gives it. A key that types
 * none, or types space, by its name: tab, enter, space, escape, backspace,
 * up, down, left, right, home, end, page-up, page-down, insert, delete.
 *
 * Ctrl or alt is always among them: a key alone, or with shift alone, is
 * typing, and a chord taken from typing would take a letter from everybody.
 * Shift goes only with a letter or a named key: with any other character it
 * types another character, and that chord would never come.
 *
 * Includer must provide uint*_t, bool and size_t BEFORE including this header.
 */

#include "kb_event.h"

/* The modifiers a chord holds: the keyboard's own bits (kb_event.h). */
#define KEY_CHORD_MODS (KB_MOD_CTRL | KB_MOD_ALT | KB_MOD_SHIFT)

/* The longest spelling KeyChordSpell writes, with its NUL. */
#define KEY_CHORD_SPELLED_MAX 24u

typedef struct {
    uint8_t Mods;       /* KB_MOD_CTRL, KB_MOD_ALT, KB_MOD_SHIFT */
    uint8_t Extended;   /* 1: Code is the scancode of a key of the 0xE0 set */
    uint8_t Code;       /* else the character it types alone, a letter in lower case */
    uint8_t Reserved;
} KeyChord;

typedef struct {
    const char *Name;
    uint8_t     Extended;
    uint8_t     Code;
} KeyChordName;

#define KEY_CHORD_NAMES 15u

static inline const KeyChordName *KeyChordNames(void)
{
    static const KeyChordName names[KEY_CHORD_NAMES] = {
        { "tab", 0, (uint8_t)KEY_TAB },           { "enter", 0, (uint8_t)KEY_ENTER },
        { "space", 0, (uint8_t)' ' },             { "escape", 0, (uint8_t)KEY_ESCAPE },
        { "backspace", 0, (uint8_t)KEY_BACKSPACE }, { "up", 1, KEY_UP },
        { "down", 1, KEY_DOWN },                  { "left", 1, KEY_LEFT },
        { "right", 1, KEY_RIGHT },                { "home", 1, KEY_HOME },
        { "end", 1, KEY_END },                    { "page-up", 1, KEY_PAGEUP },
        { "page-down", 1, KEY_PAGEDN },           { "insert", 1, KEY_INSERT },
        { "delete", 1, KEY_DELETE },
    };
    return names;
}

static inline uint8_t KeyChordLower(uint8_t c)
{
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c - 'A' + 'a') : c;
}

/* Whether `length` bytes at `at` spell `word`, in any case. */
static inline bool KeyChordIs(const uint8_t *at, uint64_t length, const char *word)
{
    uint64_t i = 0;
    for (; i < length && word[i]; i++)
        if (KeyChordLower(at[i]) != (uint8_t)word[i]) return false;
    return i == length && word[i] == '\0';
}

/* A character a key types alone, that a chord may name. */
static inline bool KeyChordCharacter(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    const char *others = "`-=[]\\;',./";
    for (unsigned i = 0; others[i]; i++)
        if ((uint8_t)others[i] == c) return true;
    return false;
}

/* The chord `length` bytes at `text` spell, into *out: false when they spell
 * none — a name not known, one said twice, no key last, no ctrl or alt, or
 * shift with a character it would change. */
static inline bool KeyChordRead(const uint8_t *text, uint64_t length, KeyChord *out)
{
    KeyChord chord = { 0, 0, 0, 0 };
    bool     keyed = false;
    uint64_t at    = 0;
    while (at <= length) {
        uint64_t end = at;
        while (end < length && text[end] != '+') end++;
        const uint8_t *name = text + at;
        uint64_t       size = end - at;
        if (size == 0 || keyed) return false;
        uint8_t mod = KeyChordIs(name, size, "ctrl") ? KB_MOD_CTRL
                    : KeyChordIs(name, size, "alt")  ? KB_MOD_ALT
                    : KeyChordIs(name, size, "shift") ? KB_MOD_SHIFT
                                                     : 0;
        if (mod != 0 && end < length) {
            if (chord.Mods & mod) return false;
            chord.Mods |= mod;
        } else if (size == 1 && KeyChordCharacter(KeyChordLower(name[0]))) {
            chord.Code = KeyChordLower(name[0]);
            keyed      = true;
        } else {
            const KeyChordName *names = KeyChordNames();
            unsigned            i     = 0;
            while (i < KEY_CHORD_NAMES && !KeyChordIs(name, size, names[i].Name)) i++;
            if (i == KEY_CHORD_NAMES) return false;
            chord.Extended = names[i].Extended;
            chord.Code     = names[i].Code;
            keyed          = true;
        }
        at = end + 1;
    }
    if (!keyed || !(chord.Mods & (KB_MOD_CTRL | KB_MOD_ALT))) return false;
    bool letter = !chord.Extended && chord.Code >= 'a' && chord.Code <= 'z';
    bool named  = chord.Extended || !KeyChordCharacter(chord.Code);
    if ((chord.Mods & KB_MOD_SHIFT) && !letter && !named) return false;
    *out = chord;
    return true;
}

static inline bool KeyChordSame(const KeyChord *a, const KeyChord *b)
{
    return a->Mods == b->Mods && a->Extended == b->Extended && a->Code == b->Code;
}

/* Whether a key the keyboard said is the chord. */
static inline bool KeyChordMatches(const KeyChord *chord, const kb_event_t *key)
{
    if ((key->mods & KEY_CHORD_MODS) != chord->Mods) return false;
    bool extended = (key->mods & KB_MOD_EXTENDED) != 0;
    if (extended != (chord->Extended != 0)) return false;
    return extended ? key->scancode == chord->Code : KeyChordLower((uint8_t)key->ascii) == chord->Code;
}

/* The chord said as it is read — "ctrl+alt+q" — into out, NUL-ended, cut to
 * capacity; KEY_CHORD_SPELLED_MAX always holds it whole. */
static inline void KeyChordSpell(const KeyChord *chord, char *out, size_t capacity)
{
    static const struct {
        uint8_t     Mod;
        const char *Name;
    } mods[3] = { { KB_MOD_CTRL, "ctrl+" }, { KB_MOD_ALT, "alt+" }, { KB_MOD_SHIFT, "shift+" } };
    char        own[2] = { (char)chord->Code, '\0' };
    const char *key    = own;
    const KeyChordName *names = KeyChordNames();
    for (unsigned i = 0; i < KEY_CHORD_NAMES; i++)
        if (names[i].Extended == chord->Extended && names[i].Code == chord->Code) key = names[i].Name;
    size_t used = 0;
    if (capacity == 0) return;
    for (unsigned m = 0; m < 3; m++) {
        if (!(chord->Mods & mods[m].Mod)) continue;
        for (const char *c = mods[m].Name; *c && used + 1 < capacity; c++) out[used++] = *c;
    }
    for (const char *c = key; *c && used + 1 < capacity; c++) out[used++] = *c;
    out[used] = '\0';
}

#endif /* KEY_CHORD_H */
