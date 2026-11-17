/* layout.cpp — a text laid out in its box: its characters set, broken into
 * lines, the lines measured, cut to the box and placed. See layout.h. */

#include "typecase/layout.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include "characters.h"

namespace typecase {

namespace {

constexpr float    DefaultSize = 16.0f;
constexpr char32_t Bullet = 0x2022, EllipsisSign = 0x2026, HyphenSign = 0x2010;

/* A style with every property said. */
struct Resolved {
    std::string   Family;
    float         Size       = DefaultSize;
    std::uint16_t Weight     = 400;
    bool          Italic     = false;
    float         Spacing    = 0;
    float         LineHeight = 0;   /* none: the font's own line */
    bool          Underline  = false;
    bool          Strike     = false;
    std::uint32_t Ink        = 0;

    auto Key() const noexcept
    {
        return std::tie(Family, Size, Weight, Italic, Spacing, LineHeight, Underline, Strike, Ink);
    }
    bool operator<(const Resolved &Other) const noexcept { return Key() < Other.Key(); }
};

/* What makes glyphs one run: their face, look and ink. */
using RunKey = std::tuple<const Font *, std::uint32_t, bool, bool, std::uint32_t>;

bool Usable(const std::optional<float> &Value) noexcept { return Value && std::isfinite(*Value); }

/* What Says says, over Into: a number past sense is held to it — a size of
 * a 64th of a pixel to 8192, a weight of 1 to 1000. */
void Apply(Resolved &Into, const Style &Says)
{
    if (Says.Family) Into.Family = *Says.Family;
    if (Usable(Says.Size) && *Says.Size > 0) Into.Size = std::clamp(*Says.Size, 1.0f / 64, 8192.0f);
    if (Says.Weight) Into.Weight = std::clamp<std::uint16_t>(*Says.Weight, 1, 1000);
    if (Says.Italic) Into.Italic = *Says.Italic;
    if (Usable(Says.LetterSpacing)) Into.Spacing = std::clamp(*Says.LetterSpacing, -8192.0f, 8192.0f);
    if (Usable(Says.LineHeight) && *Says.LineHeight >= 0) Into.LineHeight = std::min(*Says.LineHeight, 64.0f);
    if (Says.Underline) Into.Underline = *Says.Underline;
    if (Says.Strike) Into.Strike = *Says.Strike;
    if (Says.Ink) Into.Ink = *Says.Ink;
}

/* Whether a character at a line's end hangs past it: drawn nowhere and
 * counted in no width. */
bool Hangs(Kind Is) noexcept
{
    return Is == Kind::Space || Is == Kind::Tab || Is == Kind::LineBreak || Is == Kind::Invisible ||
           Is == Kind::ZeroSpace || Is == Kind::SoftHyphen;
}

/* A character as it is set. */
struct Item {
    char32_t      Shown   = 0;       /* what it is drawn as */
    Kind          Is      = Kind::Letter;
    std::uint32_t Style   = 0;
    std::int32_t  Choice  = -1;      /* of its style's choices; none: drawn as its code, or nothing */
    GlyphId       Glyph   = 0;
    bool          Ink     = false;   /* whether it draws anything */
    float         Advance = 0;       /* how far it moves the pen, its kerning and spacing after it in */
    float         Kern    = 0;       /* of which, kerning with the next */
    float         Spacing = 0;       /* and letter spacing */
    float         Shift   = 0;       /* how far from its pen it is drawn */
    float         Lead    = 0;       /* of which, by kerning with the one before: none at a line's start */
};

/* A line, as it is broken. */
struct Piece {
    std::uint32_t From     = 0;       /* its characters, those hanging at its end and its break too */
    std::uint32_t To       = 0;
    std::uint32_t End      = 0;       /* past its last character that does not hang */
    std::uint32_t Shown    = 0;       /* past the last drawn: End, or where an ellipsis cuts it */
    bool          Hyphen   = false;   /* ends at a soft hyphen, shown */
    bool          Last     = false;   /* its paragraph's last */
    bool          Ellipsis = false;
    float         Height   = 0;
    float         Ascent   = 0;
};

/* A glyph a line ends in that stands for no character of its own: the
 * hyphen of a soft hyphen, an ellipsis. */
struct Sign {
    std::int32_t Choice  = -1;
    GlyphId      Glyph   = 0;
    char32_t     Shown   = 0;
    int          Times   = 0;   /* drawn this many times */
    float        Advance = 0;   /* each */
};

}  // namespace

struct Setter {
    const Text           &Set;
    const TextBox        &In;
    const Fonts          &Book;
    Layout               &Out;
    std::vector<Resolved>                Styles;
    std::map<Resolved, std::uint32_t>    StyleKept;
    std::map<RunKey, std::uint32_t>      RunKept;
    std::vector<std::vector<FaceChoice>> Choices;
    std::uint32_t                        EndStyle = 0;
    std::vector<Item>                    Items;
    std::vector<Piece>                   Pieces;

