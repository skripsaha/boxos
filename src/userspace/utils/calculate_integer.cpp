/* The whole number of any size: a sign and 32-bit limbs, least first, with no
 * zero limb at the top. Zero has no limbs and is never negative. */

#include "calculate.h"

#include <utility>

namespace {

using Magnitude = std::vector<std::uint32_t>;
using Wide  = std::uint64_t;

constexpr Wide LimbBase = Wide{1} << 32;

void Trim(Magnitude &Value)
{
    while (!Value.empty() && Value.back() == 0) Value.pop_back();
}

int CompareMagnitude(const Magnitude &A, const Magnitude &B)
{
    if (A.size() != B.size()) return A.size() < B.size() ? -1 : 1;
    for (std::size_t I = A.size(); I-- > 0;)
        if (A[I] != B[I]) return A[I] < B[I] ? -1 : 1;
    return 0;
}

Magnitude AddMagnitude(const Magnitude &A, const Magnitude &B)
{
    const Magnitude &Long  = A.size() >= B.size() ? A : B;
    const Magnitude &Short = A.size() >= B.size() ? B : A;
    Magnitude        Sum;
    Sum.reserve(Long.size() + 1);
    Wide Carry = 0;
    for (std::size_t I = 0; I < Long.size(); I++) {
        Wide Part = Carry + Long[I] + (I < Short.size() ? Short[I] : 0);
        Sum.push_back(static_cast<std::uint32_t>(Part));
        Carry = Part >> 32;
    }
    if (Carry) Sum.push_back(static_cast<std::uint32_t>(Carry));
    return Sum;
}

/* A is not below B. */
Magnitude SubtractMagnitude(const Magnitude &A, const Magnitude &B)
{
    Magnitude Difference(A.size());
    Wide  Borrow = 0;
    for (std::size_t I = 0; I < A.size(); I++) {
        Wide Take = (I < B.size() ? B[I] : 0) + Borrow;
        if (A[I] >= Take) {
            Difference[I] = static_cast<std::uint32_t>(A[I] - Take);
            Borrow        = 0;
        } else {
            Difference[I] = static_cast<std::uint32_t>(A[I] + LimbBase - Take);
            Borrow        = 1;
        }
    }
    Trim(Difference);
    return Difference;
}

Magnitude MultiplyMagnitude(const Magnitude &A, const Magnitude &B)
{
    if (A.empty() || B.empty()) return {};
    Magnitude Product(A.size() + B.size(), 0);
    for (std::size_t I = 0; I < A.size(); I++) {
        Wide Carry = 0;
        Wide Left  = A[I];
        for (std::size_t J = 0; J < B.size(); J++) {
            Wide Part          = Product[I + J] + Left * B[J] + Carry;
            Product[I + J]     = static_cast<std::uint32_t>(Part);
            Carry              = Part >> 32;
        }
        Product[I + B.size()] = static_cast<std::uint32_t>(Carry);
    }
    Trim(Product);
    return Product;
}

void MultiplyAddSmall(Magnitude &Value, std::uint32_t Factor, std::uint32_t Addend)
{
    Wide Carry = Addend;
    for (std::uint32_t &Limb : Value) {
        Wide Part = Wide{Limb} * Factor + Carry;
        Limb      = static_cast<std::uint32_t>(Part);
        Carry     = Part >> 32;
    }
    if (Carry) Value.push_back(static_cast<std::uint32_t>(Carry));
}

/* Value becomes its quotient by Divisor; the remainder is returned. */
std::uint32_t DivideSmall(Magnitude &Value, std::uint32_t Divisor)
{
    Wide Rest = 0;
    for (std::size_t I = Value.size(); I-- > 0;) {
        Wide Part = (Rest << 32) | Value[I];
        Value[I]  = static_cast<std::uint32_t>(Part / Divisor);
        Rest      = Part % Divisor;
    }
    Trim(Value);
    return static_cast<std::uint32_t>(Rest);
}

Magnitude ShiftLeftMagnitude(const Magnitude &Value, std::size_t Bits)
{
    if (Value.empty()) return {};
    std::size_t Whole = Bits / 32;
    unsigned    Part  = static_cast<unsigned>(Bits % 32);
    Magnitude       Moved(Value.size() + Whole + 1, 0);
    for (std::size_t I = 0; I < Value.size(); I++) {
        Wide Shifted          = Wide{Value[I]} << Part;
        Moved[I + Whole]     |= static_cast<std::uint32_t>(Shifted);
        Moved[I + Whole + 1] |= static_cast<std::uint32_t>(Shifted >> 32);
    }
    Trim(Moved);
    return Moved;
}

Magnitude ShiftRightMagnitude(const Magnitude &Value, std::size_t Bits)
{
    std::size_t Whole = Bits / 32;
    if (Whole >= Value.size()) return {};
    unsigned Part = static_cast<unsigned>(Bits % 32);
    Magnitude    Moved(Value.size() - Whole);
    for (std::size_t I = 0; I < Moved.size(); I++) {
        Wide Both = Value[I + Whole];
        if (I + Whole + 1 < Value.size()) Both |= Wide{Value[I + Whole + 1]} << 32;
        Moved[I] = static_cast<std::uint32_t>(Both >> Part);
    }
    Trim(Moved);
    return Moved;
}

/* Knuth's algorithm D: the divisor is not zero. */
void DivideMagnitude(const Magnitude &Dividend, const Magnitude &Divisor, Magnitude &Quotient, Magnitude &Remainder)
{
    if (CompareMagnitude(Dividend, Divisor) < 0) {
        Quotient.clear();
        Remainder = Dividend;
        return;
    }
    if (Divisor.size() == 1) {
        Quotient          = Dividend;
        std::uint32_t Rest = DivideSmall(Quotient, Divisor[0]);
        Remainder.clear();
        if (Rest) Remainder.push_back(Rest);
        return;
    }

    unsigned Shift = static_cast<unsigned>(__builtin_clz(Divisor.back()));
    Magnitude    V     = ShiftLeftMagnitude(Divisor, Shift);
    Magnitude    U     = ShiftLeftMagnitude(Dividend, Shift);
    U.resize(Dividend.size() + 1, 0);

    std::size_t N = V.size();
    std::size_t M = Dividend.size() - N;
    Quotient.assign(M + 1, 0);

    for (std::size_t J = M + 1; J-- > 0;) {
        Wide Top      = (Wide{U[J + N]} << 32) | U[J + N - 1];
        Wide Estimate = Top / V[N - 1];
        Wide Rest     = Top % V[N - 1];
        while (Estimate >= LimbBase || Estimate * V[N - 2] > ((Rest << 32) | U[J + N - 2])) {
            Estimate--;
            Rest += V[N - 1];
            if (Rest >= LimbBase) break;
        }

        std::int64_t Borrow = 0;
        for (std::size_t I = 0; I < N; I++) {
            Wide         Part = Estimate * V[I];
            std::int64_t Take = static_cast<std::int64_t>(U[I + J]) - Borrow - static_cast<std::int64_t>(Part & 0xFFFFFFFF);
            U[I + J]          = static_cast<std::uint32_t>(Take);
            Borrow            = static_cast<std::int64_t>(Part >> 32) - (Take >> 32);
        }
        std::int64_t Last = static_cast<std::int64_t>(U[J + N]) - Borrow;
        U[J + N]          = static_cast<std::uint32_t>(Last);

        Quotient[J] = static_cast<std::uint32_t>(Estimate);
        if (Last < 0) {
            Quotient[J]--;
            Wide Carry = 0;
            for (std::size_t I = 0; I < N; I++) {
                Wide Part = Wide{U[I + J]} + V[I] + Carry;
                U[I + J]  = static_cast<std::uint32_t>(Part);
                Carry     = Part >> 32;
            }
            U[J + N] += static_cast<std::uint32_t>(Carry);
        }
    }
    Trim(Quotient);
    U.resize(N);
    Trim(U);
    Remainder = ShiftRightMagnitude(U, Shift);
}

unsigned DigitValue(char Ch)
{
    if (Ch >= '0' && Ch <= '9') return static_cast<unsigned>(Ch - '0');
    if (Ch >= 'a' && Ch <= 'f') return static_cast<unsigned>(Ch - 'a' + 10);
    return static_cast<unsigned>(Ch - 'A' + 10);
}

std::string PowerOfTwoDigits(const Magnitude &Value, std::size_t BitCount, unsigned BitsADigit)
{
    std::size_t Count = (BitCount + BitsADigit - 1) / BitsADigit;
    std::string Text(Count, '0');
    for (std::size_t Place = 0; Place < Count; Place++) {
        unsigned Digit = 0;
        for (unsigned Bit = 0; Bit < BitsADigit; Bit++) {
            std::size_t At = Place * BitsADigit + Bit;
            if (At / 32 < Value.size() && ((Value[At / 32] >> (At % 32)) & 1)) Digit |= 1u << Bit;
        }
        Text[Count - 1 - Place] = "0123456789abcdef"[Digit];
    }
    return Text;
}

std::string DecimalDigits(Magnitude Value)
{
    constexpr std::uint32_t Chunk = 1000000000;
    std::vector<std::uint32_t> Pieces;
    while (!Value.empty()) Pieces.push_back(DivideSmall(Value, Chunk));

    std::string Text = std::to_string(Pieces.back());
    for (std::size_t I = Pieces.size() - 1; I-- > 0;) {
        std::string Piece = std::to_string(Pieces[I]);
        Text.append(9 - Piece.size(), '0');
        Text += Piece;
    }
    return Text;
}

}

