#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <limits>
#include <print>
#include <ranges>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Game state: flags that record what the player did.
// ---------------------------------------------------------------------------
struct State {
    int courage = 0;
    int caution = 0;
    int kindness = 0;
    bool took_lamp = false;
    bool helped_stranger = false;
    bool read_letter = false;
    bool opened_door = false;
    bool trusted_voice = false;
};

// ---------------------------------------------------------------------------
// A choice: label shown to the player, effect on state, next node id.
// ---------------------------------------------------------------------------
struct Choice {
    std::string label;
    int next;
    void (*apply)(State &);
};

// ---------------------------------------------------------------------------
// A story node: text, then a list of choices.
// ---------------------------------------------------------------------------
struct Node {
    std::string text;
    std::vector<Choice> choices;
};

// Helper to add a choice with a lambda effect.
template <class F> Choice make_choice(std::string label, int next, F &&f) {
    return Choice{std::move(label), next, [](State &s) { /* placeholder */ }};
}

class Game {
  public:
    Game() { build(); }

    void run() {
        std::print("=== THE LAMP IN THE RAIN ===\n");
        std::print("A short interactive story.\n\n");

        int current = 0;
        while (current >= 0 && current < static_cast<int>(nodes_.size())) {
            current = visit(current);
        }

        show_ending();
    }

  private:
    std::vector<Node> nodes_;
    State state_;

    // -----------------------------------------------------------------
    // Build the story graph.
    // -----------------------------------------------------------------
    void build() {
        // 0: start
        nodes_.push_back(
            {"It is night. Rain falls on an empty road.\n"
             "You see a small wooden house with one lit window.\n"
             "On the porch lies a rusty lamp.",
             {
                 {"Knock on the door.", 1, [](State &s) { s.courage++; }},
                 {"Take the lamp and walk away.", 2,
                  [](State &s) {
                      s.took_lamp = true;
                      s.caution++;
                  }},
                 {"Wait in the rain.", 3, [](State &s) { s.caution++; }},
             }});

        // 1: knocked on door
        nodes_.push_back(
            {"An old woman opens the door. She says nothing,\n"
             "but points at a letter on the table.",
             {
                 {"Read the letter.", 4,
                  [](State &s) { s.read_letter = true; }},
                 {"Ask her who she is.", 5, [](State &s) { s.kindness++; }},
                 {"Leave quietly.", 3, [](State &) {}},
             }});

        // 2: took lamp
        nodes_.push_back(
            {"The lamp is warm. Inside it something moves.\n"
             "A voice whispers: 'Turn me on, and I will show the way.'",
             {
                 {"Turn it on.", 6, [](State &s) { s.trusted_voice = true; }},
                 {"Put it down and run.", 3, [](State &s) { s.caution++; }},
             }});

        // 3: waited in rain (neutral path)
        nodes_.push_back(
            {"You wait. The rain stops.\n"
             "A stranger with a broken umbrella walks up to you.",
             {
                 {"Offer your coat.", 7,
                  [](State &s) {
                      s.kindness++;
                      s.helped_stranger = true;
                  }},
                 {"Ask for directions.", 8, [](State &s) { s.caution++; }},
                 {"Ignore him.", 9, [](State &) {}},
             }});

        // 4: read the letter
        nodes_.push_back(
            {"The letter is addressed to you.\n"
             "'The door under the stairs is not a door.\n"
             "Do not open it unless you are ready.'",
             {
                 {"Open the door under the stairs.", 10,
                  [](State &s) {
                      s.opened_door = true;
                      s.courage++;
                  }},
                 {"Burn the letter.", 11, [](State &s) { s.caution++; }},
                 {"Ask the old woman about it.", 5,
                  [](State &s) { s.kindness++; }},
             }});

        // 5: old woman
        nodes_.push_back(
            {"She smiles. 'I have waited a long time for someone\n"
             "who asks instead of takes.'",
             {
                 {"Ask what she means.", 12, [](State &s) { s.kindness++; }},
                 {"Say goodbye and leave.", 3, [](State &) {}},
             }});

        // 6: turned lamp on
        nodes_.push_back(
            {"The lamp fills the road with pale light.\n"
             "The rain becomes silver threads. A path appears.",
             {
                 {"Follow the path.", 10, [](State &s) { s.courage++; }},
                 {"Turn the lamp off.", 3, [](State &s) { s.caution++; }},
             }});

        // 7: helped stranger
        nodes_.push_back(
            {"The stranger takes your coat and laughs.\n"
             "'You are the first kind one in years.'\n"
             "He hands you a small brass key.",
             {
                 {"Take the key.", 10, [](State &s) { s.courage++; }},
                 {"Refuse politely.", 3, [](State &) {}},
             }});

        // 8: asked directions
        nodes_.push_back({"He points down the road and says nothing more.\n"
                          "You walk. The road loops back to the house.",
                          {
                              {"Enter the house again.", 1, [](State &) {}},
                              {"Keep walking into the dark.", 13,
                               [](State &s) { s.courage++; }},
                          }});

        // 9: ignored him
        nodes_.push_back({"The stranger sighs and disappears into the fog.\n"
                          "You are alone again.",
                          {
                              {"Go to the house.", 1, [](State &) {}},
                              {"Walk away from everything.", 13,
                               [](State &s) { s.caution++; }},
                          }});

        // 10: the door
        nodes_.push_back({"The door opens into a room full of lamps.\n"
                          "Every lamp holds a memory. One of them is yours.",
                          {
                              {"Take your lamp.", 14,
                               [](State &s) { s.opened_door = true; }},
                              {"Leave the room untouched.", 11,
                               [](State &s) { s.caution++; }},
                          }});

        // 11: burned letter / refused
        nodes_.push_back({"The house grows quiet. The lit window goes dark.\n"
                          "You feel you have missed something.",
                          {
                              {"Walk away.", 13, [](State &) {}},
                          }});

        // 12: asked what she means
        nodes_.push_back(
            {"'Everything you need is already in your hands,' she says.\n"
             "She gives you the lamp from the porch.",
             {
                 {"Accept the lamp.", 6,
                  [](State &s) {
                      s.took_lamp = true;
                      s.kindness++;
                  }},
                 {"Leave it on the porch.", 3, [](State &s) { s.caution++; }},
             }});

        // 13: walk away (neutral ending trigger)
        nodes_.push_back(
            {"You walk until the road ends. The rain begins again.",
             {
                 {"Stop and think.", -2, [](State &) {}},
             }});

        // 14: took your lamp
        nodes_.push_back(
            {"Your lamp is warm and familiar.\n"
             "When you touch it, the room turns into the road you came from.",
             {
                 {"Finish the journey.", -3, [](State &) {}},
             }});
    }

