#ifndef PAINTER_TUI_GLYPH_H
#define PAINTER_TUI_GLYPH_H

namespace painter::tui::Glyphs {

inline constexpr char UpperHalf  = static_cast<char>(0xDF);
inline constexpr char LowerHalf  = static_cast<char>(0xDC);
inline constexpr char Full       = static_cast<char>(0xDB);
inline constexpr char LightShade = static_cast<char>(0xB0);
inline constexpr char MidShade   = static_cast<char>(0xB1);
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

}  // namespace painter::tui::Glyphs

#endif  // PAINTER_TUI_GLYPH_H
