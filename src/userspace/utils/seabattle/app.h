/* app.h — the loop: the page that is shown, drawn and given the keys, until
 * the person quits or the program is asked to finish. */
#ifndef SEABATTLE_APP_H
#define SEABATTLE_APP_H

#include "context.h"

namespace seabattle {

void Run(Context &Shared, PageId First);

}  // namespace seabattle

#endif
