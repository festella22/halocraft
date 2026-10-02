#pragma once

// Streams Halo's level collision (the BSP) to Minecraft around the player, in SkyCraft's region
// format: exact triangles for the smooth collider plus a 1/8-block occupancy grid.
namespace Collision {
    // Game thread, once per frame. Player feet in Minecraft coordinates.
    void update(double mcX, double mcY, double mcZ);
}
