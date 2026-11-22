/* game.cpp — the game from the menu to its end. See game.h. */

#include "game.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "box/alarm.h"
#include "box/clock.h"

#include "look.h"

namespace chess {

namespace {

namespace KeyCodes = painter::tui::KeyCodes;
using painter::core::PointF;

constexpr Kind Promotions[4] = {Queen, Rook, Bishop, Knight};
constexpr std::uint64_t LateUs = 15000;   // the computer not yet done at its moment: looked at again so soon after

/* A key, or the keyboard gone, as Wait tells it. */
gui::Event AsEvent(const painter::tui::Event &Typed)
{
    gui::Event Made;
    Made.What = Typed.Kind == painter::tui::EventKind::Key        ? gui::Event::Kind::Key
              : Typed.Kind == painter::tui::EventKind::EndAsked ? gui::Event::Kind::EndAsked
                                                                : gui::Event::Kind::Gone;
    Made.Key  = Typed.Key;
    return Made;
}

bool Leaves(const painter::tui::KeyEvent &Key)
{
    return Key.IsCtrl() && (Key.Character == U'q' || Key.Character == U'Q');
}

const char *SideName(Side Of)
{
    return Of == White ? "White" : "Black";
}

}  // namespace

Game::Game(gui::Area Opened, painter::tui::Keyboard Heard) : Screen(std::move(Opened)), Keys(std::move(Heard)) {}

void Game::Build()
{
    View.reset();
    Beside.reset();
    Ending.reset();
    Options.reset();
    const PointF Size   = Screen.Size();
    BuiltFor            = Size;
    const float  Margin = std::max(16.0f, Size.Y * 0.045f);
    const float  Wide   = std::clamp(Size.X * 0.26f, 250.0f, 360.0f);
    const float  Square = std::max(20.0f, std::floor(std::min((Size.Y - 2 * Margin) / BoardView::Across(1.0f),
                                                              (Size.X - 3 * Margin - Wide) / BoardView::Across(1.0f))));
    const float  Across = BoardView::Across(Square);
    const PointF Corner{std::max(Margin, (Size.X - Across - Margin - Wide) / 2), (Size.Y - Across) / 2};

    gui::Fill Felt;
    Felt.What   = gui::Fill::Kind::Radial;
    Felt.From   = {Size.X / 2, Size.Y / 2};
    Felt.Radius = std::hypot(Size.X, Size.Y) * 0.6f;
    Felt.Stops  = {gui::Stop{0, ink::Table}, gui::Stop{1, ink::TableEdge}};
    Backdrop    = Screen.Rectangle();
    Backdrop.SetSize(Size).SetFill(Felt);

    View.emplace(Screen, Square, Corner, Flipped);
    Beside.emplace(Screen, PointF{Corner.X + Across + Margin, Corner.Y}, PointF{Wide, Across});
    Ending.emplace(Screen, PointF{Corner.X + Across / 2, Corner.Y + Across / 2});
    Options.emplace(Screen);
    View->Show(Board, false);
    Beside->Seats(Names[White], Names[Black], Flipped);
    Beside->Mode(Rules.Computer ? std::format("against the computer, level {}", Rules.Level) : "two players, one keyboard");
}

void Game::OpenMenu()
{
    Mind.Cancel();
    Asked = Rules;
    Row   = Ask::Opponent;
    Options->Show(Asked, Row, Started);
}

void Game::Begin()
{
    Mind.Cancel();
    Rules  = Asked;
    Seed   = clock_uptime_us() * 0x9E3779B97F4A7C15ull;
    Person = Rules.Side == Choice::Plays::Either ? ((Seed >> 40) & 1 ? Black : White) : Rules.Side == Choice::Plays::Black ? Black : White;
    Board  = Position();
    Written.clear();
    Flipped = Rules.Computer && Person == Black;
    Names[White] = !Rules.Computer ? "White" : Person == White ? "You" : std::format("Computer · level {}", Rules.Level);
    Names[Black] = !Rules.Computer ? "Black" : Person == Black ? "You" : std::format("Computer · level {}", Rules.Level);
    View->Flip(Flipped);
    View->Show(Board, Started);
    Beside->Seats(Names[White], Names[Black], Flipped);
    Beside->Mode(Rules.Computer ? std::format("against the computer, level {}", Rules.Level) : "two players, one keyboard");
    CursorAt = Person == Black ? SquareAt(4, 6) : SquareAt(4, 1);
    Started  = true;
    Options->Hide();
    Turned();
}

std::string Game::Status(Outcome Now) const
{
    const Side ToMove = Board.ToMove(), Mover = Other(ToMove);
    const auto Who    = [&](Side Of) -> std::string {
        if (!Rules.Computer) return SideName(Of);
        return Of == Person ? "you" : "the computer";
    };
    switch (Now) {
    case Outcome::Checkmate: return Rules.Computer ? (Mover == Person ? "Checkmate — you win" : "Checkmate — the computer wins") : std::format("Checkmate — {} wins", SideName(Mover));
    case Outcome::Stalemate: return "Stalemate — a draw";
    case Outcome::Repetition: return "A draw: the same position a third time";
    case Outcome::FiftyMoves: return "A draw: fifty moves with no capture and no pawn moved";
    case Outcome::Material: return "A draw: too little left to mate";
    case Outcome::Playing: break;
    }
    if (!Rules.Computer) return Board.InCheck() ? std::format("{} is in check", SideName(ToMove)) : std::format("{} to move", SideName(ToMove));
    if (ToMove == Person) return Board.InCheck() ? "Check — your move" : "Your move";
    return Board.InCheck() ? "Check — " + Who(ToMove) + " thinks" : "The computer thinks";
}

void Game::Turned()
{
    From.reset();
    Promoting.reset();
    Reach.Count = 0;
    View->Close();
    View->Pick(std::nullopt, Reach, Board);
    View->Last(Board.Last());
    View->Check(Board.InCheck() ? std::optional<Square>(Board.KingOf(Board.ToMove())) : std::nullopt);
    Beside->Moves(Written);
    Beside->Taken(Board);
    const Outcome Now = Board.Result();
    Over              = Now != Outcome::Playing;
    const bool Thinks = !Over && !Human(Board.ToMove());
    Beside->Turn(Board.ToMove(), Thinks, Over);
    Beside->Status(Status(Now), Over || Board.InCheck());
    View->Cursor(CursorAt, !Over && !Thinks);
    if (Over) {
        const bool Won = Now == Outcome::Checkmate;
        const Side Winner = Other(Board.ToMove());
        Ending->Show(Won ? "Checkmate" : Now == Outcome::Stalemate ? "Stalemate" : "Draw",
                     Won ? (Rules.Computer ? (Winner == Person ? "You win" : "The computer wins") : std::format("{} wins", SideName(Winner)))
                         : Now == Outcome::Repetition ? "the same position a third time"
                         : Now == Outcome::FiftyMoves ? "fifty moves with no capture and no pawn moved"
                         : Now == Outcome::Material   ? "too little left to mate"
                                                      : "no move, and no check");
        return;
    }
    Ending->Hide();
    if (Thinks) {
        Mind.Start(Board, Rules.Level, Seed);
        (void)alarm_set_us(Mind.DueUs());
    }
}

void Game::Answer()
{
    if (!Mind.Busy()) return;
    const std::uint64_t Now = clock_uptime_us();
    if (Mind.Ready() && Now >= Mind.DueUs()) {
        if (const Move One = Mind.Take()) Play(One);
        return;
    }
    (void)alarm_set_us(Now >= Mind.DueUs() ? Now + LateUs : Mind.DueUs());
}

void Game::Play(Move One)
{
    if (Human(Board.ToMove())) CursorAt = One.To;
    Written.push_back(Board.Written(One));
    (void)Board.Play(One);
    View->Show(Board, true);
    Turned();
}

void Game::TakeBack()
{
    if (Board.Played() == 0) return;
    int Count = 1;
    /* Against the computer, back to the person's own move before. */
    if (Rules.Computer && Human(Board.ToMove())) Count = 2;
    Count = std::min(Count, Board.Played());
    if (Rules.Computer && !Human(Count % 2 == 1 ? Other(Board.ToMove()) : Board.ToMove())) return;
    Mind.Cancel();
    for (int Undone = 0; Undone < Count; Undone++) {
        Board.Undo();
        Written.pop_back();
    }
    View->Show(Board, true);
    Turned();
}

void Game::Pick()
{
    if (Over || Mind.Busy() || !Human(Board.ToMove())) return;
    const Piece There = Board.At(CursorAt);
    if (From) {
        if (CursorAt == *From) {
            From.reset();
            View->Pick(std::nullopt, Reach, Board);
            return;
        }
        for (const Move One : Reach)
            if (One.From == *From && One.To == CursorAt) {
                if (One.Promote == NoKind) {
                    Play(One);
                    return;
                }
                Promoting = One;
                Offered   = 0;
                View->Offer(One.To, Board.ToMove(), Offered);
                return;
            }
    }
    if (There != 0 && SideOf(There) == Board.ToMove()) {
        From = CursorAt;
        Board.Moves(Reach);
        View->Pick(From, Reach, Board);
        return;
    }
    From.reset();
    View->Pick(std::nullopt, Reach, Board);
}

void Game::Step(int Right, int Down)
{
    int Col = Flipped ? 7 - FileOf(CursorAt) : FileOf(CursorAt);
    int Row = Flipped ? RankOf(CursorAt) : 7 - RankOf(CursorAt);
    Col     = std::clamp(Col + Right, 0, 7);
    Row     = std::clamp(Row + Down, 0, 7);
    CursorAt = SquareAt(Flipped ? 7 - Col : Col, Flipped ? Row : 7 - Row);
    View->Cursor(CursorAt, !Over && !Mind.Busy());
}

void Game::Flip()
{
    Flipped = !Flipped;
    View->Flip(Flipped);
    View->Pick(From, Reach, Board);
    Beside->Seats(Names[White], Names[Black], Flipped);
}

bool Game::OnMenuKey(const painter::tui::KeyEvent &Key)
{
    const int Rows = Asked.Computer ? 3 : 1;
    if (Key.IsExtended()) {
        const int By = Key.Scancode == KeyCodes::Left ? -1 : Key.Scancode == KeyCodes::Right ? 1 : 0;
        if (Key.Scancode == KeyCodes::Up || Key.Scancode == KeyCodes::Down)
            Row = static_cast<Ask>((static_cast<int>(Row) + (Key.Scancode == KeyCodes::Up ? Rows - 1 : 1)) % Rows);
        else if (By != 0) switch (Row) {
            case Ask::Opponent: Asked.Computer = !Asked.Computer; break;
            case Ask::Side: Asked.Side = static_cast<Choice::Plays>((static_cast<int>(Asked.Side) + 3 + By) % 3); break;
            case Ask::Level: Asked.Level = std::clamp(Asked.Level + By, 1, Levels); break;
            }
        Options->Show(Asked, Row, Started);
        return true;
    }
    switch (Key.Character) {
    case KeyCodes::Enter:
    case U' ': Begin(); return true;
    case KeyCodes::Escape:
        if (!Started) return false;
        Options->Hide();
        Turned();
        return true;
    case U'q':
    case U'Q': return false;
    default: return true;
    }
}

void Game::OnOfferKey(const painter::tui::KeyEvent &Key)
{
    /* The choices run from the edge of the board inward: towards the middle
     * is the next of them. */
    const bool Upward = (Flipped ? RankOf(Promoting->To) : 7 - RankOf(Promoting->To)) >= 4;
    if (Key.IsExtended()) {
        int By = 0;
        if (Key.Scancode == KeyCodes::Down || Key.Scancode == KeyCodes::Right) By = Upward && Key.Scancode == KeyCodes::Down ? -1 : 1;
        if (Key.Scancode == KeyCodes::Up || Key.Scancode == KeyCodes::Left) By = Upward && Key.Scancode == KeyCodes::Up ? 1 : -1;
        Offered = (Offered + 4 + By) % 4;
        View->Offer(Promoting->To, Board.ToMove(), Offered);
        return;
    }
    if (Key.Character == KeyCodes::Escape) {
        Promoting.reset();
        View->Close();
        return;
    }
    if (Key.Character != KeyCodes::Enter && Key.Character != U' ') return;
    const Move One = Board.Find(Promoting->From, Promoting->To, Promotions[Offered]);
    Promoting.reset();
    View->Close();
    if (One) Play(One);
}

bool Game::OnKey(const painter::tui::KeyEvent &Key)
{
    if (Leaves(Key)) return false;
    if (Options->Open()) return OnMenuKey(Key);
    if (Promoting) {
        OnOfferKey(Key);
        return true;
    }
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case KeyCodes::Left: Step(-1, 0); break;
        case KeyCodes::Right: Step(1, 0); break;
        case KeyCodes::Up: Step(0, -1); break;
        case KeyCodes::Down: Step(0, 1); break;
        default: break;
        }
        return true;
    }
    switch (Key.Character) {
    case KeyCodes::Enter:
    case U' ': Pick(); return true;
    case KeyCodes::Escape:
        if (!From) return false;
        From.reset();
        View->Pick(std::nullopt, Reach, Board);
        return true;
    case U'u':
    case U'U': TakeBack(); return true;
    case U'f':
    case U'F': Flip(); return true;
    case U'n':
    case U'N': OpenMenu(); return true;
    case U'q':
    case U'Q': return false;
    default: return true;
    }
}

