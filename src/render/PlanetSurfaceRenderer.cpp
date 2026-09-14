#include "render/PlanetSurfaceRenderer.hpp"

#include "world/Block.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace elysium {
namespace {

Color4u shade(Color4u c, float s) {
    c.r = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.r) * s), 0, 255));
    c.g = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.g) * s), 0, 255));
    c.b = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(c.b) * s), 0, 255));
    return c;
}

} // namespace

PlanetSurfaceRenderer::PlanetSurfaceRenderer() {
    // placeholder - full tip content pushed via cloud/MCP
}

} // namespace elysium
