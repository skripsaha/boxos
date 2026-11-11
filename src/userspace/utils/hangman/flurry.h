/* flurry.h — what flies about in the scene: leaves the wind takes from the
 * tree, the confetti of a win, and sparks. Positions are in the scene's units. */
#ifndef HANGMAN_FLURRY_H
#define HANGMAN_FLURRY_H

#include <cstdint>
#include <vector>

#include "painter/tui/fine_canvas.h"

#include "luck.h"
#include "numbers.h"
#include "view.h"

namespace hangman {

class Flurry {
public:
    void Leaf(Luck &Chance, float X, float Y, Color Ink);
    void Confetti(Luck &Chance, float X, float Y, int Count);
    void Sparks(Luck &Chance, float X, float Y, int Count, Color Ink);
    void Step(std::uint32_t ElapsedMs, float Wind);
    void Paint(painter::tui::FineCanvas &Fine, const View &Where) const;
    void Clear() { Specks.clear(); }

private:
    enum class Kind : std::uint8_t { Leaf, Confetti, Spark };

    struct Speck {
        float X, Y;
        float Vx, Vy;
        float Turn, Spin;
        float Age, Life;      // seconds
        Color Ink;
        Kind  What;
    };

    void Add(const Speck &One);

    std::vector<Speck> Specks;
};

}  // namespace hangman

#endif
