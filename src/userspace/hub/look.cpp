/* look.cpp — what can be set in the hub, and how it is set now. */

#include "look.h"

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("greeting on-or-off default on \"the line the hub says when it starts\"\n"
             "text color default light-gray \"the color of text no program chose a color for, on the whole screen\"\n"
             "page color default black \"the color of the whole screen behind the text\"\n"
             "sign word default ~ \"what stands before what you type\"\n"
             "    color color default light-gray \"its color\"\n"
             "context on-or-off default on \"the Use Context in brackets before the sign\"\n"
             "    color color default cyan \"its color\"\n"
             "results on-or-off default on "
             "\"how many files the last command gave — them — before the sign\"\n"
             "    color color default green \"its color\"\n"
             "behind one-of before-sign top-line off default before-sign "
             "\"the programs living on their own behind the hub: how many, before the sign; their names, on a line "
             "at the top of the screen; or off\"\n"
             "    color color default yellow \"its color\"\n"
             "    order one-of newest-first oldest-first last-shown default newest-first "
             "\"which of them the key next brings first: the one gone behind last, the one gone behind first, or the "
             "one on the screen last\"\n"
             "    context-first on-or-off default on "
             "\"those gone behind in the Use Context you are in come first\"\n"
             "keys \"the keys the hub hears over every program\"\n"
             "    stop key default ctrl+alt+q "
             "\"end the program on the screen: pressed once it is asked to finish, twice it ends\"\n"
             "    let-go key default ctrl+alt+b "
             "\"let the program on the screen live on its own behind the hub, and come back to the hub\"\n"
             "    next key default ctrl+alt+v "
             "\"bring those behind to the screen one by one, and after the last the hub again\"\n"
             "messages \"what the hub says of programs and commands\"\n"
             "    color color default amber \"their color\"\n"
             "    crashes on-or-off default on \"say how a program ended when it did not end well\"\n"
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
             "\"how the hub asks which one is meant of the files a name could mean: in a window, or as a numbered "
             "list\"\n");

namespace hub {

namespace {

BehindShown BehindAt(std::string_view Where)
{
    return Where == "top-line" ? BehindShown::Line : Where == "off" ? BehindShown::Nowhere : BehindShown::Prompt;
}

BehindOrder OrderOf(std::string_view Order)
{
    return Order == "oldest-first" ? BehindOrder::OldestFirst
         : Order == "last-shown"   ? BehindOrder::LastShown
                                   : BehindOrder::NewestFirst;
}

}  // namespace

Look ReadLook()
{
    return Look{.Greeting        = box::settings::on("greeting"),
                .Text            = box::settings::color("text"),
                .Page            = box::settings::color("page"),
                .Sign            = box::settings::word("sign"),
                .Context         = box::settings::on("context"),
                .Them            = box::settings::on("results"),
                .SignColor       = box::settings::color("sign color"),
                .ContextColor    = box::settings::color("context color"),
                .ThemColor       = box::settings::color("results color"),
                .Behind          = BehindAt(box::settings::choice("behind")),
                .BehindColor     = box::settings::color("behind color"),
                .Order           = OrderOf(box::settings::choice("behind order")),
                .BehindContextFirst = box::settings::on("behind context-first"),
                .Keys            = {box::settings::key("keys stop"), box::settings::key("keys let-go"),
                                    box::settings::key("keys next")},
                .NewsColor       = box::settings::color("messages color"),
                .RefusalColor    = box::settings::color("errors"),
                .Ended           = box::settings::on("messages crashes"),
                .LongCommand     = box::settings::number("messages timing"),
                .Whisper         = box::settings::on("suggestions"),
                .Needs           = box::settings::on("hints"),
                .DidYouMean      = box::settings::on("did-you-mean"),
                .History         = box::settings::on("history"),
                .RecallHereFirst = box::settings::choice("history up") == "context-first",
                .ChooseInWindow  = box::settings::choice("choices") == "window"};
}

}  // namespace hub
