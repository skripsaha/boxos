/* whisper.cpp — the line read as it is typed, and what it could go on to be. */

#include "whisper.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "box/form.h"
#include "box/memory.h"

#include "builtins.h"
#include "hub.h"
#include "itinerary.h"
#include "programs.h"
#include "tags.h"
#include "text.h"

namespace hub {

namespace {

// A heap string for the editor, which frees it; NULL for nothing.
char *Handed(const std::string &Text)
{
    if (Text.empty()) return nullptr;
    char *Copy = static_cast<char *>(malloc(Text.size() + 1));
    if (Copy) std::memcpy(Copy, Text.c_str(), Text.size() + 1);
    return Copy;
}

// Name can follow the typed bytes in a word: it begins with them, goes on
// past them, and holds only what the word can — no blank, comma or quote
// outside quotes, no quote inside, nothing the editor could not type.
bool Fits(std::string_view Name, std::string_view Typed, bool Quoted)
{
    if (Name.size() <= Typed.size() || Name.substr(0, Typed.size()) != Typed) return false;
    for (unsigned char C : Name) {
        if (C < 0x20 || C >= 0x7F || C == '"') return false;
        if (!Quoted && (C == ' ' || C == ',')) return false;
    }
    return true;
}

// A filler's lookup that answers from what the line can see.
struct Sight {
    BoxFormLookup Lookup;   // first: what the filler is handed
    View         *Seen;
    bool          Bounded;

