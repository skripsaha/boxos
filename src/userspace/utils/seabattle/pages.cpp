/* pages.cpp — the page for each place the game can be in. */

#include "pages.h"

namespace seabattle {

std::unique_ptr<Page> MakePage(PageId Which, Context &Shared)
{
    switch (Which) {
    case PageId::Setup:    return MakeSetup(Shared);
    case PageId::Rules:    return MakeRules(Shared);
    case PageId::Hall:     return MakeHall(Shared);
    case PageId::Handover: return MakeHandover(Shared);
    case PageId::Place:    return MakePlace(Shared);
    case PageId::Battle:   return MakeBattle(Shared);
    case PageId::Victory:  return MakeVictory(Shared);
    case PageId::Title:
    case PageId::Quit:     break;
    }
    return MakeTitle(Shared);
}

}  // namespace seabattle
