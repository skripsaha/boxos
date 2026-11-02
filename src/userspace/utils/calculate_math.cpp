/* What can be done with numbers: the operators, the functions, the constants.
 * A number stays exact while every step of it can be; one step that cannot
 * (a root of a non-square, a sine) makes it a double from there on. */

#include "calculate.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace {

using Arguments = std::vector<Number>;

constexpr double Pi  = 3.141592653589793;
constexpr double Tau = 6.283185307179586;
constexpr double E   = 2.718281828459045;
/* Past this many roots a power is left to the double: a fraction like 3/1001
 * is no root anyone meant. */
constexpr std::uint64_t MostRootDegree = 1000;

Refusal NoRealAnswer(std::string_view Why, std::size_t Operand = 0)
{
    return Refusal{std::string(Why), Operand};
}

Refusal NeedsWhole(std::string_view Who, std::size_t Operand)
{
    return Refusal{std::string(Who) + " takes whole numbers", Operand};
}

BigInt WholeFromDouble(double Value)
{
    int    Power;
    double Mantissa = std::frexp(std::fabs(Value), &Power);
    BigInt Bits     = BigInt::FromUnsigned(static_cast<std::uint64_t>(std::ldexp(Mantissa, 53)));
    BigInt Whole    = Power >= 53 ? Bits.ShiftedLeft(static_cast<std::size_t>(Power - 53)) : Bits.ShiftedRight(static_cast<std::size_t>(53 - Power));
    return Value < 0 ? Whole.Negated() : Whole;
}

/* For a root of degree 2 or 3 of a number a double cannot hold: its mantissa
 * and power of two are taken apart, so the root is made of the mantissa and a
 * power that halves or thirds. */
double RootOfDouble(const Number &Value, unsigned Degree)
{
    if (!Value.Exact) return Degree == 2 ? std::sqrt(Value.Approximate) : std::cbrt(Value.Approximate);
    long   Power;
    double Mantissa = Value.Value.Mantissa(Power);
    long   Rest     = ((Power % Degree) + Degree) % Degree;
    double Moved    = std::ldexp(Mantissa, static_cast<int>(Rest));
    double Root     = Degree == 2 ? std::sqrt(Moved) : std::cbrt(Moved);
    return std::ldexp(Root, static_cast<int>((Power - Rest) / static_cast<long>(Degree)));
}

Number ExactOrRoot(const Number &Value, unsigned Degree)
{
    if (Value.Exact) {
        if (auto Root = Value.Value.ExactRoot(Degree)) return Number::Of(*Root);
    }
    return Number::Approx(RootOfDouble(Value, Degree));
}

bool IsNegative(const Number &Value)
{
    return Value.Exact ? Value.Value.Sign() < 0 : Value.Approximate < 0;
}

Number Sqrt(const Arguments &A)
{
    if (IsNegative(A[0])) throw NoRealAnswer("the square root of a negative number has no real answer");
    return ExactOrRoot(A[0], 2);
}

Number Cbrt(const Arguments &A)
{
    if (!IsNegative(A[0])) return ExactOrRoot(A[0], 3);
    Number Positive = Negate(A[0]);
    return Negate(ExactOrRoot(Positive, 3));
}

Number Abs(const Arguments &A)
{
    return IsNegative(A[0]) ? Negate(A[0]) : A[0];
}

Number Floor(const Arguments &A)
{
    if (A[0].Exact) return Number::Whole(A[0].Value.Floor());
    return Number::Whole(WholeFromDouble(std::floor(A[0].Approximate)));
}

Number Ceil(const Arguments &A)
{
    if (A[0].Exact) return Number::Whole(A[0].Value.Ceiling());
    return Number::Whole(WholeFromDouble(std::ceil(A[0].Approximate)));
}

Number Round(const Arguments &A)
{
    if (A[0].Exact) return Number::Whole(A[0].Value.Round());
    return Number::Whole(WholeFromDouble(std::round(A[0].Approximate)));
}

/* A function of one double that is a whole number at one argument: sin 0 is 0,
 * cos 0 is 1. Those are said exactly. */
