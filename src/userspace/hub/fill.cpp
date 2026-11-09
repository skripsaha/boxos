/* fill.cpp — fill a Form and answer its questions. */

#include "fill.h"

#include "box/error.h"
#include "box/file.h"

#include "asking.h"
#include "hub.h"
#include "say.h"

namespace hub::fill {

namespace {

// The command was stopped and the person told why.
constexpr int Stopped = 1;

// The answer to a field left empty: the field's question, and why it is asked
// again when the last words named no file.
asking::Answer AnswerField(BoxFormFiller *Filler, const BoxForm &Declared, std::string_view Who,
                           const BoxFormQuestion &Question, int &Rc)
{
    if (Question.Word) Say("{}: {}\n", Who, Question.Word);
    Say("{}: {}?\n", Who, Declared.Fields[Question.Field].What);
    std::string          Typed;
    const asking::Answer Asked = asking::Line("> ", Typed);
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
    const int   Rc   = form_fill_arrow(Filler, &Screen, &Make);
    if (Rc < 0 || !Make) return Rc;
    const int64_t Made = create(Make, nullptr);
    if (Made <= 0) {
        Say("{}: cannot make the file after -> ({})\n", Who, Reason(Made < 0 ? static_cast<int>(Made) : -ERR_UNKNOWN));
        return Stopped;
    }
    Screen = static_cast<file_id_t>(Made);
    return 0;
}

// The filler, done, made into what the program starts with: the arrow's file
// made when it names one not there yet, word 0 the name the person used, the
// Souvenir asked for when it is kept — the Form gives things and the arrow did
// not make the screen a file — and the Luggage. Nothing when it cannot be,
// and why was said.
std::optional<Filled> Finish(BoxFormFiller *Filler, const BoxForm &Declared, file_id_t Program, std::string_view Who)
{
    Filled What;
    What.Program = Program;
    int Rc       = ArrowScreen(Filler, Who, What.Screen);
    if (Rc == 0) Rc = form_fill_program(Filler, std::string(Who).c_str());
    What.Souvenir = What.Screen == 0 && Declared.Gives == BOX_FORM_GIVES_THINGS;
    if (Rc == 0 && What.Souvenir) Rc = form_fill_souvenir(Filler);

    char  *Bytes  = nullptr;
    size_t Length = 0;
    if (Rc == 0) Rc = form_fill_luggage(Filler, What.Screen, &Bytes, &Length);
    const HeldHeap<char> Luggage(Bytes);
    if (Rc < 0) Say("{}: could not be started ({})\n", Who, Reason(Rc));
    if (Rc != 0) return std::nullopt;
    What.Luggage.assign(Bytes, Length);
    return What;
}

// The program's Form, read from its file: true, or false and said why.
bool ReadForm(file_id_t Program, std::string_view Who, HeldForm &Declared)
{
    const int Rc = form_of_file(Program, Declared.Take());
    if (Rc == 0) return true;
    Declared.Drop();
    // The program's own file cannot be read — its medium gone, most often: it
    // is there and would not start, which is what the person is told when the
    // start itself is refused.
    Say("{}: is there and would not start ({})\n", Who, Reason(Rc));
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
    if (Rc < 0) Say("{}: could not be started ({})\n", Who, Reason(Rc));
    if (Rc != 0) Filler.reset();
    return Filler;
}

std::optional<Filled> FromLine(const Hub &Self, file_id_t Program, std::string_view Who, std::string_view Line)
{
    HeldForm Declared;
    if (!ReadForm(Program, Who, Declared)) return std::nullopt;
    BoxFormLookup View;
    Lookup(Self, View);
    const HeldFiller Filler = Form(*Declared, View, Who, Line);
    if (!Filler) return std::nullopt;
    return Finish(Filler.get(), *Declared, Program, Who);
}

std::optional<Filled> OnThing(file_id_t Program, std::string_view Who, file_id_t Thing)
{
    HeldForm Declared;
    if (!ReadForm(Program, Who, Declared)) return std::nullopt;

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
    const HeldFiller Filler(Begun);
    if (Rc == 0) Rc = form_fill_step(Filler.get(), &Question);
    if (Rc < 0) {
        Say("{}: could not be started ({})\n", Who, Reason(Rc));
        return std::nullopt;
    }
    if (Question.Step == BOX_FORM_REFUSED) {
        Say("{}: {}\n", Who, Question.Word);
        return std::nullopt;
    }
    if (Question.Step != BOX_FORM_DONE) {
        Say("{}: it would have to ask what it takes, and no one is asked here\n", Who);
        return std::nullopt;
    }
    return Finish(Filler.get(), *Declared, Program, Who);
}

}  // namespace hub::fill
