/* records.h — the hall of records: for each name that has played, its games,
 * wins, shots and hits, kept in a file of the volume (seabattle.records) and
 * found again at the next start. */
#ifndef SEABATTLE_RECORDS_H
#define SEABATTLE_RECORDS_H

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace seabattle {

struct Record {
    std::string Name;
    int         Games = 0;
    int         Wins = 0;
    int         Shots = 0;
    int         Hits = 0;

    int AccuracyPercent() const { return Shots == 0 ? 0 : Hits * 100 / Shots; }
};

class Records {
public:
    Records();

    // The names, the most wins first.
    std::span<const Record> Ranked() const { return Kept; }
    // A game played by Name is counted, and the file is written.
    void Add(std::string_view Name, bool Won, int Shots, int Hits);
    // Why the file could not be read or written, else empty.
    const std::string &Trouble() const { return Said; }

private:
    void Rank();
    void Save();

    std::vector<Record> Kept;
    std::string         Said;
    bool                Writable = true;
};

}  // namespace seabattle

#endif