Number Real(const Number &Value, double (*Run)(double), int At, int Gives)
{
    if (Value.Exact && Value.Value.IsInteger() && Value.Value.Numerator == BigInt(At)) return Number::Whole(BigInt(Gives));
    return Number::Approx(Run(Value.AsDouble()));
}

Number Sin(const Arguments &A) { return Real(A[0], std::sin, 0, 0); }
Number Cos(const Arguments &A) { return Real(A[0], std::cos, 0, 1); }
Number Tan(const Arguments &A) { return Real(A[0], std::tan, 0, 0); }
Number Atan(const Arguments &A) { return Real(A[0], std::atan, 0, 0); }
Number Exp(const Arguments &A) { return Exponential(A[0]); }

Number Asin(const Arguments &A)
{
    if (std::fabs(A[0].AsDouble()) > 1) throw NoRealAnswer("asin takes a number from -1 to 1");
    return Real(A[0], std::asin, 0, 0);
}

Number Acos(const Arguments &A)
{
    if (std::fabs(A[0].AsDouble()) > 1) throw NoRealAnswer("acos takes a number from -1 to 1");
    return Real(A[0], std::acos, 1, 0);
}

/* K when Value is Base to the K, K below zero for one over such a power. */
std::optional<std::int64_t> ExactLogarithm(const Fraction &Value, unsigned Base)
{
    const BigInt *Whole = Value.IsInteger() ? &Value.Numerator : Value.Numerator.IsOne() ? &Value.Denominator : nullptr;
    if (!Whole) return std::nullopt;
    if (Whole->IsOne()) return 0;
    std::int64_t Near = static_cast<std::int64_t>(static_cast<double>(Whole->BitLength() - 1) / std::log2(static_cast<double>(Base)));
    for (std::int64_t K = std::max<std::int64_t>(Near - 1, 1); K <= Near + 2; K++)
        if (BigInt(static_cast<std::int64_t>(Base)).Power(static_cast<std::uint64_t>(K)) == *Whole)
            return Value.IsInteger() ? K : -K;
    return std::nullopt;
}

/* The logarithm of an exact positive number in Base (0 is the natural one),
 * made as a double. Near one the distance from one is taken exactly, so the
 * digits that would be lost in rounding the number are not; a number a double
 * cannot hold has its power of two taken apart: log(m * 2^p) is log(m) plus p
 * logs of 2. */
double LogarithmOfExact(const Fraction &Value, unsigned Base)
{
    double   Natural;
    Fraction Half = Fraction::Of(BigInt(1), BigInt(2));
    if (Value.Compare(Half) > 0 && Value.Compare(Fraction::Whole(BigInt(2))) < 0) {
        Fraction Distance;
        Distance.Numerator   = Value.Numerator - Value.Denominator;
        Distance.Denominator = Value.Denominator;
        Natural              = std::log1p(Distance.ToDouble());
    } else {
        long   Power;
        double Mantissa = Value.Mantissa(Power);
        if (Power > -1000 && Power < 1000) Natural = std::log(std::ldexp(Mantissa, static_cast<int>(Power)));
        else                               Natural = std::log(Mantissa) + static_cast<double>(Power) * std::log(2.0);
    }
    if (Base != 0) Natural /= std::log(static_cast<double>(Base));
    return Natural;
}

Number Logarithm(const Number &Value, unsigned Base)
{
    bool Positive = Value.Exact ? Value.Value.Sign() > 0 : Value.Approximate > 0;
    if (!Positive) throw NoRealAnswer("a logarithm takes a number above zero");

    if (Value.Exact) {
        if (Base == 0 && Value.Value.IsInteger() && Value.Value.Numerator.IsOne()) return Number::Whole(BigInt(0));
        if (Base != 0)
            if (auto Power = ExactLogarithm(Value.Value, Base)) return Number::Whole(BigInt(*Power));
        return Number::Approx(LogarithmOfExact(Value.Value, Base));
    }
    if (Base == 0) return Number::Approx(std::log(Value.Approximate));
    if (Base == 2) return Number::Approx(std::log2(Value.Approximate));
    return Number::Approx(std::log10(Value.Approximate));
}

