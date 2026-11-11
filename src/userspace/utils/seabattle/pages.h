/* pages.h — the pages of the sea battle, each made by a function of its own file. */
#ifndef SEABATTLE_PAGES_H
#define SEABATTLE_PAGES_H

#include <memory>

#include "page.h"

namespace seabattle {

std::unique_ptr<Page> MakeTitle(Context &Shared);
std::unique_ptr<Page> MakeSetup(Context &Shared);
std::unique_ptr<Page> MakeRules(Context &Shared);
std::unique_ptr<Page> MakeHall(Context &Shared);
std::unique_ptr<Page> MakeHandover(Context &Shared);
std::unique_ptr<Page> MakePlace(Context &Shared);
std::unique_ptr<Page> MakeBattle(Context &Shared);
std::unique_ptr<Page> MakeVictory(Context &Shared);

}  // namespace seabattle

#endif
