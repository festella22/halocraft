#pragma once
#include "spark/mod/ModId.hpp"

// Halo's characters <-> Minecraft combat. Every living biped near the player is mirrored in
// Minecraft as an invisible, hittable stand-in (Minecraft does swords, crits, knockback, bows).
// Minecraft's hits come back as damage to the Halo character; Halo's damage to Chief goes to
// Minecraft's hearts and armour instead; Steve dying kills Chief so Halo respawns him.
namespace Combat {
    void install(Spark::ModId owner);
}
