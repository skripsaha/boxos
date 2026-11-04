/* fireworks — a night over a city, and fireworks in it.
 *
 *   fireworks        the show: rockets go up of themselves, now and then a finale
 *   space            a rocket now
 *   1 - 9            a rocket of one kind: peony, chrysanthemum, ring, willow,
 *                    crossette, two colours, strobe, palm, heart
 *   f                a finale
 *   q, ESC           leave, and say how many went up
 *
 * Light is kept, not drawn: every spark adds its colour to a field of light
 * that fades a little each frame, so what moves leaves a trail and a burst
 * lights the sky round it with no more than that. The sky, the moon and the
 * city are under the light; the light is added to them. */

#include <algorithm>
#include <cmath>
#include <format>
#include <new>
#include <vector>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"

#include "stage.h"

BOX_FORM("");

namespace {

constexpr std::uint64_t FrameUs    = 33333;
constexpr float         MostStep   = 0.05f;   /* seconds a step of the sparks covers at most */
constexpr float         MostGap    = 0.25f;   /* a stall longer than this is not a leap */
constexpr float         Keeps      = 0.0012f; /* of the light, what is left after a second */
constexpr float         KeysShown  = 6.0f;
constexpr float         RocketPull = 0.5f;    /* screen heights a second, a second: a rocket's fall */
constexpr int           KindCount  = 9;

/* The night and the city are greys: the text console keeps them black. The
 * night lightens toward the horizon with a violet tint kept the same share of
 * its lightness all the way down, so no row of it is saturated enough to turn
 * blue there. */
constexpr int NightTop = 12, NightLow = 30;
const Color Building   = COLOR_RGB(0x06, 0x06, 0x08);
const Color Window     = COLOR_RGB(0xFF, 0xC8, 0x50);
const Color MoonInk    = COLOR_RGB(0xF0, 0xEC, 0xC8);
const Color StarInk    = COLOR_RGB(0xB0, 0xB8, 0xD0);
const Color SparkInk   = COLOR_RGB(0xFF, 0xA0, 0x40);
const Color BarPaper   = COLOR_RGB(0x3A, 0x3A, 0x3A);
const Color BarInk     = COLOR_RGB(0xEE, 0xEE, 0xEE);
const Color KeyInk     = COLOR_AMBER;
const Color HintInk    = COLOR_RGB(0x8C, 0x8C, 0x8C);

const Color Inks[] = {COLOR_RGB(0xFF, 0x30, 0x30), COLOR_RGB(0xFF, 0x90, 0x20), COLOR_RGB(0xFF, 0xD8, 0x40),
                      COLOR_RGB(0x40, 0xFF, 0x60), COLOR_RGB(0x30, 0x80, 0xFF), COLOR_RGB(0xC0, 0x50, 0xFF),
                      COLOR_RGB(0xFF, 0x60, 0xC0), COLOR_RGB(0x40, 0xE0, 0xFF), COLOR_RGB(0xFF, 0xFF, 0xFF)};
const Color Gold   = COLOR_RGB(0xFF, 0xC0, 0x40);

/* In the order of the keys 1 to 9. */
enum class Kind : std::uint8_t { Peony, Chrysanthemum, Ring, Willow, Crossette, TwoColour, Strobe, Palm, Heart };

struct Spark {
    float X, Y, Vx, Vy;
    float Life, Span;     /* seconds left, seconds it had */
    float Drag;           /* of its speed lost a second */
    Color Ink;
    bool  Strobe = false; /* blinks */
    bool  Splits = false; /* a crossette's star: bursts again */
    bool  Rocket = false; /* climbing; bursts at the top */
    Kind  Bursts = Kind::Peony;
};

struct Light {
    std::uint16_t R = 0, G = 0, B = 0;
};

class Show {
public:
    Stage Glass;

    void Begin()
    {
        Width  = Glass.Cols;
        Height = Glass.Rows * 2;
        Field.assign(static_cast<std::size_t>(Width) * Height, Light{});
        Pixels.assign(Field.size(), COLOR_BLACK);
        BuildSky();
    }

    void Step(float Seconds);
    void Draw(float Seconds, float Shown);
    void Launch(Kind Which, float X);
    void Finale();
    Kind AnyKind() { return static_cast<Kind>(Roll() % KindCount); }
    float AnyX() { return Between(0.12f, 0.88f) * static_cast<float>(Width); }

