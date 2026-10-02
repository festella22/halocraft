// Ported from SkyCraft's skse/src/WorldRender.cpp (BuildEntities, Box, BuildArrow, BuildOutline;
// MIT, chasmlol), minus Skyrim's stuck-arrow and contact-shadow parts.
#include "WorldEntities.hpp"
#include <array>
#include <cmath>
#include <numbers>

namespace WorldEntities {
    namespace {
        namespace proto = skycraft::proto;

        constexpr float kPi = std::numbers::pi_v<float>;
        constexpr std::uint32_t kCutout = 1u << 0, kTranslucent = 1u << 1;
        constexpr std::uint32_t kFullSkyLight = 15u << 8;
        // Face normal flag (bits 4-6): Minecraft Direction ordinal + 1, for the fixed face shading.
        constexpr std::uint32_t face(int ordinalPlusOne) { return std::uint32_t(ordinalPlusOne) << 4; }

        std::vector<proto::RenVertex>* out = nullptr;

        void quad(const float p[4][3], const float uv[4], std::uint32_t color, std::uint32_t flags) {
            const float t[4][2] = { { uv[0], uv[1] }, { uv[2], uv[1] }, { uv[2], uv[3] }, { uv[0], uv[3] } };
            for (int k : { 0, 1, 2, 0, 2, 3 })
                out->push_back({ p[k][0], p[k][1], p[k][2], t[k][0], t[k][1], color, kFullSkyLight, flags });
        }

        // A brightness times a tint (RGBA8, r in the low byte), as a vertex colour.
        std::uint32_t tinted(std::uint32_t tint) {
            if (!tint)
                return 0xFFFFFFFFu;
            return 0xFF000000u | (tint & 0x00FFFFFFu);
        }

        // An axis-aligned box (turned `yaw` about its vertical centre line), one texture per face
        // group: sides, top, bottom.
        void box(const float mn[3], const float size[3], float yaw, const float* side, const float* top, const float* bottom, std::uint32_t topTint,
            std::uint32_t flags, bool shaded) {
            const float cx = mn[0] + size[0] * 0.5f, cz = mn[2] + size[2] * 0.5f;
            const float c = std::cos(yaw), s = std::sin(yaw);
            auto corner = [&](int i, float o[3]) {
                const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0], lz = ((i & 4) ? 0.5f : -0.5f) * size[2];
                o[0] = cx + lx * c - lz * s;
                o[1] = mn[1] + ((i & 2) ? size[1] : 0.0f);
                o[2] = cz + lx * s + lz * c;
            };
            // corner bits: 1 = +x, 2 = +y, 4 = +z; each face TL, TR, BR, BL seen from outside
            static constexpr int kFaces[6][4] = { { 6, 7, 5, 4 }, { 3, 2, 0, 1 }, { 7, 3, 1, 5 }, { 2, 6, 4, 0 }, { 2, 3, 7, 6 }, { 4, 5, 1, 0 } };
            static constexpr int kNormal[6] = { 4, 3, 6, 5, 2, 1 };  // south, north, east, west, up, down
            for (int f = 0; f < 6; ++f) {
                float p[4][3];
                for (int k = 0; k < 4; ++k)
                    corner(kFaces[f][k], p[k]);
                const float* uv = f == 4 ? top : f == 5 ? bottom : side;
                quad(p, uv, f == 4 ? tinted(topTint) : 0xFFFFFFFFu, flags | (shaded ? face(kNormal[f]) : 0));
            }
        }

