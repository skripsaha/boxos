#ifndef BOXOS_APPS_BONSAI_TREE_H
#define BOXOS_APPS_BONSAI_TREE_H

#include <cstdint>
#include <vector>

namespace bonsai {

struct Parameters {
  unsigned life = 32;
  unsigned density = 5;
};

enum class Material : std::uint8_t { leaf, wood };

struct Mark {
  unsigned row;
  unsigned col;
  float birth;
  Material material;
  char glyph;
  std::uint8_t shade;
};

struct Tree {
  std::vector<Mark> marks;
  unsigned root_col = 0;
};

Tree generate(unsigned rows, unsigned cols, std::uint32_t seed,
              Parameters parameters);

} // namespace bonsai

#endif
