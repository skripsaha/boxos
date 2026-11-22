/* request.cpp — the words of the line read into a Request. */

#include "request.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>

#include "box/cxx/whose.h"
#include "box/file.h"

namespace random_pick {

namespace {

// Fewer digits than a whole number may have, so that the width of a range
// always fits one.
constexpr std::size_t MostDigits = 18;

std::optional<std::int64_t> Whole(std::string_view Word)
{
    const bool Below = !Word.empty() && Word.front() == '-';
    if (Below) Word.remove_prefix(1);
    if (Word.empty() || Word.size() > MostDigits) return std::nullopt;
    std::int64_t Value = 0;
    for (const char Digit : Word) {
        if (Digit < '0' || Digit > '9') return std::nullopt;
        Value = Value * 10 + (Digit - '0');
    }
    return Below ? -Value : Value;
}

std::string NameOf(file_id_t File)
{
    FileMetadata Facts;
    if (FileMetadataRead(box::tagfs::my_tags, File, &Facts) != 0) return {};
    std::string Name = Facts.Filename ? Facts.Filename : "";
    FileMetadataRelease(&Facts);
    return Name;
}

// How many and how, when the words begin by saying so; the place after it.
std::expected<std::size_t, std::string> ReadHow(const std::vector<std::string> &Words, Request &Asked)
{
    if (Words.empty()) return 0;
    if (Words[0] == "shuffle") {
        Asked.All       = true;
        Asked.Different = true;
        return 1;
    }
    const std::optional<std::int64_t> Count = Whole(Words[0]);
    if (!Count || Words.size() < 2) return 0;
    const bool        Different = Words[1] == "different";
    const std::size_t Of        = Different ? 2 : 1;
    if (Words.size() <= Of || Words[Of] != "of") return 0;
    if (*Count < 1) return std::unexpected(std::format("{} of — there is nothing to draw", Words[0]));
    Asked.Count     = static_cast<std::uint64_t>(*Count);
    Asked.Different = Different;
    return Of + 1;
}

std::expected<void, std::string> ReadFrom(const std::vector<std::string> &Rest, const std::vector<file_id_t> &Files,
                                          Bounds Fallback, Request &Asked)
{
    if (!Files.empty()) {
        if (!Rest.empty())
            return std::unexpected("files and words together: choose among the files, or among the words, not both");
        Asked.From  = Kind::Files;
        Asked.Files = Files;
        for (const file_id_t File : Files) Asked.Names.push_back(NameOf(File));
        return {};
    }
    if (Rest.empty()) {
        if (Fallback.Low > Fallback.High)
            return std::unexpected(std::format("the range set in the settings begins above where it ends "
                                               "(range least {}, range most {})",
                                               Fallback.Low, Fallback.High));
        Asked.Low  = Fallback.Low;
        Asked.High = Fallback.High;
        return {};
    }
    std::vector<std::int64_t> Numbers;
    for (const std::string &Word : Rest)
        if (const std::optional<std::int64_t> One = Whole(Word)) Numbers.push_back(*One);
    if (Numbers.size() == Rest.size() && Numbers.size() <= 2) {
        if (Numbers.size() == 1 && Numbers[0] < 1)
            return std::unexpected(
                std::format("{} is no range from 1: say both ends, as in random {} 1", Rest[0], Rest[0]));
        Asked.Low  = Numbers.size() == 1 ? 1 : std::min(Numbers[0], Numbers[1]);
        Asked.High = Numbers.size() == 1 ? Numbers[0] : std::max(Numbers[0], Numbers[1]);
        return {};
    }
    Asked.From  = Kind::Words;
    Asked.Names = Rest;
    return {};
}

}  // namespace

std::uint64_t Request::Pool() const noexcept
{
    if (From == Kind::Range) return static_cast<std::uint64_t>(High) - static_cast<std::uint64_t>(Low) + 1;
    return Names.size();
}

std::expected<Request, std::string> Read(const std::vector<std::string> &Words, const std::vector<file_id_t> &Files,
                                         Bounds Fallback)
{
    Request                                  Asked;
    const std::expected<std::size_t, std::string> At = ReadHow(Words, Asked);
    if (!At) return std::unexpected(At.error());
    const std::vector<std::string> Rest(Words.begin() + static_cast<std::ptrdiff_t>(*At), Words.end());
    if (const std::expected<void, std::string> From = ReadFrom(Rest, Files, Fallback, Asked); !From)
        return std::unexpected(From.error());

    const std::uint64_t Pool = Asked.Pool();
    if (Asked.All) Asked.Count = Pool;
    if (Asked.Count > MostDrawn)
        return std::unexpected(std::format("{} at once are more than it draws ({} at most)", Asked.Count, MostDrawn));
    if (Asked.Different && Asked.Count > Pool)
        return std::unexpected(
            std::format("{} different cannot be drawn: there are only {} to choose among", Asked.Count, Pool));
    return Asked;
}

}  // namespace random_pick
