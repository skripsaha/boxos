/* fill.cpp — fill a Form, answer its questions, start the program. */

#include "fill.h"

#include <string>
#include <vector>

#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/print.h"

#include "asking.h"
#include "hub.h"
#include "remake.h"
#include "say.h"

namespace hub::fill {

namespace {

// The command was stopped and the person told why.
constexpr int Stopped = 1;

void SayFailed(std::string_view Who, const char *What, int Rc)
{
    Say("{}: {} (error {})\n", Who, What, static_cast<int>(box_errno_of(Rc)));
}

// The answer to a field left empty: the field's question, and why it is asked
// again when the last words named no file.
asking::Answer AnswerField(BoxFormFiller *Filler, const BoxForm &Declared, std::string_view Who,
                           const BoxFormQuestion &Question, int &Rc)
{
    if (Question.Word) Say("{}: {}\n", Who, Question.Word);
    Say("{}: {}?\n", Who, Declared.Fields[Question.Field].What);
    std::string    Typed;
    asking::Answer Asked = asking::Line("> ", Typed);
    if (Asked == asking::Answer::Given) Rc = form_fill_words(Filler, Typed.c_str());
    return Asked;
}

// One question put and its answer given to the filler: 0, Stopped when the
// person did not answer, or a negative -error_t.
int Answer(BoxFormFiller *Filler, const BoxForm &Declared, std::string_view Who, const BoxFormQuestion &Question)
{
    int            Rc = 0;
    asking::Answer Asked;
    if (Question.Step == BOX_FORM_ASK_WHICH) {
        file_id_t Chosen = 0;
        Asked = asking::Which(Question.Word, {Question.Choices, Question.ChoiceCount}, Chosen);
        if (Asked == asking::Answer::Given) Rc = form_fill_choose(Filler, Chosen);
    } else if (Question.Step == BOX_FORM_ASK_FIELD) {
        Asked = AnswerField(Filler, Declared, Who, Question, Rc);
    } else {
        bool Yes = false;
        Asked = asking::Sure(Question.Count, Yes);
        if (Asked == asking::Answer::Given) Rc = form_fill_sure(Filler, Yes);
    }
    if (Asked != asking::Answer::Given) {
        asking::SayCancelled(Who, Asked);
        return Stopped;
    }
    return Rc;
}

// The filler stepped until the line is filled: 0, Stopped (refused or not
// answered, said), or a negative -error_t.
int FillLine(BoxFormFiller *Filler, const BoxForm &Declared, std::string_view Who)
{
    for (;;) {
        BoxFormQuestion Question;
        int             Rc = form_fill_step(Filler, &Question);
        if (Rc < 0) return Rc;
        if (Question.Step == BOX_FORM_DONE) return 0;
        if (Question.Step == BOX_FORM_REFUSED) {
            Say("{}: {}\n", Who, Question.Word);
            return Stopped;
        }
        Rc = Answer(Filler, Declared, Who, Question);
        if (Rc != 0) return Rc;
    }
}

// The file the program's screen goes to: made here when the arrow names a
// file that is not there yet. 0 for none.
int ArrowScreen(BoxFormFiller *Filler, std::string_view Who, file_id_t &Screen)
{
    const char *Make = nullptr;
    int         Rc = form_fill_arrow(Filler, &Screen, &Make);
    if (Rc < 0 || !Make) return Rc;
    int64_t Made = create(Make, nullptr);
    if (Made <= 0) {
        SayFailed(Who, "cannot make the file after ->", static_cast<int>(Made));
        return Stopped;
    }
    Screen = static_cast<file_id_t>(Made);
    return 0;
}

// How a started program ended, with what it gave back that is kept.
struct Launched {
    file_id_t Screen = 0;      // the arrow's file, 0 for none
    bool      Asked  = false;  // its Souvenir was asked for
    Ending    End;
};

// The program started with its Form filled, and stood by until it is gone:
// 0 with Out filled, Stopped (said), or a negative -error_t.
int Launch(Hub &Self, BoxFormFiller *Filler, const BoxForm &Declared, file_id_t Program, std::string_view Who,
           Launched &Out)
{
    const std::string Name(Who);
    int               Rc = ArrowScreen(Filler, Who, Out.Screen);
    if (Rc == 0) Rc = form_fill_program(Filler, Name.c_str());

    // The Souvenir is asked for only when it is kept: the Form gives things
    // and the arrow did not make the screen a file.
    Out.Asked = Out.Screen == 0 && Declared.Gives == BOX_FORM_GIVES_THINGS;
    if (Rc == 0 && Out.Asked) Rc = form_fill_souvenir(Filler);

    char  *Bytes  = nullptr;
    size_t Length = 0;
    if (Rc == 0) Rc = form_fill_luggage(Filler, Out.Screen, &Bytes, &Length);
    if (Rc != 0) return Rc;

    std::expected<Ending, int> Ran = Self.Started.Run(Program, std::string_view(Bytes, Length));
    free(Bytes);
    if (!Ran) {
        SayFailed(Who, "is there and would not start", Ran.error());
        return Stopped;
    }
    Out.End = std::move(*Ran);
    SayEnding(Self, Who, Out.End);
    return 0;
}

// What the command gave becomes `them`: the arrow's file when its screen
// went into one, otherwise the Souvenir the hub asked for.
void KeepGift(Hub &Self, Launched &Done)
{
    if (Done.Screen != 0) Self.Given.Keep({Done.Screen});
    else if (Done.End.Collected && Done.Asked) Self.Given.Keep(std::move(Done.End.Given));
}

// The program's Form, read from its file: true, or false and said why.
bool ReadForm(file_id_t Program, std::string_view Who, HeldForm &Declared)
{
    int Rc = form_of_file(Program, Declared.Take());
    if (Rc == 0) return true;
    Declared.Drop();
    // The program's own file cannot be read — its medium gone, most often: it
    // is there and would not start, which is what the person is told when the
    // start itself is refused.
    SayFailed(Who, "is there and would not start", Rc);
    return false;
}

}  // namespace

void Lookup(const Hub &Self, BoxFormLookup &Out)
{
    form_lookup_volume(&Out);
    Self.Given.Show(Out);
    Self.Travelling.Show(Out);
}

HeldFiller Form(const BoxForm &Declared, BoxFormLookup &Lookup, std::string_view Who, std::string_view Line)
{
    const std::string Typed(Line);
    BoxFormFiller    *Begun = nullptr;
    int               Rc    = form_fill_begin(&Declared, Typed.c_str(), &Lookup, &Begun);
    HeldFiller        Filler(Begun);
    if (Rc == 0) Rc = FillLine(Filler.get(), Declared, Who);
    if (Rc < 0) SayFailed(Who, "could not be started", Rc);
    if (Rc != 0) Filler.reset();
    return Filler;
}

bool AndRun(Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line)
{
    if (!remake::WhenChanged(Self, Program, Who)) return false;

    HeldForm Declared;
    if (!ReadForm(Program, Who, Declared)) return false;

    BoxFormLookup View;
    Lookup(Self, View);
    HeldFiller Filler = Form(*Declared, View, Who, Line);
    if (!Filler) return false;

    Launched Done;
    int      Rc = Launch(Self, Filler.get(), *Declared, Program, Who, Done);
    if (Rc < 0) SayFailed(Who, "could not be started", Rc);
    if (Rc != 0) return false;
    KeepGift(Self, Done);
    return Done.End.Seen && Done.End.Code == 0;
}

bool AndRunOn(Hub &Self, file_id_t Program, std::string_view Who, file_id_t Thing, file_id_t &Given)
{
    Given = 0;
    HeldForm Declared;
    if (!ReadForm(Program, Who, Declared)) return false;

    // `it` is the one file handed over: the lookup knows it and no other
    // `it`, so the person's own `them` is neither read nor changed.
    BoxFormLookup View;
    form_lookup_volume(&View);
    View.Them      = &Thing;
    View.ThemCount = 1;
    View.ThemKnown = true;

    BoxFormFiller  *Begun = nullptr;
    BoxFormQuestion Question;
    int             Rc = form_fill_begin(&*Declared, "it", &View, &Begun);
    HeldFiller      Filler(Begun);
    if (Rc == 0) Rc = form_fill_step(Filler.get(), &Question);
    if (Rc == 0 && Question.Step == BOX_FORM_REFUSED) {
        Say("{}: {}\n", Who, Question.Word);
        Rc = Stopped;
    } else if (Rc == 0 && Question.Step != BOX_FORM_DONE) {
        Say("{}: it would have to ask what it takes, and no one is asked here\n", Who);
        Rc = Stopped;
    }
    Launched Done;
    if (Rc == 0) Rc = Launch(Self, Filler.get(), *Declared, Program, Who, Done);
    if (Rc < 0) SayFailed(Who, "could not be started", Rc);

    const bool EndedWell = Rc == 0 && Done.End.Seen && Done.End.Code == 0;
    if (EndedWell && Done.Asked && Done.End.Collected && !Done.End.Given.empty()) Given = Done.End.Given[0];
    return EndedWell;
}

}  // namespace hub::fill
