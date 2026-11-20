/*
 * animator.cpp — an area's animations, played by the clock. See animator.h.
 */

#include "animator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "item_draw.h"

namespace items {

void DropLeaving::operator()(Leaving *Of) const noexcept { delete Of; }

namespace {

/* What less motion leaves out: where things are, how large, turned or
 * shaped. */
bool Moves(std::uint16_t Property) noexcept
{
    switch (Property) {
    case ITEM_PROP_PLACE:
    case ITEM_PROP_SIZE:
    case ITEM_PROP_TURN:
    case ITEM_PROP_SCALE:
    case ITEM_PROP_ANCHOR:
    case ITEM_PROP_SHAPE:
    case ITEM_PROP_CORNERS:
        return true;
    default:
        return false;
    }
}

/* A property whose look an item may fade from into another. */
bool Fadable(std::uint16_t Property, std::uint8_t Kind) noexcept
{
    if (Kind != ITEM_TEXT && Kind != ITEM_SHAPE && Kind != ITEM_PICTURE) return false;
    switch (Property) {
    case ITEM_PROP_TEXT:
    case ITEM_PROP_TEXT_SPANS:
    case ITEM_PROP_TEXT_BOX:
    case ITEM_PROP_TEXT_STYLE:
    case ITEM_PROP_SECRET:
    case ITEM_PROP_PICTURE:
    case ITEM_PROP_SHAPE:
    case ITEM_PROP_FILLS:
    case ITEM_PROP_STROKE:
    case ITEM_PROP_SHADOWS:
    case ITEM_PROP_ROLE:
        return true;
    default:
        return false;
    }
}

/* What turns or scales an item about its anchor: its kept picture laid
 * through it while it moves. */
bool Turns(std::uint16_t Property) noexcept
{
    return Property == ITEM_PROP_TURN || Property == ITEM_PROP_SCALE || Property == ITEM_PROP_ANCHOR;
}

/* Less motion's longest fade. */
constexpr std::uint64_t LessLongest = 200000;

float OfOne(std::int32_t Value) noexcept { return static_cast<float>(Value) / ITEM_ONE; }

painter::core::Easing CurveOf(const ItemAnimation &How, bool Less) noexcept
{
    using painter::core::Easing;
    Easing Made;
    switch (How.Curve) {
    case ITEM_CURVE_IN: Made.What = Easing::Kind::In; break;
    case ITEM_CURVE_OUT: Made.What = Easing::Kind::Out; break;
    case ITEM_CURVE_IN_OUT: Made.What = Easing::Kind::InOut; break;
    case ITEM_CURVE_STEPS: Made.What = Easing::Kind::Steps; break;
    case ITEM_CURVE_SPRING: Made.What = Easing::Kind::Spring; break;
    case ITEM_CURVE_OWN: Made.What = Easing::Kind::Own; break;
    default: Made.What = Easing::Kind::Linear; break;
    }
    Made.Steps  = std::max<int>(1, How.Steps);
    Made.Bounce = Less ? 0.0f : OfOne(How.Bend[0]);
    Made.X1 = OfOne(How.Bend[0]), Made.Y1 = OfOne(How.Bend[1]), Made.X2 = OfOne(How.Bend[2]), Made.Y2 = OfOne(How.Bend[3]);
    return Made;
}

/* A size as the item shows it: said, or its content's where unsaid. False
 * where neither is known. */
bool SizeAsShown(const Item &Of, Properties &Said) noexcept
{
    ItemPoint Size = Said.Size ? *Said.Size : ItemPoint{ITEM_BY_CONTENT, ITEM_BY_CONTENT};
    if (Size.X == ITEM_BY_CONTENT || Size.Y == ITEM_BY_CONTENT) {
        if (!Of.Drawn) return false;
        if (Size.X == ITEM_BY_CONTENT) Size.X = Of.Drawn->Measured.X;
        if (Size.Y == ITEM_BY_CONTENT) Size.Y = Of.Drawn->Measured.Y;
    }
    Said.Size = Size;
    return true;
}

std::uint64_t Times(std::uint64_t A, std::uint64_t B) noexcept
{
    return B && A > std::numeric_limits<std::uint64_t>::max() / B ? std::numeric_limits<std::uint64_t>::max() : A * B;
}

}  // namespace

Animator::~Animator()
{
    for (const auto &[Serial, Each] : All) {
        Held.Animations--;
        Held.AnimationBytes -= Each.Value.size();
    }
}

std::uint64_t Animator::DelayOf(const Animation &Of) const noexcept
{
    const std::uint64_t Speed = std::clamp<std::uint32_t>(Wanted.Speed, 25, 400);
    return std::uint64_t{Of.How.Delay} * 1000u * 100u / Speed;
}

namespace {

/* The heap's order: the earliest moment on top — at one moment an end before
 * a beginning, so what waits on it begins after it. */
struct Later {
    template <typename Moment> bool operator()(const Moment &A, const Moment &B) const noexcept
    {
        return A.At != B.At ? A.At > B.At : (A.Ends != B.Ends ? !A.Ends : A.Serial > B.Serial);
    }
};

void Forget(std::vector<std::uint64_t> &From, std::uint64_t Serial) noexcept
{
    const auto Found = std::find(From.begin(), From.end(), Serial);
    if (Found != From.end()) From.erase(Found);
}

}  // namespace

void Animator::Schedule(std::uint64_t At, bool Ends, std::uint64_t Serial)
{
    Moments.push_back(Moment{At, Ends, Serial});
    std::push_heap(Moments.begin(), Moments.end(), Later{});
}

/* A moment put off: its animation cut, or due otherwise now. */
bool Animator::Stale(const Moment &Of) const noexcept
{
    const auto It = All.find(Of.Serial);
    if (It == All.end()) return true;
    const Animation &Each = It->second;
    if (Of.Ends) return Each.Stage != State::Under || Each.Runs == 0 || Each.Begins + Times(Each.Run, Each.Runs) != Of.At;
    return Each.Stage != State::Due || Each.Begins != Of.At;
}

/* The moments put off taken from the top, so the next one is a true one. */
void Animator::Prune() noexcept
{
    if (All.empty()) Moments.clear();
    while (!Moments.empty() && Stale(Moments.front())) {
        std::pop_heap(Moments.begin(), Moments.end(), Later{});
        Moments.pop_back();
    }
}

error_t Animator::Said(std::uint32_t Item, const ItemAnimation &How, std::uint16_t Property,
                       std::span<const std::uint8_t> Value) noexcept
{
    const items::Item *Of = Items.Find(Item);
    if (!Of) return ERR_OBJECT_NOT_FOUND;
    if (How.Curve >= ITEM_CURVES || How.Back > 1 || How.Delay > ITEM_TIME_MOST || How.Duration > ITEM_TIME_MOST)
        return ERR_INVALID_ARGUMENT;
    if (How.Curve == ITEM_CURVE_SPRING && (How.Bend[0] < 0 || How.Bend[0] > ITEM_ONE)) return ERR_INVALID_ARGUMENT;
    if (How.Curve == ITEM_CURVE_OWN && (How.Bend[0] < 0 || How.Bend[0] > ITEM_ONE || How.Bend[2] < 0 || How.Bend[2] > ITEM_ONE ||
                                        std::abs(How.Bend[1]) > 8 * ITEM_ONE || std::abs(How.Bend[3]) > 8 * ITEM_ONE))
        return ERR_INVALID_ARGUMENT;
    if (Held.Animations >= ITEM_ANIMATIONS_MOST || Held.AnimationBytes + Value.size() > ITEM_ANIMATION_BYTES_MOST)
        return ERR_QUOTA_EXCEEDED;
    try {
        Animation Made;
        std::uint16_t Role = Of->Role;
        if (const error_t Read = Decode(Property, Value, Made.To, Role); Read != OK) return Read;
        Made.Item     = Item;
        Made.Property = Property;
        Made.How      = How;
        Made.Value.assign(Value.begin(), Value.end());
        Made.Serial = ++Serials;
        Made.Fresh  = !Of->Drawn && !IsSaid(Property, Of->Said);
        OfItem[Item].push_back(Made.Serial);
        NewlySaid.push_back(Made.Serial);
        All.emplace(Made.Serial, std::move(Made));
        if (How.Number) ByNumber[How.Number].Count++;
    } catch (...) {
        return ERR_NO_MEMORY;
    }
    Held.Animations++;
    Held.AnimationBytes += Value.size();
    return OK;
}

void Animator::Cut(std::uint32_t Item, std::uint16_t Property) noexcept
{
    const auto Found = OfItem.find(Item);
    if (Found == OfItem.end()) return;
    const std::vector<std::uint64_t> Serials2 = Found->second;
    for (std::uint64_t Serial : Serials2) {
        const auto It = All.find(Serial);
        if (It == All.end() || (Property != 0 && It->second.Property != Property)) continue;
        const Animation &Each = It->second;
        /* Said plainly, a thing shown or hidden is wholly what it is now. */
        if (Property != 0 && Each.Stage == State::Under && (Each.Goes == Way::Shows || Each.Goes == Way::Hides))
            if (items::Item *Of = Items.Find(Item)) {
                Of->Seen = 1;
                Items.Recompose(Item);
            }
        End(Serial, ReadAt, false);
    }
    Prune();
}

bool Animator::WaitsOnItself(std::uint32_t Number, std::uint32_t After) const
{
    if (Number == 0) return false;
    std::vector<std::uint32_t> Ahead{After}, Seen{After};
    while (!Ahead.empty()) {
        const std::uint32_t Next = Ahead.back();
        Ahead.pop_back();
        for (const auto &[Serial, Each] : All) {
            if (Each.How.Number != Next || Each.How.After == 0 || (Each.Stage != State::Said && Each.Stage != State::Waits))
                continue;
            if (Each.How.After == Number) return true;
            if (std::find(Seen.begin(), Seen.end(), Each.How.After) == Seen.end()) {
                Seen.push_back(Each.How.After);
                Ahead.push_back(Each.How.After);
            }
        }
    }
    return false;
}

void Animator::StepWhole() noexcept
{
    const std::vector<std::uint64_t> Said2 = std::move(NewlySaid);
    NewlySaid.clear();
    for (std::uint64_t Serial : Said2) {
        const auto It = All.find(Serial);
        if (It == All.end() || It->second.Stage != State::Said) continue;
        Animation &Each  = It->second;
        bool       Waits = false;
        try {
            if (Each.How.After != 0) {
                const auto          Found = ByNumber.find(Each.How.After);
                const std::uint32_t Own   = Each.How.Number == Each.How.After ? 1u : 0u;
                Waits = Found != ByNumber.end() && Found->second.Count > Own && !WaitsOnItself(Each.How.Number, Each.How.After);
            }
            if (Waits) {
                WaitingOn[Each.How.After].push_back(Serial);
                Each.Stage = State::Waits;
            } else {
                Each.Stage  = State::Due;
                Each.Begins = ReadAt + DelayOf(Each);
                Schedule(Each.Begins, false, Serial);
            }
        } catch (...) {
            // No room to keep it waiting: it is said at once.
            (void)Items.Change(Each.Item, Each.Property, Each.Value);
            End(Serial, ReadAt, true);
        }
    }
}

float Animator::Progress(const Animation &Of, std::uint64_t At, std::uint64_t &RunOf) const noexcept
{
    RunOf = 0;
    if (Of.Run == 0) return 1;
    const std::uint64_t Elapsed = At > Of.Begins ? At - Of.Begins : 0;
    std::uint64_t       Run     = Elapsed / Of.Run;
    float               Share   = static_cast<float>(static_cast<double>(Elapsed - Run * Of.Run) / static_cast<double>(Of.Run));
    if (Of.Runs != 0 && Run >= Of.Runs) {
        Run   = Of.Runs - 1;
        Share = 1;
    }
    if (Of.How.Back && Run % 2 == 1) Share = 1 - Share;
    RunOf = Run;
    return Of.Curve.At(Share);
}

void Animator::Show(Animation &Of, std::uint64_t At, const Theme &Look)
{
    items::Item *It = Items.Find(Of.Item);
    if (!It) return;
    std::uint64_t Run    = 0;
    const float   Share  = Progress(Of, At, Run);
    const float   Within = std::clamp(Share, 0.0f, 1.0f);
    switch (Of.Goes) {
    case Way::Walks: {
        const std::vector<double> &From = Of.From.Values, &To = Of.Toward.Values;
        std::vector<double>        Now(From.size());
        const bool Pushed = Of.Curve.What == painter::core::Easing::Kind::Spring && Run == 0 &&
                            std::any_of(Of.Speed.begin(), Of.Speed.end(), [](double Each) { return Each != 0; });
        if (Pushed) {
            /* A spring taking on the speed of the one it cut. */
            const painter::core::Spring Swing = painter::core::Spring::For(static_cast<float>(Of.Run) / 1e6f, Of.Curve.Bounce);
            const float                 T     = static_cast<float>(At - Of.Begins) / 1e6f;
            for (std::size_t I = 0; I < Now.size(); I++)
                Now[I] = To[I] + Swing.At(static_cast<float>(From[I] - To[I]), static_cast<float>(Of.Speed[I]), T).Offset;
        } else {
            for (std::size_t I = 0; I < Now.size(); I++) Now[I] = From[I] + (To[I] - From[I]) * Share;
        }
        Properties Walked;
        SayNumbers(Of.Property, Now, Of.Like, Look, Walked);
        (void)Items.Assign(Of.Item, Of.Property, Walked);
        return;
    }
    case Way::Fades:
        if (It->Fading) It->Fading->Share = Within;
        Items.Recompose(Of.Item);
        return;
    case Way::Shows:
        It->Seen = Of.SeenFrom + (1 - Of.SeenFrom) * Within;
        Items.Recompose(Of.Item);
        return;
    case Way::Hides:
        It->Seen = Of.SeenFrom * (1 - Within);
        Items.Recompose(Of.Item);
        return;
    case Way::AtOnce:
        return;
    }
}

std::vector<double> Animator::SpeedOf(const Animation &Of, std::uint64_t At) const
{
    const std::size_t   Count = Of.From.Values.size();
    std::vector<double> Made(Count, 0.0);
    if (Of.Goes != Way::Walks || Of.Run == 0) return Made;
    std::uint64_t Run = 0;
    (void)Progress(Of, At, Run);
    if (Of.Curve.What == painter::core::Easing::Kind::Spring && Run == 0) {
        const painter::core::Spring Swing = painter::core::Spring::For(static_cast<float>(Of.Run) / 1e6f, Of.Curve.Bounce);
        const float                 T     = static_cast<float>(At - Of.Begins) / 1e6f;
        for (std::size_t I = 0; I < Count; I++)
            Made[I] = Swing.At(static_cast<float>(Of.From.Values[I] - Of.Toward.Values[I]),
                               static_cast<float>(I < Of.Speed.size() ? Of.Speed[I] : 0.0), T)
                          .Speed;
        return Made;
    }
    /* Elsewhere, as the progress moves a millisecond either side. */
    constexpr std::uint64_t Near = 1000;
    std::uint64_t           Unused = 0;
    const float Before = Progress(Of, At > Of.Begins + Near ? At - Near : Of.Begins, Unused);
    const float After  = Progress(Of, At + Near, Unused);
    const double Seconds = static_cast<double>(At + Near - (At > Of.Begins + Near ? At - Near : Of.Begins)) / 1e6;
    for (std::size_t I = 0; I < Count; I++)
        Made[I] = (Of.Toward.Values[I] - Of.From.Values[I]) * static_cast<double>(After - Before) / Seconds;
    return Made;
}

void Animator::Begin(std::uint64_t Serial, std::uint64_t At, const Theme &Look)
{
    {
        Animation &Of = All.at(Serial);
        /* How long it takes, as the person wants things to move now. */
        const std::uint64_t Speed = std::clamp<std::uint32_t>(Wanted.Speed, 25, 400);
        Of.Run   = std::uint64_t{Of.How.Duration} * 1000u * 100u / Speed;
        Of.Curve = CurveOf(Of.How, Wanted.Less);
        if (Of.How.Repeats == ITEM_FOREVER) {
            Of.Runs = 0;
        } else {
            const std::uint64_t Again = std::uint64_t{Of.How.Repeats} + (Of.How.Back && Of.How.Repeats % 2 == 1 ? 1u : 0u);
            Of.Runs = Again + 1;
        }
        if (Wanted.Less) {
            Of.Run  = Moves(Of.Property) ? 0 : std::min(Of.Run, LessLongest);
            Of.Runs = 1;
        }
        if (Of.Runs != 0 && Times(Of.Run, Of.Runs) == std::numeric_limits<std::uint64_t>::max()) Of.Runs = 0;
    }

    /* The one under way on this property gives way where it stands, handing
     * on its speed. */
    std::vector<double>        Carried;
    std::vector<std::uint32_t> CarriedForm;
    const std::uint32_t        Item     = All.at(Serial).Item;
    const std::uint16_t        Property = All.at(Serial).Property;
    if (const auto Same = OfItem.find(Item); Same != OfItem.end()) {
        const std::vector<std::uint64_t> Others = Same->second;
        for (std::uint64_t Next : Others) {
            const auto It = All.find(Next);
            if (Next == Serial || It == All.end() || It->second.Stage != State::Under || It->second.Property != Property) continue;
            Animation &Other = It->second;
            Show(Other, At, Look);
            if (Other.Goes == Way::Walks) {
                Carried     = SpeedOf(Other, At);
                CarriedForm = Other.Toward.Form;
            }
            End(Next, At, false);
        }
    }
    Animation &Of = All.at(Serial);
    items::Item *It = Items.Find(Of.Item);
    if (!It) {
        End(Serial, At, false);
        return;
    }
    Of.Stage  = State::Under;
    Of.Begins = At;
    Of.Goes   = Way::AtOnce;

    if (Of.Fresh) {
        /* Nothing seen to go from: at once. */
    } else if (Of.Run != 0 && Of.Property == ITEM_PROP_HIDDEN) {
        const bool Hide = Of.To.Hidden && *Of.To.Hidden, Hidden = HiddenOf(It->Said);
        if (!Hide && Hidden) {
            /* Shown from nothing: hidden no longer, then seen more and more. */
            (void)Items.Change(Of.Item, Of.Property, Of.Value);
            Of.SeenFrom = 0;
            Of.Goes     = Way::Shows;
        } else if (Hide != Hidden || It->Seen < 1) {
            Of.SeenFrom = It->Seen;
            Of.Goes     = Hide ? Way::Hides : Way::Shows;
        }
    } else if (Of.Run != 0) {
        Properties From, To;
        bool       Walkable = false;
        try {
            painter::items::Assign(Of.Property, It->Said, From);
            painter::items::Assign(Of.Property, Of.To, To);
            Walkable = Of.Property == ITEM_PROP_SIZE
                           ? SizeAsShown(*It, From)
                           : SayAsShown(Of.Property, It->Kind, It->Role, Look, From) && SayAsShown(Of.Property, It->Kind, It->Role, Look, To);
            if (Walkable) {
                MakeAlike(Of.Property, From, To);
                Walkable = NumbersOf(Of.Property, From, Look, Of.From) && NumbersOf(Of.Property, To, Look, Of.Toward) &&
                           Of.From.Form == Of.Toward.Form && Of.From.Values.size() == Of.Toward.Values.size();
            }
            if (Walkable) {
                Of.Like  = std::move(To);
                Of.Speed = CarriedForm == Of.Toward.Form && Carried.size() == Of.Toward.Values.size()
                               ? std::move(Carried)
                               : std::vector<double>(Of.Toward.Values.size(), 0.0);
                Of.Goes  = Way::Walks;
            }
        } catch (...) {
            Walkable = false;
        }
        if (!Walkable && Fadable(Of.Property, It->Kind) && It->Drawn && !HiddenOf(It->Said) && It->Seen > 0) {
            /* Faded from the look it has: its drawing kept as it was, and
             * the new look said at once. */
            try {
                std::unique_ptr<Leaving, DropLeaving> Old(new Leaving);
                Old->Was.Id     = It->Id;
                Old->Was.Kind   = It->Kind;
                Old->Was.Role   = It->Role;
                Old->Was.Parent = It->Parent;
                Old->Was.Depth  = It->Depth;
                Old->Was.Said   = It->Said;
                Old->Was.Drawn  = std::move(It->Drawn);
                It->Fading      = std::move(Old);
                (void)Items.Change(Of.Item, Of.Property, Of.Value);
                Of.Goes = Way::Fades;
            } catch (...) {
            }
        }
    }
    if (Of.Goes == Way::AtOnce) {
        (void)Items.Change(Of.Item, Of.Property, Of.Value);
        End(Serial, At, true);
        return;
    }
    Moving++;
    if (Of.Goes == Way::Walks && items::Turns(Of.Property)) It->Turning = true;
    if (Of.Runs != 0) Schedule(Of.Begins + Times(Of.Run, Of.Runs), true, Serial);
    Show(Of, At, Look);
}

void Animator::Turns(std::uint32_t Item) noexcept
{
    items::Item *It = Items.Find(Item);
    if (!It || !It->Turning) return;
    if (const auto Same = OfItem.find(Item); Same != OfItem.end())
        for (std::uint64_t Serial : Same->second)
            if (const auto Each = All.find(Serial); Each != All.end() && Each->second.Stage == State::Under &&
                                                     Each->second.Goes == Way::Walks && items::Turns(Each->second.Property))
                return;
    /* Still at last: drawn sharp again where it lies. */
    It->Turning = false;
    Items.Recompose(Item);
}

void Animator::Finish(std::uint64_t Serial, std::uint64_t At)
{
    Animation &Of = All.at(Serial);
    if (items::Item *It = Items.Find(Of.Item)) {
        /* Exactly what was said, said as a plain change says it — a fade's
         * new look was said as it began. */
        if (Of.Goes != Way::Fades) (void)Items.Change(Of.Item, Of.Property, Of.Value);
        if (Of.Goes == Way::Shows || Of.Goes == Way::Hides) {
            It->Seen = 1;
            Items.Recompose(Of.Item);
        }
    }
    End(Serial, At, true);
}

void Animator::End(std::uint64_t Serial, std::uint64_t At, bool Finished) noexcept
{
    const auto Found = All.find(Serial);
    if (Found == All.end()) return;
    const Animation &Of = Found->second;
    if (Of.Goes == Way::Fades && Of.Stage == State::Under)
        if (items::Item *It = Items.Find(Of.Item); It && It->Fading) {
            It->Fading.reset();
            Items.Recompose(Of.Item);
        }
    if (Of.Stage == State::Under && Of.Goes != Way::AtOnce) Moving--;
    if (Of.Stage == State::Said) Forget(NewlySaid, Serial);
    if (Of.Stage == State::Waits)
        if (const auto Waits = WaitingOn.find(Of.How.After); Waits != WaitingOn.end()) {
            Forget(Waits->second, Serial);
            if (Waits->second.empty()) WaitingOn.erase(Waits);
        }
    Held.Animations--;
    Held.AnimationBytes -= Of.Value.size();
    const std::uint32_t Number = Of.How.Number, Item = Of.Item;
    const bool          Turned = items::Turns(Of.Property);
    All.erase(Found);
    if (const auto Same = OfItem.find(Item); Same != OfItem.end()) {
        Forget(Same->second, Serial);
        if (Same->second.empty()) OfItem.erase(Same);
    }
    if (Turned) Turns(Item);
    if (Number == 0) return;
    const auto Counted = ByNumber.find(Number);
    if (Counted == ByNumber.end()) return;
    Counted->second.Count--;
    Counted->second.Cut = Counted->second.Cut || !Finished;
    if (Counted->second.Count != 0) return;
    const bool Whole = !Counted->second.Cut;
    ByNumber.erase(Counted);
    try {
        Told.push_back({Item, ItemAnimationEnd{Number, static_cast<std::uint8_t>(Whole), {0, 0, 0}}});
    } catch (...) {
        // No room to keep the end to tell: the program does not hear it.
    }
    /* Those waiting on the number begin from the moment it ended. */
    const auto Waits = WaitingOn.find(Number);
    if (Waits == WaitingOn.end()) return;
    const std::vector<std::uint64_t> Woken = std::move(Waits->second);
    WaitingOn.erase(Waits);
    for (std::uint64_t Next : Woken) {
        const auto It = All.find(Next);
        if (It == All.end() || It->second.Stage != State::Waits) continue;
        It->second.Stage  = State::Due;
        It->second.Begins = At + DelayOf(It->second);
        try {
            Schedule(It->second.Begins, false, Next);
        } catch (...) {
            // No room to keep its moment: it waits until it is cut.
        }
    }
}

void Animator::Advance(std::uint64_t Now, const Theme &Look, const Ending &Ended) noexcept
{
    try {
        if (Wanted.Instant) {
            /* A text page: every change at once, in the order said. */
            std::vector<std::uint64_t> Order;
            Order.reserve(All.size());
            for (const auto &[Serial, Each] : All) Order.push_back(Serial);
            std::sort(Order.begin(), Order.end());
            for (std::uint64_t Serial : Order) {
                const auto It = All.find(Serial);
                if (It == All.end()) continue;
                (void)Items.Change(It->second.Item, It->second.Property, It->second.Value);
                End(Serial, Now, true);
            }
            Moments.clear();
        } else {
            /* What animates an item that is gone is cut. */
            std::vector<std::uint32_t> Gone;
            for (const auto &[Item, Serials2] : OfItem)
                if (!Items.Find(Item)) Gone.push_back(Item);
            for (std::uint32_t Item : Gone) Cut(Item, 0);
            /* What is due by now, earliest first; a moment since put off — its
             * animation cut, or its beginning moved — passed over. */
            while (!Moments.empty() && Moments.front().At <= Now) {
                std::pop_heap(Moments.begin(), Moments.end(), Later{});
                const Moment Next = Moments.back();
                Moments.pop_back();
                if (Stale(Next)) continue;
                if (Next.Ends) Finish(Next.Serial, Next.At);
                else Begin(Next.Serial, Next.At, Look);
            }
            for (auto &[Serial, Each] : All)
                if (Each.Stage == State::Under) Show(Each, Now, Look);
        }
    } catch (...) {
        // No room to bring them on: where they stand, brought on at the next
        // frame.
    }
    Prune();
    Shown = Now;
    for (const auto &[Item, End2] : Told) Ended(Item, End2);
    Told.clear();
}

std::uint64_t Animator::Due(bool Seen) const noexcept
{
    if (!Told.empty() || (Wanted.Instant && !All.empty())) return 1;
    std::uint64_t Next = Moments.empty() ? 0 : std::max<std::uint64_t>(Moments.front().At, 1);
    if (Seen && Moving != 0) Next = Next == 0 ? FrameAfter(Shown) : std::min(Next, FrameAfter(Shown));
    return Next;
}

}  // namespace items
