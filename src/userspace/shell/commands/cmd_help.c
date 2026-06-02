#include "commands.h"
#include "../shell.h"
#include "box/print.h"
#include "box/string.h"

extern const ShellCommand g_commands[];

int cmd_help(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    println("BoxOS Shell — Available commands:");
    println("");

    for (int i = 0; g_commands[i].name != NULL; i++) {
        print("  ");
        print(g_commands[i].usage);

        size_t usage_len = strlen(g_commands[i].usage);
        for (size_t pad = usage_len + 2; pad < 20; pad++)
            print(" ");

        print(g_commands[i].description);
        println("");
    }

    println("");
    println("Any other name runs an external utility.");
    return 0;
}
