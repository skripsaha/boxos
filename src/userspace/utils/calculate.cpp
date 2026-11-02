/* calculate — a sum worked out exactly where it can be.
 *
 *   calculate 2+2*3          7
 *   calculate 1/3 + 1/6      1/2 = 0.5
 *   calculate 2^100          1267650600228229401496703205376
 *   calculate sqrt(2)        1.4142135623730951
 *   calculate 255 in hex     0xff
 *   calculate                a calculator: sums one after another
 *
 * Whole numbers and fractions are exact and as large as 100000 digits; what is
 * not a fraction (a root of a non-square, a sine, pi) is a decimal number from
 * there on. In the calculator `ans` is the last answer and `x = 5` keeps one
 * under a name; an empty line, `quit` or `q` leaves. Under an arrow only the
 * answers go to the file: the prompt and any refusal are for the person.
 * The number is calculate_integer.cpp and calculate_fraction.cpp, its
 * operators calculate_math.cpp, its look calculate_show.cpp, and the reading
 * of a sum calculate_parse.cpp.
 */

#include <algorithm>
#include <cstdlib>
#include <format>
#include <new>
#include <string>
#include <string_view>

#include "box/color.h"
#include "box/form.h"
#include "box/print.h"

#include "calculate.h"

BOX_FORM("takes optional text \"the sum to work out\"\n"
         "gives text\n");

namespace {

constexpr Color AnswerShade      = COLOR_WHITE;
constexpr Color ApproximateShade = COLOR_RGB(0x88, 0x88, 0x88);
constexpr Color PromptShade      = COLOR_CYAN;
constexpr Color ErrorShade       = COLOR_RED;

constexpr std::string_view Prompt = "calc> ";

/* Where the text goes: the answers to wherever an arrow sends the program's
 * output, anything meant for the person to the screen itself. */
enum class Where { Output, Person };

void Paint(Color Shade, std::string_view Text, Where Goes = Where::Output)
{
    Color Before = get_color();
    set_color(Shade);
    if (Goes == Where::Person) print_bytes_to_screen(Text.data(), Text.size());
    else                       print_bytes(Text.data(), Text.size());
    set_color(Before);
}

void SayToPerson(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_bytes_to_screen(Line.data(), Line.size());
}

/* The columns before Place: a character is one, however many bytes it takes. */
std::size_t Columns(std::string_view Text, std::size_t Place)
{
    std::size_t Count = 0;
    for (std::size_t I = 0; I < Place && I < Text.size(); I++)
        if ((static_cast<unsigned char>(Text[I]) & 0xC0) != 0x80) Count++;
    return Count + (Place > Text.size() ? Place - Text.size() : 0);
}

/* A refusal: the caret under the place in the sum, and what is wrong. Indent
 * is where the sum begins on the line above, when it is on the screen already. */
void Refuse(std::string_view Sum, const CalculateError &Error, std::size_t Indent, bool EchoSum)
{
    if (EchoSum) SayToPerson(Sum);
    std::size_t From  = Columns(Sum, Error.Begin);
    std::size_t Width = std::max<std::size_t>(Columns(Sum, Error.End) - From, 1);
    std::string Line(Indent + From, ' ');
    Line.append(Width, '^');
    Line += ' ';
    Line += Error.Reason;
    Line += '\n';
    Paint(ErrorShade, Line, Where::Person);
}

void Announce(const Answer &Result)
{
    Shown Written = Show(Result.Value, Result.Base);
    if (!Result.Assigned.empty()) Paint(ApproximateShade, Result.Assigned + " = ");
    Paint(AnswerShade, Written.Value);
    if (!Written.Decimal.empty()) Paint(ApproximateShade, " = " + Written.Decimal);
    Paint(AnswerShade, "\n");
}

/* One sum, worked and written; false when it was refused. */
bool Work(Calculator &Calc, std::string_view Sum, std::size_t Indent, bool EchoSum)
{
    try {
        Announce(Calc.Work(Sum));
        return true;
    } catch (const CalculateError &Error) {
        Refuse(Sum, Error, Indent, EchoSum);
    } catch (const std::bad_alloc &) {
        SayToPerson("calculate: there is no memory for this sum");
    }
    return false;
}

std::string_view Trimmed(std::string_view Text)
{
    while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t')) Text.remove_prefix(1);
    while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t')) Text.remove_suffix(1);
    return Text;
}

bool Leaving(std::string_view Line)
{
    return Line.empty() || Line == "quit" || Line == "q";
}

void Interactive(Calculator &Calc)
{
    Paint(ApproximateShade, "calculate: type a sum. ans is the last answer, x = 5 keeps a number; an empty line leaves.\n", Where::Person);
    for (;;) {
        Paint(PromptShade, Prompt, Where::Person);
        char *Typed = nullptr;
        int   Length = readline_grow(&Typed);
        if (Length < 0) return;

        std::string_view Line = Trimmed(std::string_view(Typed, static_cast<std::size_t>(Length)));
        bool             Done = Leaving(Line);
        if (!Done) Work(Calc, Line, Prompt.size() + static_cast<std::size_t>(Typed ? Line.data() - Typed : 0), false);
        std::free(Typed);
        if (Done) return;
    }
}

}

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;

        Calculator Calc;
        if (Count == 0) {
            Interactive(Calc);
            return 0;
        }
        const char *Text = form_text(0);
        if (!Text) return 1;
        return Work(Calc, Trimmed(Text), 0, true) ? 0 : 1;
    } catch (const std::bad_alloc &) {
        SayToPerson("calculate: there is no memory for this sum");
        return 1;
    }
}