    // -----------------------------------------------------------------
    // Show a node, read a choice, apply it, return next id.
    // -----------------------------------------------------------------
    int visit(int id) {
        const Node &n = nodes_[id];
        std::print("\n{}\n\n", n.text);

        if (n.choices.empty())
            return -1;

        for (std::size_t i = 0; i < n.choices.size(); ++i) {
            std::print("  {}) {}\n", i + 1, n.choices[i].label);
        }

        while (true) {
            std::print("\n> ");
            std::string line;
            if (!std::getline(std::cin, line))
                return -1;
            if (line.empty())
                continue;

            int pick{};
            auto [p, ec] =
                std::from_chars(line.data(), line.data() + line.size(), pick);
            if (ec != std::errc{} || p != line.data() + line.size()) {
                std::print("Please enter a number.\n");
                continue;
            }
            if (pick < 1 || pick > static_cast<int>(n.choices.size())) {
                std::print("Choose between 1 and {}.\n", n.choices.size());
                continue;
            }

            const Choice &c = n.choices[pick - 1];
            c.apply(state_);
            return c.next;
        }
    }

    // -----------------------------------------------------------------
    // Choose an ending based on flags.
    // -----------------------------------------------------------------
    void show_ending() {
        std::print("\n=== ENDING ===\n\n");

        // Ending A: the full kind path
        if (state_.opened_door && state_.helped_stranger &&
            state_.kindness >= 2) {
            std::print("You step through the door holding your lamp.\n"
                       "The stranger is there, smiling. The old woman too.\n"
                       "They were waiting for someone kind enough to return.\n"
                       "The rain outside stops for good.\n\n"
                       "ENDING: THE HOUSE THAT REMEMBERS\n");
            return;
        }

        // Ending B: trusted the voice
        if (state_.trusted_voice && state_.opened_door) {
            std::print("The lamp's voice leads you deeper.\n"
                       "You never come back, but you are not lost.\n"
                       "You are simply somewhere else now.\n\n"
                       "ENDING: THE SILVER PATH\n");
            return;
        }

        // Ending C: caution path
        if (state_.caution >= 3 && !state_.opened_door) {
            std::print("You leave everything behind and keep walking.\n"
                       "The road is long, but it is yours.\n"
                       "Some doors are better left closed.\n\n"
                       "ENDING: THE LONG ROAD\n");
            return;
        }

        // Ending D: read the letter but never opened door
        if (state_.read_letter && !state_.opened_door) {
            std::print(
                "You fold the letter and put it in your pocket.\n"
                "Years later you still wonder what was behind the door.\n"
                "But you are alive, and that counts.\n\n"
                "ENDING: THE UNOPENED DOOR\n");
            return;
        }

        // Ending E: default
        std::print("The night ends without answers.\n"
                   "You walk on, carrying nothing but the memory of rain.\n\n"
                   "ENDING: THE QUIET ROAD\n");
    }
};

int main() {
    Game g;
    g.run();
    return 0;
}
