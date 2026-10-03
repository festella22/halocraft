#pragma once
#include <cstdint>
#include <vector>
#include "Clip.h"

// Digging into Halo's world. Minecraft decides what's dug (its world keeps the dug blocks and
// sends them per section as kRenDug); in a dug block Halo's geometry is gone: not in the collision
// Minecraft sees (Collision.cpp) and not drawn (WorldRender's hole pass). Minecraft draws the
// hole's walls and the blocks it revealed itself. Data part ported from SkyCraft's skse/src/Dig.cpp
// (MIT, chasmlol). Render thread only.
namespace Dig {
    void onDug(const std::uint8_t* data, std::uint32_t bytes);  // a kRenDug message
    void clear();                                                // Minecraft is resending everything
    bool any();
    // Dug blocks whose cube overlaps [lo, hi] (Minecraft coordinates).
    void collect(const float lo[3], const float hi[3], std::vector<skycraft::Clip::Cube>& out);
    // Blocks dug (or filled back in) since the last call.
    void takeChanged(std::vector<skycraft::Clip::Cube>& out);
}
