/* The small sums: CRC-32 (IEEE 802.3, the one zip uses), MD5, SHA-1 and
 * BLAKE2b. */

#include <array>

#include "checksum.h"

namespace checksum {

namespace {

using CrcTables = std::array<std::array<std::uint32_t, 256>, 8>;

/* Table 0 is the byte at a time; table K is that byte followed by K zero
 * bytes, which lets eight bytes be folded in by eight independent lookups. */
constexpr CrcTables MakeCrcTables()
{
    CrcTables Tables{};
    for (std::uint32_t Byte = 0; Byte < 256; Byte++) {
        std::uint32_t Value = Byte;
        for (int Bit = 0; Bit < 8; Bit++) Value = Value & 1 ? 0xEDB88320u ^ Value >> 1 : Value >> 1;
        Tables[0][Byte] = Value;
    }
    for (std::size_t K = 1; K < 8; K++)
        for (std::size_t Byte = 0; Byte < 256; Byte++)
            Tables[K][Byte] = Tables[0][Tables[K - 1][Byte] & 255] ^ Tables[K - 1][Byte] >> 8;
    return Tables;
}

constexpr CrcTables Crc = MakeCrcTables();

class Crc32 final : public Hasher {
public:
    void Add(const void *Data, std::size_t Length) override
    {
        const auto *At = static_cast<const std::uint8_t *>(Data);
        std::uint32_t Value = State;
        for (; Length >= 8; At += 8, Length -= 8) {
            std::uint32_t Low  = LoadLe32(At) ^ Value;
            std::uint32_t High = LoadLe32(At + 4);
            Value = Crc[7][Low & 255] ^ Crc[6][Low >> 8 & 255] ^ Crc[5][Low >> 16 & 255] ^ Crc[4][Low >> 24] ^
                    Crc[3][High & 255] ^ Crc[2][High >> 8 & 255] ^ Crc[1][High >> 16 & 255] ^ Crc[0][High >> 24];
        }
        for (; Length != 0; At++, Length--) Value = Crc[0][(Value ^ *At) & 255] ^ Value >> 8;
        State = Value;
    }

