/* board.cpp — the board's places and the cursor (board.h). */

#include "board.h"

#include <algorithm>
#include <vector>

namespace casino {

namespace {

std::vector<Place> Build()
{
    std::vector<Place> All;
    All.push_back(Place{Spot{Kind::Straight, 0, 0}, 0, 0, 0, 2});
    for (int Across = 1; Across <= 12; Across++)
        for (int Down = 0; Down < 3; Down++) {
            const auto Number = static_cast<std::uint8_t>(NumberIn(Across, Down));
            All.push_back(Place{Spot{Kind::Straight, Number, 0}, Across, Across, Down, Down});
        }
    for (int Down = 0; Down < 3; Down++)
        All.push_back(Place{Spot{Kind::Column, static_cast<std::uint8_t>(2 - Down), 0}, 13, 13, Down, Down});
    for (int Dozen = 0; Dozen < 3; Dozen++)
        All.push_back(Place{Spot{Kind::Dozen, static_cast<std::uint8_t>(Dozen), 0}, 1 + Dozen * 4, 4 + Dozen * 4, 3, 3});
    constexpr Kind Outside[6] = {Kind::Low, Kind::Even, Kind::Red, Kind::Black, Kind::Odd, Kind::High};
    for (int At = 0; At < 6; At++) All.push_back(Place{Spot{Outside[At], 0, 0}, 1 + At * 2, 2 + At * 2, 4, 4});
    return All;
}

const std::vector<Place> &Places()
{
    static const std::vector<Place> All = Build();
    return All;
}

}  // namespace

int NumberIn(int Across, int Down)
{
    return 3 * (Across - 1) + 3 - Down;
}

const Place *PlaceAt(int Across, int Down)
{
    for (const Place &One : Places())
        if (Across >= One.Left && Across <= One.Right && Down >= One.Top && Down <= One.Bottom) return &One;
    return nullptr;
}

const Place *PlaceOf(const Spot &On)
{
    for (const Place &One : Places())
        if (One.On == On) return &One;
    return nullptr;
}

void Cursor::Go(int Across, int Down)
{
    const Place &From = Here();
    if (Across != 0) Col = Across > 0 ? From.Right + 1 : From.Left - 1;
    if (Down != 0) Row += Down;
    Col = std::clamp(Col, 0, BoardWide - 1);
    Row = std::clamp(Row, 0, BoardHigh - 1);
    // The zero's column holds only the zero, in the rows of the numbers.
    if (Col == 0 && Row > 2) Col = 1;
    if (Col == 13 && Row > 2) Col = 12;
}

const Place &Cursor::Here() const
{
    return *PlaceAt(Col, Row);
}

}  // namespace casino
