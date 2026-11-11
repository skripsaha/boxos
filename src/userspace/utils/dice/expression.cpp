/* expression.cpp — an expression read from what was typed, and the grammar's
 * words for help. Every refusal says what was wrong, where, and what is
 * understood in its place. */

#include "expression.h"

#include <array>
#include <format>

namespace dice {

namespace {

constexpr std::array<std::string_view, 10> Grammar = {
    "3          a number alone is that many d6: 3 is 3d6",
    "d20        one die: d and its sides, 2 to 1000;  d% is d100",
    "3d6        dice of one kind: how many before the d, 100 at most",
    "2d6+3      a plain number added, or taken away: d20-1",
    "4d6kh3     keep the highest 3 of them;  kl keeps the lowest: 4d6kl1",
    "4d6dl1     drop the lowest 1 of them;  dh drops the highest: 4d6dh1",
    "adv  dis   a d20 with advantage (two d20, the higher counts) or",
    "           disadvantage (the lower counts): adv+5",
    "4d6 1d8 d4 groups apart by a space or a comma are added up",
    "           the whole is 200 dice at most",
};

char Lower(char Letter)
{
    return Letter >= 'A' && Letter <= 'Z' ? static_cast<char>(Letter - 'A' + 'a') : Letter;
}

bool IsDigit(char Letter)
{
    return Letter >= '0' && Letter <= '9';
}

bool IsLetter(char Letter)
{
    return Lower(Letter) >= 'a' && Lower(Letter) <= 'z';
}

class Reader {
public:
    explicit Reader(std::string_view Written) : Text(Written) {}

    std::expected<Expression, ParseError> Run();

private:
    using Failure = std::unexpected<ParseError>;

    bool AtEnd() const { return At >= Text.size(); }
    char Peek(std::size_t Ahead = 0) const { return At + Ahead < Text.size() ? Lower(Text[At + Ahead]) : '\0'; }
    void SkipBlanks();
    void SkipSeparators();
    // What is left of the text from here, for a refusal to quote.
    std::string Rest() const;
    // " (at "...")" for a refusal to say where, nothing at the end of the text.
    std::string Where() const;
    Failure Refuse(const std::string &Message) const { return Failure(ParseError{Message}); }

    std::expected<int, ParseError>  Number(int Most, std::string_view Whose);
    std::expected<Term, ParseError> ReadTerm();
    std::expected<Term, ParseError> ReadWord();
    std::expected<Term, ParseError> ReadDice(int Count);
    std::expected<Expression, ParseError> Counted(Expression Whole) const;
    std::expected<void, ParseError> ReadKeep(Term &Group);

