#include "tree.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace bonsai {
namespace {

struct Point {
  float x;
  float y;
};

struct Branch {
  Point origin;
  float angle;
  float length;
  float width;
  float birth;
  unsigned depth;
};

struct Stroke {
  Point from;
  Point to;
  float width;
  float birth;
  std::uint8_t shade;
};

struct Crown {
  Point center;
  float radius;
  float birth;
};

class Generator {
public:
  Generator(std::uint32_t seed, Parameters parameters)
      : rng_(seed), parameters_(parameters),
        budget_(1 + parameters.density * 24) {
    pending_.reserve(budget_);
    strokes_.reserve(budget_ * 12);
    crowns_.reserve(budget_);
  }

  Tree run(unsigned rows, unsigned cols) {
    if (rows == 0 || cols == 0)
      return {};
    pending_.push_back({{0, 0}, random(-0.12f, 0.12f), 1, 0.075f, 0, 0});
    for (std::size_t index = 0; index < pending_.size(); ++index)
      grow(pending_[index]);
    return rasterize(rows, cols);
  }

private:
  std::mt19937 rng_;
  Parameters parameters_;
  unsigned budget_;
  unsigned branches_ = 1;
  std::vector<Branch> pending_;
  std::vector<Stroke> strokes_;
  std::vector<Crown> crowns_;

  float random(float low, float high) {
    return std::uniform_real_distribution<float>{low, high}(rng_);
  }

  void shoot(const Branch &parent, Point origin, float angle, float length,
             float birth, float width) {
    if (branches_ >= budget_)
      return;
    pending_.push_back({origin, std::clamp(angle, -1.45f, 1.45f), length,
                        width, birth, parent.depth + 1});
    ++branches_;
  }

  void grow(Branch branch) {
    const unsigned steps = std::max(4u, static_cast<unsigned>(
        std::ceil(parameters_.life * branch.length)));
    const unsigned max_depth = parameters_.density >= 8 ? 5 : 4;
    Point point = branch.origin;
    float angle = branch.angle;
    float bend = random(-0.035f, 0.035f);
    const unsigned side_count = std::min(6u, parameters_.density);
    unsigned side = 0;
    const float handedness = random(0, 1) < 0.5f ? -1 : 1;

    for (unsigned step = 1; step <= steps; ++step) {
      const float progress = static_cast<float>(step) / steps;
      bend = bend * 0.8f + random(-0.018f, 0.018f);
      angle = std::clamp(angle + bend - angle * 0.018f, -1.45f, 1.45f);
      const float distance = branch.length / steps;
      const Point next{point.x + std::sin(angle) * distance,
                       point.y + std::cos(angle) * distance};
      const float width = branch.width * (1 - 0.7f * progress);
      const float birth = branch.birth + progress * branch.length;
      const auto shade = static_cast<std::uint8_t>(random(0, 4));
      strokes_.push_back({point, next, width, birth, shade});
      point = next;

      if (branch.depth == 0 && side < side_count &&
          progress >= 0.3f + 0.5f * (side + 1) / (side_count + 1)) {
        const float direction = side % 2 == 0 ? handedness : -handedness;
        shoot(branch, point, direction * random(0.65f, 1.2f),
              random(0.40f, 0.58f) * (1 - 0.25f * progress), birth,
              width * 0.6f);
        ++side;
      }
    }

    const float birth = branch.birth + branch.length;
    if (branch.depth < max_depth && branch.length > 0.07f &&
        branches_ < budget_) {
      const float split = random(0.38f, 0.85f);
      const float width = branch.width * 0.48f;
      shoot(branch, point, angle - split,
            branch.length * random(0.48f, 0.68f), birth, width);
      shoot(branch, point, angle + split,
            branch.length * random(0.48f, 0.68f), birth, width);
    } else {
      crowns_.push_back({point, random(0.07f, 0.13f), birth});
    }
  }

