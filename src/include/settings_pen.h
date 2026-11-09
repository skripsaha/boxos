#ifndef SETTINGS_PEN_H
#define SETTINGS_PEN_H

/*
 * The pen the person's settings file (settings_reader.h) is changed with —
 * by the utility settings and by a program changing its own settings at the
 * person's word. A change touches the lines of the one setting it is about
 * and no other byte: comments, the order of the lines, names this program
 * does not declare (another version's) and `when` lines stay as they were.
 *
 * Each change is one function called twice: with no `out` it measures the
 * file it makes into *bytes, with `out` (that much room) it writes it. The
 * value is the caller's to have read with SettingValueRead first: the pen
 * writes what it is given.
 *
 * No allocation and no library. Integer types come from the includer.
 */

#include "settings_reader.h"

static inline void SettingsPenPut(uint8_t *out, uint64_t *at, const uint8_t *bytes, uint64_t count)
{
    if (out)
        for (uint64_t i = 0; i < count; i++) out[*at + i] = bytes[i];
    *at += count;
}

static inline void SettingsPenText(uint8_t *out, uint64_t *at, const char *text)
{
    uint64_t length = 0;
    while (text[length] != '\0') length++;
    SettingsPenPut(out, at, (const uint8_t *)text, length);
}

/* Whether a value must go between double quotes to be read back as it is:
 * empty, a blank at an end, the word default, or quotes at both ends. */
static inline bool SettingsPenQuotes(SettingSpan value)
{
    return value.Length == 0 || SettingBlank(value.At[0]) || SettingBlank(value.At[value.Length - 1]) ||
           SettingSpanIs(value, "default") ||
           (value.Length >= 2 && value.At[0] == '"' && value.At[value.Length - 1] == '"');
}

/* "NAME VALUE\n". */
static inline void SettingsPenLine(uint8_t *out, uint64_t *at, SettingSpan name, SettingSpan value)
{
    bool quotes = SettingsPenQuotes(value);
    SettingsPenPut(out, at, name.At, name.Length);
    SettingsPenText(out, at, quotes ? " \"" : " ");
    SettingsPenPut(out, at, value.At, value.Length);
    SettingsPenText(out, at, quotes ? "\"\n" : "\n");
}

/* The file with `name` set to `value`: its last line for the name rewritten
 * where it stands and every other line for it taken out — or, with none, the
 * line added at the end. A file that is not there yet (no bytes) begins with
 * "settings 1". */
static inline void SettingsPenSet(const uint8_t *file, uint64_t size, SettingSpan name, SettingSpan value,
                                  uint8_t *out, uint64_t *bytes)
{
    SettingsLine   last, line;
    SettingsChosen chosen;
    uint64_t       at    = 0;
    bool           found = SettingsChosenFind(file, size, name, &last);

    if (size == 0) SettingsPenText(out, &at, "settings " SETTINGS_VERSION_WORD "\n");
    SettingsChosenBegin(file, size, &chosen);
    while (SettingsChosenNext(&chosen, &line)) {
        bool mine = line.Kind == SETTINGS_LINE_SET && SettingSpanSame(line.Name, name);
        if (!mine) SettingsPenPut(out, &at, file + line.Start, line.End - line.Start);
        else if (line.Line == last.Line) SettingsPenLine(out, &at, name, value);
    }
    if (!found) {
        if (size > 0 && file[size - 1] != '\n') SettingsPenText(out, &at, "\n");
        SettingsPenLine(out, &at, name, value);
    }
    *bytes = at;
}

/* The file with every line for `name` taken out: the program's own value. */
static inline void SettingsPenErase(const uint8_t *file, uint64_t size, SettingSpan name, uint8_t *out,
                                    uint64_t *bytes)
{
    SettingsChosen chosen;
    SettingsLine   line;
    uint64_t       at = 0;
    SettingsChosenBegin(file, size, &chosen);
    while (SettingsChosenNext(&chosen, &line))
        if (!(line.Kind == SETTINGS_LINE_SET && SettingSpanSame(line.Name, name)))
            SettingsPenPut(out, &at, file + line.Start, line.End - line.Start);
    *bytes = at;
}

/* The file with every line for a setting the declaration declares taken out:
 * everything as the program came. Comments, names it does not declare and
 * `when` lines stay. */
static inline void SettingsPenClear(const uint8_t *file, uint64_t size, const uint8_t *declaration,
                                    uint64_t declaration_size, uint8_t *out, uint64_t *bytes)
{
    SettingsChosen chosen;
    SettingsLine   line;
    Setting        setting;
    uint64_t       at = 0;
    SettingsChosenBegin(file, size, &chosen);
    while (SettingsChosenNext(&chosen, &line))
        if (!(line.Kind == SETTINGS_LINE_SET &&
              SettingsDeclaredFind(declaration, declaration_size, line.Name, &setting)))
            SettingsPenPut(out, &at, file + line.Start, line.End - line.Start);
    *bytes = at;
}

#endif /* SETTINGS_PEN_H */
