/* Reading the text of a sum: the names, the signs and the numbers. A number is
 * read exactly, so that 0.1 is one tenth and 1e-3 one thousandth. */

#include "calculate_read.h"

#include <format>

namespace {

bool IsDigit(char Ch) { return Ch >= '0' && Ch <= '9'; }
bool IsLetter(char Ch) { return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || Ch == '_'; }

/* The width in bytes of the character that begins here, so that a place never
 * cuts one in two. */
std::size_t CharacterWidth(std::string_view Text, std::size_t At)
{
    std::size_t Width = 1;
    while (At + Width < Text.size() && (static_cast<unsigned char>(Text[At + Width]) & 0xC0) == 0x80) Width++;
    return Width;
}

struct Radix {
    char        Letter;
    unsigned    Base;
    const char *Name;
};

const Radix Radixes[] = {{'x', 16, "hex"}, {'b', 2, "binary"}, {'o', 8, "octal"}};

unsigned DigitOf(char Ch)
{
    if (IsDigit(Ch)) return static_cast<unsigned>(Ch - '0');
    char Low = Lowered(Ch);
    if (Low >= 'a' && Low <= 'f') return static_cast<unsigned>(Low - 'a' + 10);
    return 99;
}

/* The digits of Text[Begin, End) with the separators taken out. A separator
 * stands between two digits and nowhere else. */
std::string CleanDigits(std::string_view Text, std::size_t Begin, std::size_t End)
{
    std::string Digits;
    for (std::size_t I = Begin; I < End; I++) {
        if (Text[I] != '_') {
            Digits += Text[I];
            continue;
        }
        bool Between = I > Begin && I + 1 < End && Text[I - 1] != '_' && Text[I + 1] != '_';
        if (!Between) throw CalculateError{I, I + 1, "a digit separator _ stands between two digits"};
    }
    return Digits;
}

Number WholeLiteral(std::string_view Text, std::size_t Begin, std::size_t End, const Radix &Kind)
{
    std::string Digits = CleanDigits(Text, Begin + 2, End);
    if (Digits.empty()) throw CalculateError{Begin, End, std::string("0") + Kind.Letter + " needs digits after it"};
    for (std::size_t I = Begin + 2; I < End; I++)
        if (Text[I] != '_' && DigitOf(Text[I]) >= Kind.Base)
            throw CalculateError{I, I + 1, std::format("'{}' is not a {} digit", Text[I], Kind.Name)};
    if (Digits.size() > MostBits) throw CalculateError{Begin, End, "that number has too many digits"};
    return Number::Whole(BigInt::FromDigits(Digits, Kind.Base));
}

/* A decimal is read exactly: 0.1 is one tenth, 1e-3 one thousandth. */
Number DecimalLiteral(std::string_view Text, std::size_t Begin, std::size_t &At)
{
    std::size_t Point = std::string_view::npos;
    std::size_t Stop  = At;
    while (Stop < Text.size() && (IsDigit(Text[Stop]) || Text[Stop] == '_')) Stop++;
    std::size_t WholeEnd = Stop;
    std::size_t FractionBegin = Stop, FractionEnd = Stop;
    if (Stop + 1 < Text.size() && Text[Stop] == '.' && IsDigit(Text[Stop + 1])) {
        Point         = Stop;
        FractionBegin = Stop + 1;
        Stop          = FractionBegin;
        while (Stop < Text.size() && (IsDigit(Text[Stop]) || Text[Stop] == '_')) Stop++;
        FractionEnd = Stop;
    }

    std::int64_t Power = 0;
    if (Stop < Text.size() && (Text[Stop] == 'e' || Text[Stop] == 'E')) {
        std::size_t Sign = Stop + 1;
        bool        Down = false;
        if (Sign < Text.size() && (Text[Sign] == '+' || Text[Sign] == '-')) {
            Down = Text[Sign] == '-';
            Sign++;
        }
        std::size_t Last = Sign;
        while (Last < Text.size() && IsDigit(Text[Last])) Last++;
        if (Last > Sign) {
            if (Last - Sign > 7) throw CalculateError{Begin, Last, "that exponent is too large"};
            for (std::size_t I = Sign; I < Last; I++) Power = Power * 10 + (Text[I] - '0');
            if (Down) Power = -Power;
            Stop = Last;
        }
    }
    At = Stop;

    std::string Digits = CleanDigits(Text, Begin, WholeEnd);
    std::size_t Places = 0;
    if (Point != std::string_view::npos) {
        std::string After = CleanDigits(Text, FractionBegin, FractionEnd);
        Places            = After.size();
        Digits += After;
    }
    if (Digits.size() > MostBits) throw CalculateError{Begin, At, "that number has too many digits"};

    std::int64_t Net = Power - static_cast<std::int64_t>(Places);
    if (Net > static_cast<std::int64_t>(MostDigits) || Net < -static_cast<std::int64_t>(MostDigits))
        throw CalculateError{Begin, At, "that number is too large or too small for calculate to hold"};
    BigInt Mantissa = BigInt::FromDigits(Digits, 10);
    try {
        if (Net >= 0) return Number::Of(Fraction::Whole(Mantissa * BigInt(10).Power(static_cast<std::uint64_t>(Net))));
        return Number::Of(Fraction::Of(Mantissa, BigInt(10).Power(static_cast<std::uint64_t>(-Net))));
    } catch (const Refusal &Why) {
        throw CalculateError{Begin, At, Why.Reason};
    }
}

Token ReadNumber(std::string_view Text, std::size_t &At)
{
    Token       Result;
    std::size_t Begin = At;
    Result.Type       = Kind::Number;

    if (Text[At] == '0' && At + 1 < Text.size()) {
        for (const Radix &Kind : Radixes) {
            if (Lowered(Text[At + 1]) != Kind.Letter) continue;
            std::size_t End = At + 2;
            while (End < Text.size() && (IsLetter(Text[End]) || IsDigit(Text[End]))) End++;
            Result.Value = WholeLiteral(Text, Begin, End, Kind);
            At           = End;
            Result.At    = {Begin, End};
            return Result;
        }
    }
    Result.Value = DecimalLiteral(Text, Begin, At);
    Result.At    = {Begin, At};
    return Result;
}

}

