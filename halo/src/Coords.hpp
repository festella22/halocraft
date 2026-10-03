#pragma once
#include <cmath>
#include <numbers>

// Halo (Z up, world units) <-> Minecraft (Y up, blocks). Scaled so the players match: Chief's eye
// is 0.62 units above his feet, Steve's 1.62 blocks (Chief's ~0.7 units then come out at Steve's
// 1.8 blocks, so Halo's doors and corridors fit him). What Halo's crosshair is on is then what
// Minecraft targets. (A block ends up 0.38 units, about 1.2 m at Halo's 10 ft per unit.)
namespace Coords {
    inline constexpr float kBlocksPerUnit = 1.62f / 0.62f;

    // Every Halo level load plays in a fresh, empty patch of the one Minecraft world, so a restarted
    // mission doesn't find the last run's blocks (dying and respawning keep them). Patches sit on a
    // 32x32 grid 8192 blocks apart (a Halo map spans a few thousand blocks), which keeps coordinates
    // under 131k, where floats still resolve 1/64 block for the collision and mesh data.
    inline constexpr double kSlotSpacing = 8192.0;
    inline constexpr int kGrid = 32;
    inline double offsetX = 0.0, offsetZ = 0.0;

    // ponytail: wraps after 1023 level loads, when the oldest patch's blocks come back.
    inline void usePatch(unsigned slot) {
        slot = 1 + slot % (kGrid * kGrid - 1);  // patch 0 (the origin) holds builds from before patches
        offsetX = (int(slot % kGrid) - kGrid / 2) * kSlotSpacing;
        offsetZ = (int(slot / kGrid) - kGrid / 2) * kSlotSpacing;
    }

    struct V3 { float x, y, z; };

    inline V3 toMc(float hx, float hy, float hz) {
        return { float(hx * kBlocksPerUnit + offsetX), hz * kBlocksPerUnit, float(-hy * kBlocksPerUnit + offsetZ) };
    }
    inline V3 toHalo(double mx, double my, double mz) {
        return { float((mx - offsetX) / kBlocksPerUnit), float(-(mz - offsetZ) / kBlocksPerUnit), float(my / kBlocksPerUnit) };
    }

    // Halo yaw: radians, 0 = +X, counter-clockwise. Minecraft yaw: degrees, 0 = +Z (south), 90 = -X.
    inline float toMcYaw(float haloYaw) { return -haloYaw * 180.0f / std::numbers::pi_v<float> - 90.0f; }
    // Halo pitch: radians, up positive. Minecraft pitch: degrees, down positive.
    inline float toMcPitch(float haloPitch) { return -haloPitch * 180.0f / std::numbers::pi_v<float>; }
}