    unsigned Launched = 0;

private:
    std::uint64_t Roll()
    {
        Seed ^= Seed << 13;
        Seed ^= Seed >> 7;
        Seed ^= Seed << 17;
        return Seed;
    }
    float Unit() { return static_cast<float>(Roll() >> 40) / static_cast<float>(1ull << 24); }
    float Between(float Low, float High) { return Low + (High - Low) * Unit(); }
    Color AnyInk() { return Inks[Roll() % (sizeof(Inks) / sizeof(Inks[0]))]; }

    void BuildSky();
    void Burst(const Spark &Shell);
    void Shed(const Spark &From, int Count, Color Ink, float Speed, float Span, float Drag);
    void Shine(float X, float Y, Color Ink, int Strength);
    void Flash(Color Ink, int Strength);

    int                 Width = 0, Height = 0;
    std::uint64_t       Seed = cpu_rdtsc() | 1;
    std::vector<Spark>  Sparks;
    std::vector<Light>  Field;
    std::vector<Color>  Sky;
    std::vector<Color>  Pixels;
    std::vector<int>    Twinkles;     /* the stars' pixels */
    float               Wait = 0.5f;  /* seconds before the next rocket of the show */
    float               ToFinale = 0;
    float               Time = 0;
};

/* The sky once: its colour falling from dark to violet, the stars, the moon,
 * and the city along the bottom with its windows lit. */
void Show::BuildSky()
{
    Sky.assign(static_cast<std::size_t>(Width) * Height, COLOR_BLACK);
    for (int Y = 0; Y < Height; Y++) {
        int   Light = NightTop + (NightLow - NightTop) * Y / std::max(1, Height - 1);
        Color Row   = COLOR_RGB(Light, (Light * 86 + 50) / 100, (Light * 120 + 50) / 100);
        for (int X = 0; X < Width; X++) Sky[static_cast<std::size_t>(Y) * Width + X] = Row;
    }
    for (int Star = Width * Height / 160; Star > 0; Star--)
        Twinkles.push_back(static_cast<int>(Roll() % static_cast<std::uint64_t>(Width * Height * 2 / 3)));

    float Mx = Width * 0.82f, My = Height * 0.16f, Mr = std::max(3.0f, Height * 0.06f);
    for (int Y = 0; Y < Height; Y++) {
        for (int X = 0; X < Width; X++) {
            float Dx = X - Mx, Dy = Y - My, Ex = X - Mx - Mr * 0.45f, Ey = Y - My + Mr * 0.2f;
            if (Dx * Dx + Dy * Dy <= Mr * Mr && Ex * Ex + Ey * Ey > Mr * Mr * 0.8f)
                Sky[static_cast<std::size_t>(Y) * Width + X] = MoonInk;
        }
    }

    ToFinale = Between(35.0f, 60.0f);
    for (int X = 0; X < Width;) {
        int Wide = static_cast<int>(Between(5.0f, 14.0f)), High = static_cast<int>(Between(0.08f, 0.26f) * Height);
        for (int Bx = X; Bx < std::min(Width, X + Wide); Bx++) {
            for (int Y = Height - High; Y < Height; Y++) {
                bool  Lit = (Bx - X) % 3 == 1 && (Height - Y) % 3 == 1 && Bx - X < Wide - 1 && Roll() % 3 == 0;
                Sky[static_cast<std::size_t>(Y) * Width + Bx] = Lit ? Window : Building;
            }
        }
        X += Wide + static_cast<int>(Roll() % 2);
    }
}

void Show::Shine(float X, float Y, Color Ink, int Strength)
{
    int Px = static_cast<int>(X), Py = static_cast<int>(Y);
    if (Px < 0 || Py < 0 || Px >= Width || Py >= Height) return;
    Light &Here = Field[static_cast<std::size_t>(Py) * Width + Px];
    Here.R = static_cast<std::uint16_t>(std::min(1020, Here.R + static_cast<int>(COLOR_R(Ink) * Strength >> 8)));
    Here.G = static_cast<std::uint16_t>(std::min(1020, Here.G + static_cast<int>(COLOR_G(Ink) * Strength >> 8)));
    Here.B = static_cast<std::uint16_t>(std::min(1020, Here.B + static_cast<int>(COLOR_B(Ink) * Strength >> 8)));
}

/* A burst lights the whole sky for a moment. */
void Show::Flash(Color Ink, int Strength)
{
    auto Add = [Strength](std::uint16_t Was, std::uint32_t Channel) {
        return static_cast<std::uint16_t>(std::min(1020u, Was + (Channel * static_cast<std::uint32_t>(Strength) >> 8)));
    };
    for (Light &Here : Field) {
        Here.R = Add(Here.R, COLOR_R(Ink));
        Here.G = Add(Here.G, COLOR_G(Ink));
        Here.B = Add(Here.B, COLOR_B(Ink));
    }
}

void Show::Launch(Kind Which, float X)
{
    Spark Rocket{};
    Rocket.X      = X;
    Rocket.Y      = static_cast<float>(Height - 1);
    Rocket.Vx     = Between(-0.06f, 0.06f) * Height;
    /* Fast enough to stop between half and four fifths of the way up. */
    float Pull    = RocketPull * static_cast<float>(Height), Rise = Between(0.5f, 0.8f) * static_cast<float>(Height);
    Rocket.Vy     = -std::sqrt(2.0f * Pull * Rise);
    Rocket.Life   = 4.0f;
    Rocket.Span   = 4.0f;
    Rocket.Ink    = SparkInk;
    Rocket.Rocket = true;
    Rocket.Bursts = Which;
    Sparks.push_back(Rocket);
    Launched++;
}

void Show::Finale()
{
    for (int Rocket = 0; Rocket < 9; Rocket++) Launch(AnyKind(), AnyX());
}

/* `Count` sparks flying out of `From` evenly, with a little scatter. */
void Show::Shed(const Spark &From, int Count, Color Ink, float Speed, float Span, float Drag)
{
    for (int At = 0; At < Count; At++) {
        float Turn = (static_cast<float>(At) + Unit() * 0.6f) / static_cast<float>(Count);
        float Pace = Speed * Between(0.55f, 1.0f);
        Spark One{};
        One.X = From.X;
        One.Y = From.Y;
        One.Vx = std::cos(Turn * 6.2831853f) * Pace + From.Vx * 0.3f;
        One.Vy = std::sin(Turn * 6.2831853f) * Pace + From.Vy * 0.3f;
        One.Span = One.Life = Span * Between(0.75f, 1.15f);
        One.Drag = Drag;
        One.Ink  = Ink;
        Sparks.push_back(One);
    }
}

void Show::Burst(const Spark &Shell)
{
    float Speed = static_cast<float>(Height) * 0.5f;
    int   Many  = std::clamp(Height, 50, 140);
    Color Ink   = AnyInk();
    Flash(color_mix(Ink, COLOR_WHITE, 170), 14);
    for (int Glow = 0; Glow < 12; Glow++) Shine(Shell.X + Between(-1, 1), Shell.Y + Between(-1, 1), COLOR_WHITE, 255);

    switch (Shell.Bursts) {
    case Kind::Peony: Shed(Shell, Many, Ink, Speed, 1.4f, 1.6f); break;
    case Kind::Chrysanthemum:
        Shed(Shell, Many, Ink, Speed * 1.1f, 2.0f, 1.3f);
        Shed(Shell, Many / 3, COLOR_WHITE, Speed * 0.6f, 1.2f, 1.6f);
        break;
    case Kind::Ring: {
        std::size_t First = Sparks.size();
        Shed(Shell, Many, Ink, Speed, 1.5f, 1.4f);
        for (std::size_t At = First; At < Sparks.size(); At++) {   /* one speed: a ring, not a ball */
            float Pace = std::sqrt(Sparks[At].Vx * Sparks[At].Vx + Sparks[At].Vy * Sparks[At].Vy);
            if (Pace > 0) { Sparks[At].Vx *= Speed / Pace; Sparks[At].Vy *= Speed / Pace * 0.55f; }
        }
        break;
    }
    case Kind::Willow: Shed(Shell, Many, Gold, Speed * 0.75f, 3.2f, 2.2f); break;
    case Kind::Crossette: {
        std::size_t First = Sparks.size();
        Shed(Shell, 8, Ink, Speed * 0.8f, 0.7f, 1.0f);
        for (std::size_t At = First; At < Sparks.size(); At++) Sparks[At].Splits = true;
        break;
    }
    case Kind::TwoColour:
        Shed(Shell, Many / 2, Ink, Speed, 1.5f, 1.5f);
        Shed(Shell, Many / 2, AnyInk(), Speed * 0.6f, 1.5f, 1.5f);
        break;
    case Kind::Strobe: {
        std::size_t First = Sparks.size();
        Shed(Shell, Many, COLOR_WHITE, Speed * 0.9f, 2.2f, 1.8f);
        for (std::size_t At = First; At < Sparks.size(); At++) Sparks[At].Strobe = true;
        break;
    }
    case Kind::Palm: Shed(Shell, 10, Gold, Speed * 1.1f, 2.4f, 1.1f); break;
    case Kind::Heart:
        for (int At = 0; At < Many; At++) {
            float T  = static_cast<float>(At) / static_cast<float>(Many) * 6.2831853f;
            float Hx = 16.0f * std::pow(std::sin(T), 3.0f);
            float Hy = -(13.0f * std::cos(T) - 5.0f * std::cos(2 * T) - 2.0f * std::cos(3 * T) - std::cos(4 * T));
            Spark One{};
            One.X = Shell.X;
            One.Y = Shell.Y;
            One.Vx = Hx / 16.0f * Speed * 0.8f;
            One.Vy = Hy / 16.0f * Speed * 0.8f;
            One.Span = One.Life = 1.6f;
            One.Drag = 1.6f;
            One.Ink  = COLOR_RGB(0xFF, 0x50, 0x90);
            Sparks.push_back(One);
        }
        break;
    }
}

void Show::Step(float Seconds)
{
    Time += Seconds;
    Wait -= Seconds;
    ToFinale -= Seconds;
    if (Wait <= 0) {
        Launch(AnyKind(), AnyX());
        Wait = Between(0.5f, 1.8f);
    }
    if (ToFinale <= 0) {
        Finale();
        ToFinale = Between(35.0f, 60.0f);
    }

    float Gravity = static_cast<float>(Height) * 0.28f;
    std::vector<Spark> Born;
    for (std::size_t At = Sparks.size(); At-- > 0;) {
        Spark &One = Sparks[At];
        One.Life -= Seconds;
        float Keep = std::max(0.0f, 1.0f - One.Drag * Seconds);
        One.Vx *= Keep;
        One.Vy  = One.Vy * Keep + Seconds * (One.Rocket ? RocketPull * static_cast<float>(Height) : Gravity);
        One.X  += One.Vx * Seconds;
        One.Y  += One.Vy * Seconds;

        if (One.Rocket) {
            Born.push_back({One.X + Between(-0.5f, 0.5f), One.Y + 1, Between(-2, 2), Between(1, 4), 0.2f, 0.2f, 2.0f,
                            SparkInk});
            if (One.Vy >= -RocketPull * static_cast<float>(Height) * 0.12f || One.Life <= 0) {
                Spark Shell = One;
                Sparks.erase(Sparks.begin() + static_cast<std::ptrdiff_t>(At));
                Burst(Shell);
                continue;
            }
        } else if (One.Splits && One.Life <= 0) {
            Spark From = One;
            From.Vx = From.Vy = 0;
            Sparks.erase(Sparks.begin() + static_cast<std::ptrdiff_t>(At));
            Shed(From, 4, From.Ink, static_cast<float>(Height) * 0.3f, 0.9f, 1.4f);
            continue;
        }
        if (One.Life <= 0 || One.Y >= static_cast<float>(Height) || One.X < 0 || One.X >= static_cast<float>(Width))
            Sparks.erase(Sparks.begin() + static_cast<std::ptrdiff_t>(At));
    }
    Sparks.insert(Sparks.end(), Born.begin(), Born.end());
}

/* The light fades; every spark adds to it, a dying one less and redder; the
 * light is laid over the sky. */
void Show::Draw(float Seconds, float Shown)
{
    std::uint32_t Fade = static_cast<std::uint32_t>(256.0f * std::pow(Keeps, Seconds));
    for (Light &Here : Field) {
        Here.R = static_cast<std::uint16_t>(Here.R * Fade >> 8);
        Here.G = static_cast<std::uint16_t>(Here.G * Fade >> 8);
        Here.B = static_cast<std::uint16_t>(Here.B * Fade >> 8);
    }
    for (const Spark &One : Sparks) {
        float Left = One.Span > 0 ? One.Life / One.Span : 1.0f;
        if (One.Strobe && (static_cast<int>(Time * 20.0f + One.X) % 3) != 0) continue;
        Color Ink = Left < 0.35f ? color_mix(One.Ink, COLOR_RGB(0xC0, 0x20, 0x10), static_cast<std::uint8_t>((0.35f - Left) * 600)) : One.Ink;
        Shine(One.X, One.Y, Ink, static_cast<int>(120 + 136 * std::min(1.0f, Left * 1.4f)));
    }

    for (std::size_t At = 0; At < Field.size(); At++) {
        Color Base = Sky[At];
        const Light &Here = Field[At];
        Pixels[At] = COLOR_RGB(std::min(255, static_cast<int>(COLOR_R(Base)) + Here.R),
                               std::min(255, static_cast<int>(COLOR_G(Base)) + Here.G),
                               std::min(255, static_cast<int>(COLOR_B(Base)) + Here.B));
    }
    for (std::size_t Star = 0; Star < Twinkles.size(); Star++) {
        std::size_t At = static_cast<std::size_t>(Twinkles[Star]);
        if ((Star * 7 + static_cast<std::size_t>(Time * 2.0f)) % 3 != 0 && Sky[At] != MoonInk)
            Pixels[At] = color_mix(Pixels[At], StarInk, 200);
    }
    Glass.Pixels(Pixels);

    if (Shown < KeysShown) {
        int Row = Glass.Rows - 1, Col = 1;
        Glass.Fill(Row, 0, Glass.Cols, ' ', BarInk, BarPaper);
        Col = Glass.Write(Row, Col, "fireworks", BarInk, BarPaper);
        for (auto [Keys, What] : {std::pair{"space", "a rocket"}, {"1-9", "a kind"}, {"f", "finale"}, {"q", "leave"}}) {
            Col = Glass.Write(Row, Col + 2, Keys, KeyInk, BarPaper);
            Col = Glass.Write(Row, Col + 1, What, HintInk, BarPaper);
        }
    }
}

}

