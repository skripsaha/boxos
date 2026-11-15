/* itinerary.cpp — an Itinerary read, its Form filled, its steps run. */

#include "itinerary.h"

#include <string>

#include "box/error.h"

#include "fill.h"
#include "hub.h"
#include "line.h"
#include "say.h"
#include "text.h"
#include "volume.h"

namespace hub {

void Trips::Show(BoxFormLookup &Lookup) const noexcept
{
    Lookup.Given      = Running.empty() ? nullptr : Running.back().Given.data();
    Lookup.GivenCount = Running.empty() ? 0 : Running.back().Given.size();
}

bool Trips::Runs(file_id_t File) const noexcept
{
    for (const Trip &One : Running)
        if (One.File == File) return true;
    return false;
}

namespace itinerary {

namespace {

// The file read and cut: its Form, as form_parse reads one, and its steps.
struct Text {
    std::string              FormText = "form\n";
    std::vector<std::string> Steps;
};

// The line's first word is takes or gives: a line of the Form.
bool FormLine(std::string_view Line)
{
    Line = text::Trimmed(Line);
    std::size_t Length = 0;
    while (Length < Line.size() && !text::Blank(Line[Length])) Length++;
    const std::string_view First = Line.substr(0, Length);
    return First == "takes" || First == "gives";
}

// Its bytes, whole: 0, or a negative -error_t — -ERR_INVALID_ARGUMENT for a
// NUL among them, so no text.
int ReadWhole(file_id_t File, std::string &Out)
{
    std::expected<std::string, int> Read = volume::ReadAll(File);
    if (!Read) return Read.error();
    Out = std::move(*Read);
    return Out.find('\0') == std::string::npos ? 0 : -ERR_INVALID_ARGUMENT;
}

// The lines cut where they end, the header line the file begins with left
// out; the leading takes and gives lines (blank lines among them passed over)
// are the Form, every line after them a step.
void Split(std::string_view Whole, Text &Out)
{
    bool InForm = true;
    const std::size_t HeaderEnd = Whole.find('\n');
    Whole.remove_prefix(HeaderEnd == std::string_view::npos ? Whole.size() : HeaderEnd + 1);
    while (!Whole.empty()) {
        const std::size_t End  = Whole.find('\n');
        std::string_view  Line = Whole.substr(0, End);
        Whole.remove_prefix(End == std::string_view::npos ? Whole.size() : End + 1);
        if (!Line.empty() && Line.back() == '\r') Line.remove_suffix(1);
        if (text::Trimmed(Line).empty()) continue;
        if (InForm && FormLine(Line)) {
            Out.FormText.append(Line).append(1, '\n');
        } else {
            InForm = false;
            Out.Steps.emplace_back(Line);
        }
    }
}

// The file read and cut into its Form and its steps; the Form parsed.
// 0, or a negative -error_t.
int Load(file_id_t File, Text &Out, HeldForm &Declared, bool &Unreadable)
{
    std::string Whole;
    int         Rc = ReadWhole(File, Whole);
    Unreadable     = Rc == -ERR_INVALID_ARGUMENT;
    if (Rc != 0) return Rc;
    Split(Whole, Out);
    Rc = form_parse(Out.FormText.data(), Out.FormText.size(), Declared.Take());
    if (Rc != 0) Declared.Drop();
    return Rc;
}

bool TakesThings(std::uint8_t Kind)
{
    return Kind == BOX_FORM_THING || Kind == BOX_FORM_THINGS;
}

// What each named field was given; the arrays are the filler's.
bool GivenOf(const BoxForm &Declared, BoxFormFiller *Filler, std::vector<BoxFormGiven> &Given)
{
    for (std::uint32_t I = 0; I < Declared.FieldCount; I++) {
        if (!Declared.Fields[I].Name) continue;
        BoxFormGiven One{};
        if (form_fill_field(Filler, I, &One.Ids, &One.IdCount, &One.Words, &One.WordCount) != 0) return false;
        One.Name   = Declared.Fields[I].Name;
        One.Things = TakesThings(Declared.Fields[I].Kind);
        Given.push_back(One);
    }
    return true;
}

// `them` as the journey begins: the files of the first things field, or
// nothing given when there is none.
bool BeginThem(Hub &Self, const BoxForm &Declared, BoxFormFiller *Filler)
{
    for (std::uint32_t I = 0; I < Declared.FieldCount; I++) {
        if (!TakesThings(Declared.Fields[I].Kind)) continue;
        const file_id_t   *Ids;
        const char *const *Words;
        size_t             IdCount, WordCount;
        if (form_fill_field(Filler, I, &Ids, &IdCount, &Words, &WordCount) != 0) return false;
        Self.Given.Keep(std::vector<file_id_t>(Ids, Ids + IdCount));
        return true;
    }
    return true;
}

// The steps, one after another, until one does not go through or `exit` is said.
bool Steps(Hub &Self, const Text &Read, std::string_view Who)
{
    for (std::size_t I = 0; I < Read.Steps.size(); I++) {
        if (!RunLine(Self, Read.Steps[I])) {
            Say("{}: step {} ({}) did not go through; the rest is not done\n", Who, I + 1, Read.Steps[I].c_str());
            return false;
        }
        if (!Self.Going) break;
    }
    return true;
}

bool Travel(Hub &Self, const Text &Read, const BoxForm &Declared, BoxFormFiller *Filler, file_id_t File,
            std::string_view Who)
{
    Them::Saved               Aside = Self.Given.Save();
    std::vector<BoxFormGiven> Given;
    if (!GivenOf(Declared, Filler, Given) || !BeginThem(Self, Declared, Filler)) {
        Self.Given.Restore(std::move(Aside));
        Say("{}: no memory to begin it\n", Who);
        return false;
    }

    Self.Travelling.Begin(Trips::Trip{File, std::move(Given)});
    const bool Went = Steps(Self, Read, Who);
    Self.Travelling.End();

    // What the steps gave is the Itinerary's own gift only when it gives things.
    if (!Went || Declared.Gives != BOX_FORM_GIVES_THINGS) Self.Given.Restore(std::move(Aside));
    return Went;
}

// An arrow on the Itinerary itself goes into its into field; with none, its
// steps' screens are many and there is no one screen to send.
bool ArrowFits(BoxFormFiller *Filler, std::string_view Who)
{
    file_id_t   Screen = 0;
    const char *Create = nullptr;
    if (form_fill_arrow(Filler, &Screen, &Create) != 0) return false;
    if (Screen == 0 && !Create) return true;
    Say("{}: -> has no place here: it takes no into, and its steps print one after another\n", Who);
    return false;
}

}  // namespace

int FormOf(file_id_t File, HeldForm &Out)
{
    Text Read;
    bool Unreadable = false;
    return Load(File, Read, Out, Unreadable);
}

bool Run(Hub &Self, file_id_t File, std::string_view Who, std::string_view Line)
{
    if (Self.Travelling.Runs(File)) {
        Say("{}: it would run itself again, and with nothing to stop it, it would never end; not started\n", Who);
        return false;
    }

    Text     Read;
    HeldForm Declared;
    bool     Unreadable = false;
    int      Rc = Load(File, Read, Declared, Unreadable);
    if (Rc != 0) {
        if (Unreadable) Say("{}: it is no text: there is a NUL in it\n", Who);
        else if (Rc == -ERR_INVALID_ARGUMENT)
            Say("{}: its takes and gives lines are no Form that can be filled\n", Who);
        else Say("{}: it cannot be read ({})\n", Who, Reason(Rc));
        return false;
    }

    BoxFormLookup View;
    fill::Lookup(Self, View);
    HeldFiller Filler = fill::Form(Self.Shown, *Declared, View, Who, Line);
    if (!Filler || !ArrowFits(Filler.get(), Who)) return false;
    return Travel(Self, Read, *Declared, Filler.get(), File, Who);
}

}  // namespace itinerary
}  // namespace hub
