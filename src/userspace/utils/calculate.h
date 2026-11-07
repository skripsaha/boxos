#ifndef CALCULATE_H
#define CALCULATE_H

/* calculate — the sum the person typed, worked out exactly where it can be.
 *
 * Seven parts, the first six free of any BoxOS header so they can be tried on
 * any machine: calculate_integer.cpp is the whole number of any size,
 * calculate_fraction.cpp the fraction made of two of them, calculate_math.cpp
 * the operators and functions over exact and approximate numbers,
 * calculate_show.cpp the way an answer is written, calculate_read.cpp cuts a
 * typed sum into tokens, calculate_parse.cpp reads them and works the sum out;
 * calculate.cpp talks to the person.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/* No exact answer is written with more digits than this, and nothing is begun
 * that would come to more: a sum has to end before the person gives up on it. */
constexpr std::size_t MostDigits = 100000;
constexpr std::size_t MostBits   = 332193;
constexpr std::size_t NoOperand  = static_cast<std::size_t>(-1);

/* A sum that cannot be done, said by the number layer. Operand is the one it is
 * about (0 the left or first, 1 the right or second ...), NoOperand when it is
 * about the operation itself; the parser turns it into a place in the text. */
struct Refusal {
    std::string Reason;
    std::size_t Operand = NoOperand;
};

class BigInt {
public:
    BigInt() = default;
    BigInt(std::int64_t Value);
    static BigInt FromUnsigned(std::uint64_t Value);
    /* Digits of Base (2 to 16) with nothing else among them. */
    static BigInt FromDigits(std::string_view Digits, unsigned Base);

    bool IsZero() const { return Limbs.empty(); }
    bool IsNegative() const { return Negative; }
    bool IsOne() const { return !Negative && Limbs.size() == 1 && Limbs[0] == 1; }
    bool IsEven() const { return Limbs.empty() || (Limbs[0] & 1) == 0; }
    int  Sign() const { return Limbs.empty() ? 0 : Negative ? -1 : 1; }
    std::size_t BitLength() const;
    std::size_t TrailingZeroBits() const;
    /* The value, when it is not negative and fits 64 bits. */
    std::optional<std::uint64_t> ToUnsigned() const;
    std::uint64_t LowBits() const;

    BigInt Negated() const;
    BigInt Absolute() const;
    int    Compare(const BigInt &Other) const;
    /* Shifts move the size of the number and keep its sign. */
    BigInt ShiftedLeft(std::size_t Bits) const;
    BigInt ShiftedRight(std::size_t Bits) const;
    BigInt Power(std::uint64_t Exponent) const;
    /* The largest whole number whose Degree-th power is not above this one
     * (this is not negative); Exact says whether its power is this one. */
    BigInt Root(unsigned Degree, bool &Exact) const;
    std::string ToString(unsigned Base = 10) const;

    /* Quotient toward zero; the remainder has the sign of the dividend. The
     * divisor is not zero. */
    static void DivideTruncated(const BigInt &Dividend, const BigInt &Divisor, BigInt &Quotient, BigInt &Remainder);
    static BigInt Gcd(BigInt A, BigInt B);

    friend BigInt operator+(const BigInt &A, const BigInt &B);
    friend BigInt operator-(const BigInt &A, const BigInt &B);
    friend BigInt operator*(const BigInt &A, const BigInt &B);

private:
    bool                       Negative = false;
    std::vector<std::uint32_t> Limbs;
};

inline bool operator==(const BigInt &A, const BigInt &B) { return A.Compare(B) == 0; }
inline bool operator!=(const BigInt &A, const BigInt &B) { return A.Compare(B) != 0; }
inline bool operator<(const BigInt &A, const BigInt &B) { return A.Compare(B) < 0; }
inline bool operator<=(const BigInt &A, const BigInt &B) { return A.Compare(B) <= 0; }
inline bool operator>(const BigInt &A, const BigInt &B) { return A.Compare(B) > 0; }
inline bool operator>=(const BigInt &A, const BigInt &B) { return A.Compare(B) >= 0; }