    std::string Finish() override
    {
        std::uint8_t Out[4];
        StoreBe32(Out, ~State);
        return Hex(Out, 4);
    }

private:
    std::uint32_t State = 0xFFFFFFFFu;
};

constexpr std::uint32_t Md5Constants[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

constexpr unsigned Md5Shifts[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};

class Md5 final : public Blocked<Md5, 64> {
public:
    void Compress(const std::uint8_t *Blocks, std::size_t Count)
    {
        for (; Count != 0; Blocks += 64, Count--) {
            std::uint32_t Words[16];
            for (unsigned I = 0; I < 16; I++) Words[I] = LoadLe32(Blocks + 4 * I);

            std::uint32_t A = State[0], B = State[1], C = State[2], D = State[3];
            for (unsigned I = 0; I < 64; I++) {
                std::uint32_t Mix;
                unsigned      Word;
                switch (I >> 4) {
                case 0:  Mix = D ^ (B & (C ^ D)); Word = I; break;
                case 1:  Mix = C ^ (D & (B ^ C)); Word = (5 * I + 1) & 15; break;
                case 2:  Mix = B ^ C ^ D;         Word = (3 * I + 5) & 15; break;
                default: Mix = C ^ (B | ~D);      Word = (7 * I) & 15; break;
                }
                std::uint32_t Next = B + Rotl(A + Mix + Md5Constants[I] + Words[Word], Md5Shifts[I >> 4][I & 3]);
                A = D;
                D = C;
                C = B;
                B = Next;
            }
            State[0] += A;
            State[1] += B;
            State[2] += C;
            State[3] += D;
        }
    }

    std::string Finish() override
    {
        std::uint64_t Bits = Total * 8;
        PadWithLength(8);
        StoreLe64(Block + 56, Bits);
        Compress(Block, 1);
        std::uint8_t Out[16];
        for (unsigned I = 0; I < 4; I++) StoreLe32(Out + 4 * I, State[I]);
        return Hex(Out, 16);
    }

private:
    std::uint32_t State[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
};

class Sha1 final : public Blocked<Sha1, 64> {
public:
    void Compress(const std::uint8_t *Blocks, std::size_t Count)
    {
        for (; Count != 0; Blocks += 64, Count--) {
            std::uint32_t Words[80];
            for (unsigned I = 0; I < 16; I++) Words[I] = LoadBe32(Blocks + 4 * I);
            for (unsigned I = 16; I < 80; I++)
                Words[I] = Rotl(Words[I - 3] ^ Words[I - 8] ^ Words[I - 14] ^ Words[I - 16], 1);

            std::uint32_t A = State[0], B = State[1], C = State[2], D = State[3], E = State[4];
            for (unsigned I = 0; I < 80; I++) {
                std::uint32_t Mix, Constant;
                if (I < 20)      { Mix = D ^ (B & (C ^ D));          Constant = 0x5a827999u; }
                else if (I < 40) { Mix = B ^ C ^ D;                  Constant = 0x6ed9eba1u; }
                else if (I < 60) { Mix = (B & C) | (D & (B | C));    Constant = 0x8f1bbcdcu; }
                else             { Mix = B ^ C ^ D;                  Constant = 0xca62c1d6u; }
                std::uint32_t Next = Rotl(A, 5) + Mix + E + Constant + Words[I];
                E = D;
                D = C;
                C = Rotl(B, 30);
                B = A;
                A = Next;
            }
            State[0] += A;
            State[1] += B;
            State[2] += C;
            State[3] += D;
            State[4] += E;
        }
    }

    std::string Finish() override
    {
        std::uint64_t Bits = Total * 8;
        PadWithLength(8);
        StoreBe64(Block + 56, Bits);
        Compress(Block, 1);
        std::uint8_t Out[20];
        for (unsigned I = 0; I < 5; I++) StoreBe32(Out + 4 * I, State[I]);
        return Hex(Out, 20);
    }

private:
    std::uint32_t State[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
};

constexpr std::uint64_t Blake2bStart[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

constexpr std::uint8_t Blake2bOrder[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

/* BLAKE2b-512 without a key. Its last block is marked as the last, so a full
 * block is held back until more input proves it is not: this one cannot use
 * Blocked, which compresses a block the moment it is whole. */
class Blake2b final : public Hasher {
public:
    Blake2b()
    {
        for (unsigned I = 0; I < 8; I++) State[I] = Blake2bStart[I];
        State[0] ^= 0x01010000ull ^ 64;
    }

    void Add(const void *Data, std::size_t Length) override
    {
        const auto *At = static_cast<const std::uint8_t *>(Data);
        while (Length != 0) {
            if (Held == 128) {
                Compress(Block, 128, false);
                Held = 0;
            }
            if (Held == 0) {
                while (Length > 128) {
                    Compress(At, 128, false);
                    At += 128;
                    Length -= 128;
                }
            }
            std::size_t Take = 128 - Held < Length ? 128 - Held : Length;
            std::memcpy(Block + Held, At, Take);
            Held += Take;
            At += Take;
            Length -= Take;
        }
    }

    std::string Finish() override
    {
        std::memset(Block + Held, 0, 128 - Held);
        Compress(Block, Held, true);
        std::uint8_t Out[64];
        for (unsigned I = 0; I < 8; I++) StoreLe64(Out + 8 * I, State[I]);
        return Hex(Out, 64);
    }

private:
    static void Mix(std::uint64_t *V, unsigned A, unsigned B, unsigned C, unsigned D, std::uint64_t X, std::uint64_t Y)
    {
        V[A] = V[A] + V[B] + X;
        V[D] = Rotr(V[D] ^ V[A], 32);
        V[C] = V[C] + V[D];
        V[B] = Rotr(V[B] ^ V[C], 24);
        V[A] = V[A] + V[B] + Y;
        V[D] = Rotr(V[D] ^ V[A], 16);
        V[C] = V[C] + V[D];
        V[B] = Rotr(V[B] ^ V[C], 63);
    }

    /* Bytes is how much of the block is input: the counter counts input, not
     * padding. */
    void Compress(const std::uint8_t *Source, std::size_t Bytes, bool Last)
    {
        std::uint64_t Words[16], V[16];
        for (unsigned I = 0; I < 16; I++) Words[I] = LoadLe64(Source + 8 * I);

        Counter += Bytes;
        for (unsigned I = 0; I < 8; I++) {
            V[I]     = State[I];
            V[I + 8] = Blake2bStart[I];
        }
        V[12] ^= Counter;
        if (Last) V[14] = ~V[14];

        for (unsigned Round = 0; Round < 12; Round++) {
            const std::uint8_t *S = Blake2bOrder[Round % 10];
            Mix(V, 0, 4, 8, 12, Words[S[0]], Words[S[1]]);
            Mix(V, 1, 5, 9, 13, Words[S[2]], Words[S[3]]);
            Mix(V, 2, 6, 10, 14, Words[S[4]], Words[S[5]]);
            Mix(V, 3, 7, 11, 15, Words[S[6]], Words[S[7]]);
            Mix(V, 0, 5, 10, 15, Words[S[8]], Words[S[9]]);
            Mix(V, 1, 6, 11, 12, Words[S[10]], Words[S[11]]);
            Mix(V, 2, 7, 8, 13, Words[S[12]], Words[S[13]]);
            Mix(V, 3, 4, 9, 14, Words[S[14]], Words[S[15]]);
        }
        for (unsigned I = 0; I < 8; I++) State[I] ^= V[I] ^ V[I + 8];
    }

    std::uint64_t State[8];
    std::uint64_t Counter = 0;
    std::uint8_t  Block[128];
    std::size_t   Held = 0;
};

}

std::unique_ptr<Hasher> NewCrc32() { return std::make_unique<Crc32>(); }
std::unique_ptr<Hasher> NewMd5() { return std::make_unique<Md5>(); }
std::unique_ptr<Hasher> NewSha1() { return std::make_unique<Sha1>(); }
std::unique_ptr<Hasher> NewBlake2b() { return std::make_unique<Blake2b>(); }

}
