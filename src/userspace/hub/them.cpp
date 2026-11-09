/* them.cpp — the files the last command gave. */

#include "them.h"

#include <algorithm>
#include <utility>

namespace hub {

void Them::Keep(std::vector<file_id_t> Files)
{
    Held  = std::move(Files);
    Given = true;
}

void Them::Replace(file_id_t Old, file_id_t Made)
{
    std::replace(Held.begin(), Held.end(), Old, Made);
}

Them::Saved Them::Save()
{
    Saved Aside{std::move(Held), Given};
    Held.clear();
    Given = false;
    return Aside;
}

void Them::Restore(Saved Back)
{
    Held  = std::move(Back.Files);
    Given = Back.Known;
}

void Them::Show(BoxFormLookup &Lookup) const noexcept
{
    Lookup.Them      = Held.data();
    Lookup.ThemCount = Held.size();
    Lookup.ThemKnown = Given;
}

}  // namespace hub
