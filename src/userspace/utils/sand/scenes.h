/* scenes.h — the places a sandbox can begin in. Each is built from the floor
 * up, on whichever side the floor is. */
#ifndef SAND_SCENES_H
#define SAND_SCENES_H

#include <string>
#include <string_view>

#include "world.h"

namespace sand {

struct Scene {
    const char *Name;
    void      (*Build)(World &Into);
};

const Scene *FindScene(std::string_view Name);

// "empty, beach, volcano or garden"
std::string SceneNames();

}  // namespace sand

#endif
