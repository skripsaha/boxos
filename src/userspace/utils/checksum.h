/* checksum — the sums, apart from the program that shows them.
 *
 * Nothing here knows BoxOS: a sum is bytes in, a lowercase hex word out, so
 * the same code runs on the host against the reference implementations.
 */

#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace checksum {

class Hasher {
public:
    virtual ~Hasher() = default;

    /* Called once per chunk, never per byte. */
    virtual void Add(const void *Data, std::size_t Length) = 0;

    /* The sum as lowercase hex; the hasher is spent after it. */
    virtual std::string Finish() = 0;
};

constexpr std::uint32_t Rotl(std::uint32_t Value, unsigned By) { return Value << By | Value >> (32 - By); }
constexpr std::uint32_t Rotr(std::uint32_t Value, unsigned By) { return Value >> By | Value << (32 - By); }
constexpr std::uint64_t Rotl(std::uint64_t Value, unsigned By) { return Value << By | Value >> (64 - By); }
constexpr std::uint64_t Rotr(std::uint64_t Value, unsigned By) { return Value >> By | Value << (64 - By); }

inline std::uint32_t LoadLe32(const std::uint8_t *At)
{
    return std::uint32_t(At[0]) | std::uint32_t(At[1]) << 8 | std::uint32_t(At[2]) << 16 | std::uint32_t(At[3]) << 24;
}

inline std::uint32_t LoadBe32(const std::uint8_t *At)
{
    return std::uint32_t(At[3]) | std::uint32_t(At[2]) << 8 | std::uint32_t(At[1]) << 16 | std::uint32_t(At[0]) << 24;
}

inline std::uint64_t LoadLe64(const std::uint8_t *At)
{
    return std::uint64_t(LoadLe32(At)) | std::uint64_t(LoadLe32(At + 4)) << 32;
}

inline std::uint64_t LoadBe64(const std::uint8_t *At)
{
    return std::uint64_t(LoadBe32(At)) << 32 | std::uint64_t(LoadBe32(At + 4));
}

inline void StoreLe32(std::uint8_t *At, std::uint32_t Value)
{
    for (unsigned I = 0; I < 4; I++) At[I] = std::uint8_t(Value >> (8 * I));
}

inline void StoreBe32(std::uint8_t *At, std::uint32_t Value)
{
    for (unsigned I = 0; I < 4; I++) At[3 - I] = std::uint8_t(Value >> (8 * I));
}

inline void StoreLe64(std::uint8_t *At, std::uint64_t Value)
{
    for (unsigned I = 0; I < 8; I++) At[I] = std::uint8_t(Value >> (8 * I));
}

inline void StoreBe64(std::uint8_t *At, std::uint64_t Value)
{
    for (unsigned I = 0; I < 8; I++) At[7 - I] = std::uint8_t(Value >> (8 * I));
}

inline std::string Hex(const std::uint8_t *Bytes, std::size_t Count)
{
    static constexpr char Digits[] = "0123456789abcdef";
    std::string Text(Count * 2, '0');
    for (std::size_t I = 0; I < Count; I++) {
        Text[2 * I]     = Digits[Bytes[I] >> 4];
        Text[2 * I + 1] = Digits[Bytes[I] & 15];
    }
    return Text;
}

/* A hasher that takes its input in whole blocks: the bytes of a block that is
 * not whole yet wait here, and Derived::Compress(Blocks, Count) is called
 * once for as many whole blocks as there are, so the hot loop is a direct
 * call and never a virtual one. */
template <class Derived, std::size_t BlockBytes>
class Blocked : public Hasher {
public:
    void Add(const void *Data, std::size_t Length) final
    {
        const auto *At = static_cast<const std::uint8_t *>(Data);
        Total += Length;
        if (Held != 0) {
            std::size_t Take = BlockBytes - Held < Length ? BlockBytes - Held : Length;
            std::memcpy(Block + Held, At, Take);
            Held += Take;
            At += Take;
            Length -= Take;
            if (Held < BlockBytes) return;
            Self().Compress(Block, 1);
            Held = 0;
        }
        std::size_t Whole = Length / BlockBytes;
        if (Whole != 0) {
            Self().Compress(At, Whole);
            At += Whole * BlockBytes;
            Length -= Whole * BlockBytes;
        }
        std::memcpy(Block, At, Length);
        Held = Length;
    }

protected:
    /* The padding of MD5, SHA-1 and SHA-2: a 1 bit, zeros, and Reserved bytes
     * at the block's end for the length, which the caller writes. */
    void PadWithLength(std::size_t Reserved)
    {
        Block[Held++] = 0x80;
        if (Held > BlockBytes - Reserved) {
            std::memset(Block + Held, 0, BlockBytes - Held);
            Self().Compress(Block, 1);
            Held = 0;
        }
        std::memset(Block + Held, 0, BlockBytes - Held);
    }

    Derived &Self() { return *static_cast<Derived *>(this); }

    std::uint8_t  Block[BlockBytes];
    std::size_t   Held  = 0;
    std::uint64_t Total = 0;
};

std::unique_ptr<Hasher> NewCrc32();
std::unique_ptr<Hasher> NewMd5();
std::unique_ptr<Hasher> NewSha1();
std::unique_ptr<Hasher> NewSha256();
std::unique_ptr<Hasher> NewSha384();
std::unique_ptr<Hasher> NewSha512();
std::unique_ptr<Hasher> NewSha3_256();
std::unique_ptr<Hasher> NewSha3_512();
std::unique_ptr<Hasher> NewBlake2b();

struct Kind {
    const char *Name;
    std::size_t HexLength;
    bool        Weak;
    std::unique_ptr<Hasher> (*Make)();
};

/* The kind a typed name means, whatever its case and with or without the
 * dashes; null when it is none. */
const Kind *FindKind(std::string_view Typed);

/* The kind whose name is a slip or two from Typed, or null. */
const Kind *NearestKind(std::string_view Typed);

/* Every kind's name, in a line, in the order they are told. */
std::string KindNames();

}

#endif
