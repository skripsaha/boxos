/* A typed sum, read and worked out in one pass: each part is a number the
 * moment it is read, so there is no tree to keep. The text is cut into tokens
 * first, each with the place it stands, and every refusal points at one. */

#include "calculate.h"
#include "calculate_read.h"

#include <algorithm>
#include <format>

namespace {

struct Operand {
    Number Value;
    Span   At;
    /* The constant e itself, which is raised by exp and not as a rounded double. */
    bool   IsEuler = false;
};

CalculateError ErrorFor(const Refusal &Why, Span Whole, const std::vector<Span> &Operands)
{
    Span At = Why.Operand < Operands.size() ? Operands[Why.Operand] : Whole;
    return {At.Begin, At.End, Why.Reason};
}

std::size_t Distance(std::string_view A, std::string_view B)
{
    std::vector<std::size_t> Row(B.size() + 1);
    for (std::size_t J = 0; J <= B.size(); J++) Row[J] = J;
    for (std::size_t I = 1; I <= A.size(); I++) {
        std::size_t Diagonal = Row[0];
        Row[0]               = I;
        for (std::size_t J = 1; J <= B.size(); J++) {
            std::size_t Above = Row[J];
            std::size_t Same  = Lowered(A[I - 1]) == Lowered(B[J - 1]) ? 0 : 1;
            Row[J]            = std::min({Row[J] + 1, Row[J - 1] + 1, Diagonal + Same});
            Diagonal          = Above;
        }
    }
    return Row[B.size()];
}

struct BaseWord {
    std::string_view Word;
    unsigned         Base;
};

constexpr BaseWord BaseWords[] = {{"hex", 16}, {"bin", 2}, {"oct", 8}, {"dec", 10}};

bool IsKeyword(std::string_view Name)
{
    return Name == "ans" || Name == "in" || FindFunction(Name) || FindConstant(Name);
}

class Parser {
public:
    Parser(const Calculator &Owner, std::vector<Token> Tokens) : Owner(Owner), Tokens(std::move(Tokens)) {}

    Answer Statement();

private:
    const Calculator  &Owner;
    std::vector<Token> Tokens;
    std::size_t        Next = 0;

    const Token &Peek(std::size_t Ahead = 0) const { return Tokens[std::min(Next + Ahead, Tokens.size() - 1)]; }
    const Token &Take() { return Tokens[Next < Tokens.size() - 1 ? Next++ : Next]; }
    bool         Is(const Token &T, char Mark) const { return T.Type == Kind::Symbol && T.Symbol == Mark; }

    [[noreturn]] void Expected(const Token &T) const;
    [[noreturn]] void Unknown(const Token &T, bool AsFunction) const;
    unsigned          BaseClause();

