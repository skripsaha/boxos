/* typer_passages.cpp — the shelf. Every text here is original, plain ASCII with
 * single spaces, and long enough to fill a few lines and short enough to be
 * finished in a minute. The prose ends in three pangrams, the code is written
 * for the symbols a keyboard makes hardest to reach. */

#include <array>

#include "typer.h"

namespace {

constexpr std::array Prose = {
    std::string_view("The kettle clicked off just as the first light reached the window. She poured the water over the leaves, watched them unfurl, and carried the cup to the porch where the morning was still quiet."),
    std::string_view("At the market on Saturday the baker sells bread that is still warm. People line up before the doors open, talk about the weather, and leave with paper bags held close like small, fragrant treasures."),
    std::string_view("A good map does not show everything. It leaves out the cracks in the road and the names of the dogs, and keeps only what you need to find your way home before dark."),
    std::string_view("He learned to repair watches from his grandmother, who kept a tray of tiny screws in a biscuit tin. Every spring that stopped was a puzzle, and every puzzle ended with a quiet little tick."),
    std::string_view("When the train stopped between two stations, nobody complained. The passengers looked out at a field of yellow flowers, and for ten minutes the whole carriage forgot where it was going."),
    std::string_view("Rain on a tin roof sounds like a thousand tiny drums. We sat inside with a board game and a plate of toast, and nobody wanted the storm to end."),
    std::string_view("The lighthouse keeper counted ships the way other people count sheep. Some nights there were none, and he fell asleep listening to the sea turn over the same smooth stones."),
    std::string_view("Learning to type is mostly learning to stop looking. Your fingers already know where the letters live; all they need is for you to trust them and keep your eyes on the words."),
    std::string_view("A small garden teaches patience. You plant the seed, water it, and then wait while nothing seems to happen, until one morning there is a green curl pushing through the dark soil."),
    std::string_view("The old library smelled of dust and cedar. Between the shelves a boy found a book with no title on its spine, opened it at random, and read until the lamps came on outside."),
    std::string_view("A folder is a box, and a file can sit in only one box at a time. A tag is more like a sticky note: put as many on a file as you like, and find it again by any of them."),
    std::string_view("In BoxOS a program does not ask the machine for a file by its path. It describes what it wants with a few tags, and the system brings back everything that fits the description."),
    std::string_view("A computer is a patient machine. It will do the same small thing a billion times without complaint, and the whole art of programming is choosing which small thing is worth repeating."),
    std::string_view("Every program is a conversation with the hardware. The keyboard says a key went down, the screen answers with a shape, and between the two a few thousand instructions decide what the shape should be."),
    std::string_view("Good software feels quiet. It starts quickly, says what happened in plain words, and when something goes wrong it tells you what, and what to try next, instead of showing a number."),
    std::string_view("A bit is the smallest answer there is, a plain yes or no. Put eight of them side by side and you can name a letter; put enough of them together and you can name a whole world."),
    std::string_view("When a machine idles, it should truly rest. A processor that waits for a key without spinning uses almost no power, and wakes the instant the finger lands."),
    std::string_view("The cabin on the ship was small, but it had a window, a bunk, and a shelf for exactly one book. He chose carefully, and read it three times before the harbor came into view."),
    std::string_view("Nobody remembers the first time they learned to whistle. One day it was only air, and the next day it was a tune, as if the lips had been practicing in secret all along."),
    std::string_view("The cat watched the printer with deep suspicion. Each time it hummed and pushed out a warm page, she crouched lower, certain that this was the machine's way of hunting."),
    std::string_view("To measure a mountain, start at the bottom and count the steps. To measure a day, start with the first cup of coffee and notice how many small plans got done before lunch."),
    std::string_view("A river never argues with the rocks. It simply goes around, and over many years the rocks grow smooth and the river learns the shape of the valley it has made."),
    std::string_view("On the last day of summer the children built a raft from old doors and rope. It floated for exactly eleven seconds, and they talked about those eleven seconds all winter long."),
    std::string_view("Please keep your hands on the keys and your shoulders loose. Speed comes later; first comes rhythm, the steady beat of one letter after another, like walking down a quiet street."),
    std::string_view("The recipe said to stir until smooth, but nobody had written down how long that takes. After twenty minutes and one sore arm, the sauce finally agreed to be a sauce."),
    std::string_view("The quick brown fox jumps over the lazy dog, while five boxing wizards jump quickly and pack my box with five dozen liquor jugs."),
    std::string_view("Sphinx of black quartz, judge my vow. How vexingly quick daft zebras jump! The five boxing wizards jump quickly."),
    std::string_view("Pack my box with five dozen liquor jugs. Jackdaws love my big sphinx of quartz, and waltz, bad nymph, for quick jigs vex."),
};

constexpr std::array Code = {
    std::string_view("for (int i = 0; i < count; i++) { total += values[i] * 2; if (total > 1000) break; }"),
    std::string_view("if (ptr != NULL && ptr->size >= 16) { memcpy(dst, ptr->data, ptr->size); free(ptr); }"),
    std::string_view("struct Point { int x, y; }; static const Point origin = { 0, 0 }; Point next = origin; next.x += 4;"),
    std::string_view("while (head != tail) { item = queue[head]; head = (head + 1) % 64; handle(item); }"),
    std::string_view("auto Sum = [](const std::vector<int> &V) { int T = 0; for (int N : V) T += N; return T; };"),
    std::string_view("unsigned mask = (1u << bits) - 1; value = (value >> shift) & mask; flags |= value ^ 0xFF;"),
    std::string_view("switch (code) { case 0: return \"ok\"; case 1: return \"retry\"; default: return \"failed\"; }"),
    std::string_view("std::map<std::string, int> Seen; ++Seen[Word]; if (Seen[Word] > 3) { Print(\"again: \" + Word); }"),
    std::string_view("int main(void) { char buf[256]; int n = read(0, buf, sizeof(buf) - 1); buf[n] = 0; return n < 0; }"),
    std::string_view("template <typename T> T Clamp(T V, T Lo, T Hi) { return V < Lo ? Lo : (V > Hi ? Hi : V); }"),
    std::string_view("x = (a + b) * (c - d) / 2 % 7; y = x << 3 | 5; z = !(x == y) && (y != 0 || z <= 9);"),
    std::string_view("#define MAX(a, b) ((a) > (b) ? (a) : (b)) int big = MAX(3, 4) + MAX(x[0], x[1]) - 1;"),
};

}

std::size_t PassageCount(TextKind Kind)
{
    return Kind == TextKind::Code ? Code.size() : Prose.size();
}

std::string_view PassageAt(TextKind Kind, std::size_t Index)
{
    return Kind == TextKind::Code ? Code[Index] : Prose[Index];
}
