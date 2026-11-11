/* look.cpp — hangman's settings, declared and read. */

#include "look.h"

#include "box/cxx/settings.h"
#include "box/settings.h"

BOX_SETTINGS("words \"which words come up\"\n"
             "    category one-of random animals food countries science nature sports home jobs music space default random "
             "\"the category words are drawn from; random draws from all of them\"\n"
             "    difficulty one-of any easy medium hard default any "
             "\"how hard the words are: easy is short common letters, hard is long or has the rare ones (j q x z k v)\"\n"
             "hints number from 0 to 3 hints default 2 \"how many hints a word allows; 0 turns them off\"\n"
             "hint-cost one-of points mistake default points "
             "\"what a hint costs: 25 points off the word's score, or one of the six mistakes (never the last)\"\n"
             "alphabet one-of keyboard abc default keyboard \"the letters shown in the order of a keyboard, or a to z\"\n"
             "colors \"the colors of the scene and the words\"\n"
             "    sky color default #4A90D9 \"the sky at the top; the sunset and the joy of a win are mixed from it\"\n"
             "    grass color default #4E9A4A \"the hill\"\n"
             "    wood color default #8A5A2E \"the gallows' timber and the tree's trunk\"\n"
             "    figure color default #2B2B3A \"the man\"\n"
             "    sun color default #FFD84A \"the sun\"\n"
             "    bars color default #14202E \"the lines at the top and the bottom\"\n"
             "    title color default #F2C14E \"titles and the letters given by a hint\"\n"
             "    text color default #D6E2EE \"what is said\"\n"
             "    quiet color default #7E92A6 \"what is said quietly\"\n"
             "    good color default #7BE08F \"a letter that is in the word, a win\"\n"
             "    bad color default #FF6B6B \"a letter that is not, a loss\"\n"
             "keys \"the keys of hangman; letters are the guesses, esc leaves and enter goes on to the next word\"\n"
             "    hint key default ctrl+t \"take a hint: a letter of the word is given\"\n"
             "    fresh key default ctrl+n \"give this word up and take another\"\n"
             "    friend key default ctrl+f \"a friend types a secret word, hidden, and the other guesses it\"\n"
             "    help key default ctrl+? \"every key of hangman\"\n");

namespace hangman {

namespace {

std::optional<std::size_t> CategoryOf(const std::string &Word)
{
    const std::size_t At = CategoryNamed(Word);
    if (At == Categories().size()) return std::nullopt;
    return At;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.Only         = CategoryOf(settings::choice("words category")),
                .Hardness     = LevelNamed(settings::choice("words difficulty")),
                .HintsAllowed = static_cast<std::uint32_t>(settings::number("hints")),
                .Price        = settings::choice("hint-cost") == "mistake" ? HintCost::Mistake : HintCost::Points,
                .Qwerty       = settings::choice("alphabet") == "keyboard",
                .Sky          = Color::FromRaw(settings::color("colors sky")),
                .Grass        = Color::FromRaw(settings::color("colors grass")),
                .Wood         = Color::FromRaw(settings::color("colors wood")),
                .Figure       = Color::FromRaw(settings::color("colors figure")),
                .Sun          = Color::FromRaw(settings::color("colors sun")),
                .Bars         = Color::FromRaw(settings::color("colors bars")),
                .Title        = Color::FromRaw(settings::color("colors title")),
                .Text         = Color::FromRaw(settings::color("colors text")),
                .Quiet        = Color::FromRaw(settings::color("colors quiet")),
                .Good         = Color::FromRaw(settings::color("colors good")),
                .Bad          = Color::FromRaw(settings::color("colors bad")),
                .Keys         = {settings::key("keys hint"), settings::key("keys fresh"), settings::key("keys friend"),
                                 settings::key("keys help")}};
}

}  // namespace hangman
