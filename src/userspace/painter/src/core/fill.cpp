/*
 * fill.cpp — what a shape is filled with. See painter/core/fill.h.
 */

#include "painter/core/fill.h"

namespace painter::core {

Fill Fill::Picture(std::shared_ptr<const core::Picture> Source, const Transform &Place, core::Extend Past, bool Sharp)
{
    return Fill(PictureFill{std::move(Source), Place, Past, Sharp});
}

}  // namespace painter::core
