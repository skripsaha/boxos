/* The clipboard side of edit. */

#include "clipboard.h"

#include <algorithm>
#include <format>

namespace edit {

namespace {

std::string Why(const box::error &Refusal)
{
    return std::string(Refusal.message());
}

}  // namespace

std::expected<std::uint64_t, std::string> PutClip(std::string_view Bytes)
{
    box::result<std::uint64_t> Number = box::clipboard::put(Bytes);
    if (!Number) return std::unexpected(std::format("the clipboard would not keep it: {}", Why(Number.error())));
    return *Number;
}

/* The newest clip can go between the look and the take — dropped, or its
 * last take spent by another program — and then the newest is looked for
 * again. */
std::expected<std::optional<Taken>, std::string> TakeNewest()
{
    for (;;) {
        box::result<std::vector<box::clip>> Newest = box::clipboard::list(1);
        if (!Newest) return std::unexpected(std::format("the clipboard would not say what it holds: {}", Why(Newest.error())));
        if (Newest->empty()) return std::optional<Taken>();

        box::result<std::string> Bytes = box::clipboard::take(Newest->front());
        if (Bytes) return std::optional<Taken>(Taken{Newest->front().number, std::move(*Bytes)});
        if (Bytes.error().code() != box::errc::object_not_found)
            return std::unexpected(std::format("the newest clip would not come: {}", Why(Bytes.error())));
    }
}

std::expected<std::vector<box::clip>, std::string> ListClips()
{
    box::result<std::vector<box::clip>> Clips = box::clipboard::list();
    if (!Clips) return std::unexpected(std::format("the clipboard would not say what it holds: {}", Why(Clips.error())));
    return std::move(*Clips);
}

std::expected<std::string, std::string> TakeWhole(const box::clip &Which)
{
    box::result<std::string> Bytes = box::clipboard::take(Which);
    if (!Bytes) {
        if (Bytes.error().code() == box::errc::object_not_found) return std::unexpected("that clip is gone");
        return std::unexpected(std::format("that clip would not come: {}", Why(Bytes.error())));
    }
    return std::move(*Bytes);
}

std::optional<std::string> TakeHead(const box::clip &Which, std::size_t Most)
{
    std::string              Head(static_cast<std::size_t>(std::min<std::uint64_t>(Which.bytes, Most)), '\0');
    box::result<std::size_t> Came = box::clipboard::take(Which.number, 0, Head);
    if (!Came) return std::nullopt;
    Head.resize(*Came);
    return Head;
}

std::optional<std::string> DropClip(std::uint64_t Number)
{
    box::status Dropped = box::clipboard::drop(Number);
    if (!Dropped && Dropped.error().code() != box::errc::object_not_found)
        return std::format("the clipboard would not let it go: {}", Why(Dropped.error()));
    return std::nullopt;
}

std::string FirstLine(std::string_view Bytes)
{
    std::string Line(Bytes.substr(0, Bytes.find('\n')));
    if (!Line.empty() && Line.back() == '\r') Line.pop_back();
    for (char &Ch : Line)
        if (static_cast<unsigned char>(Ch) < 0x20 || Ch == 0x7F) Ch = ' ';
    return Line;
}

}  // namespace edit
