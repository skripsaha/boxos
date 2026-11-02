/* The fraction: two whole numbers in lowest terms. */

#include "calculate.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

Refusal TooLarge()
{
    return Refusal{"the answer would have more than 100000 digits, more than calculate will write out"};
}

/* A product of these two is only begun when it fits: a long reduction after a
 * product twice the size would take longer than a sum is worth. */
void MustFit(const BigInt &A, const BigInt &B)
{
    if (A.BitLength() + B.BitLength() > MostBits) throw TooLarge();
}

Fraction Lowest(BigInt Numerator, BigInt Denominator)
{
    Fraction Result = Fraction::Of(std::move(Numerator), std::move(Denominator));
    if (Result.Numerator.BitLength() > MostBits || Result.Denominator.BitLength() > MostBits) throw TooLarge();
    return Result;
}

}

Fraction Fraction::Of(BigInt Numerator, BigInt Denominator)
{
    Fraction Result;
    if (Numerator.IsZero()) return Result;
    if (Denominator.IsNegative()) {
        Numerator   = Numerator.Negated();
        Denominator = Denominator.Negated();
    }
    if (!Denominator.IsOne()) {
        BigInt Common = BigInt::Gcd(Numerator, Denominator);
        if (!Common.IsOne()) {
            BigInt Rest;
            BigInt::DivideTruncated(Numerator, Common, Numerator, Rest);
            BigInt::DivideTruncated(Denominator, Common, Denominator, Rest);
        }
    }
    Result.Numerator   = std::move(Numerator);
    Result.Denominator = std::move(Denominator);
    return Result;
}

Fraction Fraction::Whole(BigInt Value)
{
    Fraction Result;
    Result.Numerator = std::move(Value);
    return Result;
}

Fraction Fraction::Negated() const
{
    Fraction Result = *this;
    Result.Numerator = Numerator.Negated();
    return Result;
}

int Fraction::Compare(const Fraction &Other) const
{
    if (Denominator == Other.Denominator) return Numerator.Compare(Other.Numerator);
    return (Numerator * Other.Denominator).Compare(Other.Numerator * Denominator);
}

BigInt Fraction::Floor() const
{
    BigInt Quotient, Rest;
    BigInt::DivideTruncated(Numerator, Denominator, Quotient, Rest);
    return Rest.IsNegative() ? Quotient - BigInt(1) : Quotient;
}

BigInt Fraction::Ceiling() const
{
    return Negated().Floor().Negated();
}

BigInt Fraction::Round() const
{
    BigInt Quotient, Rest;
    BigInt::DivideTruncated(Numerator.Absolute().ShiftedLeft(1) + Denominator, Denominator.ShiftedLeft(1), Quotient, Rest);
    return Numerator.IsNegative() ? Quotient.Negated() : Quotient;
}

double Fraction::Mantissa(long &Exponent) const
{
    if (IsZero()) {
        Exponent = 0;
        return 0;
    }
    BigInt Top    = Numerator.Absolute();
    long   Shift  = 63 + static_cast<long>(Denominator.BitLength()) - static_cast<long>(Top.BitLength());
    BigInt Scaled = Shift >= 0 ? Top.ShiftedLeft(static_cast<std::size_t>(Shift)) : Top;
    BigInt Under  = Shift >= 0 ? Denominator : Denominator.ShiftedLeft(static_cast<std::size_t>(-Shift));
    BigInt Quotient, Rest;
    BigInt::DivideTruncated(Scaled, Under, Quotient, Rest);

    /* The quotient has 63 or 64 bits; a bit set below them says "and more",
     * which is all that rounding to 53 bits needs to know. */
    std::uint64_t Bits = Quotient.LowBits();
    if (!Rest.IsZero()) Bits |= 1;
    int    Power;
    double Fractional = std::frexp(static_cast<double>(Bits), &Power);
    Exponent          = Power - Shift;
    return Numerator.IsNegative() ? -Fractional : Fractional;
}

double Fraction::ToDouble() const
{
    long   Exponent;
    double Fractional = Mantissa(Exponent);
    if (Exponent > 2000) return Fractional < 0 ? -HUGE_VAL : HUGE_VAL;
    if (Exponent < -2200) return 0;
    return std::ldexp(Fractional, static_cast<int>(Exponent));
}

std::optional<Fraction> Fraction::ExactRoot(unsigned Degree) const
{
    if (Numerator.IsNegative() && Degree % 2 == 0) return std::nullopt;
    bool   Whole, Under;
    BigInt Top = Numerator.Absolute().Root(Degree, Whole);
    BigInt Low = Denominator.Root(Degree, Under);
    if (!Whole || !Under) return std::nullopt;
    Fraction Result;
    Result.Numerator   = Numerator.IsNegative() ? Top.Negated() : Top;
    Result.Denominator = Low;
    return Result;
}

Fraction operator+(const Fraction &A, const Fraction &B)
{
    if (A.Denominator == B.Denominator) return Lowest(A.Numerator + B.Numerator, A.Denominator);
    MustFit(A.Numerator, B.Denominator);
    MustFit(B.Numerator, A.Denominator);
    MustFit(A.Denominator, B.Denominator);
    return Lowest(A.Numerator * B.Denominator + B.Numerator * A.Denominator, A.Denominator * B.Denominator);
}

Fraction operator-(const Fraction &A, const Fraction &B)
{
    return A + B.Negated();
}

Fraction operator*(const Fraction &A, const Fraction &B)
{
    MustFit(A.Numerator, B.Numerator);
    MustFit(A.Denominator, B.Denominator);
    return Lowest(A.Numerator * B.Numerator, A.Denominator * B.Denominator);
}

Fraction operator/(const Fraction &A, const Fraction &B)
{
    MustFit(A.Numerator, B.Denominator);
    MustFit(A.Denominator, B.Numerator);
    return Lowest(A.Numerator * B.Denominator, A.Denominator * B.Numerator);
}

Fraction PowerOf(const Fraction &Base, const BigInt &Exponent)
{
    if (Exponent.IsZero()) return Fraction::Whole(BigInt(1));
    if (Base.IsZero()) {
        if (Exponent.IsNegative()) throw Refusal{"division by zero", 0};
        return Base;
    }
    bool Flips = Exponent.IsNegative();
    if (Base.IsInteger() && Base.Numerator.Absolute().IsOne())
        return Fraction::Whole(!Exponent.IsEven() && Base.Numerator.IsNegative() ? BigInt(-1) : BigInt(1));

    std::optional<std::uint64_t> Times = Exponent.Absolute().ToUnsigned();
    std::size_t Widest = std::max(Base.Numerator.BitLength(), Base.Denominator.BitLength());
    if (!Times || (Widest - 1) * *Times > MostBits) throw TooLarge();

    BigInt Top = Base.Numerator.Power(*Times);
    BigInt Low = Base.Denominator.Power(*Times);
    if (Top.BitLength() > MostBits || Low.BitLength() > MostBits) throw TooLarge();
    if (Flips) std::swap(Top, Low);
    if (Low.IsNegative()) {
        Top = Top.Negated();
        Low = Low.Negated();
    }
    Fraction Result;
    Result.Numerator   = std::move(Top);
    Result.Denominator = std::move(Low);
    return Result;
}
