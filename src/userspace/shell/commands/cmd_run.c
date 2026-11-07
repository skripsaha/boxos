/*
 * run — start a program (or an Itinerary) given as a thing rather than as a name.
 *
 *   run planets.elf        the file called planets.elf
 *   run it                 the program the last command gave back
 *
 * The rest of the line is the program's own: it is sorted into its Form like
 * the words after a program's name.
 */

#include "commands.h"
#include "../executor.h"
#include "../parser.h"
#include "../program.h"
#include "../fill.h"
#include "../itinerary.h"
#include "box/print.h"
#include "box/memory.h"

int cmd_run(int argc, char *argv[])
{
    if (argc < 2) {
        println("run: run what? A file such as planets.elf, or it");
        return 1;
    }

    file_id_t program;
    if (ProgramPickThing(argv[1], &program) != PROGRAM_PICKED)
        return 1;

    char       *name = ProgramNameOf(program);
    const char *who  = name ? name : argv[1];
    const char *rest = ParserRest(ExecutorLine(), 2);
    bool went = ProgramIsItinerary(program) ? ItineraryRun(program, who, rest)
                                            : FillAndRun(program, who, rest);
    free(name);
    return went ? 0 : 1;
}