    Operand Sum();
    Operand Product();
    Operand Signed();
    Operand Powered();
    Operand Postfix();
    Operand Primary();
    Operand Call(const Token &Name, const Function &Which);
    Operand Name(const Token &T);
    Operand Join(const Operand &Left, char Mark, Span Where, const Operand &Right);
    bool    StartsFactor() const;
};

[[noreturn]] void Parser::Expected(const Token &T) const
{
    if (T.Type == Kind::End) throw CalculateError{T.At.Begin, T.At.End, "the sum ends here, but a number, a name or ( was expected"};
    if (T.Type == Kind::Symbol) {
        if (T.Symbol == ')') throw CalculateError{T.At.Begin, T.At.End, "a number, a name or ( was expected before this )"};
        throw CalculateError{T.At.Begin, T.At.End, std::format("a number, a name or ( was expected here, not {}", T.Symbol)};
    }
    throw CalculateError{T.At.Begin, T.At.End, "a number, a name or ( was expected here"};
}

[[noreturn]] void Parser::Unknown(const Token &T, bool AsFunction) const
{
    std::vector<std::string> Known;
    for (std::string_view Function : FunctionNames()) Known.emplace_back(Function);
    for (std::string_view Constant : ConstantNames()) Known.emplace_back(Constant);
    for (std::string &Variable : Owner.VariableNames()) Known.push_back(std::move(Variable));
    Known.emplace_back("ans");

    std::size_t Allowed = T.Name.size() >= 4 ? 2 : T.Name.size() == 3 ? 1 : 0;
    std::size_t Best    = Allowed + 1;
    std::string Near;
    for (const std::string &Candidate : Known) {
        std::size_t Apart = Distance(T.Name, Candidate);
        if (Apart < Best) {
            Best = Apart;
            Near = Candidate;
        }
    }
    std::string Reason = std::format("calculate does not know {} '{}'", AsFunction ? "a function called" : "the name", T.Name);
    if (!Near.empty()) Reason += std::format("; did you mean {}?", Near);
    throw CalculateError{T.At.Begin, T.At.End, Reason};
}

Operand Parser::Join(const Operand &Left, char Mark, Span Where, const Operand &Right)
{
    try {
        Number Value;
        switch (Mark) {
        case '+': Value = Add(Left.Value, Right.Value); break;
        case '-': Value = Subtract(Left.Value, Right.Value); break;
        case '*': Value = Multiply(Left.Value, Right.Value); break;
        case '/': Value = Divide(Left.Value, Right.Value); break;
        case '%': Value = Modulo(Left.Value, Right.Value); break;
        default:  Value = Power(Left.Value, Right.Value); break;
        }
        return {std::move(Value), {Left.At.Begin, Right.At.End}};
    } catch (const Refusal &Why) {
        throw ErrorFor(Why, Where, {Left.At, Right.At});
    }
}

Operand Parser::Sum()
{
    Operand Left = Product();
    while (Is(Peek(), '+') || Is(Peek(), '-')) {
        const Token Mark  = Take();
        Operand     Right = Product();
        Left              = Join(Left, Mark.Symbol, Mark.At, Right);
    }
    return Left;
}

bool Parser::StartsFactor() const
{
    const Token &T = Peek();
    return Is(T, '(') || (T.Type == Kind::Name && T.Name != "in");
}

Operand Parser::Product()
{
    Operand Left = Signed();
    for (;;) {
        const Token &T = Peek();
        if (Is(T, '*') || Is(T, '/') || Is(T, '%')) {
            const Token Mark  = Take();
            Operand     Right = Signed();
            Left              = Join(Left, Mark.Symbol, Mark.At, Right);
        } else if (StartsFactor()) {
            Operand Right = Powered();
            Left          = Join(Left, '*', Right.At, Right);
        } else {
            return Left;
        }
    }
}

/* A minus is less than a power: -2^2 is -4. */
Operand Parser::Signed()
{
    const Token &T = Peek();
    if (!Is(T, '-') && !Is(T, '+')) return Powered();
    const Token Mark  = Take();
    Operand     Inner = Signed();
    return {Mark.Symbol == '-' ? Negate(Inner.Value) : Inner.Value, {Mark.At.Begin, Inner.At.End}};
}

/* Right to left: 2^3^2 is 2^9. The power may itself be signed: 2^-1. */
Operand Parser::Powered()
{
    Operand Base = Postfix();
    if (!Is(Peek(), '^')) return Base;
    const Token Mark = Take();
    Operand     Raised = Signed();
    if (Base.IsEuler) {
        try {
            return {Exponential(Raised.Value), {Base.At.Begin, Raised.At.End}};
        } catch (const Refusal &Why) {
            throw ErrorFor(Why, Mark.At, {Base.At, Raised.At});
        }
    }
    return Join(Base, '^', Mark.At, Raised);
}

Operand Parser::Postfix()
{
    Operand Left = Primary();
    while (Is(Peek(), '!')) {
        const Token Mark = Take();
        try {
            Left = {Factorial(Left.Value), {Left.At.Begin, Mark.At.End}};
        } catch (const Refusal &Why) {
            throw ErrorFor(Why, Mark.At, {Left.At});
        }
    }
    return Left;
}

Operand Parser::Call(const Token &Name, const Function &Which)
{
    const Token Open = Take();
    std::vector<Operand> Given;
    if (!Is(Peek(), ')')) {
        for (;;) {
            Given.push_back(Sum());
            if (Is(Peek(), ',')) {
                Take();
                continue;
            }
            break;
        }
    }
    if (!Is(Peek(), ')')) {
        const Token &T = Peek();
        if (T.Type == Kind::End || Is(T, ')')) throw CalculateError{Open.At.Begin, Open.At.End, "this ( is never closed"};
        throw CalculateError{T.At.Begin, T.At.End, "an operator, a comma or ) was expected here"};
    }
    const Token Close = Take();

    bool Few  = Given.size() < Which.Least;
    bool Many = Which.Most != 0 && Given.size() > Which.Most;
    if (Few || Many) {
        std::string Wants = Which.Least == Which.Most ? std::format("{}", Which.Least)
                            : Which.Most == 0       ? std::format("{} or more", Which.Least)
                                                    : std::format("{} to {}", Which.Least, Which.Most);
        throw CalculateError{Name.At.Begin, Close.At.End,
                             std::format("{} takes {} number{}, not {}", Name.Name, Wants, Which.Least == 1 && Which.Most == 1 ? "" : "s", Given.size())};
    }

    std::vector<Number> Values;
    std::vector<Span>   Places;
    for (const Operand &Each : Given) {
        Values.push_back(Each.Value);
        Places.push_back(Each.At);
    }
    try {
        return {Which.Run(Values), {Name.At.Begin, Close.At.End}};
    } catch (const Refusal &Why) {
        throw ErrorFor(Why, Name.At, Places);
    }
}

Operand Parser::Name(const Token &T)
{
    if (T.Name == "ans") {
        if (const Number *Last = Owner.Last()) return {*Last, T.At};
        throw CalculateError{T.At.Begin, T.At.End, "there is no answer yet for ans to be"};
    }
    if (std::optional<Number> Constant = FindConstant(T.Name)) return {*Constant, T.At, T.Name == "e"};
    if (const Number *Held = Owner.Variable(T.Name)) return {*Held, T.At};
    Unknown(T, false);
}

Operand Parser::Primary()
{
    const Token T = Take();
    if (T.Type == Kind::Number) return {T.Value, T.At};
    if (Is(T, '(')) {
        if (Is(Peek(), ')')) throw CalculateError{T.At.Begin, Peek().At.End, "there is nothing between ( and )"};
        Operand Inside = Sum();
        if (Is(Peek(), ',')) throw CalculateError{Peek().At.Begin, Peek().At.End, "a comma goes between the numbers of a function"};
        if (!Is(Peek(), ')')) throw CalculateError{T.At.Begin, T.At.End, "this ( is never closed"};
        Take();
        return {Inside.Value, {T.At.Begin, Tokens[Next - 1].At.End}};
    }
    if (T.Type == Kind::Name) {
        if (T.Name == "in") throw CalculateError{T.At.Begin, T.At.End, "in comes after a sum: 255 in hex"};
        if (const Function *Which = FindFunction(T.Name)) {
            if (!Is(Peek(), '(')) throw CalculateError{T.At.Begin, T.At.End, std::format("{} is a function; write {}(...) with its numbers", T.Name, T.Name)};
            return Call(T, *Which);
        }
        if (Is(Peek(), '(') && !FindConstant(T.Name) && !Owner.Variable(T.Name) && T.Name != "ans") Unknown(T, true);
        return Name(T);
    }
    Expected(T);
}

unsigned Parser::BaseClause()
{
    Take();
    const Token &T = Peek();
    if (T.Type == Kind::Name) {
        for (const BaseWord &Known : BaseWords) {
            if (T.Name == Known.Word) {
                Take();
                return Known.Base;
            }
        }
    }
    throw CalculateError{T.At.Begin, T.At.End, "after in comes hex, bin, oct or dec"};
}

Answer Parser::Statement()
{
    Answer Result;
    if (Peek().Type == Kind::Name && Is(Peek(1), '=')) {
        const Token Target = Take();
        Take();
        if (IsKeyword(Target.Name))
            throw CalculateError{Target.At.Begin, Target.At.End,
                                 std::format("{} already means something in a sum; it cannot be given a value", Target.Name)};
        Result.Value    = Sum().Value;
        Result.Assigned = Target.Name;
    } else {
        Operand Worked = Sum();
        Result.Value   = std::move(Worked.Value);
        if (Peek().Type == Kind::Name && Peek().Name == "in") {
            Span Clause = Peek().At;
            Result.Base = BaseClause();
            Clause.End  = Tokens[Next - 1].At.End;
            if (Result.Base != 10 && !Result.Value.IsWhole())
                throw CalculateError{Clause.Begin, Clause.End, "only a whole number can be written in another base"};
        }
    }

    const Token &Rest = Peek();
    if (Rest.Type == Kind::End) return Result;
    if (Is(Rest, ')')) throw CalculateError{Rest.At.Begin, Rest.At.End, "this ) has no ( before it"};
    if (Is(Rest, ',')) throw CalculateError{Rest.At.Begin, Rest.At.End, "a comma goes between the numbers of a function"};
    if (Is(Rest, '=')) throw CalculateError{Rest.At.Begin, Rest.At.End, "= gives a name a value: x = 5"};
    throw CalculateError{Rest.At.Begin, Rest.At.End, "an operator was expected here"};
}

}

const Number *Calculator::Variable(std::string_view Name) const
{
    auto Found = Variables.find(Name);
    return Found == Variables.end() ? nullptr : &Found->second;
}

std::vector<std::string> Calculator::VariableNames() const
{
    std::vector<std::string> Names;
    for (const auto &[Name, Value] : Variables) Names.push_back(Name);
    return Names;
}

Answer Calculator::Work(std::string_view Text)
{
    Parser Reader(*this, Tokenize(Text));
    Answer Result = Reader.Statement();
    if (!Result.Assigned.empty()) Variables[Result.Assigned] = Result.Value;
    LastAnswer = Result.Value;
    HasLast    = true;
    return Result;
}
