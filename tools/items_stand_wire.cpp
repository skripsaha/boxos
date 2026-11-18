/* items stand — the language on the wire: what a program says arrives as it
 * said it; a step shows whole or not at all; a full ring keeps each
 * property's last word and the order of what makes and removes items; every
 * value past the language refused and told back, the tree unchanged; the
 * bounds of a program held; a ring that lies said broken; random bytes read
 * whole; what a clock's change costs on the wire. */

#include <cstdio>
#include <cstring>
#include <string>

#include "items_stand.h"

namespace stand {

namespace {

using painter::core::Color;
using painter::gui::Changes;
using painter::gui::Kind;
using painter::gui::Role;

/* A record written into a bench's ring as it is, past the program's own
 * writer: a value the writer would never make. */
void Raw(Bench &On, std::uint16_t Op, std::uint32_t Item, const void *Body, std::size_t Bytes)
{
    const std::uint32_t Length = static_cast<std::uint32_t>((sizeof(ItemRecord) + Bytes + 7u) & ~std::size_t{7});
    std::uint64_t       Write  = On.Head->Write;
    std::uint8_t *At = item_ring_place(On.Memory.data() + sizeof(ItemAreaHeader), On.RingBytes, &Write, On.Head->Read, Length);
    if (!At) return;
    std::memset(At, 0, Length);
    const ItemRecord Head{Op, static_cast<std::uint16_t>(Length / 8u), Item};
    std::memcpy(At, &Head, sizeof(Head));
    if (Bytes) std::memcpy(At + sizeof(Head), Body, Bytes);
    On.Head->Write = Write;
}

/* A property's raw value, its change record round it. */
void RawChange(Bench &On, std::uint32_t Item, std::uint16_t Property, const void *Value, std::uint32_t Bytes)
{
    std::vector<std::uint8_t> Body(sizeof(ItemChange) + Bytes);
    const ItemChange          What{Property, 0, Bytes};
    std::memcpy(Body.data(), &What, sizeof(What));
    if (Bytes) std::memcpy(Body.data() + sizeof(What), Value, Bytes);
    Raw(On, ITEM_OP_CHANGE, Item, Body.data(), Body.size());
}

items::Reading Read(Bench &On)
{
    Changes None;
    return On.Step(None);
}

void CheckRoundTrip(Tally &Count, const items::Theme &Look)
{
    items::ProgramState Program = HostProgram(Look, 1);
    Bench               On(Program);
    Changes             Say;
    Say.Create(1, Kind::Group);
    Say.Create(2, Kind::Text, 1, ITEM_TOP, Role::Heading);
    Say.Text(2, "12:46");
    Say.Place(2, {10.5f, 20});
    Say.Style(2, painter::gui::TextStyle{.Size = 24.0f, .Weight = 700});
    const painter::gui::Fill Red = painter::gui::Fill::Of(Color::Rgb(255, 0, 0));
    Say.Fills(2, std::span(&Red, 1));
    Say.Create(3, Kind::Shape, 1, 2);
    Say.Corners(3, painter::core::Corners::All(8));
    Say.Outline(3, painter::core::Path::Circle({50, 50}, 20));
    painter::gui::Stroke Line;
    Line.Width  = 2;
    Line.Dashes = {6, 3};
    Line.With   = painter::gui::Fill::OfTheme(painter::gui::ThemeColor::Accent);
    Say.Stroke(3, Line);
    const items::Reading Got = On.Step(Say);

    const items::Item *Text = On.Area.Items.Find(2), *Shape = On.Area.Items.Find(3), *Group = On.Area.Items.Find(1);
    bool Holds = Got.Stepped && !Got.Broken && On.Refusals.empty() && Text && Shape && Group && Say.Empty();
    Holds = Holds && Text->Kind == ITEM_TEXT && Text->Role == ITEM_ROLE_HEADING && Text->Parent == 1 &&
            Text->Said.Text == std::string("12:46") && Text->Said.Place && Text->Said.Place->X == 2688 &&
            Text->Said.Place->Y == 5120 && Text->Said.Style && Text->Said.Style->Size == 24 * ITEM_POINT &&
            Text->Said.Style->Weight == 700 && (Text->Said.Style->Said & ITEM_SAYS_WEIGHT) &&
            !(Text->Said.Style->Said & ITEM_SAYS_ITALIC) && Text->Said.Fills && Text->Said.Fills->size() == 1 &&
            (*Text->Said.Fills)[0].Color.Red == 65535 && (*Text->Said.Fills)[0].Color.Green == 0;
    Holds = Holds && Group->Children.size() == 2 && Group->Children[0] == 2 && Group->Children[1] == 3 &&
            Shape->Said.Corners && (*Shape->Said.Corners)[2] == 8 * ITEM_POINT && Shape->Said.Shape &&
            Shape->Said.Shape->Form == ITEM_FORM_PATH && !Shape->Said.Shape->Steps.empty() && Shape->Said.Stroke &&
            Shape->Said.Stroke->Dashes.size() == 2 && Shape->Said.Stroke->With.Kind == ITEM_FILL_THEME;
    Count.Check(Holds, "what a program says arrives as it said it: items in their groups and order, every property's "
                       "value to a 256th of a point and sixteen bits a channel");

    /* The step's end cut off: nothing of it shows until the end comes. */
    Say.Text(2, "12:47");
    std::uint8_t *Ring  = On.Memory.data() + sizeof(ItemAreaHeader);
    std::uint64_t Write = On.Head->Write;
    (void)Say.Flush(Ring, On.RingBytes, Write, On.Head->Read, 99);
    On.Head->Write              = Write - 16;   /* the step record withheld */
    const items::Reading Half   = Read(On);
    const bool           Before = On.Area.Items.Find(2)->Said.Text == std::string("12:46") && !Half.Stepped;
    On.Head->Write              = Write;
    const items::Reading Whole  = Read(On);
    Count.Check(Before && Whole.Stepped && Whole.Step == 99 && On.Area.Items.Find(2)->Said.Text == std::string("12:47"),
                "a step shows whole or not at all: a change whose step has not ended waits for the end");
}

void CheckFullRing(Tally &Count, const items::Theme &Look)
{
    items::ProgramState Program = HostProgram(Look, 1);
    Bench               On(Program, 4096);
    Changes             Say;
    Say.Create(5, Kind::Text);
    Say.Create(7, Kind::Shape);
    (void)On.Step(Say);
    /* The ring filled and not read: what is said meanwhile waits. */
    std::uint8_t *Ring  = On.Memory.data() + sizeof(ItemAreaHeader);
    std::uint64_t Write = On.Head->Write;
    Say.Text(5, std::string(1900, 'x'));
    (void)Say.Flush(Ring, On.RingBytes, Write, On.Head->Read, 1);
    Say.Caption(5, std::string(1900, 'y'));
    (void)Say.Flush(Ring, On.RingBytes, Write, On.Head->Read, 2);
    On.Head->Write = Write;
    const std::string Last = std::string(300, 'z') + " the last word";
    Say.Text(5, std::string(300, 'a'));
    Say.Text(5, std::string(300, 'b'));
    Say.Text(5, Last);
    Say.Opacity(7, 0.5f);
    Say.Remove(7);
    /* The removal fits and goes; the text does not, and waits. */
    const bool Waits = !Say.Flush(Ring, On.RingBytes, Write, On.Head->Read, 3) && Say.Waiting() == 1;
    On.Head->Write   = Write;
    (void)Read(On);
    const items::Reading Got = On.Step(Say);
    Count.Check(Waits && Got.Stepped && On.Refusals.empty() && On.Area.Items.Find(5)->Said.Text == Last &&
                    !On.Area.Items.Find(7),
                "a full ring: what is said meanwhile waits with the program, a property's last word over the words "
                "before it, a removed item's words gone with it — nothing refused when room comes");
}

void CheckRefusals(Tally &Count, const items::Theme &Look)
{
    items::ProgramState Program = HostProgram(Look, 1);
    Bench               On(Program);
    Changes             Say;
    Say.Create(10, Kind::Shape);
    Say.Create(11, Kind::Text);
    Say.Create(12, Kind::Group);
    Say.Create(13, Kind::Group, 12);
    (void)On.Step(Say);

    const ItemCorners Crooked{-5, 0, 0, 0};
    RawChange(On, 10, ITEM_PROP_CORNERS, &Crooked, sizeof(Crooked));
    const ItemBytes Lying{1000};
    RawChange(On, 11, ITEM_PROP_TEXT, &Lying, sizeof(Lying));
    const ItemFills Many{17, 0};
    RawChange(On, 10, ITEM_PROP_FILLS, &Many, sizeof(Many));
    const ItemFlag On2{1, {}};
    RawChange(On, 10, 999, &On2, sizeof(On2));
    const ItemCreate Again{ITEM_SHAPE, 0, 0, ITEM_AREA, ITEM_TOP, 0};
    Raw(On, ITEM_OP_CREATE, 10, &Again, sizeof(Again));
    const ItemCreate IntoText{ITEM_SHAPE, 0, 0, 11, ITEM_TOP, 0};
    Raw(On, ITEM_OP_CREATE, 20, &IntoText, sizeof(IntoText));
    const ItemPlaceAmong IntoItself{13, ITEM_TOP};
    Raw(On, ITEM_OP_RESTACK, 12, &IntoItself, sizeof(IntoItself));
    Raw(On, ITEM_OP_REMOVE, 77, nullptr, 0);
    const ItemGrid NoCells{8, 2};
    RawChange(On, 10, ITEM_PROP_GRID, &NoCells, sizeof(NoCells));
    const ItemStep End{5};
    Raw(On, ITEM_OP_STEP, 0, &End, sizeof(End));
    const items::Reading Got = Read(On);

    const std::vector<error_t> Want = {ERR_INVALID_ARGUMENT, ERR_INVALID_ARGUMENT, ERR_INVALID_ARGUMENT, ERR_UNSUPPORTED,
                                       ERR_ALREADY_EXISTS,  ERR_INVALID_ARGUMENT, ERR_INVALID_ARGUMENT, ERR_OBJECT_NOT_FOUND,
                                       ERR_INVALID_ARGUMENT};
    const items::Item *Shape = On.Area.Items.Find(10);
    Count.Check(Got.Stepped && !Got.Broken && On.Refusals == Want && Shape && !Shape->Said.Corners && !Shape->Said.Fills &&
                    !On.Area.Items.Find(11)->Said.Text && !On.Area.Items.Find(20) && On.Area.Items.Find(12)->Parent == ITEM_AREA &&
                    On.Area.Items.Count() == 4,
                "every change past the language refused, its reason told back and nothing changed: a corner less than "
                "none, a text longer than its record, seventeen fills, a property not known, an item made twice, an "
                "item in a text, a group into its own, removing what is not, a grid on what holds no cells");
}

void CheckBounds(Tally &Count, const items::Theme &Look)
{
    items::ProgramState Program = HostProgram(Look, 1);
    Bench               On(Program);
    Changes             Say;
    /* Groups in groups, to the deepest allowed and one past. */
    for (std::uint32_t Depth = 1; Depth <= ITEM_DEPTH_MOST + 1; Depth++)
        Say.Create(Depth, Kind::Group, Depth == 1 ? ITEM_AREA : Depth - 1);
    (void)On.Step(Say);
    const bool Deep = On.Area.Items.Count() == ITEM_DEPTH_MOST && On.Refusals.size() == 1 && On.Refusals[0] == ERR_OUT_OF_RANGE;

    /* As many items as a program may hold, and one past. */
    On.Refusals.clear();
    for (std::uint32_t Id = 1000; Id < 1000 + ITEM_ITEMS_MOST - ITEM_DEPTH_MOST + 1; Id++) {
        Say.Create(Id, Kind::Shape);
        if (Say.Bytes() > ITEM_RING_BYTES / 2) (void)On.Step(Say);
    }
    (void)On.Step(Say);
    const bool Many = Program.Held.Items == ITEM_ITEMS_MOST && On.Refusals.size() == 1 && On.Refusals[0] == ERR_QUOTA_EXCEEDED;

    /* Texts, to the bytes a program may hold. */
    On.Refusals.clear();
    const std::string Long(ITEM_TEXT_MOST - 64, 't');
    std::size_t       Taken = 0;
    for (std::uint32_t Id = 1000; Id < 1100; Id++) {
        Say.Text(Id, Long);
        (void)On.Step(Say);
    }
    for (std::uint32_t Id = 1000; Id < 1100; Id++)
        if (On.Area.Items.Find(Id)->Said.Text) Taken++;
    const bool Texts = Program.Held.TextBytes <= ITEM_TEXTS_MOST && !On.Refusals.empty() &&
                       On.Refusals.back() == ERR_QUOTA_EXCEEDED && Taken * Long.size() <= ITEM_TEXTS_MOST && Taken > 50;
    char Line[300];
    std::snprintf(Line, sizeof Line,
                  "a program's bounds held: groups %u deep and not one more, %u items and not one more, texts to %u "
                  "MiB (%zu of %zu-KiB texts taken)",
                  ITEM_DEPTH_MOST, ITEM_ITEMS_MOST, ITEM_TEXTS_MOST >> 20, Taken, Long.size() >> 10);
    Count.Check(Deep && Many && Texts, Line);
}

void CheckBroken(Tally &Count, const items::Theme &Look)
{
    items::ProgramState Program = HostProgram(Look, 1);
    int                 Said    = 0;
    {
        Bench On(Program);
        On.Head->Write = 8;
        On.Head->Read  = 16;
        Said += Read(On).Broken;
    }
    {
        Bench          On(Program);
        const ItemRecord Long{ITEM_OP_CHANGE, 0xFFFF, 1};
        std::memcpy(On.Memory.data() + sizeof(ItemAreaHeader) + On.RingBytes - 16, &Long, sizeof(Long));
        On.Head->Read = On.Head->Write = On.RingBytes - 16;
        On.Head->Write += 16;
        Said += Read(On).Broken;
    }
    {
        Bench            On(Program);
        const ItemRecord None{ITEM_OP_CREATE, 0, 1};
        std::memcpy(On.Memory.data() + sizeof(ItemAreaHeader), &None, sizeof(None));
        On.Head->Write = 8;
        Said += Read(On).Broken;
    }
    Count.Check(Said == 3, "a ring that lies said broken, nothing of it applied: its reader past its writer, a record "
                           "past its end, a record of no words");

    /* Random bytes read whole: never a fall, never past the bounds. */
    int Broke = 0, Rounds = 0;
    for (; Rounds < 3000; Rounds++) {
        Bench               On(Program, 8192);
        const std::uint32_t Records = 1 + Random() % 20;
        for (std::uint32_t R = 0; R < Records; R++) {
            std::vector<std::uint8_t> Body(Random() % 200);
            for (std::uint8_t &Each : Body) Each = static_cast<std::uint8_t>(Random() % 4 ? Random() % 8 : Random());
            Raw(On, static_cast<std::uint16_t>(Random() % 9), Random() % 5, Body.data(), Body.size());
        }
        const ItemStep End{1};
        Raw(On, ITEM_OP_STEP, 0, &End, sizeof(End));
        if (Random() % 4 == 0) On.Memory[sizeof(ItemAreaHeader) + Random() % 1024] = static_cast<std::uint8_t>(Random());
        Broke += Read(On).Broken;
    }
    Count.Check(Program.Held.Items == 0 && Program.Held.TextBytes == 0,
                std::to_string(Rounds) + " rings of random records read whole, " + std::to_string(Broke) +
                    " said broken: no fall, and every item they made given back with their areas");
}

void CheckCosts(Tally &Count)
{
    Changes Say;
    Say.Text(3, "12:46");
    const std::size_t Clock = Say.Bytes();
    std::vector<std::uint8_t> Ring(4096);
    std::uint64_t             Write = 0;
    (void)Say.Flush(Ring.data(), 4096, Write, 0, 1);
    Changes Move;
    Move.Place(3, {10, 20});
    const std::size_t Moved = Move.Bytes();
    char Line[240];
    std::snprintf(Line, sizeof Line,
                  "what a change costs on the wire: a clock's \"item 3: its text is 12:46\" %zu bytes and its step's "
                  "end — %llu with it; moving an item %zu",
                  Clock, static_cast<unsigned long long>(Write), Moved);
    Count.Check(Clock <= 40 && Write <= 56 && Moved <= 32, Line);
}

void CheckEvents(Tally &Count)
{
    std::vector<std::uint8_t> Memory(sizeof(ItemAreaHeader) + 4096, 0);
    auto                     *Head = reinterpret_cast<ItemAreaHeader *>(Memory.data());
    const std::span<std::uint8_t> Events(Memory.data() + sizeof(ItemAreaHeader), 4096);
    const ItemPoint               Size{1234, 5678};
    int                           Added = 0;
    while (items::AddEvent(*Head, Events, ITEM_EVENT_SIZE, 9, &Size, sizeof(Size))) Added++;
    const std::uint32_t Lost = Head->Lost;
    /* Read back as a program reads them. */
    int  Read  = 0;
    bool Right = true;
    while (Head->EventRead < Head->EventWrite) {
        const auto Offset = static_cast<std::uint32_t>(Head->EventRead % 4096);
        ItemEvent  Event{};
        std::memcpy(&Event, Events.data() + Offset, sizeof(Event));
        const std::uint32_t Length = Event.Kind == ITEM_EVENT_PAD && Event.Words == 0 ? 4096 - Offset : Event.Words * 8u;
        if (Event.Kind == ITEM_EVENT_SIZE) {
            ItemPoint Got{};
            std::memcpy(&Got, Events.data() + Offset + sizeof(Event), sizeof(Got));
            Right = Right && Event.Item == 9 && Got.X == 1234 && Got.Y == 5678;
            Read++;
        }
        Head->EventRead += Length;
    }
    const bool Again = items::AddEvent(*Head, Events, ITEM_EVENT_SIZE, 9, &Size, sizeof(Size));
    Count.Check(Added == 256 && Lost == 1 && Read == Added && Right && Again,
                "events told back in order and read as a program reads them; one more than the ring holds counted "
                "lost, never written over what was not read");
}

}  // namespace

void CheckWire(Tally &Count)
{
    const items::Theme Look;
    CheckRoundTrip(Count, Look);
    CheckFullRing(Count, Look);
    CheckRefusals(Count, Look);
    CheckBounds(Count, Look);
    CheckBroken(Count, Look);
    CheckCosts(Count);
    CheckEvents(Count);
}

}  // namespace stand
