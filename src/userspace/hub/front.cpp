/* front.cpp — front: the programs living behind, and one of them brought
 * forward.
 *
 *   front            who lives behind, and who wants the keyboard: in a
 *                    window, where Enter brings the one marked forward — or
 *                    numbered, as the person set the hub (choose)
 *   front radio      radio brought forward, stood by as one in front
 *   front 2          the second of the list brought forward
 *
 * A name that more than one program behind answers to is asked about. */

#include <string>
#include <vector>

#include "asking.h"
#include "builtins.h"
#include "hub.h"
#include "launch.h"
#include "say.h"
#include "windows.h"
#include "words.h"

namespace hub {

namespace {

// "radio", "edit — wants the keyboard": a program behind as the list says it.
std::string Told(const BehindNow &One)
{
    return One.Wants ? One.Who + " — wants the keyboard" : One.Who;
}

// The number typed, 1 to Count, or 0 when the word is not one.
std::size_t Numbered(std::string_view Word, std::size_t Count)
{
    std::size_t Value = 0;
    for (char C : Word) {
        if (C < '0' || C > '9' || Value > Count) return 0;
        Value = Value * 10 + static_cast<std::size_t>(C - '0');
    }
    return Value <= Count ? Value : 0;
}

}  // namespace

bool Front(Hub &Self, const Words &Typed)
{
    const std::vector<BehindNow> Living = Self.Started.Living();
    if (Typed.Count() < 2) {
        if (Living.empty()) {
            Say("front: nothing lives behind\n");
            return true;
        }
        if (Self.Shown.ChooseInWindow) {
            std::vector<std::string> Choices;
            for (const BehindNow &One : Living) Choices.push_back(Told(One));
            std::size_t Chosen = 0;
            if (const std::optional<asking::Answer> Asked = windows::Choose("living behind", Choices, Chosen))
                return *Asked == asking::Answer::Given ? launch::Forward(Self, Living[Chosen]) : true;
        }
        Say("living behind:\n");
        for (std::size_t I = 0; I < Living.size(); I++) Say("  {}  {}\n", I + 1, Told(Living[I]));
        return true;
    }

    const std::string_view Word = Typed[1];
    if (const std::size_t Number = Numbered(Word, Living.size()); Number != 0)
        return launch::Forward(Self, Living[Number - 1]);

    std::vector<BehindNow> Named;
    for (const BehindNow &One : Living)
        if (One.Who == Word) Named.push_back(One);
    if (Named.empty()) {
        Say("front: nothing called {} lives behind\n", Word);
        return false;
    }
    if (Named.size() == 1) return launch::Forward(Self, Named.front());

    std::vector<std::string> Choices;
    for (const BehindNow &One : Named) Choices.push_back(Told(One));
    std::size_t          Chosen = 0;
    const asking::Answer Asked =
        asking::Choose(Self.Shown, std::string(Word) + " lives behind more than once:", Choices, Chosen);
    if (Asked != asking::Answer::Given) {
        if (Asked != asking::Answer::Ended) Say("front: no answer given, so {} stays behind\n", Word);
        return false;
    }
    return launch::Forward(Self, Named[Chosen]);
}

}  // namespace hub