BigInt::BigInt(std::int64_t Value)
{
    Negative           = Value < 0;
    std::uint64_t Rest = Negative ? 0 - static_cast<std::uint64_t>(Value) : static_cast<std::uint64_t>(Value);
    while (Rest) {
        Limbs.push_back(static_cast<std::uint32_t>(Rest));
        Rest >>= 32;
    }
}

BigInt BigInt::FromUnsigned(std::uint64_t Value)
{
    BigInt Result;
    while (Value) {
        Result.Limbs.push_back(static_cast<std::uint32_t>(Value));
        Value >>= 32;
    }
    return Result;
}

BigInt BigInt::FromDigits(std::string_view Digits, unsigned Base)
{
    BigInt        Result;
    std::uint32_t Factor = 1;
    std::uint32_t Chunk  = 0;
    for (char Ch : Digits) {
        if (static_cast<Wide>(Factor) * Base >= LimbBase) {
            MultiplyAddSmall(Result.Limbs, Factor, Chunk);
            Factor = 1;
            Chunk  = 0;
        }
        Factor *= Base;
        Chunk = Chunk * Base + DigitValue(Ch);
    }
    MultiplyAddSmall(Result.Limbs, Factor, Chunk);
    Trim(Result.Limbs);
    return Result;
}

std::size_t BigInt::BitLength() const
{
    if (Limbs.empty()) return 0;
    return (Limbs.size() - 1) * 32 + (32 - static_cast<unsigned>(__builtin_clz(Limbs.back())));
}

