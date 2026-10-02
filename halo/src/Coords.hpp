#pragma once
#include <cmath>
#include <numbers>

// Halo (Z up, world units) <-> Minecraft (Y up, blocks). One Halo world unit is 10 ft = 3.048 m,
// and a block is 1 m, so Chief (~0.7 units, 2.1 m) and Steve (1.8 blocks) come out about right.
namespace Coords {
    inline constexpr float kBlocksPerUnit = 3.048f;

    struct V3 { float x, y, z; };

    inline V3 toMc(float hx, float hy, float hz) { return { hx * kBlocksPerUnit, hz * kBlocksPerUnit, -hy * kBlocksPerUnit }; }
    inline V3 toHalo(double mx, double my, double mz) {
        return { float(mx / kBlocksPerUnit), float(-mz / kBlocksPerUnit), float(my / kBlocksPerUnit) };
    }

    // Halo yaw: radians, 0 = +X, counter-clockwise. Minecraft yaw: degrees, 0 = +Z (south), 90 = -X.
    inline float toMcYaw(float haloYaw) { return -haloYaw * 180.0f / std::numbers::pi_v<float> - 90.0f; }
    inline float toHaloYaw(float mcYaw) { return -(mcYaw + 90.0f) * std::numbers::pi_v<float> / 180.0f; }
    // Halo pitch: radians, up positive. Minecraft pitch: degrees, down positive.
    inline float toMcPitch(float haloPitch) { return -haloPitch * 180.0f / std::numbers::pi_v<float>; }
}
