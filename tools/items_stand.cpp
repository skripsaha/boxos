/* items stand — the display's items (src/userspace/display: item_wire,
 * item_tree, item_look, item_draw, item_reception, compositor), its cells and
 * frame (cell_grid, cells_draw, console_model, mirror, frame) and the
 * program's side of the language (painter/gui/changes.h), the very sources
 * BoxOS builds, built for the host and held to what they promise
 * (tools/items_stand.sh runs it):
 *
 *   items_stand checks        every check
 *   items_stand sheet OUT     a scene of items composed, a PPM to look at
 */

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

#include "items_stand.h"

namespace stand {

void Tally::Check(bool Holds, const std::string &What)
{
    (Holds ? Held : Failed)++;
    std::printf("%s %s\n", Holds ? "held" : "FAILED", What.c_str());
}

std::uint32_t Random()
{
    static std::uint32_t State = 0x1234567u;
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
}

Bench::Bench(items::ProgramState &Program, std::uint32_t Ring)
    : Memory(sizeof(ItemAreaHeader) + Ring + ITEM_EVENT_BYTES, 0), RingBytes(Ring), Area(Program)
{
    Head             = reinterpret_cast<ItemAreaHeader *>(Memory.data());
    Head->Magic      = ITEM_AREA_MAGIC;
    Head->Bytes      = Ring;
    Head->EventBytes = ITEM_EVENT_BYTES;
}

items::Reading Bench::Step(painter::gui::Changes &Changes)
{
    std::uint8_t *Ring  = Memory.data() + sizeof(ItemAreaHeader);
    std::uint64_t Write = Head->Write;
    (void)Changes.Flush(Ring, RingBytes, Write, Head->Read, ++Steps);
    Head->Write = Write;
    items::Listener Tell;
    Tell.Refused = [this](std::uint32_t Item, std::uint16_t, std::uint16_t, error_t Reason) {
        Refusals.push_back(Reason);
        RefusedItems.push_back(Item);
    };
    Tell.BlockChanged = [this](std::uint32_t Block, const ItemBlockChanged &) { Area.Items.BlockChanged(Block); };
    return items::ReadSteps(*Head, std::span<const std::uint8_t>(Ring, RingBytes), Area.Items, Tell);
}

namespace {

std::vector<std::uint8_t> Load(const char *Path)
{
    std::ifstream In(Path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
}

const typecase::Fonts &TreeBook()
{
    static const std::unique_ptr<typecase::Fonts> Book = [] {
        auto Made = std::make_unique<typecase::Fonts>();
        for (const char *Face : {"Inter-Regular", "Inter-Italic", "Inter-SemiBold", "Inter-Bold", "JetBrainsMonoNL-Regular"})
            if (auto Read = typecase::Font::Read(Load((std::string("src/userspace/typecase/fonts/") + Face + ".ttf").c_str())))
                Made->Add(std::move(*Read));
        Made->SetDefault("Inter");
        return Made;
    }();
    return *Book;
}

}  // namespace

items::ProgramState HostProgram(const items::Theme &Look, float Scale)
{
    items::ProgramState Made;
    Made.From.Look  = &Look;
    Made.From.Scale = Scale;
    Made.From.Book  = []() -> const typecase::Fonts & { return TreeBook(); };
    return Made;
}

}  // namespace stand

int main(int Count, char **Words)
{
    if (Count == 3 && std::strcmp(Words[1], "sheet") == 0) return stand::WriteSheet(Words[2]) ? 0 : 1;
    if (Count != 2 || std::strcmp(Words[1], "checks") != 0) {
        std::fprintf(stderr, "usage: items_stand checks | sheet OUT.ppm\n");
        return 2;
    }
    stand::Tally Tally;
    stand::CheckWire(Tally);
    stand::CheckCompose(Tally);
    stand::CheckCells(Tally);
    std::printf("items stand: %d held, %d failed\n", Tally.Held, Tally.Failed);
    return Tally.Failed ? 1 : 0;
}
