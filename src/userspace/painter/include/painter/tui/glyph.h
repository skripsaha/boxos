#ifndef PAINTER_TUI_GLYPH_H
#define PAINTER_TUI_GLYPH_H

namespace painter::tui::Glyphs {

inline constexpr char UpperHalf  = static_cast<char>(0xDF);
inline constexpr char LowerHalf  = static_cast<char>(0xDC);
inline constexpr char Full       = static_cast<char>(0xDB);
inline constexpr char LightShade = static_cast<char>(0xB0);
inline constexpr char MidShade   = static_cast<char>(0xB1);
inline constexpr char DenseShade = static_cast<char>(0xB2);
inline constexpr char Dot        = static_cast<char>(0xFA);
inline constexpr char Bullet     = static_cast<char>(0x07);
inline constexpr char Tick       = static_cast<char>(0xFB);
inline constexpr char Left       = static_cast<char>(0x1B);
inline constexpr char Right      = static_cast<char>(0x1A);
inline constexpr char Up         = static_cast<char>(0x18);
inline constexpr char Down       = static_cast<char>(0x19);
inline constexpr char Pointer    = static_cast<char>(0x10);
inline constexpr char LeftHalf   = static_cast<char>(0xDD);
inline constexpr char RightHalf  = static_cast<char>(0xDE);

/* The box rules a frame is drawn with, and the marks of a line that goes on. */
inline constexpr char RuleAcross = static_cast<char>(0xC4);
inline constexpr char DoubleRule = static_cast<char>(0xCD);
inline constexpr char RuleDown   = static_cast<char>(0xB3);
inline constexpr char CornerTL   = static_cast<char>(0xDA);
inline constexpr char CornerTR   = static_cast<char>(0xBF);
inline constexpr char CornerBL   = static_cast<char>(0xC0);
inline constexpr char CornerBR   = static_cast<char>(0xD9);
inline constexpr char TeeLeft    = static_cast<char>(0xC3);
inline constexpr char TeeRight   = static_cast<char>(0xB4);
inline constexpr char MoreLeft   = static_cast<char>(0xAE);   /* « a line goes on to the left */
inline constexpr char MoreRight  = static_cast<char>(0xAF);   /* » and to the right */

}  // namespace painter::tui::Glyphs

#endif  // PAINTER_TUI_GLYPH_H
