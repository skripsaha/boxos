/* play.cpp — a round from its word to its end (game.h). */

#include "game.h"

#include <algorithm>
#include <format>
#include <utility>

namespace hangman {

namespace {

constexpr std::uint32_t BasePoints = 40;
constexpr std::uint32_t LetterPoints = 12;
constexpr std::uint32_t LevelPoints = 30;
constexpr std::uint32_t SparePoints = 20;
constexpr std::uint32_t StreakPoints = 15;
constexpr std::uint32_t StreakCap = 10;
constexpr std::uint64_t LetterGapMs = 110;

}  // namespace

// A word from the deck, by what was asked at the door, else by the settings.
void Game::Next()
{
    const std::optional<std::size_t> Only = Asked.Only ? Asked.Only : Chosen.Only;
    const std::optional<Level>       Hardness = Asked.Hardness ? Asked.Hardness : Chosen.Hardness;
    std::optional<Drawn> Picked = Words.Draw(Chance, Only, Hardness);
    if (!Picked) Picked = Words.Draw(Chance, std::nullopt, std::nullopt);
    Begin(std::string(Picked->Word), std::string(Categories()[Picked->Category].Name), true);
}

void Game::Begin(std::string Word, std::string Label, bool Count)
{
    const Level Hardness = LevelOf(Word);
    Lit.assign(Word.size(), 0);
    Now.emplace(std::move(Word), std::move(Label), Hardness);
    Given.reset();
    EndedAt = 0;
    Earned = 0;
    Paid = 0;
    Refused = 0;
    Counted = Count;
    Note.clear();
    Picture.Begin(Time);
}

void Game::Tell(std::string Words, NoteKind Kind)
{
    Note = std::move(Words);
    NoteWas = Kind;
    NoteAt = Time;
}

// The places of Letter in the word light up one after another.
void Game::Light(char Letter)
{
    std::uint64_t Moment = Time;
    for (std::size_t Place = 0; Place < Now->Word().size(); Place++) {
        if (Now->Word()[Place] != Letter) continue;
        Lit[Place] = Moment;
        Moment += LetterGapMs;
    }
}

void Game::Say(char Raw)
{
    if (Now->State() != Phase::Playing) return;
    const char Letter = Round::Upper(Raw);
    switch (Now->Say(Letter)) {
    case Guess::Repeated:
        Refused = Letter;
        RefusedAt = Time;
        Tell(std::format("{} has been tried already", Letter), NoteKind::Warn);
        break;
    case Guess::Right:
        Light(Letter);
        break;
    case Guess::Wrong:
        Picture.Mistake(Now->Mistakes() - 1, Time);
        Tell(std::format("no {} in it", Letter), NoteKind::Bad);
        break;
    }
    if (Now->State() != Phase::Playing) Finish();
}

void Game::TakeHint()
{
    if (Now->State() != Phase::Playing) return;
    if (Chosen.HintsAllowed == 0) {
        Tell("hints are off: settings hangman hints turns them on", NoteKind::Warn);
        return;
    }
    if (static_cast<std::uint32_t>(Now->Hints()) >= Chosen.HintsAllowed) {
        Tell("no hints left for this word", NoteKind::Warn);
        return;
    }
    const bool Mistake = Chosen.Price == HintCost::Mistake;
    if (Mistake && !Now->Pay()) {
        Tell("a hint would cost the last mistake: guess on", NoteKind::Warn);
        return;
    }
    const std::optional<char> Letter = Now->Reveal(Chance);
    if (!Letter) return;
    if (Mistake) Picture.Mistake(Now->Mistakes() - 1, Time);
    else Paid += HintPoints;
    Given.set(static_cast<std::size_t>(*Letter - 'A'));
    Light(*Letter);
    Tell(std::format("a hint: there is {} in it", *Letter), NoteKind::Warn);
    if (Now->State() != Phase::Playing) Finish();
}

void Game::GiveUp()
{
    Now->Concede();
    Tell("given up", NoteKind::Bad);
    Finish();
}

// What the round earned, once it is won.
std::uint32_t Game::Points() const
{
    const std::uint32_t Rank = static_cast<std::uint32_t>(Now->Hardness());
    const std::uint32_t Gross = BasePoints + LetterPoints * static_cast<std::uint32_t>(Now->Word().size()) + LevelPoints * Rank +
                                SparePoints * static_cast<std::uint32_t>(MostMistakes - Now->Mistakes()) +
                                StreakPoints * std::min(Book.Totals().Streak, StreakCap);
    return Gross > Paid ? Gross - Paid : 0;
}

void Game::Finish()
{
    const bool Won = Now->State() == Phase::Won;
    EndedAt = Time;
    if (Won) {
        Earned = Points();
        Picture.Free(Time);
    } else {
        Picture.Lose(Time);
    }
    if (!Counted) return;
    Book.Add(Won, Earned);
    if (!Book.Trouble().empty()) Tell(Book.Trouble(), NoteKind::Bad);
}

}  // namespace hangman
