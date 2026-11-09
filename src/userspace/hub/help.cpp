/* help — what there is.
 *
 *   help          the hub's own commands
 *   help NAME     the command, or the program NAME: each field its Form
 *                 takes, what it gives back, and what can be set in it —
 *                 each setting as it is now, said as the utility settings
 *                 says it, and the lines of the person's file it could not
 *                 take */

#include <algorithm>
#include <string>

#include "box/color.h"
#include "box/form.h"
#include "box/life.h"
#include "box/print.h"
#include "box/settings.h"
#include "say.h"
#include "settings_pen.h"

#include "builtins.h"
#include "held.h"
#include "hub.h"
#include "itinerary.h"
#include "programs.h"
#include "words.h"

namespace hub {

namespace {

void ListCommands()
{
    Say("The hub's own commands:\n\n");
    // The descriptions stand in one column, two blanks past the longest usage.
    std::size_t Column = 0;
    for (const Builtin &One : Builtins()) Column = std::max(Column, One.Usage.size());
    for (const Builtin &One : Builtins()) {
        std::string Line = "  ";
        Line.append(One.Usage).append(Column + 2 - One.Usage.size(), ' ').append(One.What).append(1, '\n');
        Say(Line);
    }
    Say("\nAny other name runs a program. help NAME shows what it takes and how it is set.\n");
}

const char *KindSaid(const BoxFormField &Field)
{
    switch (Field.Kind) {
    case BOX_FORM_THING:  return "one file";
    case BOX_FORM_THINGS: return "one or more files";
    case BOX_FORM_NAME:   return "one word, a name";
    case BOX_FORM_WORDS:  return "words";
    case BOX_FORM_TEXT:   return "the rest of the line, as typed";
    default:              return "the name after ->";
    }
}

const char *GivesSaid(const BoxForm &Declared)
{
    switch (Declared.Gives) {
    case BOX_FORM_GIVES_THINGS: return "files (the next command reaches them as them or it)";
    case BOX_FORM_GIVES_TEXT:   return "text";
    default:                    return "nothing";
    }
}

void ShowForm(const std::string &Name, const BoxForm &Declared)
{
    if (!Declared.Declared) {
        Say("{} takes its words as typed.\n", Name.c_str());
        return;
    }
    if (Declared.FieldCount == 0) {
        Say("{} takes nothing.\n", Name.c_str());
    } else {
        Say("{} takes:\n", Name.c_str());
        for (std::uint32_t I = 0; I < Declared.FieldCount; I++) {
            const BoxFormField &Field = Declared.Fields[I];
            if (Field.Name) Say("  {}: ", Field.Name);
            else Say("  ");
            Say("{} - {}{}\n", Field.What, KindSaid(Field), Field.Optional ? " (optional)" : "");
        }
    }
    Say("{} gives: {}\n", Name.c_str(), GivesSaid(Declared));
}

// A value as the person's file writes it: between double quotes when it must be.
void SayValue(const char *Value)
{
    const SettingSpan Span = {reinterpret_cast<const uint8_t *>(Value), std::char_traits<char>::length(Value)};
    if (SettingsPenQuotes(Span)) Say("\"{}\"", Value);
    else Say(Value);
}

void ShowSetting(const BoxSettings &Settings, const BoxSetting &Setting)
{
    char Wants[160];
    settings_wants(&Setting, Wants, sizeof(Wants));
    Say("  ");
    SayIn(COLOR_CYAN, Setting.Name);
    Say(" ");
    SayValue(Setting.Value);
    if (Setting.Kind == SETTING_NUMBER && Setting.Unit[0]) Say(" {}", Setting.Unit);
    Say(" — {}\n      {}; ", Setting.What, Wants);
    if (Setting.Chosen) {
        SayIn(COLOR_GREEN, "you set it");
        Say(" in {}.settings, ", Settings.Program);
        SayValue(Setting.Default);
        Say(" as it came");
    } else {
        Say("as it came");
    }
    Say(Setting.NextStart ? "; takes effect when it starts next\n" : "\n");
}

// What can be set in the program, as it is set now. Nothing is said of a
// program that declares no settings.
void ShowSettings(file_id_t Program, const std::string &Name)
{
    BoxSettings Settings;
    if (settings_read(Program, &Settings) != 0) {
        Say("help: what can be set in {} cannot be read\n", Name.c_str());
        return;
    }
    if (Settings.Count > 0) {
        Say("{} can be set — settings {} NAME VALUE:\n", Name.c_str(), Name.c_str());
        for (std::size_t I = 0; I < Settings.Count; I++) ShowSetting(Settings, Settings.Settings[I]);
    }
    for (std::size_t I = 0; I < Settings.ProblemCount; I++) SayIn(COLOR_YELLOW, "{}\n", Settings.Problems[I]);
    settings_release(&Settings);
}

// How the program lives, said as the hub will act on it — nothing for a task,
// what most programs are.
void ShowLife(file_id_t Program, const std::string &Name)
{
    ProgramLife Life;
    if (program_life(Program, &Life) != 0) {
        Say("help: how {} lives cannot be read\n", Name);
        return;
    }
    if (Life.Living == PROGRAM_STAYS) Say("{} lives on by itself: the hub does not wait for it", Name);
    else if (Life.Living == PROGRAM_SERVES) Say("{} serves others for as long as the machine runs", Name);
    else if (Life.Rise) Say("{} is a task", Name);
    else return;
    Say(Life.Rise ? "; when it falls, the system raises it again.\n" : ".\n");
}

bool HelpProgram(const Look &Shown, std::string_view Asked)
{
    const std::string Name(Asked);
    programs::Picked  Program = programs::Pick(Shown, Asked);
    if (!Program) {
        if (Program.error() != programs::Said) Say("help: {} is neither a command nor a program\n", Name.c_str());
        return false;
    }
    const bool Itinerary = programs::IsItinerary(*Program);
    HeldForm   Declared;
    int        Rc = 0;
    if (Itinerary) {
        Rc = itinerary::FormOf(*Program, Declared);
    } else {
        Rc = form_of_file(*Program, Declared.Take());
        if (Rc != 0) Declared.Drop();
    }
    if (Rc != 0) {
        Say("help: cannot read what {} takes ({})\n", Name, Reason(Rc));
        return false;
    }
    ShowForm(Name, *Declared);
    if (Itinerary) return true;
    ShowLife(*Program, Name);
    ShowSettings(*Program, Name);
    return true;
}

}  // namespace

bool Help(Hub &Self, const Words &Typed)
{
    if (Typed.Count() < 2) {
        ListCommands();
        return true;
    }
    if (const Builtin *Own = BuiltinNamed(Typed[1])) {
        Say("{} - {}\n", Own->Usage, Own->What);
        return true;
    }
    return HelpProgram(Self.Shown, Typed[1]);
}

}  // namespace hub
