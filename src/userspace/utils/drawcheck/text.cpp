/*
 * text.cpp — text the engine draws. See drawcheck.h.
 */

#include <string>

#include "drawcheck.h"
#include "painter/core/text.h"
#include "typecase/layout.h"

namespace drawcheck {

namespace {

using namespace painter::core;
/* BoxLib's C colour type is named Color too: Painter's is meant here. */
using painter::core::Color;

const char *const Words[] = {"Box", "OS", "draws", "text", "in", "any", "font,", "size", "and", "place;",
                             "\xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82", "\xD0\xB2", "\xD1\x80\xD0\xB0\xD0\xBC\xD0\xBA\xD0\xB5,",
                             "AVATAR", "Wa", "To", "kerned", "\xE2\x80\x94", "wrapped", "justified\xE2\x80\xA6", "\xF0\x9F\x98\x80"};

std::uint32_t Next(std::uint32_t &State)
{
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
}

}  // namespace

void DrawTextScene(Canvas &On, std::uint32_t Seed, const typecase::Fonts &Book)
{
    std::uint32_t State = Seed * 2654435761u + 7u;
    On.Clear(Color::Rgb(Next(State) & 63u, Next(State) & 63u, Next(State) & 63u));
    for (int Piece = 0; Piece < 6; Piece++) {
        std::string Said;
        for (int W = 0, Many = 3 + static_cast<int>(Next(State) % 24u); W < Many; W++)
            Said += std::string(W ? " " : "") + Words[Next(State) % (sizeof Words / sizeof Words[0])];
        typecase::Text Text = typecase::Text::Of(Said);
        Text.Style.Size     = 9.0f + static_cast<float>(Next(State) % 400u) / 10.0f;
        Text.Style.Weight   = Next(State) % 2u ? 400 : 700;
        Text.Style.Italic   = Next(State) % 4u == 0;
        typecase::Style Under;
        Under.Underline = true;
        Under.Ink       = 1;
        Text.Spans.push_back(typecase::Span{0, 4, Under});
        typecase::TextBox Box;
        Box.Width    = static_cast<float>(80 + Next(State) % 500u);
        Box.Align    = static_cast<typecase::TextAlign>(Next(State) % 4u);
        Box.MaxLines = Next(State) % 3u ? 0 : 2;
        Box.Overflow = Box.MaxLines ? typecase::TextOverflow::Ellipsis : typecase::TextOverflow::Grow;
        const typecase::Layout Laid = typecase::Layout::Of(Text, Box, Book);
        const PointF           At{static_cast<float>(Next(State) % 400u) + static_cast<float>(Next(State) % 4u) / 4.0f,
                                  static_cast<float>(Next(State) % 340u)};
        const Fill Ink = Next(State) % 3u ? Fill(Color::Rgba(255, 255 - (Next(State) & 127u), 220, 120u + Next(State) % 136u))
                                          : Fill(Gradient::Linear({At.X, 0}, {At.X + Box.Width, 0},
                                                                  {{0, Color::Rgb(250, 120, 140)}, {1, Color::Rgb(120, 160, 250)}}));
        const Transform Place = Next(State) % 5u == 0 ? Transform::Rotate(static_cast<float>(Next(State) % 40u) - 20.0f)
                                                      : Transform::Identity();
        On.FillText(Laid, At, Ink, Place, 1.0f, 0);
        On.FillText(Laid, At, Fill(Color::Rgb(250, 210, 90)), Place, 1.0f, 1);
    }
}

}  // namespace drawcheck
