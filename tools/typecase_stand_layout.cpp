/* typecase stand — text laid out in a box (typecase/layout.h), in the
 * stand's own family, whose every advance is known: a size of 10 makes a
 * unit a hundredth of a pixel, so an ordinary letter is 5 pixels and a line
 * 10. Lines broken between words, a word too long between its characters,
 * at a soft hyphen showing a hyphen, after a hyphen, between ideographs but
 * never before closing punctuation, never at a no-break space; aligned and
 * justified; kerned, spaced, of a line height; cut to the box with and
 * without an ellipsis, or grown; styled by spans that say only what they
 * say; characters from a fallback face and drawn as their code; the caret,
 * the nearest character, where a stretch lies, the text to copy, a secret;
 * and thousands of texts of every kind of character laid whole. */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "typecase/layout.h"
#include "typecase_stand.h"

namespace stand {

namespace {

bool Near(float A, float B) { return std::fabs(A - B) < 1.0f / 256; }

typecase::Layout Lay(const typecase::Fonts &Book, std::u32string Characters, typecase::TextBox Box = {},
                     typecase::Style Style = {}, std::vector<typecase::Span> Spans = {}, bool Secret = false)
{
    typecase::Text Text;
    Text.Characters = std::move(Characters);
    Style.Size      = Style.Size ? Style.Size : 10.0f;
    Text.Style      = Style;
    Text.Spans      = std::move(Spans);
    Text.Secret     = Secret;
    return typecase::Layout::Of(Text, Box, Book);
}

/* The lines as their characters' ranges: "0-10 10-14". */
std::string Ranges(const typecase::Layout &Of)
{
    std::string Said;
    for (const auto &Line : Of.Lines()) Said += (Said.empty() ? "" : " ") + std::to_string(Line.From) + "-" + std::to_string(Line.To);
    return Said;
}

std::string Num(float Value)
{
    char Text[32];
    std::snprintf(Text, sizeof Text, "%g", static_cast<double>(Value));
    return Text;
}

/* The first glyph showing character Index. */
const typecase::LaidGlyph *GlyphAt(const typecase::Layout &Of, std::uint32_t Index)
{
    for (const auto &G : Of.Glyphs())
        if (G.Character == Index) return &G;
    return nullptr;
}

void CheckBreaking(Tally &Count, const typecase::Fonts &Book)
{
    const auto One = Lay(Book, U"Hello world");
    Count.Check(One.Lines().size() == 1 && Near(One.Width(), 49) && Near(One.Height(), 10) && Near(One.Lines()[0].Baseline, 8) &&
                    One.Glyphs().size() == 10,
                "a line with no box as wide as its advances (" + Num(One.Width()) + "), a line of the font's own "
                "height, its baseline at its ascender; a space draws no glyph");

    typecase::TextBox Narrow;
    Narrow.Width   = 45;
    const auto Two = Lay(Book, U"aaaa bbbb cccc", Narrow);
    Count.Check(Ranges(Two) == "0-10 10-14" && Near(Two.Lines()[0].Width, 42.5f) && Near(Two.Lines()[0].Right, 45) &&
                    Near(Two.Lines()[1].Width, 20) && Near(Two.Lines()[1].Top, 10) && Near(Two.Height(), 20),
                "lines broken between words within the width, the space at a line's end hanging past it: " + Ranges(Two));

    typecase::TextBox Tight;
    Tight.Width     = 22;
    const auto Long = Lay(Book, U"abcdefghij", Tight);
    Count.Check(Ranges(Long) == "0-4 4-8 8-10", "a word wider than the box broken between its characters: " + Ranges(Long));

    typecase::TextBox Thirty;
    Thirty.Width     = 30;
    const auto Soft  = Lay(Book, U"aaaa­bbbb", Thirty);
    const auto *Sign = Soft.Glyphs().size() > 4 ? &Soft.Glyphs()[4] : nullptr;
    Count.Check(Ranges(Soft) == "0-5 5-9" && Sign && Sign->Glyph == SansGlyphOf(U'‐') && Near(Sign->X, 20) &&
                    Near(Soft.Lines()[0].Width, 23.33f),
                "a line broken at a soft hyphen shows a hyphen there: " + Ranges(Soft));
    const auto Unbroken = Lay(Book, U"aa­bb");
    Count.Check(Unbroken.Glyphs().size() == 4 && Near(Unbroken.Width(), 20),
                "a soft hyphen not broken at shows nothing and takes no room");

    const auto Hyphen = Lay(Book, U"well-known", Thirty);
    const auto Digits = Lay(Book, U"aaaa-5555", Thirty);
    Count.Check(Ranges(Hyphen) == "0-5 5-10" && Ranges(Digits) == "0-6 6-9",
                "a line broken after a hyphen between letters (" + Ranges(Hyphen) + "); before digits it is not, and the "
                "word is broken between characters (" + Ranges(Digits) + ")");
    const auto Glued = Lay(Book, U"aaaa bbbb", Thirty);
    Count.Check(Ranges(Glued) == "0-6 6-9", "never broken at a no-break space: " + Ranges(Glued));

    typecase::TextBox Ideographs;
    Ideographs.Width = 25;
    const auto Han   = Lay(Book, U"中国人中国人", Ideographs);
    const auto Stop  = Lay(Book, U"中国。人", Ideographs);
    Count.Check(Ranges(Han) == "0-2 2-4 4-6" && Ranges(Stop) == "0-1 1-3 3-4",
                "ideographs broken between any two (" + Ranges(Han) + "), never before a full stop (" + Ranges(Stop) + ")");

    const auto Breaks = Lay(Book, U"a\nb\r\nc d");
    const auto Ended  = Lay(Book, U"a\n");
    const auto Empty  = Lay(Book, U"");
    Count.Check(Ranges(Breaks) == "0-2 2-5 5-7 7-8" && Ranges(Ended) == "0-2 2-2" && Ranges(Empty) == "0-0" &&
                    Near(Empty.Height(), 10) && Ended.CaretAt(2).Line == 1 && Near(Ended.CaretAt(2).X, 0),
                "a line feed, CR LF as one, a line separator each end a line; a text ending in a break has an empty "
                "line after it for the caret, and an empty text one line: " + Ranges(Breaks));

    const auto Tabbed = Lay(Book, U"a\tb");
    Count.Check(GlyphAt(Tabbed, 2) && Near(GlyphAt(Tabbed, 2)->X, 20), "a tab to the next stop, eight spaces on");
}

void CheckPlacing(Tally &Count, const typecase::Fonts &Book)
{
    typecase::TextBox Box;
    Box.Width = 45;
    Box.Align = typecase::TextAlign::Right;
    const auto Right = Lay(Book, U"aaaa bbbb cccc", Box);
    Box.Align        = typecase::TextAlign::Center;
    const auto Middle = Lay(Book, U"aaaa bbbb cccc", Box);
    Box.Align         = typecase::TextAlign::Justify;
    const auto Full   = Lay(Book, U"aaaa bbbb cccc", Box);
    Count.Check(Near(Right.Lines()[0].Left, 2.5f) && Near(Right.Lines()[1].Left, 25) && Near(Middle.Lines()[1].Left, 12.5f) &&
                    GlyphAt(Full, 5) && Near(GlyphAt(Full, 5)->X, 25) && GlyphAt(Full, 10) && Near(GlyphAt(Full, 10)->X, 0),
                "aligned right and in the middle, its hanging spaces not counted; justified, a line spread at its "
                "spaces to the width, a paragraph's last not");

    const auto Kerned = Lay(Book, U"AVA");
    const auto Moved  = Lay(Book, U"LT");
    Count.Check(Near(Kerned.Width(), 14) && GlyphAt(Moved, 0) && Near(GlyphAt(Moved, 0)->X, 0.1f) && GlyphAt(Moved, 1) &&
                    Near(GlyphAt(Moved, 1)->X, 4.9f),
                "kerned as the font says: AVA " + Num(Kerned.Width()) + " pixels, a pair that moves its glyphs moving them");

    typecase::Style Spaced;
    Spaced.LetterSpacing = 1;
    const auto Apart     = Lay(Book, U"abc", {}, Spaced);
    typecase::Style Tall;
    Tall.LineHeight = 2;
    const auto High = Lay(Book, U"abc", {}, Tall);
    Count.Check(Near(Apart.Width(), 17) && GlyphAt(Apart, 2) && Near(GlyphAt(Apart, 2)->X, 12) && Near(High.Height(), 20) &&
                    Near(High.Lines()[0].Baseline, 13),
                "letter spacing between characters, none after the last; a line twice its size, the font's line in its "
                "middle");

    typecase::TextBox Cut;
    Cut.Width    = 45;
    Cut.MaxLines = 1;
    Cut.Overflow = typecase::TextOverflow::Ellipsis;
    const auto Dots   = Lay(Book, U"aaaa bbbb cccc", Cut);
    const auto &Last  = Dots.Glyphs().back();
    Count.Check(Dots.Lines().size() == 1 && Dots.Cut() && Last.Glyph == SansGlyphOf(U'…') && Near(Last.X, 37.5f) &&
                    Dots.Glyphs().size() == 8 && Near(Dots.CaretAt(12).X, 37.5f) && Near(Dots.Lines()[0].Width, 45),
                "cut to its lines with an ellipsis, characters taken off until it fits the width; a character cut off "
                "has the caret where the ellipsis stands");
    typecase::TextBox Clip;
    Clip.Width    = 45;
    Clip.Height   = 25;
    Clip.Overflow = typecase::TextOverflow::Clip;
    const auto Clipped = Lay(Book, U"aaaa bbbb cccc dddd eeee", Clip);
    Clip.Overflow      = typecase::TextOverflow::Grow;
    const auto Grown   = Lay(Book, U"aaaa bbbb cccc dddd eeee", Clip);
    typecase::TextBox Line;
    Line.Width       = 30;
    Line.Wrap        = false;
    Line.Overflow    = typecase::TextOverflow::Ellipsis;
    const auto Ended = Lay(Book, U"aaaaaaaa\nbb", Line);
    Count.Check(Clipped.Lines().size() == 2 && Clipped.Cut() && Near(Clipped.Height(), 25) && Grown.Lines().size() == 3 &&
                    !Grown.Cut() && Near(Grown.Height(), 30) && Ended.Lines().size() == 2 && Ended.Cut() &&
                    Near(Ended.Lines()[0].Width, 27.5f) && Near(Ended.Lines()[1].Width, 10),
                "cut to the lines that fit its height, or grown to hold them all; unwrapped, each line too long ends in "
                "an ellipsis");
}

void CheckStyles(Tally &Count, const typecase::Fonts &Book)
{
    typecase::Style Big;
    Big.Size        = 20;
    const auto Span = Lay(Book, U"aaa bbb", {}, {}, {{4, 7, Big}});
    const auto &Runs = Span.Runs();
    const auto *B    = GlyphAt(Span, 4);
    Count.Check(B && Runs[B->Run].Look.Em == 20 && Runs[GlyphAt(Span, 0)->Run].Look.Em == 10 &&
                    Runs[B->Run].Face == Runs[GlyphAt(Span, 0)->Run].Face && Near(Span.Height(), 20) &&
                    Near(Span.Lines()[0].Baseline, 16) && Near(B->X, 17.5f),
                "a span says its size and only that: its family the text's; the line as tall as its tallest style");

    typecase::Style Bold;
    Bold.Weight = 700;
    typecase::Style Lean;
    Lean.Italic      = true;
    const auto Faces = Lay(Book, U"ab", {}, {}, {{0, 1, Bold}, {1, 2, Lean}});
    Count.Check(Runs.size() >= 1 && Faces.Runs()[GlyphAt(Faces, 0)->Run].Face->Style().Weight == 700 &&
                    Faces.Runs()[GlyphAt(Faces, 1)->Run].Face->Style().Italic && Near(GlyphAt(Faces, 1)->X, 5.5f),
                "a span bold set in the bold face, its advances its own; a span italic in the italic");

    typecase::Style Ink;
    Ink.Ink        = 3;
    Ink.Underline  = true;
    typecase::Style Through;
    Through.Strike   = true;
    const auto Lines = Lay(Book, U"aa bb cc", {}, {}, {{3, 5, Ink}, {6, 8, Through}});
    const auto &D    = Lines.Decorations();
    Count.Check(D.size() == 2 && D[0].Ink == 3 && Near(D[0].Of.X, 12.5f) && Near(D[0].Of.Width, 10) && Near(D[0].Of.Y, 9) &&
                    Near(D[0].Of.Height, 0.5f) && Near(D[1].Of.Y, 5) && Lines.Runs()[GlyphAt(Lines, 3)->Run].Ink == 3,
                "a line under a stretch where post puts it, one through where OS/2 does, each in its stretch's ink");

    const auto Fallback = Lay(Book, U"AЖ中\U0001F600");
    const auto *Zhe = GlyphAt(Fallback, 1), *Smile = GlyphAt(Fallback, 3);
    Count.Check(Zhe && Fallback.Runs()[Zhe->Run].Face->Style().Family == "Stand Extra" && Smile &&
                    !Fallback.Runs()[Smile->Run].Face && Smile->Glyph == 0x1F600 &&
                    Fallback.Runs()[GlyphAt(Fallback, 0)->Run].Face->Style().Family == "Stand Sans",
                "a character the family has not set from the family that has it; one no font has drawn as its code");

    const auto Hidden = Lay(Book, U"pa ss", {}, {}, {}, true);
    bool       Dots   = Hidden.Glyphs().size() == 5;
    for (const auto &G : Hidden.Glyphs()) Dots = Dots && G.Glyph == SansGlyphOf(U'•');
    const auto Open = Lay(Book, U"Hello world");
    Count.Check(Dots && Hidden.Copy(0, 5).empty() && Open.Copy(6, 11) == "world" && Open.Copy(0, 99) == "Hello world",
                "a secret text shows every character as a dot and is never copied; another is copied as it is");
}

void CheckCaret(Tally &Count, const typecase::Fonts &Book)
{
    const auto One = Lay(Book, U"Hello world");
    bool       Back = true;
    for (std::uint32_t I = 0; I <= One.Count(); I++) {
        const typecase::Caret At = One.CaretAt(I);
        Back = Back && One.IndexAt(At.X + 0.2f, (At.Top + At.Bottom) / 2) == I && Near(At.Bottom - At.Top, 10);
    }
    Count.Check(Back && One.IndexAt(-5, 5) == 0 && One.IndexAt(500, 5) == 11 && Near(One.CaretAt(11).X, 49),
                "the caret before every character, and the character nearest each caret the same one");

    const auto Marks = Lay(Book, U"éx");
    Count.Check(Marks.Next(0) == 2 && Marks.Previous(2) == 0 && Near(Marks.CaretAt(1).X, 0) && Marks.Next(3) == 3,
                "the caret never between a letter and the mark on it");

    typecase::TextBox Narrow;
    Narrow.Width    = 45;
    const auto Two  = Lay(Book, U"aaaa bbbb cccc", Narrow);
    const auto Area = Two.Cover(2, 12);
    Count.Check(Two.CaretAt(10).Line == 1 && Near(Two.CaretAt(10).X, 0) && Two.CaretAt(9).Line == 0 && Area.size() == 2 &&
                    Near(Area[0].X, 10) && Near(Area[0].Width, 35) && Near(Area[1].Width, 10) && Near(Area[1].Y, 10) &&
                    Two.IndexAt(44, 5) == 9 && Two.IndexAt(1, 15) == 10,
                "where a line breaks the caret stands at the next one's start; a stretch over two lines covers each");
}

/* Texts of every kind of character, every kind of span and box, laid: each
 * kept to what a layout promises. */
void CheckMany(Tally &Count, const typecase::Fonts &Book)
{
    std::mt19937 Random(20261118);
    const char32_t Kinds[] = {U'a', U'W', U'i', U' ', U' ', U'\n', U'\t', U'-', U'­', U' ', U'́', U'​',
                              U'中', U'。', U'Ж', U'\U0001F600', U'\r', U' ', U'.', U'(', U'‍', U'A',
                              U'V', U'T', U'—', U'\u0007', U'﻿'};
    int        Broken = 0;
    std::string First;
    for (int Round = 0; Round < 3000; Round++) {
        std::u32string Characters;
        const int      Length = static_cast<int>(Random() % 60);
        for (int I = 0; I < Length; I++) Characters += Kinds[Random() % (sizeof Kinds / sizeof Kinds[0])];
        typecase::TextBox Box;
        Box.Width    = Random() % 3 ? static_cast<float>(Random() % 80) : 0;
        Box.Height   = Random() % 3 ? 0 : static_cast<float>(Random() % 50);
        Box.Align    = static_cast<typecase::TextAlign>(Random() % 4);
        Box.Wrap     = Random() % 4 != 0;
        Box.MaxLines = Random() % 3 ? 0 : Random() % 4;
        Box.Overflow = static_cast<typecase::TextOverflow>(Random() % 3);
        std::vector<typecase::Span> Spans;
        for (int S = 0, Many = static_cast<int>(Random() % 4); S < Many; S++) {
            typecase::Span Each;
            Each.From = Random() % 70, Each.To = Random() % 70;
            if (Random() % 2) Each.Style.Size = static_cast<float>(1 + Random() % 40);
            if (Random() % 2) Each.Style.Weight = static_cast<std::uint16_t>(100 * (1 + Random() % 9));
            if (Random() % 2) Each.Style.Italic = Random() % 2 == 0;
            if (Random() % 3 == 0) Each.Style.LetterSpacing = static_cast<float>(static_cast<int>(Random() % 9) - 4);
            if (Random() % 3 == 0) Each.Style.Underline = true;
            if (Random() % 4 == 0) Each.Style.Family = "Stand Extra";
            Spans.push_back(Each);
        }
        const auto L = Lay(Book, Characters, Box, {}, Spans, Random() % 10 == 0);
        bool       Holds = !L.Lines().empty() && std::isfinite(L.Width()) && std::isfinite(L.Height());
        std::uint32_t Next = 0;
        for (const auto &Line : L.Lines()) {
            Holds = Holds && Line.From == Next && Line.To >= Line.From && Line.Height >= 1;
            Next  = Line.To;
        }
        Holds = Holds && (L.Cut() || Next == L.Count());
        for (const auto &G : L.Glyphs()) Holds = Holds && G.Character <= L.Count() && G.Run < L.Runs().size() && std::isfinite(G.X);
        for (std::uint32_t I = 0; I <= L.Count(); I++) {
            const auto At = L.CaretAt(I);
            Holds = Holds && std::isfinite(At.X) && At.Line < L.Lines().size() && L.IndexAt(At.X, At.Top) <= L.Count();
        }
        Holds = Holds && (L.Secret() || L.Copy(0, L.Count()) == typecase::Utf8Of(Characters));
        if (!Holds && Broken++ == 0) First = typecase::Utf8Of(Characters);
    }
    /* Many spans: each stretch set from the spans open over it, not from all. */
    std::u32string Long(20000, U'a');
    for (std::size_t I = 7; I < Long.size(); I += 8) Long[I] = U' ';
    std::vector<typecase::Span> Many;
    for (std::uint32_t S = 0; S < 10000; S++) {
        typecase::Span Each;
        Each.From = S * 2, Each.To = S * 2 + 3;
        Each.Style.Size = static_cast<float>(8 + S % 7);
        Each.Style.Ink  = S % 5;
        Many.push_back(Each);
    }
    typecase::TextBox Wide;
    Wide.Width       = 400;
    const auto Start = std::chrono::steady_clock::now();
    const auto Spans = Lay(Book, Long, Wide, {}, Many);
    const double Ms  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
    Count.Check(Spans.Glyphs().size() == 17500 && Spans.Runs().size() == 35,
                "20000 characters under 10000 spans laid, each stretch from the spans open over it — " + Num(static_cast<float>(Ms)) +
                    " ms here, " + std::to_string(Spans.Runs().size()) + " runs");
    Count.Check(Broken == 0, "3000 texts of every kind of character, span and box laid whole: lines in order holding "
                             "every character shown, every glyph and caret in its place — " +
                                 std::to_string(Broken) + " broken" + (Broken ? ", the first: " + First : ""));
}

}  // namespace

void CheckLayout(Tally &Count)
{
    const typecase::Fonts Book = StandBook();
    CheckBreaking(Count, Book);
    CheckPlacing(Count, Book);
    CheckStyles(Count, Book);
    CheckCaret(Count, Book);
    CheckMany(Count, Book);
}

}  // namespace stand