  Tree rasterize(unsigned rows, unsigned cols) {
    float min_x = 0, max_x = 0, max_y = 0;
    for (const Stroke &stroke : strokes_) {
      min_x = std::min(min_x, stroke.to.x - stroke.width);
      max_x = std::max(max_x, stroke.to.x + stroke.width);
      max_y = std::max(max_y, stroke.to.y + stroke.width);
    }
    for (const Crown &crown : crowns_) {
      min_x = std::min(min_x, crown.center.x - crown.radius);
      max_x = std::max(max_x, crown.center.x + crown.radius);
      max_y = std::max(max_y, crown.center.y + crown.radius * 0.7f);
    }

    const float scale = std::min((cols - 1.0f) / (2 * (max_x - min_x)),
                                 (rows - 1.0f) / max_y);
    const float origin = (cols - 1.0f) / 2 - (max_x + min_x) * scale;
    Tree tree;
    tree.root_col = static_cast<unsigned>(std::clamp(
        std::lround(origin), 0L, static_cast<long>(cols - 1)));
    tree.marks.reserve(strokes_.size() * 4 + crowns_.size() * 24);
    for (const Stroke &stroke : strokes_)
      raster_stroke(tree, stroke, rows, cols, origin, scale);
    for (const Crown &crown : crowns_)
      raster_crown(tree, crown, rows, cols, origin, scale);
    std::stable_sort(tree.marks.begin(), tree.marks.end(),
                     [](const Mark &a, const Mark &b) { return a.birth < b.birth; });
    return tree;
  }

  static void mark(Tree &tree, int x, int y, unsigned rows, unsigned cols,
                   float birth, Material material, char glyph,
                   std::uint8_t shade) {
    if (x < 0 || y < 0 || static_cast<unsigned>(x) >= cols ||
        static_cast<unsigned>(y) >= rows)
      return;
    tree.marks.push_back({static_cast<unsigned>(y), static_cast<unsigned>(x),
                          birth, material, glyph, shade});
  }

  static void raster_stroke(Tree &tree, const Stroke &stroke, unsigned rows,
                            unsigned cols, float origin, float scale) {
    const float x1 = origin + stroke.from.x * scale * 2;
    const float y1 = rows - 1.0f - stroke.from.y * scale;
    const float x2 = origin + stroke.to.x * scale * 2;
    const float y2 = rows - 1.0f - stroke.to.y * scale;
    const float dx = x2 - x1, dy = y2 - y1;
    const int steps = std::max(1, static_cast<int>(
        std::ceil(std::max(std::abs(dx), std::abs(dy)))));
    const int radius = static_cast<int>(std::lround(stroke.width * scale));
    const char glyph = std::abs(dx) < 0.35f * std::abs(dy) ? '|'
                       : std::abs(dy) < 0.25f * std::abs(dx) ? '_'
                       : dx * dy < 0 ? '/' : '\\';
    for (int step = 0; step <= steps; ++step) {
      const float part = static_cast<float>(step) / steps;
      const int x = static_cast<int>(std::lround(x1 + dx * part));
      const int y = static_cast<int>(std::lround(y1 + dy * part));
      for (int offset = -radius; offset <= radius; ++offset)
        mark(tree, x + offset, y, rows, cols, stroke.birth, Material::wood,
             radius > 0 && std::abs(offset) < radius ? ':' : glyph, stroke.shade);
    }
  }

  void raster_crown(Tree &tree, const Crown &crown, unsigned rows,
                    unsigned cols, float origin, float scale) {
    const int center_x = static_cast<int>(std::lround(origin + crown.center.x * scale * 2));
    const int center_y = static_cast<int>(std::lround(rows - 1.0f - crown.center.y * scale));
    const int radius_x = std::max(2, static_cast<int>(std::lround(crown.radius * scale * 2)));
    const int radius_y = std::max(1, static_cast<int>(std::lround(crown.radius * scale * 0.7f)));
    for (int dy = -radius_y; dy <= radius_y; ++dy) {
      for (int dx = -radius_x; dx <= radius_x; ++dx) {
        const float distance = static_cast<float>(dx * dx) / (radius_x * radius_x) +
                               static_cast<float>(dy * dy) / (radius_y * radius_y);
        if (distance > random(0.75f, 1.2f) || random(0, 1) < 0.10f)
          continue;
        const auto shade = static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(random(0, 3) - dy + 1), 0, 3));
        mark(tree, center_x + dx, center_y + dy, rows, cols,
             crown.birth + distance * 0.18f, Material::leaf, '&', shade);
      }
    }
  }
};

} // namespace

Tree generate(unsigned rows, unsigned cols, std::uint32_t seed,
              Parameters parameters) {
  parameters.life = std::clamp(parameters.life, 8u, 160u);
  parameters.density = std::min(parameters.density, 12u);
  return Generator{seed, parameters}.run(rows, cols);
}

} // namespace bonsai