        // Minecraft's arrow model, a bit smaller (it's chunky next to Halo's people), or a trident
        // icon, at a position flying along d (Minecraft axes, unit length).
        void arrow(float px, float py, float pz, const float d[3], const float* uvSide, const float* uvBack, bool trident) {
            constexpr float kArrowScale = 0.55f;
            float s[3] = { d[2], 0.0f, -d[0] };
            float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
            if (sl < 1e-3f)
                s[0] = 1.0f, s[2] = 0.0f, sl = 1.0f;
            s[0] /= sl, s[2] /= sl;
            const float u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
            constexpr float r = 0.70710678f;
            const float fins[2][3] = { { (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r }, { (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r } };
            auto at = [&](float along, const float* q, float side, const float* q2, float side2, float o[3]) {
                const float pos[3] = { px, py, pz };
                for (int k = 0; k < 3; ++k)
                    o[k] = pos[k] + d[k] * along + q[k] * side + (q2 ? q2[k] * side2 : 0.0f);
            };
            if (!trident) {
                constexpr float k = 0.9f / 16.0f * kArrowScale;
                for (const auto& q : fins) {
                    float p[4][3];
                    at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
                    at(4 * k, q, -2 * k, nullptr, 0, p[1]);
                    at(4 * k, q, 2 * k, nullptr, 0, p[2]);
                    at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
                    quad(p, uvSide, 0xFFFFFFFFu, kCutout);
                }
                float p[4][3];
                at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
                at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
                at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
                at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
                quad(p, uvBack, 0xFFFFFFFFu, kCutout);
            } else {
                constexpr float h = 0.9f;  // the icon's diagonal runs handle (bottom-left) to tip
                for (const auto& q : fins) {
                    float p[4][3];
                    at(0, q, h, nullptr, 0, p[0]);
                    at(h, q, 0, nullptr, 0, p[1]);
                    at(0, q, -h, nullptr, 0, p[2]);
                    at(-h, q, 0, nullptr, 0, p[3]);
                    quad(p, uvSide, 0xFFFFFFFFu, kCutout);
                }
            }
        }
    }

    void build(const proto::WorldEntities& in, const double o[3], std::vector<proto::RenVertex>& tris, std::vector<proto::RenVertex>& outline) {
        out = &tris;
        for (std::uint32_t i = 0; i < in.count; ++i) {
            const auto& e = in.entities[i];
            const float px = float(e.x - o[0]), py = float(e.y - o[1]), pz = float(e.z - o[2]);
            switch (e.kind) {
            case proto::kWeBlock: {  // a dropped block: a small cube spinning about its centre
                const float s = e.scale;
                const float mn[3] = { px - s * 0.5f, py - s * 0.5f, pz - s * 0.5f };
                const float sz[3] = { s, s, s };
                box(mn, sz, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], e.tint, kCutout, true);
                break;
            }
            case proto::kWeCrack: {
                const float mn[3] = { px, py, pz };
                box(mn, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], 0, kTranslucent, false);
                break;
            }
            case proto::kWeArrow:
            case proto::kWeTrident: {  // Minecraft arrows face (sin yaw, sin pitch, cos yaw)
                const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
                const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
                arrow(px, py, pz, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
                break;
            }
            case proto::kWeItem: {  // a flat sprite turning about the vertical
                const float spin = e.yaw * kPi / 180.0f, half = e.scale * 0.5f;
                const float rx = std::cos(spin) * half, rz = std::sin(spin) * half;
                const float p[4][3] = { { px - rx, py + half, pz - rz }, { px + rx, py + half, pz + rz }, { px + rx, py - half, pz + rz }, { px - rx, py - half, pz - rz } };
                quad(p, e.uv[0], 0xFFFFFFFFu, kCutout);
                break;
            }
            default:
                break;  // ponytail: contact shadows (kWeShadow) aren't drawn
            }
        }

        if (!in.hasSelection)
            return;
        constexpr float g = 0.002f;
        const float lo[3] = { float(in.selMin[0] - o[0]) - g, float(in.selMin[1] - o[1]) - g, float(in.selMin[2] - o[2]) - g };
        const float hi[3] = { float(in.selMax[0] - o[0]) + g, float(in.selMax[1] - o[1]) + g, float(in.selMax[2] - o[2]) + g };
        auto corner = [&](int i) { return std::array<float, 3>{ (i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2] }; };
        static constexpr int kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        constexpr std::uint32_t kBlack45 = 0x73000000u;  // Minecraft's outline
        for (const auto& edge : kEdges)
            for (int k : edge) {
                const auto c = corner(k);
                outline.push_back({ c[0], c[1], c[2], 0, 0, kBlack45, kFullSkyLight, kTranslucent | kFlagUntextured });
            }
    }
}
