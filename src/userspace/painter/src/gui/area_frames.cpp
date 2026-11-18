/*
 * area_frames.cpp — an area drawn in frames (Area::Draw), apart from the rest
 * of it: a program that draws no frames carries none of this. See
 * painter/gui/area.h.
 */

#include <memory>

#include "painter/gui/area.h"
#include "painter/gui/frame.h"

namespace painter::gui {

Frame &Area::BeginFrame()
{
    if (!Frames) Frames = decltype(Frames)(new detail::Drawer(Anchored), [](detail::Drawer *Of) { delete Of; });
    return Frames->Begin();
}

void Area::EndFrame()
{
    Frames->End();
    Present();
}

}  // namespace painter::gui
