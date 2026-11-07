/* qr_code.cpp — a QR Code made of bytes (qr_code.h).
 *
 * Laid out as the standard has it and as Project Nayuki's qrcodegen reads
 * it: function patterns first, the codewords zigzagged into what is left,
 * then each mask tried with its format bits and scored. */

#include <algorithm>
#include <array>
#include <cstdlib>

#include "qr_code.h"

namespace {

constexpr int LeastVersion = 1;
constexpr int MostVersion  = 40;

/* ISO/IEC 18004 Table 9, by level (L, M, Q, H) then version; 0 is unused. */
constexpr std::int8_t EccPerBlock[4][41] = {
    {-1, 7,  10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
     28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
     26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30,
     28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28,
     30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
};
constexpr std::int8_t BlockCount[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,
     8,  9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,  9,  10, 10, 11, 13, 14, 16,
     17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1, 1,  1,  2,  2,  4,  4,  6,  6,  8,  8,  8,  10, 12, 16, 12, 17, 16, 18, 21, 20,
     23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1, 1,  1,  2,  4,  4,  4,  5,  6,  8,  8,  11, 11, 16, 16, 18, 16, 19, 21, 25, 25,
     25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
};
/* The two bits each level is written with in the format information. */
constexpr int LevelBits[4] = {1, 0, 3, 2};

constexpr int PenaltyRun     = 3;
constexpr int PenaltyBlock   = 3;
constexpr int PenaltyFinder  = 40;
constexpr int PenaltyBalance = 10;

int Index(QrLevel Level)
{
    return static_cast<int>(Level);
}

bool Bit(long Value, int At)
{
    return ((Value >> At) & 1) != 0;
}

/* The modules of a version left for data and error correction once every
 * function pattern has taken its own. */
int RawModules(int Version)
{
    int Modules = (16 * Version + 128) * Version + 64;
    if (Version >= 2) {
        int Aligns = Version / 7 + 2;
        Modules -= (25 * Aligns - 10) * Aligns - 55;
        if (Version >= 7) Modules -= 36;
    }
    return Modules;
}

int DataCodewords(int Version, QrLevel Level)
{
    return RawModules(Version) / 8 - EccPerBlock[Index(Level)][Version] * BlockCount[Index(Level)][Version];
}

/* Bits the byte-mode segment of `Count` bytes takes: mode, count, bytes. */
int SegmentBits(int Version, std::size_t Count)
{
    return 4 + (Version <= 9 ? 8 : 16) + static_cast<int>(Count) * 8;
}

/* Multiplication in GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1. */
std::uint8_t Multiply(std::uint8_t X, std::uint8_t Y)
{
    int Z = 0;
    for (int I = 7; I >= 0; I--) {
        Z = (Z << 1) ^ ((Z >> 7) * 0x11D);
        Z ^= ((Y >> I) & 1) * X;
    }
    return static_cast<std::uint8_t>(Z);
}

/* The generator polynomial of `Degree`, highest power left out. */
std::vector<std::uint8_t> Divisor(int Degree)
{
    std::vector<std::uint8_t> Result(static_cast<std::size_t>(Degree), 0);
    Result.back()     = 1;
    std::uint8_t Root = 1;
    for (int I = 0; I < Degree; I++) {
        for (std::size_t J = 0; J < Result.size(); J++) {
            Result[J] = Multiply(Result[J], Root);
            if (J + 1 < Result.size()) Result[J] ^= Result[J + 1];
        }
        Root = Multiply(Root, 0x02);
    }
    return Result;
}

std::vector<std::uint8_t> Remainder(const std::vector<std::uint8_t> &Data, const std::vector<std::uint8_t> &By)
{
    std::vector<std::uint8_t> Result(By.size(), 0);
    for (std::uint8_t Byte : Data) {
        std::uint8_t Factor = Byte ^ Result.front();
        Result.erase(Result.begin());
        Result.push_back(0);
        for (std::size_t I = 0; I < Result.size(); I++) Result[I] ^= Multiply(By[I], Factor);
    }
    return Result;
}

/* The bytes as codewords: the segment, the terminator, padding to a byte and
 * then 0xEC 0x11 to the version's capacity. */
std::vector<std::uint8_t> Codewords(std::string_view Bytes, int Version, QrLevel Level)
{
    std::vector<bool> Bits;
    auto Put = [&](long Value, int Count) {
        for (int I = Count - 1; I >= 0; I--) Bits.push_back(Bit(Value, I));
    };
    int Capacity = DataCodewords(Version, Level) * 8;
    Put(0x4, 4);
    Put(static_cast<long>(Bytes.size()), Version <= 9 ? 8 : 16);
    for (char Ch : Bytes) Put(static_cast<unsigned char>(Ch), 8);
    Put(0, std::min(4, Capacity - static_cast<int>(Bits.size())));
    Put(0, (8 - static_cast<int>(Bits.size()) % 8) % 8);
    for (std::uint8_t Pad = 0xEC; static_cast<int>(Bits.size()) < Capacity; Pad ^= 0xEC ^ 0x11) Put(Pad, 8);

    std::vector<std::uint8_t> Words(Bits.size() / 8, 0);
    for (std::size_t I = 0; I < Bits.size(); I++)
        if (Bits[I]) Words[I >> 3] |= static_cast<std::uint8_t>(1 << (7 - (I & 7)));
    return Words;
}

/* The codewords cut into blocks, each with its error correction, and
 * interleaved: a column of first bytes, of second bytes, and so on. */
std::vector<std::uint8_t> WithCorrection(const std::vector<std::uint8_t> &Data, int Version, QrLevel Level)
{
    int Blocks   = BlockCount[Index(Level)][Version];
    int EccLen   = EccPerBlock[Index(Level)][Version];
    int Raw      = RawModules(Version) / 8;
    int Short    = Blocks - Raw % Blocks;
    int ShortLen = Raw / Blocks;

    std::vector<std::uint8_t>              By = Divisor(EccLen);
    std::vector<std::vector<std::uint8_t>> Cut;
    std::size_t                            Taken = 0;
    for (int I = 0; I < Blocks; I++) {
        std::size_t Length = static_cast<std::size_t>(ShortLen - EccLen + (I < Short ? 0 : 1));
        std::vector<std::uint8_t> Block(Data.begin() + static_cast<std::ptrdiff_t>(Taken),
                                        Data.begin() + static_cast<std::ptrdiff_t>(Taken + Length));
        Taken += Length;
        std::vector<std::uint8_t> Ecc = Remainder(Block, By);
        if (I < Short) Block.push_back(0);
        Block.insert(Block.end(), Ecc.begin(), Ecc.end());
        Cut.push_back(std::move(Block));
    }

    std::vector<std::uint8_t> Result;
    for (std::size_t I = 0; I < Cut.front().size(); I++)
        for (int J = 0; J < Blocks; J++)
            if (I != static_cast<std::size_t>(ShortLen - EccLen) || J >= Short) Result.push_back(Cut[static_cast<std::size_t>(J)][I]);
    return Result;
}

/* Where the alignment patterns' centres are, along either axis. */
std::vector<int> AlignmentPlaces(int Version)
{
    if (Version == 1) return {};
    int Count = Version / 7 + 2;
    int Step  = Version == 32 ? 26 : (Version * 4 + Count * 2 + 1) / (Count * 2 - 2) * 2;
    std::vector<int> Places{6};
    for (int Place = Version * 4 + 17 - 7; static_cast<int>(Places.size()) < Count; Place -= Step)
        Places.insert(Places.begin() + 1, Place);
    return Places;
}

/* The finder-like runs a history of run lengths ends with: dark light dark
 * dark dark light dark, 1:1:3:1:1, with four light on either side. */
int FinderRuns(const std::array<int, 7> &Runs)
{
    int  N    = Runs[1];
    bool Core = N > 0 && Runs[2] == N && Runs[3] == N * 3 && Runs[4] == N && Runs[5] == N;
    return (Core && Runs[0] >= N * 4 && Runs[6] >= N ? 1 : 0) + (Core && Runs[6] >= N * 4 && Runs[0] >= N ? 1 : 0);
}

}

int QrCode::Capacity(int Version, QrLevel Level)
{
    int Bits = DataCodewords(Version, Level) * 8 - 4 - (Version <= 9 ? 8 : 16);
    return std::max(0, Bits / 8);
}

QrCode::QrCode(int Ver, QrLevel Lev)
    : Version(Ver), Size(Ver * 4 + 17), Level(Lev),
      Modules(static_cast<std::size_t>(Size) * Size, false), Fixed(Modules.size(), false)
{
}

std::optional<QrCode> QrCode::Make(std::string_view Bytes, QrLevel Least, int Most)
{
    Most = std::clamp(Most, LeastVersion, MostVersion);
    int Version = LeastVersion;
    while (SegmentBits(Version, Bytes.size()) > DataCodewords(Version, Least) * 8) {
        if (++Version > Most) return std::nullopt;
    }
    /* The version is the least the bytes fit; raise the error correction as
     * far as it still holds them. */
    QrLevel Level = Least;
    for (QrLevel Higher : {QrLevel::Medium, QrLevel::Quartile, QrLevel::High})
        if (Index(Higher) > Index(Level) && SegmentBits(Version, Bytes.size()) <= DataCodewords(Version, Higher) * 8)
            Level = Higher;

    QrCode Code(Version, Level);
    Code.DrawPatterns();
    Code.DrawCodewords(WithCorrection(Codewords(Bytes, Version, Level), Version, Level));

    long Best = -1;
    for (int Try = 0; Try < 8; Try++) {
        Code.ApplyMask(Try);
        Code.DrawFormat(Try);
        long Score = Code.Penalty();
        if (Best < 0 || Score < Best) {
            Best      = Score;
            Code.Mask = Try;
        }
        Code.ApplyMask(Try);
    }
    Code.ApplyMask(Code.Mask);
    Code.DrawFormat(Code.Mask);
    return Code;
}

void QrCode::Set(int X, int Y, bool IsDark)
{
    std::size_t At = static_cast<std::size_t>(Y) * Size + X;
    Modules[At]    = IsDark;
    Fixed[At]      = true;
}

void QrCode::DrawPatterns()
{
    for (int I = 0; I < Size; I++) {
        Set(6, I, I % 2 == 0);
        Set(I, 6, I % 2 == 0);
    }
    DrawFinder(3, 3);
    DrawFinder(Size - 4, 3);
    DrawFinder(3, Size - 4);

    std::vector<int> Places = AlignmentPlaces(Version);
    std::size_t      Count  = Places.size();
    for (std::size_t I = 0; I < Count; I++)
        for (std::size_t J = 0; J < Count; J++)
            if (!((I == 0 && J == 0) || (I == 0 && J == Count - 1) || (I == Count - 1 && J == 0)))
                DrawAlignment(Places[I], Places[J]);

    DrawFormat(0);   /* reserved now, written for real once the mask is chosen */
    DrawVersion();
}

void QrCode::DrawFinder(int X, int Y)
{
    for (int Dy = -4; Dy <= 4; Dy++) {
        for (int Dx = -4; Dx <= 4; Dx++) {
            int Ring = std::max(std::abs(Dx), std::abs(Dy)), Xx = X + Dx, Yy = Y + Dy;
            if (Xx >= 0 && Xx < Size && Yy >= 0 && Yy < Size) Set(Xx, Yy, Ring != 2 && Ring != 4);
        }
    }
}

void QrCode::DrawAlignment(int X, int Y)
{
    for (int Dy = -2; Dy <= 2; Dy++)
        for (int Dx = -2; Dx <= 2; Dx++) Set(X + Dx, Y + Dy, std::max(std::abs(Dx), std::abs(Dy)) != 1);
}

/* Fifteen bits — the level, the mask, ten of BCH — twice: round the top
 * left finder, and split between the other two, with the dark module. */
void QrCode::DrawFormat(int WhichMask)
{
    long Data = LevelBits[Index(Level)] << 3 | WhichMask;
    long Rem  = Data;
    for (int I = 0; I < 10; I++) Rem = (Rem << 1) ^ ((Rem >> 9) * 0x537);
    long Bits = (Data << 10 | Rem) ^ 0x5412;

    for (int I = 0; I <= 5; I++) Set(8, I, Bit(Bits, I));
    Set(8, 7, Bit(Bits, 6));
    Set(8, 8, Bit(Bits, 7));
    Set(7, 8, Bit(Bits, 8));
    for (int I = 9; I < 15; I++) Set(14 - I, 8, Bit(Bits, I));

    for (int I = 0; I < 8; I++) Set(Size - 1 - I, 8, Bit(Bits, I));
    for (int I = 8; I < 15; I++) Set(8, Size - 15 + I, Bit(Bits, I));
    Set(8, Size - 8, true);
}

/* Eighteen bits of version and BCH, from version 7 on, by two corners. */
void QrCode::DrawVersion()
{
    if (Version < 7) return;
    long Rem = Version;
    for (int I = 0; I < 12; I++) Rem = (Rem << 1) ^ ((Rem >> 11) * 0x1F25);
    long Bits = static_cast<long>(Version) << 12 | Rem;
    for (int I = 0; I < 18; I++) {
        int A = Size - 11 + I % 3, B = I / 3;
        Set(A, B, Bit(Bits, I));
        Set(B, A, Bit(Bits, I));
    }
}

/* Up and down columns two wide, from the right, skipping the vertical timing
 * pattern; every module no pattern took gets the next bit. */
void QrCode::DrawCodewords(const std::vector<std::uint8_t> &Words)
{
    std::size_t Next = 0, Total = Words.size() * 8;
    for (int Right = Size - 1; Right >= 1; Right -= 2) {
        if (Right == 6) Right = 5;
        for (int Vert = 0; Vert < Size; Vert++) {
            for (int J = 0; J < 2; J++) {
                int  X      = Right - J;
                bool Upward = ((Right + 1) & 2) == 0;
                int  Y      = Upward ? Size - 1 - Vert : Vert;
                std::size_t At = static_cast<std::size_t>(Y) * Size + X;
                if (Fixed[At] || Next >= Total) continue;
                Modules[At] = Bit(Words[Next >> 3], 7 - static_cast<int>(Next & 7));
                Next++;
            }
        }
    }
}

/* XOR: applying a mask twice takes it off again. */
void QrCode::ApplyMask(int WhichMask)
{
    for (int Y = 0; Y < Size; Y++) {
        for (int X = 0; X < Size; X++) {
            bool Flip = false;
            switch (WhichMask) {
            case 0: Flip = (X + Y) % 2 == 0; break;
            case 1: Flip = Y % 2 == 0; break;
            case 2: Flip = X % 3 == 0; break;
            case 3: Flip = (X + Y) % 3 == 0; break;
            case 4: Flip = (X / 3 + Y / 2) % 2 == 0; break;
            case 5: Flip = X * Y % 2 + X * Y % 3 == 0; break;
            case 6: Flip = (X * Y % 2 + X * Y % 3) % 2 == 0; break;
            default: Flip = ((X + Y) % 2 + X * Y % 3) % 2 == 0; break;
            }
            std::size_t At = static_cast<std::size_t>(Y) * Size + X;
            if (Flip && !Fixed[At]) Modules[At] = !Modules[At];
        }
    }
}

/* Runs of five and more of a colour, and finder-like patterns, along every
 * row or every column. The edge counts as light, as the quiet zone is. */
long QrCode::RunPenalty(bool Rows) const
{
    long Result = 0;
    for (int Line = 0; Line < Size; Line++) {
        bool           Colour = false;
        int            Run    = 0;
        std::array<int, 7> History{};
        auto Remember = [&](int Length) {
            if (History[0] == 0) Length += Size;
            for (int I = 6; I > 0; I--) History[static_cast<std::size_t>(I)] = History[static_cast<std::size_t>(I - 1)];
            History[0] = Length;
        };
        for (int Along = 0; Along < Size; Along++) {
            bool Here = Rows ? Dark(Along, Line) : Dark(Line, Along);
            if (Here == Colour) {
                Run++;
                if (Run == 5) Result += PenaltyRun;
                else if (Run > 5) Result++;
                continue;
            }
            Remember(Run);
            if (!Colour) Result += FinderRuns(History) * PenaltyFinder;
            Colour = Here;
            Run    = 1;
        }
        if (Colour) {
            Remember(Run);
            Run = 0;
        }
        Remember(Run + Size);
        Result += FinderRuns(History) * PenaltyFinder;
    }
    return Result;
}

long QrCode::Penalty() const
{
    long Result = RunPenalty(true) + RunPenalty(false);
    for (int Y = 0; Y + 1 < Size; Y++) {
        for (int X = 0; X + 1 < Size; X++) {
            bool C = Dark(X, Y);
            if (C == Dark(X + 1, Y) && C == Dark(X, Y + 1) && C == Dark(X + 1, Y + 1)) Result += PenaltyBlock;
        }
    }
    long DarkCount = std::count(Modules.begin(), Modules.end(), true);
    long Total     = static_cast<long>(Size) * Size;
    long Steps     = (std::labs(DarkCount * 20 - Total * 10) + Total - 1) / Total - 1;
    return Result + Steps * PenaltyBalance;
}
