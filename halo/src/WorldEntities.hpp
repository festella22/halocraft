#pragma once
#include <vector>
#include "skycraft_protocol.h"

// Geometry for Minecraft's per-frame world things: dropped items and blocks, arrows, tridents,
// block-breaking cracks, and the targeted block's outline. Positions are relative to `origin`
// (Minecraft coordinates) in Minecraft axes, the same vertex format as the chunk meshes.
namespace WorldEntities {
    // Extra vertex flag (ours, above Minecraft's): untextured, the colour is the whole look.
    inline constexpr std::uint32_t kFlagUntextured = 1u << 8;

    // cracks: block-breaking overlays, drawn last with Minecraft's multiply ("crumbling") blend.
    void build(const skycraft::proto::WorldEntities& in, const double origin[3], std::vector<skycraft::proto::RenVertex>& tris,
        std::vector<skycraft::proto::RenVertex>& cracks, std::vector<skycraft::proto::RenVertex>& outline);
}