std::vector<Token> Tokenize(std::string_view Text)
{
    std::vector<Token> Tokens;
    std::size_t        At = 0;
    while (At < Text.size()) {
        char Ch = Text[At];
        if (Ch == ' ' || Ch == '\t') {
            At++;
        } else if (IsDigit(Ch) || (Ch == '.' && At + 1 < Text.size() && IsDigit(Text[At + 1]))) {
            Tokens.push_back(ReadNumber(Text, At));
        } else if (IsLetter(Ch)) {
            Token Word;
            Word.Type  = Kind::Name;
            std::size_t Begin = At;
            while (At < Text.size() && (IsLetter(Text[At]) || IsDigit(Text[At]))) At++;
            Word.Name = std::string(Text.substr(Begin, At - Begin));
            Word.At   = {Begin, At};
            Tokens.push_back(std::move(Word));
        } else if (std::string_view("+-*/%^!(),=").find(Ch) != std::string_view::npos) {
            Token Mark;
            Mark.Type   = Kind::Symbol;
            Mark.Symbol = Ch;
            Mark.At     = {At, At + 1};
            if (Ch == '*' && At + 1 < Text.size() && Text[At + 1] == '*') {
                Mark.Symbol = '^';
                Mark.At.End = At + 2;
            }
            At = Mark.At.End;
            Tokens.push_back(std::move(Mark));
        } else {
            std::size_t Width = CharacterWidth(Text, At);
            throw CalculateError{At, At + Width, "calculate does not know what this means in a sum"};
        }
    }
    Token Last;
    Last.At = {Text.size(), Text.size() + 1};
    Tokens.push_back(std::move(Last));
    return Tokens;
}

