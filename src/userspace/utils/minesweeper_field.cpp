/* minesweeper_field.cpp — the field and its rules (minesweeper.h). */

#include <algorithm>
#include <cstdlib>

#include "minesweeper.h"

std::uint64_t MineField::Roll()
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    return Seed;
}

void MineField::Start(const MineSize &Size, std::uint64_t Seeded)
{
    Wide   = Size.Wide;
    High   = Size.High;
    Mines  = Size.Mines;
    Seed   = Seeded | 1;
    State  = MineState::Fresh;
    Flags  = Opened = 0;
    HitX   = HitY = -1;
    Cells.assign(static_cast<std::size_t>(Wide) * High, MineCell{});
}

/* The mines among the squares not within one of (SafeX, SafeY), each square
 * as likely as another; then every square's count of them. */
void MineField::Lay(int SafeX, int SafeY)
{
    std::vector<int> Free;
    for (int Y = 0; Y < High; Y++)
        for (int X = 0; X < Wide; X++)
            if (std::abs(X - SafeX) > 1 || std::abs(Y - SafeY) > 1) Free.push_back(Y * Wide + X);
    int Count = std::min(Mines, static_cast<int>(Free.size()));
    for (int I = 0; I < Count; I++) {
        std::size_t Pick = I + static_cast<std::size_t>(Roll() % (Free.size() - I));
        std::swap(Free[static_cast<std::size_t>(I)], Free[Pick]);
        Cells[static_cast<std::size_t>(Free[static_cast<std::size_t>(I)])].Mine = true;
    }
    Mines = Count;
    for (int Y = 0; Y < High; Y++) {
        for (int X = 0; X < Wide; X++) {
            int Near = 0;
            for (int Dy = -1; Dy <= 1; Dy++)
                for (int Dx = -1; Dx <= 1; Dx++)
                    if ((Dx || Dy) && Inside(X + Dx, Y + Dy) && At(X + Dx, Y + Dy).Mine) Near++;
            Cell(X, Y).Near = static_cast<std::uint8_t>(Near);
        }
    }
}

/* From an empty square outward: every square reached is opened, and an empty
 * one among them goes on. A stack, not recursion — an expert field is 480. */
void MineField::Spread(int X, int Y)
{
    std::vector<int> Stack{Y * Wide + X};
    while (!Stack.empty()) {
        int At = Stack.back();
        Stack.pop_back();
        int Cx = At % Wide, Cy = At / Wide;
        for (int Dy = -1; Dy <= 1; Dy++) {
            for (int Dx = -1; Dx <= 1; Dx++) {
                int Nx = Cx + Dx, Ny = Cy + Dy;
                if (!Inside(Nx, Ny)) continue;
                MineCell &Next = Cell(Nx, Ny);
                if (Next.Open || Next.Flag || Next.Mine) continue;
                Next.Open = true;
                Opened++;
                if (Next.Near == 0) Stack.push_back(Ny * Wide + Nx);
            }
        }
    }
}

void MineField::Open(int X, int Y)
{
    if (State == MineState::Won || State == MineState::Lost || !Inside(X, Y)) return;
    if (State == MineState::Fresh) {
        Lay(X, Y);
        State = MineState::Playing;
    }
    MineCell &Here = Cell(X, Y);
    if (Here.Open || Here.Flag) return;
    Here.Open = true;
    if (Here.Mine) {
        State = MineState::Lost;
        HitX  = X;
        HitY  = Y;
        return;
    }
    Opened++;
    if (Here.Near == 0) Spread(X, Y);
    if (Opened == Wide * High - Mines) {
        State = MineState::Won;
        for (MineCell &One : Cells)
            if (One.Mine && !One.Flag) One.Flag = true;
        Flags = Mines;
    }
}

void MineField::Chord(int X, int Y)
{
    if (State != MineState::Playing || !Inside(X, Y)) return;
    const MineCell &Here = At(X, Y);
    if (!Here.Open || Here.Near == 0) return;
    int Flagged = 0;
    for (int Dy = -1; Dy <= 1; Dy++)
        for (int Dx = -1; Dx <= 1; Dx++)
            if (Inside(X + Dx, Y + Dy) && At(X + Dx, Y + Dy).Flag) Flagged++;
    if (Flagged != Here.Near) return;
    for (int Dy = -1; Dy <= 1 && State == MineState::Playing; Dy++)
        for (int Dx = -1; Dx <= 1 && State == MineState::Playing; Dx++)
            if (Inside(X + Dx, Y + Dy)) Open(X + Dx, Y + Dy);
}

void MineField::ToggleFlag(int X, int Y)
{
    if ((State != MineState::Playing && State != MineState::Fresh) || !Inside(X, Y)) return;
    MineCell &Here = Cell(X, Y);
    if (Here.Open) return;
    Here.Flag = !Here.Flag;
    Flags += Here.Flag ? 1 : -1;
}

std::vector<int> MineField::MinesByDistance() const
{
    std::vector<int> Found;
    for (int At = 0; At < Wide * High; At++)
        if (Cells[static_cast<std::size_t>(At)].Mine) Found.push_back(At);
    auto Far = [&](int At) {
        int Dx = At % Wide - HitX, Dy = At / Wide - HitY;
        return Dx * Dx + Dy * Dy;
    };
    std::stable_sort(Found.begin(), Found.end(), [&](int A, int B) { return Far(A) < Far(B); });
    return Found;
}
