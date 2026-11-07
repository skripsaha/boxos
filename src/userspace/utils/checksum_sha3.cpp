/* SHA-3-256 and SHA-3-512 (FIPS 202): the Keccak sponge, 1600 bits of state,
 * 24 rounds of the permutation per block taken in. */

#include "checksum.h"

namespace checksum {

namespace {

constexpr std::uint64_t RoundConstants[24] = {
    0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull, 0x8000000080008000ull,
    0x000000000000808bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
    0x000000000000008aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull,
    0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull, 0x8000000000008003ull,
    0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800aull, 0x800000008000000aull,
    0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull,
};

/* Where each lane goes in the pi step, and by how much it turns in rho, in
 * the order the lanes are walked. */
constexpr unsigned PiLane[24]   = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};
constexpr unsigned RhoTurn[24]  = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};

void Permute(std::uint64_t *Lane)
{
    for (unsigned Round = 0; Round < 24; Round++) {
        std::uint64_t Column[5];
        for (unsigned X = 0; X < 5; X++)
            Column[X] = Lane[X] ^ Lane[X + 5] ^ Lane[X + 10] ^ Lane[X + 15] ^ Lane[X + 20];
        for (unsigned X = 0; X < 5; X++) {
            std::uint64_t Mix = Column[(X + 4) % 5] ^ Rotl(Column[(X + 1) % 5], 1);
            for (unsigned Y = 0; Y < 25; Y += 5) Lane[Y + X] ^= Mix;
        }

        std::uint64_t Carried = Lane[1];
        for (unsigned I = 0; I < 24; I++) {
            std::uint64_t Next = Lane[PiLane[I]];
            Lane[PiLane[I]]    = Rotl(Carried, RhoTurn[I]);
            Carried            = Next;
        }

        for (unsigned Y = 0; Y < 25; Y += 5) {
            std::uint64_t Row[5];
            for (unsigned X = 0; X < 5; X++) Row[X] = Lane[Y + X];
            for (unsigned X = 0; X < 5; X++) Lane[Y + X] = Row[X] ^ (~Row[(X + 1) % 5] & Row[(X + 2) % 5]);
        }
        Lane[0] ^= RoundConstants[Round];
    }
}

/* Rate is the bytes of a block, DigestBytes the sum's length: the block is
 * 200 minus twice the sum, so a longer sum leaves less room for input. */
template <std::size_t DigestBytes>
class Sha3 final : public Blocked<Sha3<DigestBytes>, 200 - 2 * DigestBytes> {
    static constexpr std::size_t Rate = 200 - 2 * DigestBytes;

public:
    void Compress(const std::uint8_t *Blocks, std::size_t Count)
    {
        for (; Count != 0; Blocks += Rate, Count--) {
            for (std::size_t I = 0; I < Rate / 8; I++) Lane[I] ^= LoadLe64(Blocks + 8 * I);
            Permute(Lane);
        }
    }

    std::string Finish() override
    {
        std::memset(this->Block + this->Held, 0, Rate - this->Held);
        this->Block[this->Held] ^= 0x06;
        this->Block[Rate - 1] ^= 0x80;
        Compress(this->Block, 1);
        std::uint8_t Out[DigestBytes];
        for (std::size_t I = 0; I < DigestBytes / 8; I++) StoreLe64(Out + 8 * I, Lane[I]);
        return Hex(Out, DigestBytes);
    }

private:
    std::uint64_t Lane[25] = {};
};

}

std::unique_ptr<Hasher> NewSha3_256() { return std::make_unique<Sha3<32>>(); }
std::unique_ptr<Hasher> NewSha3_512() { return std::make_unique<Sha3<64>>(); }

}
