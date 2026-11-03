/* The kinds of sum by name, and SHA-256, which is BoxLib's. */

#include <algorithm>
#include <vector>

#include "box/sha256.h"

#include "checksum.h"

namespace checksum {

namespace {

class Sha256Sum final : public Hasher {
public:
    Sha256Sum() { sha256_begin(&State); }

    void Add(const void *Data, std::size_t Length) override { sha256_add(&State, Data, Length); }

    std::string Finish() override
    {
        std::uint8_t Out[SHA256_BYTES];
        sha256_end(&State, Out);
        return Hex(Out, SHA256_BYTES);
    }

private:
    ::Sha256 State;
};

/* MD5 and SHA-1 are marked weak: anyone can make two files they sum alike. */
const Kind Kinds[] = {
    {"crc32",    8,   false, NewCrc32},
    {"md5",      32,  true,  NewMd5},
    {"sha1",     40,  true,  NewSha1},
    {"sha256",   64,  false, NewSha256},
    {"sha384",   96,  false, NewSha384},
    {"sha512",   128, false, NewSha512},
    {"sha3-256", 64,  false, NewSha3_256},
    {"sha3-512", 128, false, NewSha3_512},
    {"blake2b",  128, false, NewBlake2b},
};

/* Lowercase, with the dashes and underscores left out, so sha-256, SHA256 and
 * sha_256 are one name. */
std::string Plain(std::string_view Name)
{
    std::string Result;
    for (char Letter : Name) {
        if (Letter == '-' || Letter == '_') continue;
        Result += Letter >= 'A' && Letter <= 'Z' ? char(Letter - 'A' + 'a') : Letter;
    }
    return Result;
}

std::size_t Distance(std::string_view From, std::string_view To)
{
    std::vector<std::size_t> Row(To.size() + 1);
    for (std::size_t J = 0; J <= To.size(); J++) Row[J] = J;
    for (std::size_t I = 1; I <= From.size(); I++) {
        std::size_t Diagonal = Row[0];
        Row[0] = I;
        for (std::size_t J = 1; J <= To.size(); J++) {
            std::size_t Above = Row[J];
            Row[J] = std::min({Row[J] + 1, Row[J - 1] + 1, Diagonal + (From[I - 1] == To[J - 1] ? 0 : 1)});
            Diagonal = Above;
        }
    }
    return Row[To.size()];
}

}

std::unique_ptr<Hasher> NewSha256() { return std::make_unique<Sha256Sum>(); }

const Kind *FindKind(std::string_view Typed)
{
    std::string Wanted = Plain(Typed);
    for (const Kind &One : Kinds)
        if (Plain(One.Name) == Wanted) return &One;
    return nullptr;
}

const Kind *NearestKind(std::string_view Typed)
{
    std::string  Wanted = Plain(Typed);
    const Kind  *Best   = nullptr;
    std::size_t  Least  = 3;
    for (const Kind &One : Kinds) {
        std::size_t Apart = Distance(Wanted, Plain(One.Name));
        if (Apart < Least) {
            Least = Apart;
            Best  = &One;
        }
    }
    return Best;
}

std::string KindNames()
{
    std::string Names;
    for (const Kind &One : Kinds) {
        if (!Names.empty()) Names += ", ";
        Names += One.Name;
    }
    return Names;
}

}