std::size_t BigInt::TrailingZeroBits() const
{
    std::size_t Count = 0;
    for (std::uint32_t Limb : Limbs) {
        if (Limb) return Count + static_cast<unsigned>(__builtin_ctz(Limb));
        Count += 32;
    }
    return Count;
}

std::optional<std::uint64_t> BigInt::ToUnsigned() const
{
    if (Negative || Limbs.size() > 2) return std::nullopt;
    return LowBits();
}

std::uint64_t BigInt::LowBits() const
{
    std::uint64_t Value = 0;
    if (!Limbs.empty()) Value = Limbs[0];
    if (Limbs.size() > 1) Value |= std::uint64_t{Limbs[1]} << 32;
    return Value;
}

BigInt BigInt::Negated() const
{
    BigInt Result = *this;
    Result.Negative = !Negative && !Limbs.empty();
    return Result;
}

BigInt BigInt::Absolute() const
{
    BigInt Result = *this;
    Result.Negative = false;
    return Result;
}

int BigInt::Compare(const BigInt &Other) const
{
    if (Negative != Other.Negative) return Negative ? -1 : 1;
    int Order = CompareMagnitude(Limbs, Other.Limbs);
    return Negative ? -Order : Order;
}

BigInt BigInt::ShiftedLeft(std::size_t Bits) const
{
    BigInt Result;
    Result.Limbs    = ShiftLeftMagnitude(Limbs, Bits);
    Result.Negative = Negative;
    return Result;
}

BigInt BigInt::ShiftedRight(std::size_t Bits) const
{
    BigInt Result;
    Result.Limbs    = ShiftRightMagnitude(Limbs, Bits);
    Result.Negative = Negative && !Result.Limbs.empty();
    return Result;
}

BigInt BigInt::Power(std::uint64_t Exponent) const
{
    BigInt Result(1);
    BigInt Square = *this;
    while (Exponent) {
        if (Exponent & 1) Result = Result * Square;
        Exponent >>= 1;
        if (Exponent) Square = Square * Square;
    }
    return Result;
}

BigInt BigInt::Root(unsigned Degree, bool &Exact) const
{
    if (IsZero() || IsOne()) {
        Exact = true;
        return *this;
    }
    BigInt Guess = BigInt(1).ShiftedLeft(BitLength() / Degree + 1);
    BigInt DegreeLess = FromUnsigned(Degree - 1);
    BigInt DegreeBig = FromUnsigned(Degree);
    for (;;) {
        BigInt Quotient, Rest;
        DivideTruncated(*this, Guess.Power(Degree - 1), Quotient, Rest);
        BigInt Better;
        DivideTruncated(DegreeLess * Guess + Quotient, DegreeBig, Better, Rest);
        if (Better >= Guess) break;
        Guess = Better;
    }
    Exact = Guess.Power(Degree) == *this;
    return Guess;
}