    static int Find(BoxFormLookup *Lookup, const char *Name, bool Everywhere, file_id_t **Ids)
    {
        return reinterpret_cast<Sight *>(Lookup)->Seen->Find(Name, Everywhere, Ids);
    }
    static int All(BoxFormLookup *Lookup, bool Everywhere, file_id_t **Ids, bool *Bounded)
    {
        Sight *Self = reinterpret_cast<Sight *>(Lookup);
        *Bounded    = !Everywhere && Self->Bounded;
        return Self->Seen->All(Everywhere, Ids);
    }
};

}  // namespace

Whisper::Whisper(Hub &SelfValue, std::string ContextValue)
    : Hooked{{ContinueHook, NeedsHook, RecallHook}, this}, Self(SelfValue), Context(std::move(ContextValue)),
      Seen(Context)
{
}

// ---- the editor's calls ---------------------------------------------------------

// The history as it is now: what another hub added since is read, and what
// was drawn from the history before is drawn again.
void Whisper::Fresh()
{
    Self.Lines.Catch();
    if (Self.Lines.Count() == SeenLines) return;
    SeenLines = Self.Lines.Count();
    Words.clear();
    WordsRead = false;
    Walk      = Self.Lines.Walk(Context, Self.Shown.RecallHereFirst);
}

// Nothing may be thrown back through the editor, which is C: a call that
// cannot be answered — memory out — answers nothing.
char *Whisper::ContinueHook(LineWhisper *Line, const char *Typed, std::uint32_t Length)
{
    try {
        Whisper *Owner = reinterpret_cast<Hooking *>(Line)->Owner;
        Owner->Fresh();
        return Handed(Owner->Continue(std::string_view(Typed, Length)));
    } catch (...) {
        return nullptr;
    }
}

char *Whisper::NeedsHook(LineWhisper *Line, const char *Typed, std::uint32_t Length)
{
    try {
        Whisper *Owner = reinterpret_cast<Hooking *>(Line)->Owner;
        Owner->Fresh();
        return Handed(Owner->Needs(std::string_view(Typed, Length)));
    } catch (...) {
        return nullptr;
    }
}

char *Whisper::RecallHook(LineWhisper *Line, std::uint32_t Back)
{
    try {
        Whisper *Owner = reinterpret_cast<Hooking *>(Line)->Owner;
        Owner->Fresh();
        return Handed(Owner->Recall(Back));
    } catch (...) {
        return nullptr;
    }
}

std::string Whisper::Recall(std::uint32_t Back)
{
    if (Back >= Walk.size()) return {};
    return Self.Lines.Before(Walk[Back]).Text;
}

// ---- the command's Form, sorted over what the line can see ---------------------------

// The Form of the program the name means, read once for the line.
bool Whisper::HoldForm(std::string_view Name)
{
    if (FormAsked && FormOf == Name) return Form.Holds();
    FormAsked = true;
    FormOf.assign(Name);
    Form.Let();
    const file_id_t Program = Seen.Program(Name);
    if (Program == 0) return false;
    if (programs::IsItinerary(Program)) (void)itinerary::FormOf(Program, Form);
    else if (form_of_file(Program, Form.Take()) != 0) Form.Drop();
    return Form.Holds();
}

// The words sorted into the held Form as Enter will sort them, over what the
// line can see and asking nobody: every field made optional, so an empty one
// stays empty rather than being asked for; a name that means several files
// taken as the first, `all` taken as it is. Filled[i]: field i got something.
// On a refusal *Refusal is its sentence.
Whisper::Sorted Whisper::SortForm(std::string_view WordsTyped, std::vector<bool> &Filled, std::string *Refusal)
{
    BoxForm Loose = *Form;
    for (std::uint32_t I = 0; I < Loose.FieldCount; I++) Loose.Fields[I].Optional = 1;

    Sight Sighted{};
    Sighted.Lookup.Find = Sight::Find;
    Sighted.Lookup.All  = Sight::All;
    Sighted.Seen        = &Seen;
    Sighted.Bounded     = Seen.Bounded();
    Self.Given.Show(Sighted.Lookup);

    const std::string Typed(WordsTyped);
    BoxFormFiller    *Begun = nullptr;
    const int         Rc    = form_fill_begin(&Loose, Typed.c_str(), &Sighted.Lookup, &Begun);
    const HeldFiller  Filler(Begun);
    if (Rc != 0) return Sorted::Failed;

    Sorted End = Sorted::Failed;
    for (;;) {
        BoxFormQuestion Question;
        if (form_fill_step(Filler.get(), &Question) != 0) break;
        if (Question.Step == BOX_FORM_DONE) {
            End = Sorted::Done;
            break;
        }
        if (Question.Step == BOX_FORM_REFUSED) {
            if (Refusal) Refusal->assign(Question.Word);
            End = Sorted::Refused;
            break;
        }
        const int Answered = Question.Step == BOX_FORM_ASK_WHICH  ? form_fill_choose(Filler.get(), Question.Choices[0])
                             : Question.Step == BOX_FORM_ASK_SURE ? form_fill_sure(Filler.get(), true)
                                                                  : -1;
        if (Answered != 0) break;
    }

    Filled = std::vector<bool>(Loose.FieldCount, false);
    for (std::uint32_t I = 0; End == Sorted::Done && I < Loose.FieldCount; I++) {
        const file_id_t   *Ids;
        const char *const *Given;
        size_t             IdCount = 0, WordCount = 0;
        Filled[I] = form_fill_field(Filler.get(), I, &Ids, &IdCount, &Given, &WordCount) == 0 &&
                    IdCount + WordCount > 0;
    }
    return End;
}

// ---- what the word being typed can be ------------------------------------------------

void Whisper::Offer::Take(std::uint8_t Kind) noexcept
{
    if (Kind == BOX_FORM_THING || Kind == BOX_FORM_THINGS) Files = true;
    if (Kind == BOX_FORM_THINGS) Several = true;
    if (Kind == BOX_FORM_WORDS) Tags = true;
}

std::vector<View::Shelf> Whisper::Offer::Shelves() const
{
    std::vector<View::Shelf> Kinds;
    if (Commands) Kinds.push_back(View::Commands);
    if (Programs) Kinds.push_back(View::ProgramFiles);
    if (Files) Kinds.push_back(Everywhere ? View::FilesEverywhere : View::FilesHere);
    if (Tags) Kinds.push_back(Everywhere ? View::TagsEverywhere : View::TagsHere);
    return Kinds;
}

// What a program's Form takes now, the words before this one sorted. A word
// begun may be anything the Form still takes; an empty one only what it
// still needs — its first required field left empty — and there `them` is
// offered when that field takes files.
void Whisper::OfferProgram(const Typing &Typed, std::string_view Name, Offer &Out)
{
    if (!HoldForm(Name) || !Form->Declared) return;
    std::vector<bool> Filled;
    if (SortForm(Typed.Line.substr(Typed.NameEnd, Typed.WordStart - Typed.NameEnd), Filled, nullptr) !=
        Sorted::Done)
        return;

    Out.Everywhere = Typed.Everywhere;
    const BoxForm &Declared = *Form;
    if (Typed.Fresh()) {
        for (std::uint32_t I = 0; I < Declared.FieldCount; I++) {
            if (Declared.Fields[I].Optional || Filled[I]) continue;
            Out.Take(Declared.Fields[I].Kind);
            Out.Them = Out.Files;
            return;
        }
        return;
    }
    for (std::uint32_t I = 0; I < Declared.FieldCount; I++)
        if (Declared.Fields[I].Kind != BOX_FORM_THING || !Filled[I]) Out.Take(Declared.Fields[I].Kind);
}

Whisper::Offer Whisper::MakeOffer(const Typing &Typed)
{
    Offer Out;
    if (Typed.Words == 0) {
        Out.Commands = true;
        return Out;
    }
    if (Typed.AfterArrow) {
        Out.Files      = true;
        Out.Everywhere = Typed.Everywhere;
        return Out;
    }
    const std::string_view Name = Typed.Name();
    const Builtin         *Own  = BuiltinNamed(Name);
    if (!Own) OfferProgram(Typed, Name, Out);
    else if (Own->Kind == Takes::Command) Out.Commands = Typed.Words == 1;
    else if (Own->Kind == Takes::Program) Out.Programs = Out.Them = Typed.Words == 1;
    else if (Own->Kind == Takes::Tags) Out.Tags = Out.Everywhere = Out.Signed = true;
    return Out;
}

// ---- the names it can be --------------------------------------------------------------

// The word is one of the names the offer allows.
bool Whisper::Named(const Offer &Offered, std::string_view Word)
{
    for (View::Shelf Kind : Offered.Shelves())
        if (View::Holds(Seen.ShelfOf(Kind), Word)) return true;
    return false;
}

// Every word of the history, each once and with the newest line it was typed
// in, in byte order: read once for the line, the first time it is wanted.
void Whisper::ReadRecent()
{
    if (WordsRead) return;
    WordsRead = true;
    for (std::size_t Back = 0; Back < Self.Lines.Count(); Back++)
        for (std::string_view Piece : Pieces(Self.Lines.Before(Back).Text)) Words.push_back(Recent{Piece, Back});
    std::stable_sort(Words.begin(), Words.end(), [](const Recent &A, const Recent &B) {
        return A.Word != B.Word ? A.Word < B.Word : A.Back < B.Back;
    });
    Words.erase(std::unique(Words.begin(), Words.end(),
                            [](const Recent &A, const Recent &B) { return A.Word == B.Word; }),
                Words.end());
}

// The name the word can become that was typed most lately.
bool Whisper::Latest(const Offer &Offered, std::string_view Typed, bool Quoted, std::string_view &Out)
{
    ReadRecent();
    const Recent *Best = nullptr;
    auto          At   = std::lower_bound(Words.begin(), Words.end(), Typed,
                                          [](const Recent &Word, std::string_view Key) { return Word.Word < Key; });
    for (; At != Words.end() && At->Word.substr(0, Typed.size()) == Typed; ++At) {
        if (Best && At->Back >= Best->Back) continue;
        if (Fits(At->Word, Typed, Quoted) && Named(Offered, At->Word)) Best = &*At;
    }
    if (!Best) return false;
    Out = Best->Word;
    return true;
}

// Of the names that begin with the word, the shortest, the first in order
// among equals; Kinds how many differ, counted to two.
bool Whisper::Shortest(const Offer &Offered, std::string_view Typed, bool Quoted, std::string_view &Out,
                       std::size_t &Kinds)
{
    bool Found = false;
    Kinds      = 0;
    for (View::Shelf Kind : Offered.Shelves()) {
        const View::Names &Shelf = Seen.ShelfOf(Kind);
        for (std::size_t I = View::From(Shelf, Typed); I < Shelf.size(); I++) {
            const std::string_view Name = Shelf[I];
            if (Name.substr(0, Typed.size()) != Typed) break;
            if (!Fits(Name, Typed, Quoted)) continue;
            const bool SameSize = Found && Out.size() == Name.size();
            if (!SameSize || Name != Out) Kinds++;
            if (!Found || Name.size() < Out.size() || (SameSize && Name < Out)) {
                Out   = Name;
                Found = true;
            }
            // An empty word is whispered only the one name there is: a second
            // ends the search.
            if (Typed.empty() && Kinds > 1) return false;
        }
    }
    return Found;
}

// The name to whisper for the word being typed: the one typed most lately,
// else the shortest. An empty word gets one only from the history, or when it
// is the only name there is; a word that already is a name gets none.
bool Whisper::Choose(const Offer &Offered, std::string_view Typed, bool Quoted, std::string_view &Out)
{
    if (!Typed.empty() && Named(Offered, Typed)) return false;
    if (Latest(Offered, Typed, Quoted, Out)) return true;
    std::size_t Kinds = 0;
    if (!Shortest(Offered, Typed, Quoted, Out, Kinds)) return false;
    return !Typed.empty() || Kinds == 1;
}

// ---- what is whispered ------------------------------------------------------------------

// `them` — `it` when it is one file — on an empty word that can be files.
std::string Whisper::FromThem(const Offer &Offered) const
{
    if (!Offered.Them || !Self.Given.Known() || Self.Given.Files().empty()) return {};
    return Self.Given.Files().size() > 1 && Offered.Several ? "them" : "it";
}

// The rest of the newest line of the history that begins with the line:
// typed in this Use Context first when the person set the hub so.
std::string Whisper::FromHistory(std::string_view Line) const
{
    for (int Pass = Self.Shown.RecallHereFirst ? 0 : 1; Pass < 2; Pass++) {
        for (std::size_t Back = 0; Back < Self.Lines.Count(); Back++) {
            const History::Line &Entry = Self.Lines.Before(Back);
            if (Pass == 0 && !tags::Same(Context, Entry.Context)) continue;
            if (Entry.Text.size() > Line.size() && std::string_view(Entry.Text).substr(0, Line.size()) == Line)
                return Entry.Text.substr(Line.size());
        }
    }
    return {};
}

std::string Whisper::FromNames(const Typing &Typed, const Offer &Offered)
{
    std::string_view Word = Typed.Piece();
    if (Offered.Signed && !Word.empty() && (Word.front() == '+' || Word.front() == '-')) Word.remove_prefix(1);
    std::string_view Chosen;
    if (!Choose(Offered, Word, Typed.Quoted, Chosen)) return {};
    std::string Rest(Chosen.substr(Word.size()));
    if (Typed.Quoted) Rest += '"';
    return Rest;
}

std::string Whisper::Continue(std::string_view Line)
{
    if (!Self.Shown.Whisper) return {};
    // The offer sorts the command's Form: made only when it is needed, as
    // the line read from the history needs none.
    const Typing         Typed = Typing::Of(Line);
    std::optional<Offer> Offered;
    if (Typed.Words > 0 && !Typed.Quoted && Typed.Fresh()) {
        Offered = MakeOffer(Typed);
        if (std::string Them = FromThem(*Offered); !Them.empty()) return Them;
    }
    if (std::string Rest = FromHistory(Line); !Rest.empty()) return Rest;
    if (!Offered) Offered = MakeOffer(Typed);
    return FromNames(Typed, *Offered);
}

// ---- what the command still needs -------------------------------------------------------

// "takes" and the fields the line has not filled: a required one as it is
// said, an optional one in brackets, the arrow's before an `into`.
std::string Whisper::FormNeeds(std::string_view Name, std::string_view WordsTyped)
{
    if (!HoldForm(Name)) return {};
    const BoxForm &Declared = *Form;
    if (!Declared.Declared) return "takes its words as typed";
    if (Declared.FieldCount == 0) return "takes nothing";

    std::vector<bool> Filled;
    std::string       Refusal;
    const Sorted      End = SortForm(WordsTyped, Filled, &Refusal);
    if (End == Sorted::Refused) return Refusal;
    if (End != Sorted::Done) return {};

    std::string Said;
    for (std::uint32_t I = 0; I < Declared.FieldCount; I++) {
        const BoxFormField &Field = Declared.Fields[I];
        if (Filled[I]) continue;
        Said += Said.empty() ? "takes " : ", ";
        if (Field.Optional) Said += '[';
        if (Field.Kind == BOX_FORM_INTO) Said += "-> ";
        Said += Field.What;
        if (Field.Optional) Said += ']';
    }
    return Said;
}

std::string Whisper::Needs(std::string_view Line)
{
    if (!Self.Shown.Needs) return {};
    const Typing Typed = Typing::Of(Line);
    if (Typed.Quoted || (Typed.Words == 0 && Typed.WordStart == Line.size())) return {};

    const std::string_view Name = Typed.Name();
    const Builtin         *Own  = BuiltinNamed(Name);
    std::string            Said;
    if (Own) {
        const std::size_t Blank = Own->Usage.find(' ');
        if (Blank != std::string_view::npos) Said.assign(Own->Usage.substr(Blank + 1));
    } else {
        Said = FormNeeds(Name, Typed.Words == 0 ? std::string_view() : Line.substr(Typed.NameEnd));
    }
    // Laid straight after the last word, it would read as part of it.
    if (Said.empty() || text::Blank(Line.back())) return Said;
    return " " + Said;
}

}  // namespace hub
