/*
 * help — what the shell and the programs on the volume take.
 *
 *   help               the shell's own commands
 *   help NAME          the command, or the program NAME and its Form: each
 *                      field it takes, and what it gives back
 */

#include "commands.h"
#include "../shell.h"
#include "../program.h"
#include "../itinerary.h"
#include "box/print.h"
#include "box/string.h"
#include "box/form.h"

extern const ShellCommand g_commands[];

static void ListCommands(void)
{
    println("BoxOS Shell - Available commands:");
    println("");

    /* The descriptions stand in one column, two blanks past the longest
     * usage. */
    size_t column = 0;
    for (int i = 0; g_commands[i].name != NULL; i++) {
        size_t usage_len = strlen(g_commands[i].usage);
        if (usage_len > column) column = usage_len;
    }

    for (int i = 0; g_commands[i].name != NULL; i++) {
        print("  ");
        print(g_commands[i].usage);

        for (size_t pad = strlen(g_commands[i].usage); pad < column + 2; pad++)
            print(" ");

        print(g_commands[i].description);
        println("");
    }

    println("");
    println("Any other name runs a program. help NAME shows what it takes.");
}

static const char *KindSaid(const BoxFormField *field)
{
    switch (field->Kind) {
    case BOX_FORM_THING:  return "one file";
    case BOX_FORM_THINGS: return "one or more files";
    case BOX_FORM_NAME:   return "one word, a name";
    case BOX_FORM_WORDS:  return "words";
    case BOX_FORM_TEXT:   return "the rest of the line, as typed";
    default:              return "the name after ->";
    }
}

static const char *GivesSaid(const BoxForm *form)
{
    switch (form->Gives) {
    case BOX_FORM_GIVES_THINGS: return "files (the next command reaches them as them or it)";
    case BOX_FORM_GIVES_TEXT:   return "text";
    default:                    return "nothing";
    }
}

static void ShowForm(const char *name, const BoxForm *form)
{
    if (!form->Declared) {
        printf("%s takes its words as typed.\n", name);
        return;
    }
    if (form->FieldCount == 0) {
        printf("%s takes nothing.\n", name);
    } else {
        printf("%s takes:\n", name);
        for (uint32_t i = 0; i < form->FieldCount; i++) {
            const BoxFormField *field = &form->Fields[i];
            if (field->Name)
                printf("  %s: %s - %s%s\n", field->Name, field->What, KindSaid(field),
                       field->Optional ? " (optional)" : "");
            else
                printf("  %s - %s%s\n", field->What, KindSaid(field),
                       field->Optional ? " (optional)" : "");
        }
    }
    printf("%s gives: %s\n", name, GivesSaid(form));
}

static int HelpProgram(const char *name)
{
    file_id_t program;
    int picked = ProgramPick(name, &program);
    if (picked == PROGRAM_SAID)
        return 1;
    if (picked < 0) {
        printf("help: %s is neither a command nor a program\n", name);
        return 1;
    }

    BoxForm form;
    int rc = ProgramIsItinerary(program) ? ItineraryForm(program, &form) : form_of_file(program, &form);
    if (rc != 0) {
        printf("help: cannot read what %s takes (error %d)\n", name, -rc);
        return 1;
    }
    ShowForm(name, &form);
    form_release(&form);
    return 0;
}

static int HelpCommand(const ShellCommand *command)
{
    printf("%s - %s\n", command->usage, command->description);
    return 0;
}

int cmd_help(int argc, char *argv[])
{
    if (argc < 2) {
        ListCommands();
        return 0;
    }
    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(argv[1], g_commands[i].name) == 0)
            return HelpCommand(&g_commands[i]);
    }
    return HelpProgram(argv[1]);
}
