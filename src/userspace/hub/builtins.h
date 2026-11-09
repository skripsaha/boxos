/* builtins.h — the hub's own commands: help, use, said, run, clear, exit.
 *
 * A built-in has no Form; what its words are is the part of one the
 * Whisper needs. Each takes the line cut into words — the rest of the line as
 * typed is the words' tail — and says whether it went through. */
#ifndef HUB_BUILTINS_H
#define HUB_BUILTINS_H

#include <span>
#include <string_view>

namespace hub {

struct Hub;
class Words;

enum class Takes {
    Nothing,
    Command,   // its first word is a command's name
    Program,   // its first word is a program's file
    Tags,      // its words are tags, + or - before each when changing
};

struct Builtin {
    std::string_view Name;
    std::string_view Usage;
    std::string_view What;
    Takes            Kind;
    bool (*Run)(Hub &Self, const Words &Typed);
};

std::span<const Builtin> Builtins();
const Builtin           *BuiltinNamed(std::string_view Name);

bool Help(Hub &Self, const Words &Typed);
bool Use(Hub &Self, const Words &Typed);
bool Said(Hub &Self, const Words &Typed);

}  // namespace hub

#endif