Number Ln(const Arguments &A) { return Logarithm(A[0], 0); }
Number Log(const Arguments &A) { return Logarithm(A[0], 10); }
Number Log2(const Arguments &A) { return Logarithm(A[0], 2); }

int Order(const Number &A, const Number &B)
{
    if (A.Exact && B.Exact) return A.Value.Compare(B.Value);
    double X = A.AsDouble();
    double Y = B.AsDouble();
    return X < Y ? -1 : X > Y ? 1 : 0;
}

Number Min(const Arguments &A)
{
    const Number *Best = &A[0];
    for (const Number &Next : A)
        if (Order(Next, *Best) < 0) Best = &Next;
    return *Best;
}

Number Max(const Arguments &A)
{
    const Number *Best = &A[0];
    for (const Number &Next : A)
        if (Order(Next, *Best) > 0) Best = &Next;
    return *Best;
}

BigInt WholeArgument(const Number &Value, std::string_view Who, std::size_t Operand)
{
    if (!Value.IsWhole()) throw NeedsWhole(Who, Operand);
    return Value.Value.Numerator.Absolute();
}

Number Gcd(const Arguments &A)
{
    BigInt Common;
    for (std::size_t I = 0; I < A.size(); I++) Common = BigInt::Gcd(Common, WholeArgument(A[I], "gcd", I));
    return Number::Whole(Common);
}

Number Lcm(const Arguments &A)
{
    Number Common = Number::Whole(WholeArgument(A[0], "lcm", 0));
    for (std::size_t I = 1; I < A.size(); I++) {
        BigInt Next = WholeArgument(A[I], "lcm", I);
        BigInt Gone = BigInt::Gcd(Common.Value.Numerator, Next);
        if (Gone.IsZero()) return Number::Whole(BigInt(0));
        Common = Divide(Multiply(Common, Number::Whole(Next)), Number::Whole(Gone));
    }
    return Common;
}

const Function Functions[] = {
    {"sqrt", 1, 1, Sqrt},   {"cbrt", 1, 1, Cbrt},   {"abs", 1, 1, Abs},   {"floor", 1, 1, Floor},
    {"ceil", 1, 1, Ceil},   {"round", 1, 1, Round}, {"sin", 1, 1, Sin},   {"cos", 1, 1, Cos},
    {"tan", 1, 1, Tan},     {"asin", 1, 1, Asin},   {"acos", 1, 1, Acos}, {"atan", 1, 1, Atan},
    {"ln", 1, 1, Ln},       {"log", 1, 1, Log},     {"log2", 1, 1, Log2}, {"exp", 1, 1, Exp},
    {"min", 1, 0, Min},     {"max", 1, 0, Max},     {"gcd", 2, 0, Gcd},   {"lcm", 2, 0, Lcm},
};

constexpr std::string_view Constants[] = {"pi", "e", "tau"};

Number PowerOfDoubles(const Number &Base, const Number &Exponent)
{
    double X = Base.AsDouble();
    double Y = Exponent.AsDouble();
    if (X == 0 && Y < 0) throw Refusal{"division by zero", 0};
    if (X < 0 && Y != std::floor(Y)) throw NoRealAnswer("a negative number has no real power that is not a whole number");
    return Number::Approx(std::pow(X, Y));
}

}

Number Number::Of(Fraction Value)
{
    Number Result;
    Result.Value = std::move(Value);
    return Result;
}

Number Number::Whole(BigInt Value)
{
    return Of(Fraction::Whole(std::move(Value)));
}

Number Number::Approx(double Value)
{
    if (std::isnan(Value)) throw NoRealAnswer("this has no real answer", NoOperand);
    if (std::isinf(Value)) throw Refusal{"the answer is too large even for a decimal number"};
    Number Result;
    Result.Exact       = false;
    Result.Approximate = Value == 0 ? 0 : Value;
    return Result;
}