int main()
{
    try {
        Show Night;
        if (std::optional<std::string> Refusal = Night.Glass.Take("fireworks", 40, 12)) {
            Night.Glass.Give();
            StageSay(*Refusal);
            return 1;
        }
        Night.Begin();

        std::uint64_t Began = clock_uptime_us(), Last = Began;
        bool          Shown = true;
        for (;;) {
            std::uint64_t Now = clock_uptime_us();
            float         Seconds = std::min(MostGap, static_cast<float>(Now - Last) / 1e6f);
            for (float Left = Seconds; Left > 0; Left -= MostStep) Night.Step(std::min(MostStep, Left));
            Last = Now;
            Night.Draw(Seconds, static_cast<float>(Now - Began) / 1e6f);
            if (!(Shown = Night.Glass.Show())) break;

            std::uint64_t Spent = clock_uptime_us() - Now;
            kb_event_t    Key;
            StageHeard    Heard = Night.Glass.Listen(Key, Spent >= FrameUs ? 1 : static_cast<std::uint32_t>((FrameUs - Spent) / 1000 + 1));
            if (Heard == StageHeard::Gone) break;
            if (Heard != StageHeard::Key || (Key.mods & KB_MOD_EXTENDED)) continue;
            if (Key.ascii == 'q' || Key.ascii == 'Q' || Key.ascii == KEY_ESCAPE) break;
            if (Key.ascii == ' ') Night.Launch(Night.AnyKind(), Night.AnyX());
            if (Key.ascii >= '1' && Key.ascii <= '9') Night.Launch(static_cast<Kind>(Key.ascii - '1'), Night.AnyX());
            if (Key.ascii == 'f' || Key.ascii == 'F') Night.Finale();
        }
        unsigned Launched = Night.Launched;
        Night.Glass.Give();
        StageSay(Shown ? std::format("[FIREWORKS] {} rocket{} went up", Launched, Launched == 1 ? "" : "s")
                       : "The screen stopped taking frames.");
        return Shown ? 0 : 1;
    } catch (const std::bad_alloc &) {
        StageSay("There is not enough memory for a sky the size of this screen.");
        return 1;
    }
}
