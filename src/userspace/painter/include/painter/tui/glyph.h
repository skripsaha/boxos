#ifndef PAINTER_TUI_GLYPH_H
#define PAINTER_TUI_GLYPH_H

/* The characters painter draws pictures with, by what they are for. A cell
 * holds a character, never a glyph's number; which glyph shows it is the
 * display's to choose. */

namespace painter::tui::Glyphs {

inline constexpr char32_t UpperHalf  = U'▀';   /* ▀ */
inline constexpr char32_t LowerHalf  = U'▄';   /* ▄ */
inline constexpr char32_t Full       = U'█';   /* █ */
inline constexpr char32_t LightShade = U'░';   /* ░ */
inline constexpr char32_t MidShade   = U'▒';   /* ▒ */
inline constexpr char32_t DenseShade = U'▓';   /* ▓ */
inline constexpr char32_t Dot        = U'·';   /* · */
inline constexpr char32_t Bullet     = U'•';   /* • */
inline constexpr char32_t Tick       = U'√';   /* √ */
inline constexpr char32_t Left       = U'←';   /* ← */
inline constexpr char32_t Right      = U'→';   /* → */
inline constexpr char32_t Up         = U'↑';   /* ↑ */
inline constexpr char32_t Down       = U'↓';   /* ↓ */
inline constexpr char32_t Pointer    = U'►';   /* ► */
inline constexpr char32_t LeftHalf   = U'▌';   /* ▌ */
inline constexpr char32_t RightHalf  = U'▐';   /* ▐ */

/* The box rules a frame is drawn with, and the marks of a line that goes on. */
inline constexpr char32_t RuleAcross = U'─';   /* ─ */
inline constexpr char32_t DoubleRule = U'═';   /* ═ */
inline constexpr char32_t RuleDown   = U'│';   /* │ */
inline constexpr char32_t CornerTL   = U'┌';   /* ┌ */
inline constexpr char32_t CornerTR   = U'┐';   /* ┐ */
inline constexpr char32_t CornerBL   = U'└';   /* └ */
inline constexpr char32_t CornerBR   = U'┘';   /* ┘ */
inline constexpr char32_t TeeLeft    = U'├';   /* ├ */
inline constexpr char32_t TeeRight   = U'┤';   /* ┤ */
inline constexpr char32_t MoreLeft   = U'«';   /* « a line goes on to the left */
inline constexpr char32_t MoreRight  = U'»';   /* » and to the right */

}  // namespace painter::tui::Glyphs

#endif  // PAINTER_TUI_GLYPH_H
