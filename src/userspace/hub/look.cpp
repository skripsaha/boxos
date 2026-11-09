/* look.cpp — what can be set in the hub, and how it is set now. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("setting greeting yes-or-no default yes \"the one line said when the hub starts\"\n"
             "setting sign word default ~ \"what stands before what you type\"\n"
             "setting context yes-or-no default yes \"whether the Use Context stands before the sign, in brackets\"\n"
             "setting them yes-or-no default yes "
             "\"whether the number of files them holds stands before the sign\"\n"
             "setting sign-color color default #AAAAAA \"the colour of the sign\"\n"
             "setting context-color color default #40C0C0 \"the colour of the Use Context\"\n"
             "setting them-color color default #40C040 \"the colour of the number of files them holds\"\n"
             "setting background one-of prompt line nowhere default prompt "
             "\"where the hub shows the programs living behind: how many, before the sign; their names, on a "
             "line at the top of the screen; or nowhere\"\n"
             "setting background-color color default #E0C040 \"the colour of how many programs live behind\"\n"
             "setting news-color color default #FFB040 "
             "\"the colour of what the hub says of a program that ended, and how long a command took\"\n"
             "setting refusal-color color default #E04040 \"the colour of what could not be done, and why\"\n"
             "setting ended yes-or-no default yes \"say how a program ended when it did not end well\"\n"
             "setting finished yes-or-no default yes "
             "\"say when a program living behind finishes, and how many files it gave\"\n"
             "setting long-command number from 0 to 86400 seconds default 10 "
             "\"say how long a command took when it took this long or longer; 0 for never\"\n"
             "setting whisper yes-or-no default yes \"lay what the line could go on to be, dim, past the caret\"\n"
             "setting needs yes-or-no default yes \"Tab with nothing laid says what the command still needs\"\n"
             "setting did-you-mean yes-or-no default yes \"say the names close to a command that is not there\"\n"
             "setting history yes-or-no default yes \"keep every line typed in the file history\"\n"
             "setting recall one-of here-first everywhere default here-first "
             "\"which lines Up brings back first: those typed in this Use Context, or all of them\"\n"
             "setting choose one-of window list default window "
             "\"how the hub asks which one is meant — of the files a name could mean, or of the programs "
             "living behind: in a window, or as a numbered list\"\n");

namespace hub {

namespace {

BehindShown BehindAt(std::string_view Where)
{
    return Where == "line" ? BehindShown::Line : Where == "nowhere" ? BehindShown::Nowhere : BehindShown::Prompt;
}

}  // namespace

Look ReadLook()
{
    return Look{.Greeting        = box::settings::yes("greeting"),
                .Sign            = box::settings::word("sign"),
                .Context         = box::settings::yes("context"),
                .Them            = box::settings::yes("them"),
                .SignColor       = box::settings::color("sign-color"),
                .ContextColor    = box::settings::color("context-color"),
                .ThemColor       = box::settings::color("them-color"),
                .Behind          = BehindAt(box::settings::choice("background")),
                .BehindColor     = box::settings::color("background-color"),
                .NewsColor       = box::settings::color("news-color"),
                .RefusalColor    = box::settings::color("refusal-color"),
                .Ended           = box::settings::yes("ended"),
                .Finished        = box::settings::yes("finished"),
                .LongCommand     = box::settings::number("long-command"),
                .Whisper         = box::settings::yes("whisper"),
                .Needs           = box::settings::yes("needs"),
                .DidYouMean      = box::settings::yes("did-you-mean"),
                .History         = box::settings::yes("history"),
                .RecallHereFirst = box::settings::choice("recall") == "here-first",
                .ChooseInWindow  = box::settings::choice("choose") == "window"};
}

}  // namespace hub