/* A whole number over a whole number, always in lowest terms, the denominator
 * above zero. Every operation that would pass MostBits refuses. */
struct Fraction {
    BigInt Numerator;
    BigInt Denominator{1};

    static Fraction Of(BigInt Numerator, BigInt Denominator);
    static Fraction Whole(BigInt Value);

    bool IsInteger() const { return Denominator.IsOne(); }
    bool IsZero() const { return Numerator.IsZero(); }
    int  Sign() const { return Numerator.Sign(); }
    Fraction Negated() const;
    int      Compare(const Fraction &Other) const;
    BigInt   Floor() const;
    BigInt   Ceiling() const;
    /* Half a unit away from zero goes away from zero. */
    BigInt   Round() const;
    /* The value as m * 2^Exponent with m in [0.5, 1), correctly rounded. */
    double   Mantissa(long &Exponent) const;
    double   ToDouble() const;
    /* The Degree-th root when it is a fraction; a negative number has one for
     * an odd Degree only. */
    std::optional<Fraction> ExactRoot(unsigned Degree) const;
};

Fraction operator+(const Fraction &A, const Fraction &B);
Fraction operator-(const Fraction &A, const Fraction &B);
Fraction operator*(const Fraction &A, const Fraction &B);
/* The divisor is not zero. */
Fraction operator/(const Fraction &A, const Fraction &B);
Fraction PowerOf(const Fraction &Base, const BigInt &Exponent);

/* An answer: exact, a fraction; or approximate, a double. */
struct Number {
    bool     Exact       = true;
    Fraction Value;
    double   Approximate = 0;

    static Number Of(Fraction Value);
    static Number Whole(BigInt Value);
    /* Refuses a number that is not a number or is past what a double holds. */
    static Number Approx(double Value);

    bool   IsWhole() const { return Exact && Value.IsInteger(); }
    bool   IsZero() const { return Exact ? Value.IsZero() : Approximate == 0; }
    double AsDouble() const { return Exact ? Value.ToDouble() : Approximate; }
};

Number Add(const Number &A, const Number &B);
Number Subtract(const Number &A, const Number &B);
Number Multiply(const Number &A, const Number &B);
Number Divide(const Number &A, const Number &B);
/* The sign of the remainder is the sign of the divisor: -7 % 3 is 2. */
Number Modulo(const Number &A, const Number &B);
Number Power(const Number &Base, const Number &Exponent);
/* e to the Exponent, correctly rounded; exactly 1 for 0. */
Number Exponential(const Number &Exponent);
Number Negate(const Number &A);
Number Factorial(const Number &A);

struct Function {
    std::string_view Name;
    unsigned         Least;
    unsigned         Most;
    Number         (*Run)(const std::vector<Number> &);
};

/* Most is 0 for a function that takes as many as are given. */
const Function         *FindFunction(std::string_view Name);
std::optional<Number>   FindConstant(std::string_view Name);
std::vector<std::string_view> FunctionNames();
std::vector<std::string_view> ConstantNames();

/* An answer as it is written: the value, and for a fraction its decimal. */
struct Shown {
    std::string Value;
    std::string Decimal;
};

/* Base 2, 8, 16 write a whole number as 0b.., 0o.., 0x..; any other base, or a
 * number that is not whole, is written in decimal. */
Shown Show(const Number &Value, unsigned Base);

/* Where in the typed text a sum went wrong, and why. */
struct CalculateError {
    std::size_t Begin;
    std::size_t End;
    std::string Reason;
};

struct Answer {
    Number      Value;
    unsigned    Base = 10;
    std::string Assigned;
};

class Calculator {
public:
    /* Works one line out: a sum, optionally `in hex|bin|oct|dec`, or
     * `name = sum`. Throws CalculateError; nothing changes when it does. */
    Answer Work(std::string_view Text);

    const Number *Variable(std::string_view Name) const;
    const Number *Last() const { return HasLast ? &LastAnswer : nullptr; }
    std::vector<std::string> VariableNames() const;

private:
    std::map<std::string, Number, std::less<>> Variables;
    Number                                     LastAnswer;
    bool                                       HasLast = false;
};

#endif