    std::string_view Text;
    std::size_t      At = 0;
};

void Reader::SkipBlanks()
{
    while (!AtEnd() && Text[At] == ' ') At++;
}

void Reader::SkipSeparators()
{
    while (!AtEnd() && (Text[At] == ' ' || Text[At] == ',')) At++;
}

std::string Reader::Rest() const
{
    return std::string(Text.substr(At, 12));
}

std::string Reader::Where() const
{
    return AtEnd() ? std::string(" (the roll ends there)") : std::format(" (at \"{}\")", Rest());
}

std::expected<int, ParseError> Reader::Number(int Most, std::string_view Whose)
{
    long Value = 0;
    while (IsDigit(Peek())) {
        Value = Value * 10 + (Peek() - '0');
        At++;
        if (Value > Most) return Refuse(std::format("{} is at most {}", Whose, Most));
    }
    return static_cast<int>(Value);
}

std::expected<void, ParseError> Reader::ReadKeep(Term &Group)
{
    const char Letter = Peek();
    if (Letter == 'k') {
        At++;
        Group.Rule = Peek() == 'l' ? Keep::Lowest : Keep::Highest;
        if (Peek() == 'h' || Peek() == 'l') At++;
    } else {
        At++;
        Group.Rule = Peek() == 'l' ? Keep::DropLowest : Keep::DropHighest;
        At++;
    }
    const bool Keeping = Group.Rule == Keep::Highest || Group.Rule == Keep::Lowest;
    const char *Verb   = Keeping ? "keep" : "drop";
    if (!IsDigit(Peek())) {
        Group.Amount = 1;
    } else {
        std::expected<int, ParseError> Amount = Number(MostPerTerm, "how many to keep or drop");
        if (!Amount) return std::unexpected(Amount.error());
        Group.Amount = *Amount;
    }
    if (Group.Amount < 1) return Refuse(std::format("{} at least 1 die, not {}", Verb, Group.Amount));
    if (Keeping && Group.Amount > Group.Count)
        return Refuse(std::format("cannot keep {} of {} dice", Group.Amount, Group.Count));
    if (!Keeping && Group.Amount >= Group.Count)
        return Refuse(std::format("dropping {} of {} dice leaves none to count", Group.Amount, Group.Count));
    return {};
}

std::expected<Term, ParseError> Reader::ReadDice(int Count)
{
    if (Count < 1) return Refuse("a group is at least 1 die, not 0");
    if (Count > MostPerTerm) return Refuse(std::format("a group is {} dice at most", MostPerTerm));
    At++;
    Term Group;
    Group.Count = Count;
    if (Peek() == '%') {
        At++;
        Group.Sides = 100;
    } else if (IsDigit(Peek())) {
        std::expected<int, ParseError> Sides = Number(MostSides, "the sides of a die");
        if (!Sides) return std::unexpected(Sides.error());
        Group.Sides = *Sides;
    } else {
        return Refuse(std::format("d wants the number of sides after it, as in d20 or 3d6{}", Where()));
    }
    if (Group.Sides < 2) return Refuse(std::format("a die has 2 sides at least, not {}", Group.Sides));
    if (Peek() == 'k' || (Peek() == 'd' && (Peek(1) == 'h' || Peek(1) == 'l'))) {
        if (std::expected<void, ParseError> Kept = ReadKeep(Group); !Kept) return std::unexpected(Kept.error());
    }
    return Group;
}

std::expected<Term, ParseError> Reader::ReadWord()
{
    const std::size_t Begin = At;
    while (IsLetter(Peek())) At++;
    std::string Word(Text.substr(Begin, At - Begin));
    for (char &Letter : Word) Letter = Lower(Letter);
    if (Word == "d") return Refuse("d wants the number of sides after it, as in d20 or 3d6");
    const bool Advantage = Word == "adv" || Word == "advantage";
    if (!Advantage && Word != "dis" && Word != "disadvantage")
        return Refuse(std::format("\"{}\" is not a roll: write d20, 3d6, 2d6+3, 4d6kh3, adv or dis", Word));
    Term Pair;
    Pair.Count  = 2;
    Pair.Sides  = 20;
    Pair.Rule   = Advantage ? Keep::Highest : Keep::Lowest;
    Pair.Amount = 1;
    return Pair;
}

std::expected<Term, ParseError> Reader::ReadTerm()
{
    if (AtEnd()) return Refuse("a roll must follow the sign: the expression ends after it");
    const char First = Peek();
    if (IsDigit(First)) {
        std::expected<int, ParseError> Value = Number(MostModifier, "a number");
        if (!Value) return std::unexpected(Value.error());
        if (Peek() == 'd') return ReadDice(*Value);
        Term Plain;
        Plain.Modifier = *Value;
        return Plain;
    }
    if (First == 'd' && (IsDigit(Peek(1)) || Peek(1) == '%')) return ReadDice(1);
    if (IsLetter(First)) return ReadWord();
    return Refuse(std::format("\"{}\" is not part of a roll: write d20, 3d6, 2d6+3, 4d6kh3, adv or dis", Rest()));
}

// The whole read: a number alone is how many d6 to roll, and numbers alone
// are nothing to roll.
std::expected<Expression, ParseError> Reader::Counted(Expression Whole) const
{
    Term &First = Whole.Terms.front();
    if (Whole.Terms.size() == 1 && First.Count == 0 && !First.Negative) {
        if (First.Modifier < 1) return Refuse("a group is at least 1 die, not 0");
        if (First.Modifier > MostPerTerm) return Refuse(std::format("a group is {} dice at most", MostPerTerm));
        First.Count    = First.Modifier;
        First.Sides    = 6;
        First.Spelled += "d6";
    }
    if (Whole.DiceCount() == 0) return Refuse("there is no die in it: add one, as d6 or 2d6");
    return Whole;
}

std::expected<Expression, ParseError> Reader::Run()
{
    SkipSeparators();
    if (AtEnd()) return Refuse("there is nothing to roll: write d20, 3d6, 2d6+3, 4d6kh3, adv or dis");
    Expression Whole;
    for (;;) {
        bool Negative = false;
        if (Peek() == '+' || Peek() == '-') {
            Negative = Peek() == '-';
            At++;
            SkipBlanks();
        }
        const std::size_t               Begin = At;
        std::expected<Term, ParseError> Next  = ReadTerm();
        if (!Next) return std::unexpected(Next.error());
        Next->Negative = Negative;
        for (std::size_t Char = Begin; Char < At; Char++) Next->Spelled += Lower(Text[Char]);
        Whole.Terms.push_back(std::move(*Next));
        if (Whole.DiceCount() > MostDice) return Refuse(std::format("{} dice is the most one roll holds", MostDice));

        const std::size_t Gap = At;
        SkipSeparators();
        if (AtEnd()) return Counted(std::move(Whole));
        if (At == Gap && Peek() != '+' && Peek() != '-')
            return Refuse(std::format("after {} the next group follows a space, a comma, + or - (at \"{}\")",
                                      Whole.Terms.back().Spelled, Rest()));
    }
}

}  // namespace

int Expression::DiceCount() const
{
    int Total = 0;
    for (const Term &One : Terms) Total += One.Count;
    return Total;
}

std::expected<Expression, ParseError> Parse(std::string_view Text)
{
    return Reader(Text).Run();
}

std::string Spell(const Expression &Of)
{
    std::string Out;
    for (const Term &One : Of.Terms) {
        if (One.Negative) Out += '-';
        else if (!Out.empty()) Out += '+';
        Out += One.Spelled;
    }
    return Out;
}

std::span<const std::string_view> GrammarLines()
{
    return Grammar;
}

}  // namespace dice
