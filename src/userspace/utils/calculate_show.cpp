/* How an answer is written. A whole number is written in full. A fraction is
 * written as it is and, beside it, as a decimal worked out by long division on
 * the whole numbers, so every digit shown is a true one: cut off, never rounded.
 * A decimal that ends is shown whole; one that does not, or is longer than
 * LongestEnding digits, is shown to SignificantDigits and then "...". A decimal
 * number (a double) is shown to 15 digits, where the 16th and 17th are noise. */

#include "calculate.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

constexpr std::size_t SignificantDigits = 30;
constexpr std::size_t LongestEnding     = 100;
constexpr std::size_t DoubleDigits      = 15;
/* Three dots, not the one character U+2026: the glass draws a 256-glyph font
 * that holds no such character, and it would be a solid box (font/glyph_fold.h). */
constexpr const char  Goes[]            = "...";

/* How many digits a decimal of 1/Denominator has after the point when it ends:
 * that is when the denominator is two to some power times five to another. */
std::optional<std::size_t> DigitsBeforeEnd(const BigInt &Denominator)
{
    std::size_t Twos = Denominator.TrailingZeroBits();
    BigInt      Rest = Denominator.ShiftedRight(Twos);
    if (Rest.IsOne()) return Twos;

    BigInt Five(5);
    std::int64_t Near = static_cast<std::int64_t>(static_cast<double>(Rest.BitLength() - 1) / std::log2(5.0));
    for (std::int64_t Fives = std::max<std::int64_t>(Near - 1, 1); Fives <= Near + 2; Fives++)
        if (Five.Power(static_cast<std::uint64_t>(Fives)) == Rest) return std::max(Twos, static_cast<std::size_t>(Fives));
    return std::nullopt;
}

/* The zeros between the point and the first digit of a number above zero and
 * below one. The size of the numbers puts it within a few of the answer, and
 * the last steps are counted. */
std::size_t ZerosAfterPoint(const BigInt &Numerator, const BigInt &Denominator)
{
    std::int64_t Gap  = static_cast<std::int64_t>(Denominator.BitLength()) - 1 - static_cast<std::int64_t>(Numerator.BitLength());
    std::size_t  Zeros = Gap > 1 ? static_cast<std::size_t>(static_cast<double>(Gap - 1) * 0.30103) : 0;
    BigInt       Moved = Numerator * BigInt(10).Power(Zeros);
    for (;;) {
        BigInt Next = Moved * BigInt(10);
        if (Next >= Denominator) return Zeros;
        Moved = std::move(Next);
        Zeros++;
    }
}

std::string DecimalOf(const Fraction &Value)
{
    BigInt Numerator = Value.Numerator.Absolute();
    BigInt Whole, Rest;
    BigInt::DivideTruncated(Numerator, Value.Denominator, Whole, Rest);
    std::size_t WholeDigits = Whole.IsZero() ? 0 : Whole.ToString().size();

    std::optional<std::size_t> Ends = DigitsBeforeEnd(Value.Denominator);
    bool                       More = true;
    std::size_t                Places;
    if (Ends && WholeDigits + *Ends <= LongestEnding) {
        Places = *Ends;
        More   = false;
    } else if (!Whole.IsZero()) {
        Places = WholeDigits >= SignificantDigits ? 10 : SignificantDigits - WholeDigits;
    } else {
        Places = ZerosAfterPoint(Numerator, Value.Denominator) + SignificantDigits;
    }

    BigInt Scaled, Ignored;
    BigInt::DivideTruncated(Numerator * BigInt(10).Power(Places), Value.Denominator, Scaled, Ignored);
    std::string Digits = Scaled.ToString();
    if (Digits.size() < Places + 1) Digits.insert(0, Places + 1 - Digits.size(), '0');

    std::string Text = Value.Numerator.IsNegative() ? "-" : "";
    Text += Digits.substr(0, Digits.size() - Places);
    std::string Fractional = Digits.substr(Digits.size() - Places);
    if (!More) {
        while (!Fractional.empty() && Fractional.back() == '0') Fractional.pop_back();
    }
    if (!Fractional.empty()) Text += "." + Fractional;
    if (More) Text += Goes;
    return Text;
}

/* Fifteen significant digits, the zeros at the end taken off. Between 1e-5 and
 * 1e15 the point is placed in the digits; beyond, the power of ten is said. */
std::string DoubleText(double Value)
{
    std::string Scientific = std::format("{:.{}e}", Value, DoubleDigits - 1);
    bool        Negative   = Scientific[0] == '-';
    std::size_t Letter     = Scientific.find('e');
    int         Power      = std::stoi(Scientific.substr(Letter + 1));
    std::string Digits;
    for (std::size_t I = Negative ? 1 : 0; I < Letter; I++)
        if (Scientific[I] != '.') Digits += Scientific[I];
    while (Digits.size() > 1 && Digits.back() == '0') Digits.pop_back();

    std::string Text = Negative ? "-" : "";
    if (Power < -5 || Power >= static_cast<int>(DoubleDigits)) {
        Text += Digits.substr(0, 1);
        if (Digits.size() > 1) Text += "." + Digits.substr(1);
        return Text + "e" + std::to_string(Power);
    }
    if (Power < 0) return Text + "0." + std::string(static_cast<std::size_t>(-Power - 1), '0') + Digits;
    std::size_t Whole = static_cast<std::size_t>(Power) + 1;
    if (Digits.size() <= Whole) return Text + Digits + std::string(Whole - Digits.size(), '0');
    return Text + Digits.substr(0, Whole) + "." + Digits.substr(Whole);
}

}

Shown Show(const Number &Value, unsigned Base)
{
    Shown Result;
    if (!Value.Exact) {
        Result.Value = DoubleText(Value.Approximate);
        return Result;
    }
    if (Value.Value.IsInteger()) {
        const BigInt &Whole = Value.Value.Numerator;
        if (Base == 2 || Base == 8 || Base == 16) {
            const char *Prefix = Base == 2 ? "0b" : Base == 8 ? "0o" : "0x";
            Result.Value       = (Whole.IsNegative() ? "-" : "") + std::string(Prefix) + Whole.Absolute().ToString(Base);
        } else {
            Result.Value = Whole.ToString();
        }
        return Result;
    }
    Result.Value   = Value.Value.Numerator.ToString() + "/" + Value.Value.Denominator.ToString();
    Result.Decimal = DecimalOf(Value.Value);
    return Result;
}