/* False to leave. */
bool Game::OnEvent(const gui::Event &Told)
{
    switch (Told.What) {
    case gui::Event::Kind::Gone:
    case gui::Event::Kind::EndAsked: return false;
    case gui::Event::Kind::Key: return OnKey(Told.Key);
    case gui::Event::Kind::Area:
        if (Told.Size.X != BuiltFor.X || Told.Size.Y != BuiltFor.Y) {
            const bool Menu_ = Options && Options->Open();
            Build();
            if (Started) Turned();
            if (Menu_ || !Started) Options->Show(Asked, Row, Started);
        }
        return true;
    default: return true;
    }
}

void Game::Run()
{
    Keys.HearEnd();
    Build();
    OpenMenu();
    for (bool Going = true; Going;) {
        (void)Screen.Present();
        Going = OnEvent(Screen.Wait(&Keys));
        /* All the display told and all that was typed meanwhile, before the
         * next wait: the display tells something every frame it plays, and
         * keys waited for only behind it would never be heard. */
        while (Going)
            if (const std::optional<gui::Event> Told = Screen.Next()) Going = OnEvent(*Told);
            else break;
        for (painter::tui::Event Typed = Keys.Poll(); Going && Typed.Kind != painter::tui::EventKind::Quiet; Typed = Keys.Poll())
            Going = OnEvent(AsEvent(Typed));
        if (Going) Answer();
    }
    Mind.Cancel();
}

}  // namespace chess
