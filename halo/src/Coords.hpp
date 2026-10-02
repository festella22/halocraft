#pragma once
#include <cmath>
#include <cstring>
#include <numbers>
#include <string_view>

// Halo (Z up, world units) <-> Minecraft (Y up, blocks). One Halo world unit is 10 ft = 3.048 m,
// and a block is 1 m, so Chief (~0.7 units, 2.1 m) and Steve (1.8 blocks) come out about right.
namespace Coords {
    inline constexpr float kBlocksPerUnit = 3.048f;

    // Every Halo map lives in its own patch of the one Minecraft world, so blocks built on one
    // map don't show up on another. Patches sit on an 8x8 grid, 8192 blocks apart (Halo maps
    // span a few thousand blocks); that keeps coordinates under ~33k, where floats still resolve
    // 1/256 block for the collision and mesh data.
    inline constexpr double kSlotSpacing = 8192.0;
    inline double offsetX = 0.0, offsetZ = 0.0;

    inline void setMap(const char* name) {
        static constexpr std::string_view kMaps[] = {
            "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40",
            "beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation", "dangercanyon",
            "deathisland", "gephyrophobia", "hangemhigh", "icefields", "infinity", "longest", "prisoner",
            "putput", "ratrace", "sidewinder", "timberland", "wizard",
        };
        const std::string_view n = name ? std::string_view(name, strnlen(name, 32)) : std::string_view();
        int slot = -1;
        for (int i = 0; i < int(std::size(kMaps)); ++i)
            if (n == kMaps[i])
                slot = i;
        if (slot < 0) {  // custom maps: hash into the rest of the grid
            std::uint32_t h = 2166136261u;
            for (char c : n)
                h = (h ^ std::uint8_t(c)) * 16777619u;
            slot = int(std::size(kMaps)) + int(h % (63 - std::size(kMaps)));
        }
        ++slot;  // slot 0 (around the origin) is left to whatever was built before patches existed
        offsetX = (slot % 8 - 4) * kSlotSpacing;
        offsetZ = (slot / 8 - 4) * kSlotSpacing;
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
