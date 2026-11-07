#ifndef TAGMAP_H
#define TAGMAP_H

/* tagmap — files as a map of their tags: what each tag holds, by size.
 *
 * tagmap_tiles.cpp lays tiles and finds the way between them, and takes
 * nothing of BoxOS; tagmap.cpp reads the files, draws, and hears the keys. */

#include <cstdint>
#include <string>
#include <vector>

/* A tile laid on the glass, in cells. */
struct TagmapTile {
    int         Row, Col, High, Wide;
    std::size_t Index;     /* which weight it shows */
};

/* How many of the weights, biggest first, get a tile of their own on High x
 * Wide cells, each at least a few rows by a word wide; what is left over is
 * one tile more, laid by `MoreWeight` (0 when nothing is left). That tile is
 * kept to a third of the room or less, so a long tail of small things is a
 * door to them and never the whole map. */
std::size_t TagmapFit(const std::vector<std::uint64_t> &Weights, int High, int Wide, std::uint64_t &MoreWeight);

/* The weights laid as near-square tiles over High x Wide cells from (Row,
 * Col), the biggest first ("squarified"): a cell is twice as tall as it is
 * wide, and the tiles are square to the eye, not to the grid. Weights must
 * come biggest first; a weight of 0 is laid as the least there can be. */
std::vector<TagmapTile> TagmapLay(const std::vector<std::uint64_t> &Weights, int Row, int Col, int High, int Wide);

enum class TagmapWay { Left, Right, Up, Down };

/* The tile the way from tile `From` leads to: of those wholly past its edge
 * that way, the nearest, being beside it counting for more than being close
 * aslant. `From` itself when there is none. */
std::size_t TagmapStep(const std::vector<TagmapTile> &Tiles, std::size_t From, TagmapWay Way);

/* 1.5 KiB, 3.2 MiB: a size as a person reads it. */
std::string TagmapSize(std::uint64_t Bytes);

#endif
