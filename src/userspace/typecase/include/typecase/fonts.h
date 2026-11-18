#ifndef TYPECASE_FONTS_H
#define TYPECASE_FONTS_H

/* The fonts text is set in: faces added, gathered into families by the
 * family's name each font gives. A style asks for families by name — one,
 * or several apart by commas, the most wanted first, as CSS's font-family —
 * a weight and whether it leans; in each family the face nearest that is
 * found as CSS finds it: leaning or upright as asked when the family has
 * such a face, then the nearest weight, and made bold or leaning when the
 * family has no face that is (glyph_look.h). A character the face asked
 * has not is asked of the next family named, then of the fallback families
 * — the system's — then of the default family, then of any face that has
 * it; a character no face has is drawn as its code. Text never vanishes.
 *
 * Faces are shared: a text laid keeps the faces it was set in alive as long
 * as it lives. A book is asked by one strand at a time. */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "typecase/font.h"

namespace typecase {

/* The face a style comes to in one family, and what it is made into. */
struct FaceChoice {
    std::shared_ptr<const Font> Face;            /* none: there is no face */
    bool                        Bold  = false;   /* made bold */
    bool                        Slant = false;   /* made to lean */
};

class Fonts {
public:
    /* Face added to its family: the family the font names, or "" for a font
     * that names none. */
    void Add(Font Face);
    void Add(std::shared_ptr<const Font> Face);

    /* The families asked after those a style names, in order. */
    void SetFallback(std::vector<std::string> Families);
    /* The family a style that names none is set in, and the last asked
     * before any face at all; the first family added when none is set. */
    void SetDefault(std::string Family);

    bool                     Empty() const noexcept { return All.empty(); }
    std::vector<std::string> Families() const;
    std::string              Default() const;

    /* Every family's face for the style — the families Families names that
     * are here, in its order, then the fallback families, then the default
     * family, then every other family — each family once. Empty only when
     * the book is. */
    std::vector<FaceChoice> Choices(std::string_view Families, std::uint16_t Weight, bool Italic) const;
    /* The first of the choices; none when the book is empty. */
    FaceChoice Choose(std::string_view Families, std::uint16_t Weight, bool Italic) const;
    /* The first of the choices whose face has Character; none when no face has it. */
    FaceChoice ChooseFor(char32_t Character, std::string_view Families, std::uint16_t Weight, bool Italic) const;

private:
    struct Family {
        std::string                              Name;
        std::vector<std::shared_ptr<const Font>> Faces;
    };

    const Family *Find(std::string_view Name) const noexcept;
    static FaceChoice Nearest(const Family &Of, std::uint16_t Weight, bool Italic);

    std::vector<Family>      All;
    std::vector<std::string> Fallback;
    std::string              Chosen;   /* the default, as set */
};

/* Whether Face has a glyph for Character that reads. */
bool Has(const Font &Face, char32_t Character) noexcept;

}  // namespace typecase

#endif  // TYPECASE_FONTS_H
