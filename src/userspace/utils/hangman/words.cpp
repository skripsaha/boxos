/* words.cpp — the word list and the deck (words.h). Every word is capital
 * letters only, four to eleven of them. */

#include "words.h"

#include <algorithm>
#include <array>

namespace hangman {

namespace {

using Word = std::string_view;

constexpr std::array Animals = std::to_array<Word>(
    {"LION", "TIGER", "ZEBRA", "GIRAFFE", "ELEPHANT", "MONKEY", "DOLPHIN", "PENGUIN", "KANGAROO", "CROCODILE", "HEDGEHOG",
     "SQUIRREL", "BUTTERFLY", "OCTOPUS", "GORILLA", "LEOPARD", "CHEETAH", "PANTHER", "RHINOCEROS", "BUFFALO", "CAMEL", "DONKEY",
     "FALCON", "EAGLE", "PARROT", "PEACOCK", "FLAMINGO", "PELICAN", "TURTLE", "LIZARD", "SCORPION", "SPIDER", "BEAVER", "OTTER",
     "BADGER", "WOLF", "RABBIT", "HAMSTER", "TORTOISE", "JELLYFISH", "SEAHORSE", "WALRUS", "PLATYPUS", "MOOSE", "RACCOON", "LOBSTER"});

constexpr std::array Food = std::to_array<Word>(
    {"BREAD", "CHEESE", "PASTA", "PIZZA", "BURGER", "SALAD", "NOODLES", "PANCAKE", "WAFFLE", "CHOCOLATE", "VANILLA", "CARAMEL",
     "HONEY", "BUTTER", "YOGURT", "OMELETTE", "SANDWICH", "SPAGHETTI", "SAUSAGE", "POTATO", "TOMATO", "CARROT", "CUCUMBER",
     "PUMPKIN", "BROCCOLI", "MUSHROOM", "ONION", "GARLIC", "PEPPER", "CINNAMON", "ALMOND", "PEANUT", "COCONUT", "BANANA", "ORANGE",
     "LEMON", "MANGO", "PEACH", "CHERRY", "GRAPE", "STRAWBERRY", "BLUEBERRY", "RASPBERRY", "PINEAPPLE", "WATERMELON"});

constexpr std::array Countries = std::to_array<Word>(
    {"FRANCE", "GERMANY", "ITALY", "SPAIN", "PORTUGAL", "BRAZIL", "ARGENTINA", "CANADA", "MEXICO", "JAPAN", "CHINA", "INDIA",
     "EGYPT", "KENYA", "NIGERIA", "MOROCCO", "TURKEY", "GREECE", "SWEDEN", "NORWAY", "FINLAND", "DENMARK", "POLAND", "IRELAND",
     "ICELAND", "AUSTRALIA", "THAILAND", "VIETNAM", "INDONESIA", "PERU", "CHILE", "CUBA", "JAMAICA", "SWITZERLAND", "AUSTRIA",
     "BELGIUM", "HUNGARY", "ESTONIA", "LATVIA", "CROATIA", "SINGAPORE", "MONGOLIA", "NEPAL", "PAKISTAN"});

constexpr std::array Science = std::to_array<Word>(
    {"ATOM", "MOLECULE", "ELECTRON", "PROTON", "NEUTRON", "GRAVITY", "ENERGY", "FORCE", "MAGNET", "PLASMA", "PHYSICS",
     "CHEMISTRY", "BIOLOGY", "GENETICS", "EVOLUTION", "MICROSCOPE", "TELESCOPE", "LABORATORY", "EXPERIMENT", "HYPOTHESIS",
     "THEORY", "VACCINE", "BACTERIA", "VIRUS", "CELL", "ENZYME", "PROTEIN", "OXYGEN", "HYDROGEN", "CARBON", "NITROGEN", "HELIUM",
     "NEON", "ISOTOPE", "CRYSTAL", "FRICTION", "VELOCITY", "MOMENTUM", "RADIATION", "ELEMENT", "REACTION", "CATALYST", "EQUATION"});

constexpr std::array Nature = std::to_array<Word>(
    {"FOREST", "RIVER", "MOUNTAIN", "VALLEY", "CANYON", "GLACIER", "VOLCANO", "DESERT", "JUNGLE", "MEADOW", "WATERFALL", "RAINBOW",
     "THUNDER", "LIGHTNING", "BLIZZARD", "HURRICANE", "TORNADO", "EARTHQUAKE", "SUNSET", "SUNRISE", "OCEAN", "LAGOON", "ISLAND",
     "PENINSULA", "CAVERN", "SWAMP", "TUNDRA", "PRAIRIE", "BREEZE", "MONSOON", "AVALANCHE", "GEYSER", "REEF", "CORAL", "BLOSSOM",
     "MOSS", "FERN", "PINE", "CACTUS", "ORCHID", "TULIP", "DAISY"});

constexpr std::array Sports = std::to_array<Word>(
    {"FOOTBALL", "BASKETBALL", "TENNIS", "CRICKET", "HOCKEY", "BASEBALL", "VOLLEYBALL", "BADMINTON", "ARCHERY", "BOXING",
     "WRESTLING", "SWIMMING", "DIVING", "ROWING", "SAILING", "SURFING", "SKIING", "SNOWBOARD", "SKATING", "CYCLING", "MARATHON",
     "SPRINT", "HURDLES", "JAVELIN", "DISCUS", "FENCING", "JUDO", "KARATE", "GYMNASTICS", "TRIATHLON", "RUGBY", "GOLF", "BOWLING",
     "CLIMBING", "POLO", "CURLING", "SQUASH", "SOFTBALL", "BILLIARDS", "PENALTY", "GOALKEEPER", "REFEREE", "STADIUM", "TROPHY"});

constexpr std::array Home = std::to_array<Word>(
    {"KITCHEN", "BEDROOM", "BATHROOM", "GARDEN", "GARAGE", "ATTIC", "CELLAR", "BALCONY", "WINDOW", "DOORBELL", "CURTAIN", "CARPET",
     "PILLOW", "BLANKET", "MATTRESS", "WARDROBE", "CUPBOARD", "BOOKCASE", "SOFA", "TABLE", "CHAIR", "LAMP", "CANDLE", "MIRROR",
     "CLOCK", "FRIDGE", "OVEN", "TOASTER", "KETTLE", "BLENDER", "SPONGE", "BROOM", "BUCKET", "LADDER", "HAMMER", "SCREWDRIVER",
     "CHIMNEY", "STAIRCASE", "FIREPLACE", "FURNITURE"});

constexpr std::array Jobs = std::to_array<Word>(
    {"DOCTOR", "NURSE", "TEACHER", "PILOT", "FARMER", "BAKER", "BUTCHER", "CARPENTER", "PLUMBER", "ELECTRICIAN", "MECHANIC",
     "ENGINEER", "ARCHITECT", "LAWYER", "JUDGE", "POLICEMAN", "FIREFIGHTER", "SOLDIER", "SAILOR", "ARTIST", "PAINTER", "SCULPTOR",
     "MUSICIAN", "WRITER", "JOURNALIST", "DENTIST", "PHARMACIST", "CHEMIST", "SCIENTIST", "ASTRONAUT", "DETECTIVE", "WAITER",
     "COOK", "BARBER", "TAILOR", "JEWELER", "GARDENER", "LIBRARIAN", "DRIVER", "FISHERMAN", "MINER"});

constexpr std::array Music = std::to_array<Word>(
    {"GUITAR", "PIANO", "VIOLIN", "CELLO", "FLUTE", "TRUMPET", "DRUMS", "HARP", "SAXOPHONE", "CLARINET", "TROMBONE", "ORGAN",
     "ACCORDION", "BANJO", "UKULELE", "MELODY", "RHYTHM", "HARMONY", "CHORUS", "TEMPO", "OCTAVE", "SYMPHONY", "ORCHESTRA",
     "CONCERT", "OPERA", "BALLAD", "LYRICS", "SINGER", "CHOIR", "TREBLE", "SONATA", "JAZZ", "BLUES", "REGGAE", "CLASSICAL",
     "CONDUCTOR", "MICROPHONE", "ENCORE", "NOTE", "SCALE"});

constexpr std::array Space = std::to_array<Word>(
    {"GALAXY", "PLANET", "COMET", "METEOR", "ASTEROID", "NEBULA", "ECLIPSE", "ORBIT", "ROCKET", "SHUTTLE", "SATELLITE",
     "OBSERVATORY", "ASTRONOMY", "MERCURY", "VENUS", "JUPITER", "SATURN", "URANUS", "NEPTUNE", "PLUTO", "COSMOS", "UNIVERSE",
     "QUASAR", "PULSAR", "SUPERNOVA", "STARDUST", "MOONLIGHT", "SOLSTICE", "EQUINOX", "CRATER", "LUNAR", "SOLAR", "ZENITH",
     "VOYAGER", "COSMONAUT", "SPACECRAFT"});

constexpr std::array AllCategories = std::to_array<Category>({
    {"animals", Animals}, {"food", Food},   {"countries", Countries}, {"science", Science}, {"nature", Nature},
    {"sports", Sports},   {"home", Home},   {"jobs", Jobs},           {"music", Music},     {"space", Space},
});

constexpr std::size_t RememberedWords = 24;

bool IsRare(char Letter)
{
    return Letter == 'J' || Letter == 'Q' || Letter == 'X' || Letter == 'Z' || Letter == 'K' || Letter == 'V';
}

}  // namespace

std::span<const Category> Categories()
{
    return AllCategories;
}

std::size_t CategoryNamed(std::string_view Name)
{
    for (std::size_t At = 0; At < AllCategories.size(); At++)
        if (AllCategories[At].Name == Name) return At;
    return AllCategories.size();
}

Level LevelOf(std::string_view Word)
{
    std::size_t Weight = Word.size();
    for (const char Letter : Word)
        if (IsRare(Letter)) Weight += 2;
    if (Weight <= 6) return Level::Easy;
    return Weight <= 9 ? Level::Medium : Level::Hard;
}

std::string_view LevelName(Level Which)
{
    switch (Which) {
    case Level::Easy:   return "easy";
    case Level::Medium: return "medium";
    case Level::Hard:   return "hard";
    }
    return "easy";
}

std::optional<Level> LevelNamed(std::string_view Name)
{
    for (const Level Each : {Level::Easy, Level::Medium, Level::Hard})
        if (LevelName(Each) == Name) return Each;
    return std::nullopt;
}

bool Deck::Recent(std::string_view Word) const
{
    return std::find(Seen.begin(), Seen.end(), Word) != Seen.end();
}

std::optional<Drawn> Deck::Draw(Luck &Chance, std::optional<std::size_t> Only, std::optional<Level> Hardness)
{
    std::vector<Drawn> Fresh;
    std::vector<Drawn> Stale;
    for (std::size_t Group = 0; Group < AllCategories.size(); Group++) {
        if (Only && *Only != Group) continue;
        for (const std::string_view Word : AllCategories[Group].Words) {
            if (Hardness && LevelOf(Word) != *Hardness) continue;
            (Recent(Word) ? Stale : Fresh).push_back({Word, Group});
        }
    }
    std::vector<Drawn> &From = Fresh.empty() ? Stale : Fresh;
    if (From.empty()) return std::nullopt;
    const Drawn Chosen = From[Chance.Below(static_cast<std::uint32_t>(From.size()))];
    Seen.push_back(Chosen.Word);
    if (Seen.size() > RememberedWords) Seen.erase(Seen.begin());
    return Chosen;
}

}  // namespace hangman
