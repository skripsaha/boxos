/* look.cpp — the map's settings, declared and read. */

#include "look.h"

#include <string>

#include "box/settings.h"
#include "box/cxx/settings.h"

BOX_SETTINGS("refresh number from 250 to 10000 milliseconds default 1000 \"how often the machine is read again\"\n"
             "view one-of owners address default owners "
             "\"what the map shows: memory laid out by who holds it, or the address space where the registry of tagged "
             "regions says what is there\"\n"
             "group one-of program kind default program "
             "\"each program apart, or all programs as one kind of memory\"\n"
             "sort one-of size age name default size \"what orders the owners: how much they hold, how long they have "
             "lived, or their name\"\n"
             "cell number from 1 to 8 pixels default 3 "
             "\"a cell's side with its gap, on a screen of pixels; the 80 by 25 text screen draws a cell as a character\"\n"
             "tint number from 20 to 100 percent default 70 \"how rich the colors made for programs are\"\n"
             "colors \"the map's own colors\"\n"
             "    page color default #0F0F0F \"behind the panels\"\n"
             "    panel color default #151515 \"inside a panel\"\n"
             "    bars color default #1E1E1E \"the lines at the top and the bottom\"\n"
             "    frame color default #464646 \"a panel's frame\"\n"
             "    title color default #EEEEEE \"titles, and the chosen owner's name\"\n"
             "    text color default #C8C8C8 \"what is said\"\n"
             "    quiet color default #7C7C7C \"what is said quietly\"\n"
             "    marked color default #FFC857 \"a marked program's mark\"\n"
             "memory \"the colors of the kinds of memory\"\n"
             "    image color default #E6A23C \"the kernel's own image\"\n"
             "    kernel color default #D2694A \"the kernel's heap, page tables and buffers\"\n"
             "    rings color default #2EC4B6 \"the rings every program talks to the system through\"\n"
             "    shared color default #B07CE8 \"windows shared between programs: Bay, Brook\"\n"
             "    device color default #8A8A8A \"devices, firmware and what no program can use\"\n"
             "    programs color default #78C26F \"every program, when they are shown as one kind\"\n"
             "    free color default #383838 \"memory nobody holds\"\n"
             "    untold color default #4C4C4C \"memory in use that the registry does not say who holds\"\n"
             "change \"the colors of what an owner gained and lost\"\n"
             "    grew color default #8BE38B \"holds more than when the map opened\"\n"
             "    shrank color default #FF6B6B \"holds less than when the map opened\"\n"
             "keys \"the map's keys\"\n"
             "    view key default ctrl+v \"the other view: by owner, or by address\"\n"
             "    group key default ctrl+g \"each program apart, or all as one kind\"\n"
             "    sort key default ctrl+s \"order the owners the next way\"\n"
             "    pause key default ctrl+p \"hold the picture still, or let it go on\"\n"
             "    help key default ctrl+? \"every key of the map\"\n");

namespace memmap {

namespace {

SortBy SortOf(const std::string &Word)
{
    return Word == "age" ? SortBy::Age : Word == "name" ? SortBy::Name : SortBy::Size;
}

}  // namespace

Look ReadLook()
{
    using box::settings;
    return Look{.RefreshMs  = static_cast<std::uint32_t>(settings::number("refresh")),
                .CellPixels = static_cast<std::uint32_t>(settings::number("cell")),
                .Tint       = static_cast<std::uint32_t>(settings::number("tint")),
                .Page       = Color::FromRaw(settings::color("colors page")),
                .Panel      = Color::FromRaw(settings::color("colors panel")),
                .Bars       = Color::FromRaw(settings::color("colors bars")),
                .Frame      = Color::FromRaw(settings::color("colors frame")),
                .Title      = Color::FromRaw(settings::color("colors title")),
                .Text       = Color::FromRaw(settings::color("colors text")),
                .Quiet      = Color::FromRaw(settings::color("colors quiet")),
                .Marked     = Color::FromRaw(settings::color("colors marked")),
                .Image      = Color::FromRaw(settings::color("memory image")),
                .Kernel     = Color::FromRaw(settings::color("memory kernel")),
                .Rings      = Color::FromRaw(settings::color("memory rings")),
                .Shared     = Color::FromRaw(settings::color("memory shared")),
                .Device     = Color::FromRaw(settings::color("memory device")),
                .Programs   = Color::FromRaw(settings::color("memory programs")),
                .Free       = Color::FromRaw(settings::color("memory free")),
                .Untold     = Color::FromRaw(settings::color("memory untold")),
                .Grew       = Color::FromRaw(settings::color("change grew")),
                .Shrank     = Color::FromRaw(settings::color("change shrank")),
                .View       = settings::choice("view") == "address" ? ViewOf::Address : ViewOf::Owners,
                .Group      = settings::choice("group") == "kind" ? GroupBy::Kind : GroupBy::Program,
                .Sort       = SortOf(settings::choice("sort")),
                .Keys       = {settings::key("keys view"), settings::key("keys group"), settings::key("keys sort"),
                               settings::key("keys pause"), settings::key("keys help")}};
}

}  // namespace memmap
