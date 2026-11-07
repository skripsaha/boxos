#ifndef MINESWEEPER_H
#define MINESWEEPER_H

/* minesweeper — the field and its rules (minesweeper_field.cpp), which take
 * nothing of BoxOS; minesweeper.cpp is the glass, the keys and the record.
 *
 * The mines are laid at the first opening and never under it nor round it,
 * so the first square opened always opens a stretch. A square with no mine
 * round it opens its neighbours; opening a number whose flags are all placed
 * opens the rest round it (a chord). The field is cleared when every square
 * without a mine is open. */

#include <cstdint>
#include <vector>

struct MineSize {
    int         Wide, High, Mines;
    const char *Name;
};

constexpr MineSize MineSizes[] = {{9, 9, 10, "beginner"}, {16, 16, 40, "intermediate"}, {30, 16, 99, "expert"}};
constexpr int      MineSizeCount = 3;

struct MineCell {
    bool         Mine    = false;
    bool         Open    = false;
    bool         Flag    = false;
    std::uint8_t Near    = 0;   /* mines in the eight round it */
};

enum class MineState { Fresh, Playing, Won, Lost };

class MineField {
public:
    void Start(const MineSize &Size, std::uint64_t Seed);

    /* The square opened; a mine there loses. */
    void Open(int X, int Y);
    /* An open number whose flags are all placed: the rest round it opened. */
    void Chord(int X, int Y);
    void ToggleFlag(int X, int Y);

    const MineCell &At(int X, int Y) const { return Cells[static_cast<std::size_t>(Y) * Wide + X]; }
    bool Inside(int X, int Y) const { return X >= 0 && Y >= 0 && X < Wide && Y < High; }
    int  FlagsLeft() const { return Mines - Flags; }
    /* The mines, nearest the one that went off first: the order they are shown in. */
    std::vector<int> MinesByDistance() const;

    MineState State = MineState::Fresh;
    int       Wide = 9, High = 9, Mines = 10;
    int       HitX = -1, HitY = -1;

private:
    MineCell &Cell(int X, int Y) { return Cells[static_cast<std::size_t>(Y) * Wide + X]; }
    void      Lay(int SafeX, int SafeY);
    void      Spread(int X, int Y);
    std::uint64_t Roll();

    std::vector<MineCell> Cells;
    int                   Flags  = 0;
    int                   Opened = 0;
    std::uint64_t         Seed   = 1;
};

#endif
