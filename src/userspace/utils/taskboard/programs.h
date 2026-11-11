/* programs.h — the programs as the board keeps them from one reading to the
 * next: each one's name and how it lives, read once by its file; the
 * processor it used since the reading before; where it is — and the list
 * filtered, grouped and ordered as the person set. Nothing of the screen. */
#ifndef TASKBOARD_PROGRAMS_H
#define TASKBOARD_PROGRAMS_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "look.h"
#include "machine.h"

namespace taskboard {

// Where a program is, or else how it lives — the groups of the list, in
// their order.
enum class Living : std::uint8_t { OnScreen, Behind, Keeper, OnItsOwn, Serving, Task };

const char *LivingName(Living Where) noexcept;

struct Program {
    ProgramSeen         Seen;
    std::string         Name;
    Living              Where = Living::Task;
    bool                System = false;       // it wears system: the machine runs on it
    double              Processor = 0;        // cores' worth used since the reading before
    std::vector<double> StrandProcessor;      // each strand's, as Seen.Strands
};

class Programs {
public:
    void Take(const Reading &Now);

    const std::vector<Program> &All() const noexcept { return List; }
    const Program              *Find(std::uint32_t Pid, std::uint32_t Generation) const noexcept;
    // The program the strand (Pid, Generation) is of — a program is started
    // by a strand of another, not always its main one.
    const Program              *OwnerOf(std::uint32_t Pid, std::uint32_t Generation) const noexcept;
    // As Find, or one the latest reading no longer had: a death heard after
    // the reading that missed it is still named.
    const Program              *Recall(std::uint32_t Pid, std::uint32_t Generation) const noexcept;
    // Those Filter keeps — by a part of the name or of a tag, in any case —
    // grouped and ordered as the person set.
    std::vector<const Program *> Shown(const Look &Settings, std::string_view Filter) const;

private:
    std::string        NameOf(const ProgramSeen &Seen);
    std::uint8_t       LifeOf(file_id_t File);

    std::vector<Program>                         List;
    std::vector<Program>                         Gone;    // those of the reading before not in the latest
    std::uint64_t                                LastAtUs = 0;
    std::unordered_map<file_id_t, std::string>   Names;
    std::unordered_map<file_id_t, std::uint8_t>  Lives;   // box/life.h ProgramLiving
};

// Whether a comma-joined tag list holds Tag whole.
bool Wears(std::string_view Tags, std::string_view Tag) noexcept;

}  // namespace taskboard

#endif
