/* bookmark — where an installation stopped (install.h).
 *
 * Kept as text, a line to a fact — "next: carrying the person's files" a
 * person reading the file understands — in install's own file on the volume
 * install runs from (keep.h): found by install's own words, no other
 * program's, and settled on the medium before the write returns, so a cut
 * after it leaves the bookmark as it was written. An installation done lets
 * its bookmark go: the file is left empty.
 */

#include "install.h"

#include <cstdlib>

#include "../keep.h"

namespace install {

namespace {

const Keep File = { BookmarkName, "install,bookmark" };

constexpr const char *Steps[] = { "begin", "table", "bios", "esp", "boxos", "lay", "shipped", "mine", "boot", "done" };

std::string Hex(const std::array<std::uint8_t, 16> &Bytes)
{
    static const char Digits[] = "0123456789abcdef";
    std::string       Text;
    for (std::uint8_t B : Bytes) {
        Text += Digits[B >> 4];
        Text += Digits[B & 15];
    }
    return Text;
}

bool Unhex(std::string_view Text, std::array<std::uint8_t, 16> &Bytes)
{
    auto Digit = [](char C) { return C >= '0' && C <= '9' ? C - '0' : C >= 'a' && C <= 'f' ? C - 'a' + 10 : -1; };
    if (Text.size() != 32) return false;
    for (std::size_t I = 0; I < 16; I++) {
        int High = Digit(Text[2 * I]), Low = Digit(Text[2 * I + 1]);
        if (High < 0 || Low < 0) return false;
        Bytes[I] = static_cast<std::uint8_t>(High << 4 | Low);
    }
    return true;
}

std::uint64_t Number(std::string_view Text)
{
    std::uint64_t N = 0;
    for (char C : Text) {
        if (C < '0' || C > '9') return N;
        N = N * 10u + static_cast<std::uint64_t>(C - '0');
    }
    return N;
}

}  // namespace

std::optional<Bookmark> BookmarkRead()
{
    std::string Text;
    if (KeepRead(File, Text) || Text.empty()) return std::nullopt;
    Bookmark Mark;
    bool     Stepped = false;
    for (std::size_t At = 0; At < Text.size();) {
        std::size_t End  = Text.find('\n', At);
        std::string Line = Text.substr(At, End == std::string::npos ? std::string::npos : End - At);
        At               = End == std::string::npos ? Text.size() : End + 1;
        std::size_t Colon = Line.find(": ");
        if (Colon == std::string::npos) continue;
        std::string Key = Line.substr(0, Colon), Value = Line.substr(Colon + 2);
        if (Key == "disk") Mark.Wanted.DiskName = Value;
        else if (Key == "disk blocks") Mark.Wanted.DiskBlocks = Number(Value);
        else if (Key == "disk guid") Mark.Wanted.GuidKnown = Unhex(Value, Mark.Wanted.DiskGuid);
        else if (Key == "where") Mark.Wanted.Where = Value == "beside" ? Place::Beside : Place::Whole;
        else if (Key == "what") Mark.Wanted.What = Value == "mine" ? Take::Mine : Take::Fresh;
        else if (Key == "without") Mark.Wanted.Without = Value;
        else if (Key == "name") Mark.Wanted.Name = Value;
        else if (Key == "first") Mark.Wanted.First = Value == "yes";
        else if (Key == "stick") Mark.Wanted.Stick = Value == "yes";
        else if (Key == "esp entry") Mark.EspEntry = static_cast<std::uint16_t>(Number(Value));
        else if (Key == "esp shared") Mark.EspShared = Value == "yes";
        else if (Key == "bios entry") Mark.BiosEntry = static_cast<std::uint16_t>(Number(Value));
        else if (Key == "boxos entry") Mark.BoxosEntry = static_cast<std::uint16_t>(Number(Value));
        else if (Key == "volume") Mark.VolumeKnown = Unhex(Value, Mark.Volume);
        else if (Key == "carried") Mark.Carried = static_cast<std::uint32_t>(Number(Value));
        else if (Key == "next")
            for (std::size_t S = 0; S < sizeof(Steps) / sizeof(Steps[0]); S++)
                if (Value == Steps[S]) {
                    Mark.Next = static_cast<Step>(S);
                    Stepped   = true;
                }
    }
    if (!Stepped || Mark.Next == Step::Done || Mark.Wanted.DiskName.empty()) return std::nullopt;
    return Mark;
}

bool BookmarkWrite(const Bookmark &Mark, std::string &Why)
{
    std::string Text;
    if (Mark.Next != Step::Done) {
        const Plan &P = Mark.Wanted;
        Text = "install bookmark — where an installation of BoxOS stopped\n";
        Text += "next: " + std::string(Steps[static_cast<std::size_t>(Mark.Next)]) + "\n";
        Text += "disk: " + P.DiskName + "\n";
        Text += "disk blocks: " + std::to_string(P.DiskBlocks) + "\n";
        if (P.GuidKnown) Text += "disk guid: " + Hex(P.DiskGuid) + "\n";
        Text += std::string("where: ") + (P.Where == Place::Beside ? "beside" : "whole") + "\n";
        Text += std::string("what: ") + (P.What == Take::Mine ? "mine" : "fresh") + "\n";
        if (!P.Without.empty()) Text += "without: " + P.Without + "\n";
        Text += "name: " + P.Name + "\n";
        Text += std::string("first: ") + (P.First ? "yes" : "no") + "\n";
        Text += std::string("stick: ") + (P.Stick ? "yes" : "no") + "\n";
        Text += "esp entry: " + std::to_string(Mark.EspEntry) + "\n";
        Text += std::string("esp shared: ") + (Mark.EspShared ? "yes" : "no") + "\n";
        Text += "bios entry: " + std::to_string(Mark.BiosEntry) + "\n";
        Text += "boxos entry: " + std::to_string(Mark.BoxosEntry) + "\n";
        if (Mark.VolumeKnown) Text += "volume: " + Hex(Mark.Volume) + "\n";
        Text += "carried: " + std::to_string(Mark.Carried) + "\n";
    }
    std::optional<std::string> Refusal = KeepWrite(File, Text);
    if (Refusal) Why = *Refusal;
    return !Refusal;
}

}  // namespace install