Number Add(const Number &A, const Number &B)
{
    if (A.Exact && B.Exact) return Number::Of(A.Value + B.Value);
    return Number::Approx(A.AsDouble() + B.AsDouble());
}

Number Subtract(const Number &A, const Number &B)
{
    if (A.Exact && B.Exact) return Number::Of(A.Value - B.Value);
    return Number::Approx(A.AsDouble() - B.AsDouble());
}

Number Multiply(const Number &A, const Number &B)
{
    if (A.Exact && B.Exact) return Number::Of(A.Value * B.Value);
    return Number::Approx(A.AsDouble() * B.AsDouble());
}

Number Divide(const Number &A, const Number &B)
{
    if (B.IsZero()) throw Refusal{"division by zero", 1};
    if (A.Exact && B.Exact) return Number::Of(A.Value / B.Value);
    return Number::Approx(A.AsDouble() / B.AsDouble());
}

Number Modulo(const Number &A, const Number &B)
{
    if (!A.IsWhole()) throw Refusal{"% takes whole numbers; this is not one", 0};
    if (!B.IsWhole()) throw Refusal{"% takes whole numbers; this is not one", 1};
    if (B.IsZero()) throw Refusal{"division by zero", 1};
    BigInt Quotient, Rest;
    BigInt::DivideTruncated(A.Value.Numerator, B.Value.Numerator, Quotient, Rest);
    if (!Rest.IsZero() && Rest.IsNegative() != B.Value.Numerator.IsNegative()) Rest = Rest + B.Value.Numerator;
    return Number::Whole(Rest);
}

Number Exponential(const Number &Exponent)
{
    return Real(Exponent, std::exp, 0, 1);
}

Number Negate(const Number &A)
{
    if (A.Exact) return Number::Of(A.Value.Negated());
    return Number::Approx(-A.Approximate);
}

Number Power(const Number &Base, const Number &Exponent)
{
    if (!Base.Exact || !Exponent.Exact) return PowerOfDoubles(Base, Exponent);
    const Fraction &Power = Exponent.Value;
    if (Power.IsInteger()) return Number::Of(PowerOf(Base.Value, Power.Numerator));

    std::optional<std::uint64_t> Degree = Power.Denominator.ToUnsigned();
    if (Degree && *Degree <= MostRootDegree) {
        if (auto Root = Base.Value.ExactRoot(static_cast<unsigned>(*Degree)))
            return Number::Of(PowerOf(*Root, Power.Numerator));
    }
    return PowerOfDoubles(Base, Exponent);
}

Number Factorial(const Number &A)
{
    if (!A.IsWhole()) throw Refusal{"! takes a whole number; this is not one", 0};
    if (A.Value.Numerator.IsNegative()) throw Refusal{"the factorial of a negative number is not defined", 0};

    std::optional<std::uint64_t> Count = A.Value.Numerator.ToUnsigned();
    double                       Digits = 0;
    for (std::uint64_t Next = 2; Count && Next <= *Count; Next++) {
        Digits += std::log10(static_cast<double>(Next));
        if (Digits > static_cast<double>(MostDigits)) Count.reset();
    }
    if (!Count) throw Refusal{"the answer would have more than 100000 digits, more than calculate will write out", 0};

    BigInt Product(1);
    for (std::uint64_t Next = 2; Next <= *Count; Next++) Product = Product * BigInt::FromUnsigned(Next);
    return Number::Whole(Product);
}

const Function *FindFunction(std::string_view Name)
{
    for (const Function &Known : Functions)
        if (Known.Name == Name) return &Known;
    return nullptr;
}

std::optional<Number> FindConstant(std::string_view Name)
{
    if (Name == "pi") return Number::Approx(Pi);
    if (Name == "tau") return Number::Approx(Tau);
    if (Name == "e") return Number::Approx(E);
    return std::nullopt;
}

std::vector<std::string_view> FunctionNames()
{
    std::vector<std::string_view> Names;
    for (const Function &Known : Functions) Names.push_back(Known.Name);
    return Names;
}

std::vector<std::string_view> ConstantNames()
{
    return {std::begin(Constants), std::end(Constants)};
}
