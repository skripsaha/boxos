/* scene.cpp — where the room's things stand for a size of glass. */

#include "scene.h"

#include <algorithm>

namespace wizard {

Scene Arranged(int Cols, int Rows)
{
    Scene Where{};
    Where.Cols = Cols;
    Where.Rows = Rows;
    Where.Width = Cols;
    Where.Height = Rows * 2;
    Where.ShelfTop = Where.Height - ShelfRows * 2;
    const int FloorHeight = std::clamp(Where.ShelfTop / 7, 3, 14);
    Where.FloorTop = Where.ShelfTop - FloorHeight;
    Where.FeetY = Where.FloorTop + FloorHeight * 6 / 10;

    const float Tall = std::clamp(static_cast<float>(Where.FeetY - 5) * 0.86f, 22.0f, 116.0f);
    Where.Unit = std::min(Tall / 100.0f, static_cast<float>(Where.Width) * 0.0036f);
    const float Unit = Where.Unit;
    Where.WizardX = static_cast<int>(static_cast<float>(Where.Width) * 0.27f);

    Where.CauldronWidth = std::max(12, static_cast<int>(Unit * 44.0f));
    Where.CauldronX = Where.WizardX + static_cast<int>(Unit * 84.0f);
    Where.PotTop = Where.FeetY - static_cast<int>(Unit * 32.0f);

    const int WindowWide = std::max(10, static_cast<int>(Unit * 30.0f));
    Where.Window = Box{static_cast<int>(static_cast<float>(Where.Width) * 0.84f) - WindowWide / 2, 3, WindowWide,
                       std::min(WindowWide * 3 / 2, Where.FloorTop - 8)};
    const int BooksWide = std::max(14, static_cast<int>(Unit * 42.0f));
    Where.Books = Box{2, 3, BooksWide, std::min(static_cast<int>(Unit * 72.0f), Where.FloorTop - 5)};
    Where.BookRows = std::clamp(Where.Books.Height / 11, 2, 5);
    const int JarsWide = std::max(16, static_cast<int>(Unit * 40.0f));
    Where.Jars = Box{static_cast<int>(static_cast<float>(Where.Width) * 0.62f) - JarsWide / 2, Where.FloorTop * 2 / 5, JarsWide,
                     std::max(6, static_cast<int>(Unit * 12.0f))};

    Where.CandleX[0] = static_cast<int>(static_cast<float>(Where.Width) * 0.455f);
    Where.CandleY[0] = Where.FeetY;
    Where.CandleX[1] = Where.Books.X + Where.Books.Width * 3 / 4;
    Where.CandleY[1] = Where.Books.Y + Where.Books.Height / Where.BookRows - 2;
    Where.CandleX[2] = Where.Window.X + Where.Window.Width / 5;
    Where.CandleY[2] = Where.Window.Y + Where.Window.Height + 1;

    Where.OwlX = Where.Books.X + Where.Books.Width + std::max(2, static_cast<int>(Unit * 3.0f));
    Where.OwlY = Where.Books.Y + std::max(8, static_cast<int>(Unit * 22.0f));
    Where.CatX = static_cast<int>(static_cast<float>(Where.Width) * 0.80f);
    Where.CatY = Where.FeetY + 1;
    return Where;
}

}  // namespace wizard
