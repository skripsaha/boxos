/* benchmark — how fast and how smooth this machine runs BoxOS, measured on it.
 *
 *   benchmark            what it measures
 *   benchmark graphics   the drawing engine, and scenes the display shows —
 *                        simple and heavy, one at a time and all at once
 *
 * Every line found is said on the console and written to a file on the
 * volume (benchmark-graphics.log for graphics), to be read on another machine
 * — `make logs FILE=benchmark-graphics.log` — and set beside other runs. */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "benchmark.h"
#include "box/current.h"
#include "box/error.h"
#include "box/form.h"

BOX_FORM("takes optional words \"what to measure: graphics\"\n");

namespace benchmark {

void Report::Say(const char *Format, ...)
{
    char    Line[640];
    va_list Rest;
    va_start(Rest, Format);
    std::vsnprintf(Line, sizeof Line, Format, Rest);
    va_end(Rest);
    Lines.emplace_back(Line);
}

void Report::Print()
{
    for (; Printed < Lines.size(); Printed++) std::printf("%s\n", Lines[Printed].c_str());
}

bool Report::Save(const char *Name)
{
    const std::string Target = std::string("file:") + Name;
    error_t           Why    = OK;
    Current          *Out    = current_open_ex(Target.c_str(), CURRENT_WRITE, 0, CURRENT_CREATE | CURRENT_FRESH, &Why);
    if (!Out) {
        std::printf("benchmark: %s could not be written (error %u)\n", Name, static_cast<unsigned>(Why));
        return false;
    }
    bool Whole = true;
    for (const std::string &Each : Lines) {
        const std::string Line = Each + "\n";
        for (std::size_t Off = 0; Whole && Off < Line.size();) {
            const int Wrote = current_write(Out, Line.data() + Off, Line.size() - Off);
            if (Wrote <= 0) Whole = false;
            else Off += static_cast<std::size_t>(Wrote);
        }
    }
    current_close(Out);
    current_release(Out);
    std::printf(Whole ? "benchmark: written to %s\n" : "benchmark: %s stopped taking lines; it is not whole\n", Name);
    return Whole;
}

}  // namespace benchmark

int main()
{
    const char *const *Words;
    std::size_t        Count;
    if (form_words(0, &Words, &Count) != 0) return 1;
    if (Count == 0) {
        std::printf("benchmark graphics   the drawing engine, and scenes the display shows: how smooth each goes\n");
        return 0;
    }
    if (std::strcmp(Words[0], "graphics") == 0) return benchmark::Graphics();
    std::printf("benchmark: %s is not measured — what is: graphics\n", Words[0]);
    return 1;
}
