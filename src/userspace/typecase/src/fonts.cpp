/* fonts.cpp — the fonts text is set in. See fonts.h. */

#include "typecase/fonts.h"

#include <algorithm>
#include <utility>

namespace typecase {

namespace {

char Lower(char Of) noexcept { return Of >= 'A' && Of <= 'Z' ? static_cast<char>(Of - 'A' + 'a') : Of; }

/* Whether two family names are one: letters of either case alike. */
bool Same(std::string_view A, std::string_view B) noexcept
{
    if (A.size() != B.size()) return false;
    for (std::size_t I = 0; I < A.size(); I++)
        if (Lower(A[I]) != Lower(B[I])) return false;
    return true;
}

/* The names in a list apart by commas, each without the spaces and quotes
 * round it. */
std::vector<std::string_view> Names(std::string_view List)
{
    std::vector<std::string_view> Said;
    while (!List.empty()) {
        const std::size_t Comma = List.find(',');
        std::string_view  Name  = List.substr(0, Comma);
        List                    = Comma == std::string_view::npos ? std::string_view() : List.substr(Comma + 1);
        while (!Name.empty() && (Name.front() == ' ' || Name.front() == '\t')) Name.remove_prefix(1);
        while (!Name.empty() && (Name.back() == ' ' || Name.back() == '\t')) Name.remove_suffix(1);
        if (Name.size() >= 2 && (Name.front() == '"' || Name.front() == '\'') && Name.back() == Name.front())
            Name = Name.substr(1, Name.size() - 2);
        if (!Name.empty()) Said.push_back(Name);
    }
    return Said;
}

/* How far a face of weight Have is from Want, in the order CSS walks the
 * weights: from 400 to 500 up to 500 first, then down, then up past 500;
 * below 400 down first, then up; above 500 up first, then down. The less
 * the nearer. */
int Distance(std::uint16_t Want, std::uint16_t Have) noexcept
{
    const int W = Want, H = Have;
    if (W >= 400 && W <= 500) {
        if (H >= W && H <= 500) return H - W;
        if (H < W) return 1000 + (W - H);
        return 2000 + (H - 500);
    }
    if (W < 400) return H <= W ? W - H : 1000 + (H - W);
    return H >= W ? H - W : 1000 + (W - H);
}

}  // namespace

bool Has(const Font &Face, char32_t Character) noexcept
{
    const GlyphId Glyph = Face.Find(Character);
    return Glyph != 0 && Face.Readable(Glyph);
}

void Fonts::Add(Font Face) { Add(std::make_shared<const Font>(std::move(Face))); }

void Fonts::Add(std::shared_ptr<const Font> Face)
{
    if (!Face) return;
    for (Family &Each : All)
        if (Same(Each.Name, Face->Style().Family)) {
            Each.Faces.push_back(std::move(Face));
            return;
        }
    All.push_back(Family{Face->Style().Family, {std::move(Face)}});
}

void Fonts::SetFallback(std::vector<std::string> Families) { Fallback = std::move(Families); }

void Fonts::SetDefault(std::string Family) { Chosen = std::move(Family); }

std::vector<std::string> Fonts::Families() const
{
    std::vector<std::string> Said;
    for (const Family &Each : All) Said.push_back(Each.Name);
    return Said;
}

std::vector<std::shared_ptr<const Font>> Fonts::Faces() const
{
    std::vector<std::shared_ptr<const Font>> Said;
    for (const Family &Each : All) Said.insert(Said.end(), Each.Faces.begin(), Each.Faces.end());
    return Said;
}

std::string Fonts::Default() const
{
    if (!Chosen.empty() && Find(Chosen)) return Find(Chosen)->Name;
    return All.empty() ? std::string() : All.front().Name;
}

const Fonts::Family *Fonts::Find(std::string_view Name) const noexcept
{
    for (const Family &Each : All)
        if (Same(Each.Name, Name)) return &Each;
    return nullptr;
}

FaceChoice Fonts::Nearest(const Family &Of, std::uint16_t Weight, bool Italic)
{
    const bool AnyLeaning =
        std::any_of(Of.Faces.begin(), Of.Faces.end(), [&](const auto &Face) { return Face->Style().Italic == Italic; });
    const std::shared_ptr<const Font> *Best         = nullptr;
    int                                BestDistance = 0;
    for (const auto &Face : Of.Faces) {
        if (AnyLeaning && Face->Style().Italic != Italic) continue;
        const int Far = Distance(Weight, Face->Style().Weight);
        if (!Best || Far < BestDistance) Best = &Face, BestDistance = Far;
    }
    if (!Best) return {};
    const FontStyle &Is = (*Best)->Style();
    return FaceChoice{*Best, Weight >= 600 && Is.Weight < 600, Italic && !Is.Italic};
}

std::vector<FaceChoice> Fonts::Choices(std::string_view Families, std::uint16_t Weight, bool Italic) const
{
    std::vector<const Family *> Order;
    const auto Take = [&](const Family *Of) {
        if (Of && std::find(Order.begin(), Order.end(), Of) == Order.end()) Order.push_back(Of);
    };
    for (std::string_view Name : Names(Families)) Take(Find(Name));
    for (const std::string &Name : Fallback) Take(Find(Name));
    if (!All.empty()) Take(Find(Default()));
    for (const Family &Each : All) Take(&Each);

    std::vector<FaceChoice> Said;
    Said.reserve(Order.size());
    for (const Family *Of : Order) Said.push_back(Nearest(*Of, Weight, Italic));
    return Said;
}

FaceChoice Fonts::Choose(std::string_view Families, std::uint16_t Weight, bool Italic) const
{
    const std::vector<FaceChoice> All = Choices(Families, Weight, Italic);
    return All.empty() ? FaceChoice{} : All.front();
}

FaceChoice Fonts::ChooseFor(char32_t Character, std::string_view Families, std::uint16_t Weight, bool Italic) const
{
    for (const FaceChoice &Each : Choices(Families, Weight, Italic))
        if (Has(*Each.Face, Character)) return Each;
    return {};
}

}  // namespace typecase
