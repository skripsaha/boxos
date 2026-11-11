/* speech.h — what the wizard says, in a bubble beside his head: a white frame of
 * box rules with a tail pointing at him, the words wrapped to fit it, typed in
 * letter by letter while his mouth moves, held while they are read, and faded.
 * The frame and the words are cells of text, so they stay crisp over the
 * pixels; only the shadow and the fading look at what is under them. */
#ifndef WIZARD_SPEECH_H
#define WIZARD_SPEECH_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "painter/tui/frame.h"

#include "look.h"
#include "scene.h"

namespace wizard {

// Words broken into lines no wider than Width cells, at spaces.
std::vector<std::string> Wrapped(std::string_view Words, std::size_t Width);

class Speech {
public:
    // A title (a spell's name, a potion's) over the words, to be said from Ms.
    void Say(std::string Title, std::string Words, std::uint64_t Ms);
    // Whether anything is still on show at Ms.
    bool Showing(std::uint64_t Ms) const;
    // 0 to 1: how far his mouth is open while the words are typed in.
    float Mouth(std::uint64_t Ms, std::uint32_t PerSecond) const;
    void Paint(painter::tui::Frame &Out, const Scene &Where, const Look &Shown, std::uint64_t Ms) const;

private:
    std::size_t Typed(std::uint64_t Ms, std::uint32_t PerSecond) const;

    std::string   Title;
    std::string   Words;
    std::uint64_t Began = 0;
    bool          Any = false;
};

}  // namespace wizard

#endif
