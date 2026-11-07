#ifndef PAINTER_CORE_COLOR_H
#define PAINTER_CORE_COLOR_H

#include <cstdint>

namespace painter::core {

class Color {
public:
    constexpr Color() noexcept = default;

    static constexpr Color Rgb(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue) noexcept
    {
        return Color((static_cast<std::uint32_t>(Red) << 16) |
                     (static_cast<std::uint32_t>(Green) << 8) |
                     static_cast<std::uint32_t>(Blue));
    }

    static constexpr Color FromRaw(std::uint32_t Raw) noexcept { return Color(Raw & 0x00FFFFFFu); }

    constexpr std::uint32_t Raw() const noexcept { return Value; }
    constexpr std::uint8_t  Red() const noexcept { return static_cast<std::uint8_t>((Value >> 16) & 0xFFu); }
    constexpr std::uint8_t  Green() const noexcept { return static_cast<std::uint8_t>((Value >> 8) & 0xFFu); }
    constexpr std::uint8_t  Blue() const noexcept { return static_cast<std::uint8_t>(Value & 0xFFu); }

    static constexpr Color Mix(Color A, Color B, std::uint8_t Weight) noexcept
    {
        const std::uint32_t Inverse = 255u - Weight;
        const std::uint32_t Red     = (static_cast<std::uint32_t>(A.Red()) * Inverse +
                                   static_cast<std::uint32_t>(B.Red()) * Weight) /
                                  255u;
        const std::uint32_t Green   = (static_cast<std::uint32_t>(A.Green()) * Inverse +
                                     static_cast<std::uint32_t>(B.Green()) * Weight) /
                                    255u;
        const std::uint32_t Blue    = (static_cast<std::uint32_t>(A.Blue()) * Inverse +
                                    static_cast<std::uint32_t>(B.Blue()) * Weight) /
                                   255u;
        return Rgb(static_cast<std::uint8_t>(Red), static_cast<std::uint8_t>(Green),
                   static_cast<std::uint8_t>(Blue));
    }

    friend constexpr bool operator==(Color, Color) noexcept = default;

private:
    explicit constexpr Color(std::uint32_t Raw) noexcept : Value(Raw & 0x00FFFFFFu) {}

    std::uint32_t Value = 0;
};

namespace Colors {
inline constexpr Color Black     = Color::Rgb(0x00, 0x00, 0x00);
inline constexpr Color White     = Color::Rgb(0xFF, 0xFF, 0xFF);
inline constexpr Color DarkGray  = Color::Rgb(0x55, 0x55, 0x55);
inline constexpr Color LightGray = Color::Rgb(0xAA, 0xAA, 0xAA);
inline constexpr Color Red       = Color::Rgb(0xE0, 0x40, 0x40);
inline constexpr Color Green     = Color::Rgb(0x40, 0xC0, 0x40);
inline constexpr Color Blue      = Color::Rgb(0x40, 0x80, 0xE0);
inline constexpr Color Yellow    = Color::Rgb(0xE0, 0xC0, 0x40);
inline constexpr Color Cyan      = Color::Rgb(0x40, 0xC0, 0xC0);
inline constexpr Color Magenta   = Color::Rgb(0xC0, 0x40, 0xC0);
inline constexpr Color DarkRed   = Color::Rgb(0xA0, 0x00, 0x00);
inline constexpr Color DarkGreen = Color::Rgb(0x00, 0x80, 0x00);
inline constexpr Color DarkBlue  = Color::Rgb(0x00, 0x00, 0xA0);
inline constexpr Color Brown     = Color::Rgb(0xA0, 0x60, 0x00);
inline constexpr Color DarkCyan  = Color::Rgb(0x00, 0x80, 0x80);
inline constexpr Color DarkMagenta = Color::Rgb(0x80, 0x00, 0x80);
inline constexpr Color Berry     = Color::Rgb(0xE0, 0x4F, 0x90);
inline constexpr Color Ocean     = Color::Rgb(0x10, 0x80, 0xC0);
inline constexpr Color Leaf      = Color::Rgb(0x60, 0xC0, 0x30);
inline constexpr Color Amber     = Color::Rgb(0xFF, 0xB0, 0x40);
inline constexpr Color Violet    = Color::Rgb(0x90, 0x60, 0xFF);
inline constexpr Color Teal      = Color::Rgb(0x00, 0xB0, 0xA0);
inline constexpr Color Coral     = Color::Rgb(0xFF, 0x80, 0x70);
inline constexpr Color Slate     = Color::Rgb(0x60, 0x70, 0x80);
}  // namespace Colors

}  // namespace painter::core

#endif  // PAINTER_CORE_COLOR_H
