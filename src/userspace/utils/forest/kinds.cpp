/* kinds.cpp — the names. */

#include "kinds.h"

namespace forest {

const char *SeasonName(Season Value)
{
    switch (Value) {
    case Season::Spring: return "spring";
    case Season::Summer: return "summer";
    case Season::Autumn: return "autumn";
    case Season::Winter: return "winter";
    }
    return "summer";
}

const char *WeatherName(Weather Value)
{
    switch (Value) {
    case Weather::Clear: return "clear";
    case Weather::Rain:  return "rain";
    case Weather::Snow:  return "snow";
    case Weather::Fog:   return "fog";
    case Weather::Wind:  return "wind";
    case Weather::Storm: return "storm";
    }
    return "clear";
}

}  // namespace forest
