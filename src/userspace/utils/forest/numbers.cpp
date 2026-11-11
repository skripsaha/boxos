/* numbers.cpp — the table the waves are read from. */

#include "numbers.h"

#include <cmath>

namespace forest {

float WaveTable[WaveSteps];

void MakeWaves()
{
    for (int Step = 0; Step < WaveSteps; Step++)
        WaveTable[Step] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * Step / WaveSteps));
}

}  // namespace forest
