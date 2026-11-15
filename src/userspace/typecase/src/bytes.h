#ifndef TYPECASE_BYTES_H
#define TYPECASE_BYTES_H

/* A stretch of a font file, read big-endian as the sfnt format writes it.
 * No read leaves the stretch: one that would is answered 0. The reader of a
 * structure asks Holds before it reads one, so a 0 from past the end is
 * never mistaken for data — it only keeps a missed check from becoming a
 * read of someone else's memory. */

#include <cstddef>
#include <cstdint>
#include <span>

namespace typecase {

class Bytes {
public:
    Bytes() = default;
    explicit Bytes(std::span<const std::uint8_t> Data) noexcept : Data(Data) {}

    std::size_t Size() const noexcept { return Data.size(); }

    /* Whether Length bytes from At lie within the stretch. */
    bool Holds(std::size_t At, std::size_t Length) const noexcept
    {
        return At <= Data.size() && Length <= Data.size() - At;
    }

    /* The Length bytes from At; an empty stretch when they do not lie within. */
    Bytes Part(std::size_t At, std::size_t Length) const noexcept
    {
        return Holds(At, Length) ? Bytes(Data.subspan(At, Length)) : Bytes();
    }

    std::uint8_t U8(std::size_t At) const noexcept { return At < Data.size() ? Data[At] : 0; }

    std::uint16_t U16(std::size_t At) const noexcept
    {
        return Holds(At, 2) ? static_cast<std::uint16_t>(Data[At] << 8 | Data[At + 1]) : 0;
    }

    std::int16_t I16(std::size_t At) const noexcept { return static_cast<std::int16_t>(U16(At)); }

    std::uint32_t U32(std::size_t At) const noexcept
    {
        if (!Holds(At, 4)) return 0;
        return static_cast<std::uint32_t>(Data[At]) << 24 | static_cast<std::uint32_t>(Data[At + 1]) << 16 |
               static_cast<std::uint32_t>(Data[At + 2]) << 8 | Data[At + 3];
    }

    /* A 2.14 fixed-point number: a component's scale. */
    float F2Dot14(std::size_t At) const noexcept { return static_cast<float>(I16(At)) / 16384.0f; }

private:
    std::span<const std::uint8_t> Data;
};

}  // namespace typecase

#endif  // TYPECASE_BYTES_H
