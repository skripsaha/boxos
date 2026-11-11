/* kinds.h — the seasons, the weathers and the kinds that live in the forest:
 * the words the person says and the settings say. */
#ifndef FOREST_KINDS_H
#define FOREST_KINDS_H

#include <cstdint>

namespace forest {

enum class Season : std::uint8_t { Spring, Summer, Autumn, Winter };
enum class Weather : std::uint8_t { Clear, Rain, Snow, Fog, Wind, Storm };

// The animals, the ground ones first, then what flies, then what only drifts.
enum class Kind : std::uint8_t { Fox, Hedgehog, Hare, Deer, Bird, Owl, Bat, Butterfly, Firefly };

inline constexpr int SeasonCount  = 4;
inline constexpr int WeatherCount = 6;
inline constexpr int KindCount    = 9;

const char *SeasonName(Season Value);
const char *WeatherName(Weather Value);

}  // namespace forest

#endif
