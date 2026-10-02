#pragma once
#include "Link.hpp"
#include "spark/mod/ModId.hpp"

// Minecraft drives the player: Halo sends where it looks, Minecraft's physics walks, jumps and
// collides (against Halo's level, see Collision.cpp), and Chief is moved to wherever Steve is.
namespace Player {
    void install(Spark::ModId owner);  // game-thread hooks: freeze Halo's movement, move Chief

    // Render thread, every frame: fill Halo's half of SkyState and stream collision.
    // mc is null while Minecraft isn't connected and in its world.
    void frame(skycraft::proto::SkyState& sky, const skycraft::proto::McState* mc);
}
