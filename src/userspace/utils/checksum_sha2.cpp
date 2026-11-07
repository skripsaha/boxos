/* SHA-384 and SHA-512 (FIPS 180-4): one compression of 128-byte blocks, the
 * two differing only in where they start and how much of the end they say.
 * SHA-256 is BoxLib's own (checksum_kinds.cpp), kept to one copy in the
 * system. */

#include "checksum.h"

namespace checksum {

namespace {

constexpr std::uint64_t Rounds[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

constexpr std::uint64_t Start512[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

constexpr std::uint64_t Start384[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
    0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
};

template <std::size_t DigestBytes>
class Sha512Family final : public Blocked<Sha512Family<DigestBytes>, 128> {
    using Base = Blocked<Sha512Family<DigestBytes>, 128>;

public:
    explicit Sha512Family(const std::uint64_t *Start)
    {
        for (unsigned I = 0; I < 8; I++) State[I] = Start[I];
    }

    void Compress(const std::uint8_t *Blocks, std::size_t Count)
    {
        for (; Count != 0; Blocks += 128, Count--) {
            std::uint64_t Words[80];
            for (unsigned I = 0; I < 16; I++) Words[I] = LoadBe64(Blocks + 8 * I);
            for (unsigned I = 16; I < 80; I++) {
                std::uint64_t Low  = Rotr(Words[I - 15], 1) ^ Rotr(Words[I - 15], 8) ^ (Words[I - 15] >> 7);
                std::uint64_t High = Rotr(Words[I - 2], 19) ^ Rotr(Words[I - 2], 61) ^ (Words[I - 2] >> 6);
                Words[I] = Words[I - 16] + Low + Words[I - 7] + High;
            }

            std::uint64_t A = State[0], B = State[1], C = State[2], D = State[3];
            std::uint64_t E = State[4], F = State[5], G = State[6], H = State[7];
            for (unsigned I = 0; I < 80; I++) {
                std::uint64_t One = H + (Rotr(E, 14) ^ Rotr(E, 18) ^ Rotr(E, 41)) + (G ^ (E & (F ^ G))) +
                                    Rounds[I] + Words[I];
                std::uint64_t Two = (Rotr(A, 28) ^ Rotr(A, 34) ^ Rotr(A, 39)) + ((A & B) | (C & (A | B)));
                H = G;
                G = F;
                F = E;
                E = D + One;
                D = C;
                C = B;
                B = A;
                A = One + Two;
            }
            State[0] += A;
            State[1] += B;
            State[2] += C;
            State[3] += D;
            State[4] += E;
            State[5] += F;
            State[6] += G;
            State[7] += H;
        }
    }

    std::string Finish() override
    {
        std::uint64_t Bits = this->Total * 8;
        this->PadWithLength(16);
        StoreBe64(this->Block + 112, this->Total >> 61);
        StoreBe64(this->Block + 120, Bits);
        Compress(this->Block, 1);
        std::uint8_t Out[64];
        for (unsigned I = 0; I < 8; I++) StoreBe64(Out + 8 * I, State[I]);
        return Hex(Out, DigestBytes);
    }

private:
    std::uint64_t State[8];
};

}

std::unique_ptr<Hasher> NewSha384() { return std::make_unique<Sha512Family<48>>(Start384); }
std::unique_ptr<Hasher> NewSha512() { return std::make_unique<Sha512Family<64>>(Start512); }

}
