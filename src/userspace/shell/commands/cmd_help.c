#include "commands.h"
#include "box/io.h"

int cmd_help(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    println("BoxOS Shell v1.0 - Available Commands:");
    println("");
    println("File Management:");
    println("  create <file> [tags]     Create new file with optional tags");
    println("  show <file>              Display file contents");
    println("  files [tags]             List all files (filtered by tags)");
    println("  tag <file> <key:val>     Add tag to file");
    println("  untag <file> <key>       Remove tag from file");
    println("  name <old> <new>         Rename file");
    println("  trash <file>             Move file to trash");
    println("  erase <file|trashed>     Delete file or all trashed files");
    println("");
    println("Context:");
    println("  use [tags]               Set/clear context tags");
    println("");
    println("System:");
    println("  me                       Show system information");
    println("  info [object]            Show detailed object information");
    println("  say <text>               Echo text to console");
    println("  reboot                   Reboot system");
    println("  bye                      Shutdown system");
    println("");
    println("General:");
    println("  clear                    Clear the screen");
    println("  help                     Show this help");
    println("  exit                     Exit shell");

    return 0;
}