std::string BigInt::ToString(unsigned Base) const
{
    if (Limbs.empty()) return "0";
    std::string Text;
    switch (Base) {
    case 2:  Text = PowerOfTwoDigits(Limbs, BitLength(), 1); break;
    case 8:  Text = PowerOfTwoDigits(Limbs, BitLength(), 3); break;
    case 16: Text = PowerOfTwoDigits(Limbs, BitLength(), 4); break;
    default: Text = DecimalDigits(Limbs); break;
    }
    return Negative ? "-" + Text : Text;
}

void BigInt::DivideTruncated(const BigInt &Dividend, const BigInt &Divisor, BigInt &Quotient, BigInt &Remainder)
{
    Magnitude Whole, Rest;
    DivideMagnitude(Dividend.Limbs, Divisor.Limbs, Whole, Rest);
    Quotient.Limbs    = std::move(Whole);
    Quotient.Negative = Dividend.Negative != Divisor.Negative && !Quotient.Limbs.empty();
    Remainder.Limbs    = std::move(Rest);
    Remainder.Negative = Dividend.Negative && !Remainder.Limbs.empty();
}

BigInt operator+(const BigInt &A, const BigInt &B)
{
    BigInt Sum;
    if (A.Negative == B.Negative) {
        Sum.Limbs    = AddMagnitude(A.Limbs, B.Limbs);
        Sum.Negative = A.Negative && !Sum.Limbs.empty();
        return Sum;
    }
    int Order = CompareMagnitude(A.Limbs, B.Limbs);
    if (Order == 0) return Sum;
    const BigInt &Bigger  = Order > 0 ? A : B;
    const BigInt &Smaller = Order > 0 ? B : A;
    Sum.Limbs             = SubtractMagnitude(Bigger.Limbs, Smaller.Limbs);
    Sum.Negative          = Bigger.Negative;
    return Sum;
}

BigInt operator-(const BigInt &A, const BigInt &B)
{
    return A + B.Negated();
}

BigInt operator*(const BigInt &A, const BigInt &B)
{
    BigInt Product;
    Product.Limbs    = MultiplyMagnitude(A.Limbs, B.Limbs);
    Product.Negative = A.Negative != B.Negative && !Product.Limbs.empty();
    return Product;
}

/* Lehmer's way: the top sixty bits of both numbers decide a run of Euclid's
 * steps at once, and the run is applied to the whole numbers in one pass,
 * where Euclid's own steps would each cost a pass over them. */
BigInt BigInt::Gcd(BigInt A, BigInt B)
{
    A.Negative = false;
    B.Negative = false;
    if (A < B) std::swap(A, B);

    while (!B.IsZero()) {
        if (A.Limbs.size() <= 2) {
            std::uint64_t X = A.LowBits();
            std::uint64_t Y = B.LowBits();
            while (Y) {
                std::uint64_t Rest = X % Y;
                X                  = Y;
                Y                  = Rest;
            }
            return FromUnsigned(X);
        }

        std::size_t  Shift = A.BitLength() - 60;
        std::int64_t X     = static_cast<std::int64_t>(A.ShiftedRight(Shift).LowBits());
        std::int64_t Y     = static_cast<std::int64_t>(B.ShiftedRight(Shift).LowBits());
        std::int64_t Pa = 1, Pb = 0, Qa = 0, Qb = 1;
        unsigned     Steps = 0;
        while (Y - Qa != 0) {
            std::int64_t Quotient = (X + (Pa - 1)) / (Y - Qa);
            std::int64_t Next     = Pb + Quotient * Qb;
            std::int64_t Rest     = X - Quotient * Y;
            if (Next > Rest) break;
            X = Y;
            Y = Rest;
            std::int64_t NewQb = Pa + Quotient * Qa;
            Pa = Qb;
            Pb = Qa;
            Qa = Next;
            Qb = NewQb;
            Steps++;
        }

        if (Steps == 0) {
            BigInt Quotient, Rest;
            DivideTruncated(A, B, Quotient, Rest);
            A = std::move(B);
            B = std::move(Rest);
            continue;
        }
        BigInt NewA, NewB;
        if (Steps & 1) {
            NewA = B * BigInt(Pa) - A * BigInt(Pb);
            NewB = A * BigInt(Qb) - B * BigInt(Qa);
        } else {
            NewA = A * BigInt(Pa) - B * BigInt(Pb);
            NewB = B * BigInt(Qb) - A * BigInt(Qa);
        }
        A = std::move(NewA);
        B = std::move(NewB);
    }
    return A;
}