    std::uint32_t Count() const noexcept { return static_cast<std::uint32_t>(Items.size()); }
    GlyphLook     LookOf(std::uint32_t Style, const FaceChoice &Face) const noexcept
    {
        return GlyphLook{Styles[Style].Size, 0, Face.Bold, Face.Slant};
    }
    const FaceChoice *ChoiceOf(const Item &It) const noexcept
    {
        return It.Choice >= 0 ? &Choices[It.Style][static_cast<std::size_t>(It.Choice)] : nullptr;
    }

    void          ResolveStyles();
    std::uint32_t StyleIndex(const Resolved &Of);
    void          SetCharacters();
    void          KernPairs();
    void          SpaceLetters();
    float         TabAdvance(const Item &It, float Pen) const noexcept;
    Sign          SignFor(std::uint32_t Style, char32_t Wanted, char32_t Instead) const;
    void          BreakParagraph(std::uint32_t From, std::uint32_t To, bool Broken);
    std::uint32_t EndOf(std::uint32_t From, std::uint32_t To) const noexcept;
    std::vector<float> Pens(const Piece &Of) const;
    float         ContentWidth(const Piece &Of, const std::vector<float> &At) const;
    void          Measure(Piece &Of) const;
    void          CutToBox();
    void          EndInEllipsis(Piece &Of);
    void          Place();
    std::uint32_t StyleBefore(std::uint32_t Index, const Piece &Of) const noexcept;
    std::uint32_t RunFor(const FaceChoice *Face, std::uint32_t Style);
    void          Decorate(const Item &It, float X, float Advance, float Baseline);
};

std::uint32_t Setter::StyleIndex(const Resolved &Of)
{
    const auto [At, Made] = StyleKept.try_emplace(Of, static_cast<std::uint32_t>(Styles.size()));
    if (Made) {
        Styles.push_back(Of);
        Choices.push_back(Book.Choices(Of.Family, Of.Weight, Of.Italic));
    }
    return At->second;
}

/* Each character's style: the text's, and over it every span that holds
 * it, in their order — worked stretch by stretch between the spans' ends,
 * each stretch from the spans open over it. */
void Setter::ResolveStyles()
{
    Resolved Base;
    Apply(Base, Set.Style);
    EndStyle = StyleIndex(Base);
    const std::uint32_t N = Count();
    /* Where each span opens and closes, by place; a close before an open. */
    std::vector<std::tuple<std::uint32_t, bool, std::uint32_t>> Ends;
    for (std::uint32_t S = 0; S < Set.Spans.size(); S++) {
        const Span &Each = Set.Spans[S];
        const std::uint32_t From = std::min(Each.From, N), To = std::min(Each.To, N);
        if (From < To) Ends.emplace_back(From, true, S), Ends.emplace_back(To, false, S);
    }
    std::sort(Ends.begin(), Ends.end());
    std::set<std::uint32_t> Open;
    std::uint32_t           At = 0;
    std::size_t             E  = 0;
    while (At < N) {
        while (E < Ends.size() && std::get<0>(Ends[E]) == At) {
            if (std::get<1>(Ends[E])) Open.insert(std::get<2>(Ends[E]));
            else Open.erase(std::get<2>(Ends[E]));
            E++;
        }
        const std::uint32_t Until = E < Ends.size() ? std::get<0>(Ends[E]) : N;
        Resolved            Here  = Base;
        for (std::uint32_t S : Open) Apply(Here, Set.Spans[S].Style);
        const std::uint32_t Index = StyleIndex(Here);
        for (std::uint32_t I = At; I < Until; I++) Items[I].Style = Index;
        At = Until;
    }
    if (N > 0) EndStyle = Items[N - 1].Style;
}

/* Each character's face, glyph and advance: the first face of its style's
 * choices that has it — a space no face has an advance of a quarter of the
 * em, anything else no face has drawn as its code. */
void Setter::SetCharacters()
{
    for (std::uint32_t I = 0; I < Count(); I++) {
        Item &It = Items[I];
        It.Shown = Set.Secret ? Bullet : Out.Said[I];
        It.Is    = Set.Secret ? Kind::Letter : KindOf(It.Shown);
        if (It.Is == Kind::LineBreak || It.Is == Kind::Invisible || It.Is == Kind::ZeroSpace ||
            It.Is == Kind::SoftHyphen)
            continue;
        const Resolved &Of = Styles[It.Style];
        if (It.Is == Kind::Tab) {
            It.Choice = Choices[It.Style].empty() ? -1 : 0;
            continue;
        }
        const auto &All = Choices[It.Style];
        for (std::size_t C = 0; C < All.size() && It.Choice < 0; C++)
            if (Has(*All[C].Face, It.Shown)) It.Choice = static_cast<std::int32_t>(C);
        if (It.Choice >= 0) {
            const FaceChoice &Face = All[static_cast<std::size_t>(It.Choice)];
            It.Glyph               = Face.Face->Find(It.Shown);
            It.Advance             = AdvanceOf(*Face.Face, It.Glyph, LookOf(It.Style, Face));
            It.Ink                 = !Blank(It.Shown);
        } else if (Blank(It.Shown)) {
            It.Advance = Of.Size / 4;
        } else {
            It.Advance = MissingAdvance(It.Shown, Of.Size);
            It.Ink     = true;
        }
    }
}

/* Each pair of glyphs side by side of one face at one look, kerned — in the
 * writing of the first, or of the writing round it when it is common to
 * all. */
void Setter::KernPairs()
{
    const std::uint32_t N = Count();
    std::vector<Script> Writing(N, Script::Common);
    Script              Last = Script::Common;
    for (std::uint32_t I = 0; I < N; I++) {
        const Script Of = ScriptOf(Items[I].Shown);
        if (Of != Script::Common) Last = Of;
        Writing[I] = Last;
    }
    for (std::uint32_t I = N; I-- > 0;) {
        if (Writing[I] != Script::Common) Last = Writing[I];
        else Writing[I] = Last;
    }
    for (std::uint32_t I = 1; I < N; I++) {
        Item             &A = Items[I - 1], &B = Items[I];
        const FaceChoice *FA = ChoiceOf(A), *FB = ChoiceOf(B);
        if (!FA || !FB || A.Is == Kind::Tab || B.Is == Kind::Tab || FA->Face != FB->Face || FA->Bold != FB->Bold ||
            FA->Slant != FB->Slant || Styles[A.Style].Size != Styles[B.Style].Size)
            continue;
        const PairKerning Pair = FA->Face->KerningOf(A.Glyph, B.Glyph, Writing[I - 1]);
        if (Pair == PairKerning{}) continue;
        const float Scale = Styles[A.Style].Size / FA->Face->Metrics().UnitsPerEm;
        A.Kern += static_cast<float>(Pair.FirstAdvance) * Scale;
        A.Advance += static_cast<float>(Pair.FirstAdvance) * Scale;
        A.Shift += static_cast<float>(Pair.FirstShift) * Scale;
        B.Lead = static_cast<float>(Pair.SecondShift) * Scale;
        B.Shift += B.Lead;
        B.Advance += static_cast<float>(Pair.SecondAdvance) * Scale;
    }
}

/* Letter spacing after each character that takes room — after the last
 * mark on it, where marks follow it. */
void Setter::SpaceLetters()
{
    const std::uint32_t N = Count();
    for (std::uint32_t I = 0; I < N; I++) {
        const float Spacing = Styles[Items[I].Style].Spacing;
        const Kind  Is      = Items[I].Is;
        if (Spacing == 0 || Is == Kind::LineBreak || Is == Kind::Invisible || Is == Kind::ZeroSpace ||
            Is == Kind::SoftHyphen || Is == Kind::Mark || Is == Kind::Tab)
            continue;
        std::uint32_t Last = I;
        while (Last + 1 < N && Items[Last + 1].Is == Kind::Mark) Last++;
        Items[Last].Spacing += Spacing;
        Items[Last].Advance += Spacing;
    }
}

/* To the next tab stop: every eight spaces of the tab's style. */
float Setter::TabAdvance(const Item &It, float Pen) const noexcept
{
    float Space = Styles[It.Style].Size / 4;
    if (const FaceChoice *Face = ChoiceOf(It))
        if (const GlyphId Glyph = Face->Face->Find(U' '))
            Space = AdvanceOf(*Face->Face, Glyph, LookOf(It.Style, *Face));
    const float Stop = std::max(8 * Space, 1.0f);
    return (std::floor(Pen / Stop) + 1) * Stop - Pen;
}

/* A sign in a style: Wanted from the first face that has it, else three of
 * Instead, else Wanted as its code. */
Sign Setter::SignFor(std::uint32_t Style, char32_t Wanted, char32_t Instead) const
{
    const auto &All = Choices[Style];
    for (char32_t Each : {Wanted, Instead})
        for (std::size_t C = 0; C < All.size(); C++)
            if (Has(*All[C].Face, Each)) {
                const GlyphId Glyph = All[C].Face->Find(Each);
                return Sign{static_cast<std::int32_t>(C), Glyph, Each, Each == Wanted ? 1 : (Wanted == EllipsisSign ? 3 : 1),
                            AdvanceOf(*All[C].Face, Glyph, LookOf(Style, All[C]))};
            }
    return Sign{-1, 0, Wanted, 1, MissingAdvance(Wanted, Styles[Style].Size)};
}

std::uint32_t Setter::EndOf(std::uint32_t From, std::uint32_t To) const noexcept
{
    while (To > From && Hangs(Items[To - 1].Is)) To--;
    return To;
}

/* A paragraph — characters From up to To, its break after them when Broken
 * — broken into lines: each as long as the box lets, broken at the last
 * place a line may break that keeps it within, a word longer than the line
 * between its characters. */
void Setter::BreakParagraph(std::uint32_t From, std::uint32_t To, bool Broken)
{
    const float W     = In.Width;
    const bool  Wraps = In.Wrap && W > 0;
    std::uint32_t Start = From;
    for (;;) {
        float                        Pen = 0;
        std::optional<std::uint32_t> Best, Over;
        for (std::uint32_t I = Start; Wraps && I < To; I++) {
            const Item &It      = Items[I];
            const float Advance = It.Is == Kind::Tab ? TabAdvance(It, Pen) : It.Advance;
            if (!Hangs(It.Is) && I > Start && Pen + Advance - It.Kern - It.Spacing > W) {
                Over = I;
                break;
            }
            Pen += Advance;
            if (I + 1 < To && BreaksBetween(I > 0 ? Items[I - 1].Shown : 0, It.Shown, Items[I + 1].Shown) &&
                (It.Is != Kind::SoftHyphen || Pen + SignFor(It.Style, HyphenSign, U'-').Advance <= W))
                Best = I;
        }
        if (!Over) {
            Pieces.push_back(Piece{Start, To + (Broken ? 1u : 0u), EndOf(Start, To), 0, false, true});
            return;
        }
        std::uint32_t Last = 0;
        if (Best) {
            Last = *Best;
        } else {
            std::uint32_t Begin = *Over;
            while (Begin > Start && (Clings(Items[Begin].Shown) || Items[Begin - 1].Shown == Joiner)) Begin--;
            Last = Begin - 1;
            if (Begin == Start) {
                Last = *Over;
                while (Last + 1 < To && (Clings(Items[Last + 1].Shown) || Items[Last].Shown == Joiner)) Last++;
            }
        }
        Pieces.push_back(Piece{Start, Last + 1, EndOf(Start, Last + 1), 0,
                               Best && Last == *Best && Items[Last].Is == Kind::SoftHyphen, false});
        Start = Last + 1;
    }
}

/* The pen before each character of a line from its start, and after its
 * last. */
std::vector<float> Setter::Pens(const Piece &Of) const
{
    std::vector<float> At(Of.To - Of.From + 1);
    float              Pen = 0;
    for (std::uint32_t I = Of.From; I < Of.To; I++) {
        At[I - Of.From] = Pen;
        Pen += Items[I].Is == Kind::Tab ? TabAdvance(Items[I], Pen) : Items[I].Advance;
    }
    At.back() = Pen;
    return At;
}

/* How wide what a line draws is: to the end of its last character but its
 * kerning and spacing with the next, and its hyphen. */
float Setter::ContentWidth(const Piece &Of, const std::vector<float> &At) const
{
    const std::uint32_t Shown = Of.Ellipsis ? Of.Shown : Of.End;
    float               Width = At[Shown - Of.From];
    if (Shown > Of.From) Width -= Items[Shown - 1].Kern + Items[Shown - 1].Spacing;
    if (Of.Hyphen) Width += SignFor(Items[Of.To - 1].Style, HyphenSign, U'-').Advance;
    if (Of.Ellipsis) {
        const Sign Mark = SignFor(StyleBefore(Of.Shown, Of), EllipsisSign, U'.');
        Width += Mark.Advance * static_cast<float>(Mark.Times);
    }
    return std::max(Width, 0.0f);
}

/* A line's height and its baseline's place in it: each style on it, in each
 * face it is set in, its line round its ascender and descender. */
void Setter::Measure(Piece &Of) const
{
    float      Ascent = 0, Descent = 0;
    bool       Any = false;
    const auto Take = [&](std::uint32_t Style, const Font *Face) {
        const Resolved &R = Styles[Style];
        float Above = R.Size * 0.8f, Below = R.Size * 0.2f, Gap = 0;
        if (Face) {
            const FontMetrics &M     = Face->Metrics();
            const float        Scale = R.Size / M.UnitsPerEm;
            Above = static_cast<float>(M.Ascender) * Scale;
            Below = -static_cast<float>(M.Descender) * Scale;
            Gap   = std::max(static_cast<float>(M.LineGap) * Scale, 0.0f);
        }
        const float Line = R.LineHeight > 0 ? R.LineHeight * R.Size : Above + Below + Gap;
        const float Half = (Line - (Above + Below)) / 2;
        Ascent  = Any ? std::max(Ascent, Above + Half) : Above + Half;
        Descent = Any ? std::max(Descent, Below + Half) : Below + Half;
        Any     = true;
    };
    std::vector<std::pair<std::uint32_t, const Font *>> Seen;
    for (std::uint32_t I = Of.From; I < Of.To; I++) {
        const FaceChoice *Face = ChoiceOf(Items[I]);
        const Font       *F    = Face ? Face->Face.get()
                                      : Choices[Items[I].Style].empty() ? nullptr : Choices[Items[I].Style].front().Face.get();
        const std::pair<std::uint32_t, const Font *> Key{Items[I].Style, F};
        if (std::find(Seen.begin(), Seen.end(), Key) != Seen.end()) continue;
        Seen.push_back(Key);
        Take(Key.first, Key.second);
    }
    if (!Any) {
        const std::uint32_t Style = Of.From < Count() ? Items[Of.From].Style : EndStyle;
        Take(Style, Choices[Style].empty() ? nullptr : Choices[Style].front().Face.get());
    }
    Of.Height = std::max(1.0f, std::round(Ascent + Descent));
    Of.Ascent = std::round(Ascent);
}

/* The lines the box holds: no more than its most, and — unless it grows —
 * no more than fit its height, the first always; the last of them ending in
 * an ellipsis where text is left out and the box asks for one, as does every
 * line cut off at its edge. */
void Setter::CutToBox()
{
    std::size_t Keep = Pieces.size();
    if (In.MaxLines) Keep = std::min<std::size_t>(Keep, In.MaxLines);
    if (In.Height > 0 && In.Overflow != TextOverflow::Grow) {
        float       Top = 0;
        std::size_t K   = 0;
        while (K < Keep && (K == 0 || Top + Pieces[K].Height <= In.Height)) Top += Pieces[K++].Height;
        Keep = K;
    }
    const bool Dropped = Keep < Pieces.size();
    Pieces.resize(Keep);
    Out.CutOff = Dropped;
    for (std::size_t K = 0; K < Pieces.size(); K++) {
        Piece &Of = Pieces[K];
        Of.Shown  = Of.End;
        const bool Wide = In.Width > 0 && ContentWidth(Of, Pens(Of)) > In.Width + 1.0f / 64;
        Out.CutOff = Out.CutOff || Wide;
        if (In.Overflow == TextOverflow::Ellipsis && (Wide || (Dropped && K + 1 == Pieces.size()))) EndInEllipsis(Of);
    }
}

/* A line cut to end in an ellipsis within the box: characters taken off its
 * end, a character never parted from the marks on it, the spaces before the
 * ellipsis too. */
void Setter::EndInEllipsis(Piece &Of)
{
    const std::vector<float> At = Pens(Of);
    Of.Ellipsis                 = true;
    Of.Hyphen                   = false;
    Of.Shown                    = Of.End;
    while (Of.Shown > Of.From) {
        if (In.Width <= 0 || ContentWidth(Of, At) <= In.Width) break;
        Of.Shown--;
        while (Of.Shown > Of.From && Of.Shown < Of.End && Clings(Items[Of.Shown].Shown)) Of.Shown--;
    }
    while (Of.Shown > Of.From && Hangs(Items[Of.Shown - 1].Is)) Of.Shown--;
    while (Of.Shown > Of.From && Blank(Items[Of.Shown - 1].Shown)) Of.Shown--;
}

/* The style of the character before Index on a line; of its first when
 * none is, and of the text's end on a line with no characters. */
std::uint32_t Setter::StyleBefore(std::uint32_t Index, const Piece &Of) const noexcept
{
    if (Index > Of.From) return Items[Index - 1].Style;
    return Of.From < Count() ? Items[Of.From].Style : EndStyle;
}

std::uint32_t Setter::RunFor(const FaceChoice *Face, std::uint32_t Style)
{
    LaidRun Wanted;
    Wanted.Face = Face ? Face->Face : nullptr;
    Wanted.Look = Face ? LookOf(Style, *Face) : GlyphLook{Styles[Style].Size};
    Wanted.Ink  = Styles[Style].Ink;
    const RunKey Key{Wanted.Face.get(), std::bit_cast<std::uint32_t>(Wanted.Look.Em), Wanted.Look.Bold, Wanted.Look.Slant,
                     Wanted.Ink};
    const auto [At, Made] = RunKept.try_emplace(Key, static_cast<std::uint32_t>(Out.RunList.size()));
    if (Made) Out.RunList.push_back(std::move(Wanted));
    return At->second;
}

/* The lines under and through a character, joined to the one before where
 * they meet it in one ink. */
void Setter::Decorate(const Item &It, float X, float Advance, float Baseline)
{
    const Resolved &R = Styles[It.Style];
    if (!R.Underline && !R.Strike) return;
    const FaceChoice *Face = ChoiceOf(It);
    if (!Face && !Choices[It.Style].empty()) Face = &Choices[It.Style].front();
    const float  Scale = Face ? R.Size / Face->Face->Metrics().UnitsPerEm : 0;
    const auto   Put   = [&](float Top, float Thick) {
        LaidDecoration Line{Area{X, Top, Advance, Thick}, R.Ink};
        if (!Out.DecorationList.empty()) {
            LaidDecoration &Before = Out.DecorationList.back();
            if (Before.Ink == Line.Ink && Before.Of.Y == Line.Of.Y && Before.Of.Height == Line.Of.Height &&
                std::fabs(Before.Of.X + Before.Of.Width - X) < 1.0f / 16) {
                Before.Of.Width = X + Advance - Before.Of.X;
                return;
            }
        }
        Out.DecorationList.push_back(Line);
    };
    const FontMetrics *M     = Face ? &Face->Face->Metrics() : nullptr;
    const float        Thick = M && M->UnderlineThickness > 0 ? M->UnderlineThickness * Scale : R.Size / 14;
    if (R.Underline) Put(Baseline + (M && M->UnderlineThickness > 0 ? -M->UnderlinePosition * Scale : R.Size / 10), Thick);
    if (R.Strike) {
        const float StrikeThick = M && M->StrikeThickness > 0 ? M->StrikeThickness * Scale : Thick;
        const float Top = M && M->StrikeThickness > 0 ? Baseline - M->StrikePosition * Scale
                          : M && M->XHeight > 0       ? Baseline - M->XHeight * Scale / 2 - StrikeThick / 2
                                                      : Baseline - R.Size * 0.3f;
        Put(Top, StrikeThick);
    }
}

/* Every line placed, top to bottom: aligned in the box — in the widest line
 * where the box has no width — a line justified spread at its spaces, every
 * glyph and the caret's every stand put where its pen is. */
void Setter::Place()
{
    std::vector<std::vector<float>> AllPens;
    float                           Widest = 0;
    for (const Piece &Of : Pieces) {
        AllPens.push_back(Pens(Of));
        Widest = std::max(Widest, ContentWidth(Of, AllPens.back()));
    }
    const float Box = In.Width > 0 ? In.Width : Widest;
    const std::uint32_t N = Count();
    Out.Stops.assign(N + 1, 0);
    Out.LineOf.assign(N + 1, 0);
    float Top = 0, EndX = 0;
    for (std::size_t K = 0; K < Pieces.size(); K++) {
        const Piece              &Of = Pieces[K];
        const std::vector<float> &At = AllPens[K];
        const float               Width = ContentWidth(Of, At);
        const auto                Line  = static_cast<std::uint32_t>(K);
        std::uint32_t             Spreading = 0;
        for (std::uint32_t I = Of.From; I < Of.End; I++) Spreading += Spreads(Items[I].Shown) ? 1u : 0u;
        const bool  Justified = In.Align == TextAlign::Justify && In.Width > 0 && !Of.Last && !Of.Ellipsis &&
                               Spreading > 0 && Width < Box;
        const float Spread    = Justified ? (Box - Width) / static_cast<float>(Spreading) : 0;
        const float Room      = std::max(Box - Width, 0.0f);
        const float Left      = In.Align == TextAlign::Center ? Room / 2 : In.Align == TextAlign::Right ? Room : 0;

        LaidLine Laid;
        Laid.Top        = Top;
        Laid.Height     = Of.Height;
        Laid.Baseline   = Top + Of.Ascent;
        Laid.Left       = Left;
        Laid.Width      = Justified ? Box : Width;
        Laid.FirstGlyph = static_cast<std::uint32_t>(Out.GlyphList.size());
        Laid.From       = Of.From;
        Laid.To         = Of.To;

        float Pen = Left, Added = 0;
        for (std::uint32_t I = Of.From; I < Of.To; I++) {
            const Item &It = Items[I];
            Pen            = Left + At[I - Of.From] + Added;
            if (I >= Of.Shown && Of.Ellipsis) break;
            Out.Stops[I]  = Pen;
            Out.LineOf[I] = Line;
            if (I < Of.Shown && It.Ink) {
                const FaceChoice *Face = ChoiceOf(It);
                const float Shift = It.Shift - (I == Of.From ? It.Lead : 0.0f);
                const float Size  = Styles[It.Style].Size;
                const float Gap   = Face ? 0.0f
                                         : (MissingAdvance(It.Shown, Size) - static_cast<float>(MissingCell(It.Shown, Size).Width)) / 2;
                Out.GlyphList.push_back(LaidGlyph{RunFor(Face, It.Style), Face ? It.Glyph : It.Shown,
                                                  Pen + Shift + Gap, Laid.Baseline, I});
            }
            if (I < Of.End) Decorate(It, Pen, It.Advance - (I + 1 == Of.End ? It.Kern + It.Spacing : 0.0f), Laid.Baseline);
            if (Spreads(It.Shown) && I < Of.End) Added += Spread;
        }
        float Last = Left + At[(Of.Ellipsis ? Of.Shown : Of.To) - Of.From] + Added;
        Laid.Right = Last;
        if (Of.Hyphen || Of.Ellipsis) {
            const std::uint32_t Where     = Of.Ellipsis ? Of.Shown : Of.To - 1;
            const std::uint32_t MarkStyle = Of.Hyphen ? Items[Of.To - 1].Style : StyleBefore(Of.Shown, Of);
            const Sign          Mark      = Of.Hyphen ? SignFor(MarkStyle, HyphenSign, U'-') : SignFor(MarkStyle, EllipsisSign, U'.');
            float X = Of.Hyphen ? Left + At[Of.To - 1 - Of.From] + Added
                                : Last - (Of.Shown > Of.From ? Items[Of.Shown - 1].Kern + Items[Of.Shown - 1].Spacing : 0.0f);
            if (Of.Ellipsis) Last = X;
            const FaceChoice *Face = Mark.Choice >= 0 ? &Choices[MarkStyle][static_cast<std::size_t>(Mark.Choice)] : nullptr;
            for (int T = 0; T < Mark.Times; T++, X += Mark.Advance)
                Out.GlyphList.push_back(LaidGlyph{RunFor(Face, MarkStyle), Face ? Mark.Glyph : Mark.Shown, X,
                                                  Laid.Baseline, Where});
            Laid.Right = std::max(Laid.Right, X);
        }
        if (Of.Ellipsis)
            for (std::uint32_t I = Of.Shown; I < Of.To; I++) Out.Stops[I] = Last, Out.LineOf[I] = Line;
        Laid.GlyphCount = static_cast<std::uint32_t>(Out.GlyphList.size()) - Laid.FirstGlyph;
        Out.LineList.push_back(Laid);
        EndX = Last;
        Top += Of.Height;
    }
    /* Characters of lines left out: where the text is cut. */
    const std::uint32_t Shown = Pieces.empty() ? 0 : Pieces.back().To;
    const auto          LastLine = static_cast<std::uint32_t>(Pieces.empty() ? 0 : Pieces.size() - 1);
    for (std::uint32_t I = Shown; I <= N; I++) Out.Stops[I] = EndX, Out.LineOf[I] = LastLine;
    if (!Pieces.empty() && Pieces.back().From == N && Pieces.back().To == N) {
        Out.Stops[N]  = Out.LineList.back().Left;
        Out.LineOf[N] = LastLine;
    }
    Out.Wide = Box;
    Out.High = In.Height > 0 && In.Overflow != TextOverflow::Grow ? In.Height
               : In.Height > 0                                     ? std::max(In.Height, Top)
                                                                   : Top;
}

Layout Layout::Of(const Text &Set, const TextBox &In, const Fonts &From)
{
    Layout Made;
    Made.Said   = Set.Characters;
    Made.Hidden = Set.Secret;
    Made.Within = In;
    Setter Work{Set, In, From, Made, {}, {}, {}, {}, 0, std::vector<Item>(Set.Characters.size()), {}};
    Work.ResolveStyles();
    Work.SetCharacters();
    Work.KernPairs();
    Work.SpaceLetters();

    const std::uint32_t N = Work.Count();
    std::uint32_t       Start = 0;
    for (std::uint32_t I = 0; I < N; I++) {
        if (Work.Items[I].Is != Kind::LineBreak) continue;
        /* CR LF is one break. */
        if (Made.Said[I] == U'\r' && I + 1 < N && Made.Said[I + 1] == U'\n') {
            Work.Items[I].Is = Kind::Invisible;
            continue;
        }
        Work.BreakParagraph(Start, I, true);
        Start = I + 1;
    }
    if (Start < N || N == 0 || Work.Items[N - 1].Is == Kind::LineBreak) Work.BreakParagraph(Start, N, false);
    for (Piece &Each : Work.Pieces) Work.Measure(Each);
    Work.CutToBox();
    Work.Place();
    return Made;
}

}  // namespace typecase
