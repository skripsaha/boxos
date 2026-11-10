/* look.cpp — what can be set in the hub, and how it is set now. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("greeting on-or-off default on \"the line the hub says when it starts\"\n"
             "sign word default ~ \"what stands before what you type\"\n"
             "    color color default light-gray \"its color\"\n"
             "context on-or-off default on \"the Use Context in brackets before the sign\"\n"
             "    color color default cyan \"its color\"\n"
             "results on-or-off default on "
             "\"how many files the last command gave — them — before the sign\"\n"
             "    color color default green \"its color\"\n"
             "behind one-of before-sign top-line off default before-sign "
             "\"the programs living behind: how many, before the sign; their names, on a line at the top of the "
             "screen; or off\"\n"
             "    color color default yellow \"its color\"\n"
             "messages \"what the hub says of programs and commands\"\n"
             "    color color default amber \"their color\"\n"
             "    crashes on-or-off default on \"say how a program ended when it did not end well\"\n"
             "    finished on-or-off default on "
             "\"say when a program living behind finishes, and how many files it gave\"\n"
             "    timing number from 1 to 86400 seconds or off default 10 "
             "\"say how long a command took when it took this long or longer\"\n"
             "errors color default red \"the color of what could not be done, and why\"\n"
             "suggestions on-or-off default on \"lay what the line could go on to be, dim, past the cursor\"\n"
             "hints on-or-off default on \"Tab with nothing suggested says what the command still needs\"\n"
             "did-you-mean on-or-off default on \"say the names close to a command that is not there\"\n"
             "history on-or-off default on \"keep every line typed in the file history\"\n"
             "    up one-of context-first all default context-first "
             "\"which lines Up brings back first: those typed in this Use Context, or all of them\"\n"
             "choices one-of window list default window "
             "\"how the hub asks which one is meant — of the files a name could mean, or of the programs "
             "living behind: in a window, or as a numbered list\"\n");

namespace hub {

namespace {

BehindShown BehindAt(std::string_view Where)
{
    return Where == "top-line" ? BehindShown::Line : Where == "off" ? BehindShown::Nowhere : BehindShown::Prompt;
}

}  // namespace

Look ReadLook()
{
    return Look{.Greeting        = box::settings::on("greeting"),
                .Sign            = box::settings::word("sign"),
                .Context         = box::settings::on("context"),
                .Them            = box::settings::on("results"),
                .SignColor       = box::settings::color("sign color"),
                .ContextColor    = box::settings::color("context color"),
                .ThemColor       = box::settings::color("results color"),
                .Behind          = BehindAt(box::settings::choice("behind")),
                .BehindColor     = box::settings::color("behind color"),
                .NewsColor       = box::settings::color("messages color"),
                .RefusalColor    = box::settings::color("errors"),
                .Ended           = box::settings::on("messages crashes"),
                .Finished        = box::settings::on("messages finished"),
                .LongCommand     = box::settings::number("messages timing"),
                .Whisper         = box::settings::on("suggestions"),
                .Needs           = box::settings::on("hints"),
                .DidYouMean      = box::settings::on("did-you-mean"),
                .History         = box::settings::on("history"),
                .RecallHereFirst = box::settings::choice("history up") == "context-first",
                .ChooseInWindow  = box::settings::choice("choices") == "window"};
}

}  // namespace hub
