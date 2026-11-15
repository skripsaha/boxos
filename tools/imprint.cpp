/* imprint — the system's characters drawn ahead of time, for the hands that
 * cannot read a font (boxos_imprint.h): made by every build from the font.
 *
 *     imprint FONT.ttf OUT.h
 *
 * OUT is written whole or not at all: beside it first, then moved over it,
 * so a build cut short never leaves half an imprint for the next to take. */

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "imprint_make.h"

int main(int Count, char **Words)
{
    if (Count != 3) {
        std::fprintf(stderr, "usage: imprint FONT.ttf OUT.h\n");
        return 2;
    }
    std::ifstream In(Words[1], std::ios::binary);
    if (!In) {
        std::fprintf(stderr, "imprint: %s cannot be read\n", Words[1]);
        return 1;
    }
    std::vector<std::uint8_t> Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    std::expected<MadeImprint, std::string> Made = MakeImprint(std::move(Bytes));
    if (!Made) {
        std::fprintf(stderr, "imprint: %s is no font this reads: %s\n", Words[1], Made.error().c_str());
        return 1;
    }

    const std::string Name = std::filesystem::path(Words[1]).filename().string();
    const std::string Side = std::string(Words[2]) + ".new";
    {
        std::ofstream Out(Side, std::ios::binary | std::ios::trunc);
        Out << ImprintHeader(*Made, Name);
        if (!Out.flush()) {
            std::fprintf(stderr, "imprint: %s cannot be written\n", Side.c_str());
            return 1;
        }
    }
    std::error_code Refused;
    std::filesystem::rename(Side, Words[2], Refused);
    if (Refused) {
        std::fprintf(stderr, "imprint: %s cannot take the place of %s\n", Side.c_str(), Words[2]);
        return 1;
    }
    std::printf("imprint: %zu characters of %s, %zu bytes of pixels\n", Made->Characters.size(), Name.c_str(),
                Made->Pixels.size());
    return 0;
}
