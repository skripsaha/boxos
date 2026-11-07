#ifndef QR_CODE_H
#define QR_CODE_H

/* qr_code — a QR Code made of bytes, as ISO/IEC 18004 spells it: the bytes
 * in byte mode, the smallest version they fit, the error correction raised
 * as far as that version allows, Reed-Solomon over GF(256) in interleaved
 * blocks, and of the eight masks the one with the least penalty. It takes
 * nothing of BoxOS. */

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

enum class QrLevel : std::uint8_t { Low, Medium, Quartile, High };

class QrCode {
public:
    /* The code for `Bytes` at version MostVersion or less, error correction
     * at least `Least`; none when they do not fit. */
    static std::optional<QrCode> Make(std::string_view Bytes, QrLevel Least, int MostVersion);

    /* The most bytes a code of `Version` holds at `Level`. */
    static int Capacity(int Version, QrLevel Level);

    bool Dark(int X, int Y) const { return Modules[static_cast<std::size_t>(Y) * Size + X]; }

    int     Version = 1;
    int     Size    = 21;   /* modules a side */
    QrLevel Level   = QrLevel::Low;
    int     Mask    = 0;

private:
    QrCode(int Version, QrLevel Level);

    void Set(int X, int Y, bool IsDark);
    void DrawPatterns();
    void DrawFinder(int X, int Y);
    void DrawAlignment(int X, int Y);
    void DrawFormat(int WhichMask);
    void DrawVersion();
    void DrawCodewords(const std::vector<std::uint8_t> &Words);
    void ApplyMask(int WhichMask);
    long Penalty() const;
    long RunPenalty(bool Rows) const;

    std::vector<bool> Modules;
    std::vector<bool> Fixed;   /* the function patterns: neither data nor masked */
};

#endif
