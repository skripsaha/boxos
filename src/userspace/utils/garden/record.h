/* record.h — what the gardener keeps and the garden shows: for every program
 * ever started, how often, and when first and last. One file on the volume,
 * garden.plants, found by its first line, "garden 1", and a line to a program
 * after it; nothing of the screen. */
#ifndef GARDEN_RECORD_H
#define GARDEN_RECORD_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/touch.h"

namespace garden {

struct Plant {
    std::string   Name;
    std::uint32_t Starts = 0;
    std::uint64_t Planted = 0;     // unix seconds of the first start heard
    std::uint64_t Last = 0;        // and of the latest
    bool          System = false;  // its cabin wears "system": the machine's own
};

struct Record {
    std::vector<Plant> Plants;

    // One start of the program Name, heard at Now.
    void Start(std::string_view Name, bool System, std::uint64_t Now);
    std::string Text() const;
    // The lines that are whole; any other is passed over.
    static Record FromText(std::string_view Text);
};

// The file read into Into — empty when there is none yet; a sentence for the
// person when it could not be read.
std::optional<std::string> ReadRecord(Record &Into);
// The file replaced by From, settled on the medium.
std::optional<std::string> WriteRecord(const Record &From);

// What the gardener tells once the file holds a new start: the viewer reads it again.
TouchTagPair GrewTag();

}  // namespace garden

#endif
