// Halo's level collision -> Minecraft. Halo gives us the whole level's collision BSP up front as
// polygons, so unlike SkyCraft (which harvests Havok shapes near the player) this triangulates the
// level once, indexes triangles by 64-block column, and streams regions outward from the player.
// Triangle voxelization (SAT, steep-surface coarsening) and the message layout are ported from
// SkyCraft's skse/src/Collision.cpp (MIT, chasmlol).
#include "Collision.hpp"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Coords.hpp"
#include "Dig.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "engine/bsp/level_bsp.hpp"
#include "engine/map.hpp"
#include "memory/Memory.hpp"

namespace Collision {
    namespace {
        namespace proto = skycraft::proto;
        using Clock = std::chrono::steady_clock;

        constexpr int kRegionSize = 8;  // blocks per region edge (must match SkyCollision.REGION_SIZE)
        constexpr int kGrid = kRegionSize * 8;
        constexpr int kRadius = 5, kBelow = 3, kAbove = 2;  // regions streamed around the player
        constexpr auto kFrameBudget = std::chrono::microseconds(2000);
        constexpr float kSteepMin = 0.1f;    // |n.y| below this is a wall: keep it fine-grained
        constexpr float kSteepMax = 0.643f;  // |n.y| below this (steeper than ~50 deg) gets block-coarsened

        struct Tri { float v[9]; };

        std::uint64_t bspSignature = 0;
        std::uint32_t epoch = 0;
        std::vector<Tri> tris;
        // Triangles by 64-block (x, z) column, all heights; a region picks its own out when it's sent.
        // (Bucketing every triangle into every 8-block region up front took 1.4 million regions on
        // The Silent Cartographer.)
        constexpr int kColumn = 64;
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> columns;
        std::vector<std::array<float, 3>> normals;  // per triangle, unit length (zero: degenerate)
        std::vector<std::uint32_t> flags;            // per triangle, ColTriFlags (diggable, its material)
        int columnOf(float v) { return int(std::floor(v / kColumn)); }
        std::unordered_set<std::uint64_t> sent;

        std::uint64_t key(int x, int y, int z) {
            return (std::uint64_t(x & 0x1FFFFF) << 42) | (std::uint64_t(y & 0x1FFFFF) << 21) | std::uint64_t(z & 0x1FFFFF);
        }
        int floorDiv(float v) { return int(std::floor(v / kRegionSize)); }

        void sub(const float* a, const float* b, float* o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
        void cross(const float* a, const float* b, float* o) {
            o[0] = a[1] * b[2] - a[2] * b[1];
            o[1] = a[2] * b[0] - a[0] * b[2];
            o[2] = a[0] * b[1] - a[1] * b[0];
        }
        float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

        bool axisTest(const float* v0, const float* v1, const float* v2, const float* axis, float h) {
            const float p0 = dot(v0, axis), p1 = dot(v1, axis), p2 = dot(v2, axis);
            const float r = h * (std::fabs(axis[0]) + std::fabs(axis[1]) + std::fabs(axis[2]));
            return !(std::min({ p0, p1, p2 }) > r || std::max({ p0, p1, p2 }) < -r);
        }

        // Box centred at c with half-size h, triangle a/b/c with face normal n.
        bool triBoxOverlap(const float* c, float h, const float* ta, const float* tb, const float* tc, const float* n) {
            float v0[3], v1[3], v2[3];
            sub(ta, c, v0);
            sub(tb, c, v1);
            sub(tc, c, v2);
            for (int i = 0; i < 3; ++i)
                if (std::min({ v0[i], v1[i], v2[i] }) > h || std::max({ v0[i], v1[i], v2[i] }) < -h)
                    return false;
            if (std::fabs(dot(n, v0)) > h * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2])))
                return false;
            float e[3][3];
            sub(v1, v0, e[0]);
            sub(v2, v1, e[1]);
            sub(v0, v2, e[2]);
            static constexpr float kAxes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            for (auto& edge : e)
                for (auto& unit : kAxes) {
                    float axis[3];
                    cross(edge, unit, axis);
                    if (!axisTest(v0, v1, v2, axis, h))
                        return false;
                }
            return true;
        }

        // The level's collision materials (the structure BSP tag's "collision materials", which
        // surfaces index), as Halo material types; empty if not found. That tag's own struct comes
        // before its children in memory: walk back from the collision BSP to the reflexive pointing
        // at it ({count 1, address}); the collision materials reflexive is the one right before it.
        // Each entry: shader reference (16 bytes), 2 pad, material type (u16).
        std::vector<std::uint16_t> loadMaterials() {
            std::vector<std::uint16_t> out;
            const std::uintptr_t bsp = Engine::getBSPPointer();
            if (!bsp)
                return out;
            const std::uint32_t mapAddr = Engine::translateToMapAddress(bsp);
            for (std::uintptr_t at = (bsp - 8) & ~std::uintptr_t(3); at + 0x10000 > bsp; at -= 4) {
                const auto count = Memory::safeRead<std::uint32_t>(at), addr = Memory::safeRead<std::uint32_t>(at + 4);
                if (!count || !addr)
                    break;
                if (*count != 1 || *addr != mapAddr)
                    continue;
                const auto n = Memory::safeRead<std::uint32_t>(at - 12), list = Memory::safeRead<std::uint32_t>(at - 8);
                if (!n || !list || *n == 0 || *n > 4096)
                    break;
                const std::uintptr_t base = Engine::translateMapAddress(*list);
                for (std::uint32_t i = 0; i < *n; ++i) {
                    const auto group = Memory::safeRead<std::uint32_t>(base + i * 20);
                    const auto type = Memory::safeRead<std::uint16_t>(base + i * 20 + 18);
                    // A shader reference ('s...' group) or none; anything else: not what we think it is.
                    if (!group || !type || (*group >> 24 != 's' && *group != 0xFFFFFFFFu)) {
                        out.clear();
                        break;
                    }
                    out.push_back(*type);
                }
                break;
            }
            return out;
        }

        // A Halo material type as the Minecraft block it digs into (ColTri flags); 0: not diggable.
        std::uint32_t digFlags(std::uint16_t haloType) {
            proto::DigMaterial m;
            switch (haloType) {
            case 0: m = proto::kDigGrass; break;  // dirt
            case 1: m = proto::kDigSand; break;
            case 2: m = proto::kDigStone; break;
            case 3: m = proto::kDigSnow; break;
            case 4: m = proto::kDigPlanks; break;  // wood
            case 5: case 6: case 7: m = proto::kDigMetal; break;  // hollow, thin, thick metal: Forerunner and human structures
            case 9: m = proto::kDigGlass; break;
            case 29: m = proto::kDigOrganic; break;  // leaves
            case 31: m = proto::kDigIce; break;
            case 10: case 28: case 30: case 32: return 0;  // force field, water, energy shields
            default: m = proto::kDigStone; break;
            }
            return proto::kTriDiggable | (std::uint32_t(m) << proto::kTriMaterialShift);
        }

        // Every collision surface is a convex polygon walked through the edge table; fan it out,
        // facing out of the solid side (its plane's way) as SkyCraft's digging expects.
        void loadBsp() {
            tris.clear();
            columns.clear();
            normals.clear();
            flags.clear();
            sent.clear();
            const auto* verts = Engine::getBSPVertexArray();
            const auto* edges = Engine::getBSPEdgeArray();
            const auto* surfaces = Engine::getBSPSurfaceArray();
            const auto* planes = Engine::getBSPPlaneArray();
            const auto nVerts = Engine::getBSPVertexCount(), nEdges = Engine::getBSPEdgeCount(), nSurfaces = Engine::getBSPSurfaceCount();
            const auto nPlanes = Engine::getBSPPlaneCount();
            if (!verts || !edges || !surfaces || !planes)
                return;
            const auto materials = loadMaterials();
            std::uint32_t perMaterial[proto::kDigMaterialCount + 1]{};
            std::vector<Coords::V3> poly;
            for (std::uint32_t s = 0; s < nSurfaces; ++s) {
                poly.clear();
                std::uint32_t f = 0;
                if (!surfaces[s].bits.invisible)  // ponytail: invisible walls (the level's edges) stay
                    f = digFlags(surfaces[s].material < materials.size() ? materials[surfaces[s].material] : 2);
                ++perMaterial[(f & proto::kTriDiggable) ? (f >> proto::kTriMaterialShift) & 0xFF : proto::kDigMaterialCount];
                float out[3]{};  // the plane's normal (out of the solid), Minecraft axes
                if (surfaces[s].planeIndex < nPlanes) {
                    const auto& n = planes[surfaces[s].planeIndex].normal;
                    const float sign = surfaces[s].isFlipped ? -1.0f : 1.0f;
                    out[0] = n.x * sign, out[1] = n.z * sign, out[2] = -n.y * sign;
                }
                const std::uint32_t first = surfaces[s].firstEdgeIndex;
                std::uint32_t e = first;
                do {
                    if (e >= nEdges)
                        break;
                    const auto& edge = edges[e];
                    const bool left = edge.leftSurface == s;
                    const std::uint32_t vi = left ? edge.startVertex : edge.endVertex;
                    if (vi >= nVerts)
                        break;
                    const auto& p = verts[vi].pos;
                    poly.push_back(Coords::toMc(p.x, p.y, p.z));
                    e = left ? edge.forwardEdge : edge.backwardEdge;
                } while (e != first && poly.size() < 64);
                for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
                    Tri t{ { poly[0].x, poly[0].y, poly[0].z, poly[i].x, poly[i].y, poly[i].z, poly[i + 1].x, poly[i + 1].y, poly[i + 1].z } };
                    float e1[3], e2[3], n[3];
                    sub(t.v + 3, t.v, e1);
                    sub(t.v + 6, t.v, e2);
                    cross(e1, e2, n);
                    if (dot(n, out) < 0.0f)
                        for (int k = 0; k < 3; ++k)
                            std::swap(t.v[3 + k], t.v[6 + k]);
                    tris.push_back(t);
                    flags.push_back(f);
                }
            }
            std::string mix;
            for (int m = 0; m <= proto::kDigMaterialCount; ++m)
                if (perMaterial[m])
                    mix += " " + std::string(m == proto::kDigMaterialCount ? "fixed" : std::to_string(m).c_str()) + ":" + std::to_string(perMaterial[m]);
            log("collision: " + std::to_string(materials.size()) + " collision materials" + (materials.empty() ? " (not found: all stone)" : "") +
                "; surfaces by dig material" + mix);
            normals.resize(tris.size());
            for (std::uint32_t i = 0; i < tris.size(); ++i) {
                const float* v = tris[i].v;
                float e1[3], e2[3], n[3];
                sub(v + 3, v, e1);
                sub(v + 6, v, e2);
                cross(e1, e2, n);
                const float len = std::sqrt(dot(n, n));
                if (len < 1e-9f)
                    continue;  // degenerate: collides with nothing
                normals[i] = { n[0] / len, n[1] / len, n[2] / len };
                const int x0 = columnOf(std::min({ v[0], v[3], v[6] }) - 0.5f), x1 = columnOf(std::max({ v[0], v[3], v[6] }) + 0.5f);
                const int z0 = columnOf(std::min({ v[2], v[5], v[8] }) - 0.5f), z1 = columnOf(std::max({ v[2], v[5], v[8] }) + 0.5f);
                for (int x = x0; x <= x1; ++x)
                    for (int z = z0; z <= z1; ++z)
                        columns[key(x, 0, z)].push_back(i);
            }
            // Floors facing up outnumber ceilings facing down on any level: if not, the planes face the other way.
            const auto up = std::count_if(normals.begin(), normals.end(), [](const auto& n) { return n[1] > 0.7f; });
            const auto down = std::count_if(normals.begin(), normals.end(), [](const auto& n) { return n[1] < -0.7f; });
            log("collision: " + std::to_string(nSurfaces) + " BSP surfaces -> " + std::to_string(tris.size()) + " triangles in " +
                std::to_string(columns.size()) + " columns; " + std::to_string(up) + " face up, " + std::to_string(down) + " down");
        }

        // The triangles within half a block of a region.
        std::vector<std::uint32_t> trianglesNear(int rx, int ry, int rz) {
            std::vector<std::uint32_t> mine;
            const auto it = columns.find(key(columnOf(float(rx * kRegionSize)), 0, columnOf(float(rz * kRegionSize))));
            if (it == columns.end())
                return mine;
            constexpr float kHalf = kRegionSize * 0.5f + 0.5f;
            const float c[3] = { (rx + 0.5f) * kRegionSize, (ry + 0.5f) * kRegionSize, (rz + 0.5f) * kRegionSize };
            for (auto i : it->second) {
                const float* v = tris[i].v;
                const auto& n = normals[i];
                if (n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f)
                    continue;
                if (triBoxOverlap(c, kHalf, v, v + 3, v + 6, n.data()))
                    mine.push_back(i);
            }
            return mine;
        }

        bool send(proto::ColType type, const proto::ColRegion& header, const void* items, std::size_t itemBytes) {
            std::vector<std::uint8_t> payload(sizeof(header) + itemBytes);
            std::memcpy(payload.data(), &header, sizeof(header));
            if (itemBytes)
                std::memcpy(payload.data() + sizeof(header), items, itemBytes);
            return Link::writeCollision(type, payload.data(), std::uint32_t(payload.size()));
        }

        // False when the ring is full; the region is simply sent again later.
        bool sendRegion(int rx, int ry, int rz) {
            proto::ColRegion header{};
            header.minX = rx * kRegionSize;
            header.minY = ry * kRegionSize;
            header.minZ = rz * kRegionSize;
            header.maxX = header.minX + kRegionSize - 1;
            header.maxY = header.minY + kRegionSize - 1;
            header.maxZ = header.minZ + kRegionSize - 1;
            header.epoch = epoch;

            const auto mine = trianglesNear(rx, ry, rz);

            // The dug blocks around this region's diggable triangles, merged into boxes once.
            std::vector<skycraft::Clip::Box> boxes;
            if (Dig::any()) {
                float dlo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, dhi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for (auto i : mine)
                    if (flags[i] & proto::kTriDiggable)
                        for (int k = 0; k < 3; ++k) {
                            dlo[k] = std::min({ dlo[k], tris[i].v[k], tris[i].v[3 + k], tris[i].v[6 + k] });
                            dhi[k] = std::max({ dhi[k], tris[i].v[k], tris[i].v[3 + k], tris[i].v[6 + k] });
                        }
                if (dlo[0] <= dhi[0]) {
                    std::vector<skycraft::Clip::Cube> cubes;
                    Dig::collect(dlo, dhi, cubes);
                    boxes = skycraft::Clip::Merge(cubes);
                }
            }

            // Exact triangles first (the smooth collider), then the voxel grid (which marks it known).
            // A diggable triangle with dug blocks in it goes as its ghost (the surface as it was, for
            // telling what's inside) plus what's left of it.
            std::vector<proto::ColTri> out;
            out.reserve(mine.size());
            auto add = [&](const float* v, std::uint32_t f) {
                proto::ColTri t{};
                std::memcpy(t.v, v, sizeof(t.v));
                t.flags = f;
                out.push_back(t);
            };
            std::vector<skycraft::Clip::Box> nearBoxes;
            std::vector<skycraft::Clip::Poly> pieces;
            for (auto i : mine) {
                const float* v = tris[i].v;
                nearBoxes.clear();
                if ((flags[i] & proto::kTriDiggable) && !boxes.empty()) {
                    float tlo[3], thi[3];
                    for (int k = 0; k < 3; ++k) {
                        tlo[k] = std::min({ v[k], v[3 + k], v[6 + k] });
                        thi[k] = std::max({ v[k], v[3 + k], v[6 + k] });
                    }
                    for (const auto& b : boxes)
                        if (thi[0] >= b.lo[0] && tlo[0] <= b.hi[0] && thi[1] >= b.lo[1] && tlo[1] <= b.hi[1] && thi[2] >= b.lo[2] && tlo[2] <= b.hi[2])
                            nearBoxes.push_back(b);
                }
                if (nearBoxes.empty()) {
                    add(v, flags[i]);
                    continue;
                }
                add(v, flags[i] | proto::kTriGhost);
                pieces.clear();
                skycraft::Clip::Subtract(skycraft::Clip::FromTriangle(v, v + 3, v + 6), nearBoxes, pieces);
                for (const auto& piece : pieces)
                    for (std::size_t k = 1; k + 1 < piece.size(); ++k) {
                        const float part[9] = { piece[0].p[0], piece[0].p[1], piece[0].p[2], piece[k].p[0], piece[k].p[1], piece[k].p[2],
                            piece[k + 1].p[0], piece[k + 1].p[1], piece[k + 1].p[2] };
                        add(part, flags[i]);
                    }
            }
            header.count = std::uint32_t(out.size());
            if (!send(proto::kColTris, header, out.data(), out.size() * sizeof(proto::ColTri)))
                return false;

            constexpr int G = kGrid;
            std::vector<std::uint64_t> solid(G * G, 0), steep(G * G, 0);
            std::vector<std::uint64_t> digSolid(G * G, 0), digSteep(G * G, 0);  // diggable geometry
            const float ox = float(header.minX), oy = float(header.minY), oz = float(header.minZ);
            auto clampLo = [](float v) { return std::clamp(int(std::floor(v)), 0, G - 1); };
            auto clampHi = [](float v) { return std::clamp(int(std::ceil(v)) - 1, 0, G - 1); };
            for (auto i : mine) {
                const float* tv = tris[i].v;
                float a[3] = { (tv[0] - ox) * 8, (tv[1] - oy) * 8, (tv[2] - oz) * 8 };
                float b[3] = { (tv[3] - ox) * 8, (tv[4] - oy) * 8, (tv[5] - oz) * 8 };
                float c[3] = { (tv[6] - ox) * 8, (tv[7] - oy) * 8, (tv[8] - oz) * 8 };
                float lo[3], hi[3];
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min({ a[k], b[k], c[k] });
                    hi[k] = std::max({ a[k], b[k], c[k] });
                }
                if (hi[0] < 0 || hi[1] < 0 || hi[2] < 0 || lo[0] > G || lo[1] > G || lo[2] > G)
                    continue;
                float e1[3], e2[3], n[3];
                sub(b, a, e1);
                sub(c, a, e2);
                cross(e1, e2, n);
                const float len = std::sqrt(dot(n, n));
                if (len < 1e-9f)
                    continue;
                n[0] /= len, n[1] /= len, n[2] /= len;
                const float ny = std::fabs(n[1]);
                const bool flat = ny >= kSteepMax || ny < kSteepMin;
                auto& grid = (flags[i] & proto::kTriDiggable) ? (flat ? digSolid : digSteep) : (flat ? solid : steep);

                // Plane-guided: walk the two axes the triangle spreads over, solve for the third.
                int dom = 0;
                if (std::fabs(n[1]) > std::fabs(n[dom])) dom = 1;
                if (std::fabs(n[2]) > std::fabs(n[dom])) dom = 2;
                const int u = (dom + 1) % 3, v = (dom + 2) % 3;
                const float d = dot(n, a);
                const float r = 0.5f * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
                for (int iu = clampLo(lo[u]); iu <= clampHi(hi[u]); ++iu) {
                    for (int iv = clampLo(lo[v]); iv <= clampHi(hi[v]); ++iv) {
                        const float cu = iu + 0.5f, cv = iv + 0.5f;
                        const float s0 = (d - r - n[u] * cu - n[v] * cv) / n[dom];
                        const float s1 = (d + r - n[u] * cu - n[v] * cv) / n[dom];
                        const int a0 = std::max(clampLo(lo[dom]), int(std::floor(std::min(s0, s1) - 0.5f)));
                        const int a1 = std::min(clampHi(hi[dom]), int(std::ceil(std::max(s0, s1) - 0.5f)));
                        for (int id = a0; id <= a1; ++id) {
                            float cen[3];
                            cen[dom] = id + 0.5f, cen[u] = cu, cen[v] = cv;
                            if (triBoxOverlap(cen, 0.5f, a, b, c, n)) {
                                int p[3];
                                p[dom] = id, p[u] = iu, p[v] = iv;
                                grid[p[1] * G + p[2]] |= 1ull << p[0];
                            }
                        }
                    }
                }
            }

            // Steep (50-84 degree) surfaces snap to whole-block footprints, so Minecraft's own
            // step-up and jump rules decide what's climbable, like a cliff made of blocks.
            auto coarsen = [&](const std::vector<std::uint64_t>& steepGrid, std::vector<std::uint64_t>& solidGrid) {
                for (int by = 0; by < kRegionSize; ++by)
                    for (int bz = 0; bz < kRegionSize; ++bz)
                        for (int bx = 0; bx < kRegionSize; ++bx) {
                            const std::uint64_t xmask = 0xFFull << (bx * 8);
                            int minY = 99, maxY = -1;
                            for (int y = by * 8; y < by * 8 + 8; ++y)
                                for (int z = bz * 8; z < bz * 8 + 8; ++z)
                                    if (steepGrid[y * G + z] & xmask) {
                                        minY = std::min(minY, y);
                                        maxY = std::max(maxY, y);
                                    }
                            for (int y = minY; y <= maxY; ++y)
                                for (int z = bz * 8; z < bz * 8 + 8; ++z)
                                    solidGrid[y * G + z] |= xmask;
                        }
            };
            coarsen(steep, solid);
            coarsen(digSteep, digSolid);

            // Dug blocks: the diggable geometry in them is gone.
            if (Dig::any()) {
                const float rlo[3] = { ox + 0.5f, oy + 0.5f, oz + 0.5f };
                const float rhi[3] = { ox + kRegionSize - 0.5f, oy + kRegionSize - 0.5f, oz + kRegionSize - 0.5f };
                std::vector<skycraft::Clip::Cube> dug;
                Dig::collect(rlo, rhi, dug);
                for (const auto& cube : dug) {
                    const int bx = cube[0] - header.minX, by = cube[1] - header.minY, bz = cube[2] - header.minZ;
                    if (bx < 0 || by < 0 || bz < 0 || bx >= kRegionSize || by >= kRegionSize || bz >= kRegionSize)
                        continue;
                    const std::uint64_t keep = ~(0xFFull << (bx * 8));
                    for (int y = by * 8; y < by * 8 + 8; ++y)
                        for (int z = bz * 8; z < bz * 8 + 8; ++z)
                            digSolid[y * G + z] &= keep;
                }
            }
            for (std::size_t i = 0; i < solid.size(); ++i)
                solid[i] |= digSolid[i];

            std::vector<proto::ColBlock> blocks;
            for (int by = 0; by < kRegionSize; ++by)
                for (int bz = 0; bz < kRegionSize; ++bz)
                    for (int bx = 0; bx < kRegionSize; ++bx) {
                        proto::ColBlock blk{};
                        bool any = false;
                        for (int sy = 0; sy < 8; ++sy) {
                            std::uint64_t layer = 0;
                            for (int sz = 0; sz < 8; ++sz)
                                layer |= ((solid[(by * 8 + sy) * G + (bz * 8 + sz)] >> (bx * 8)) & 0xFF) << (sz * 8);
                            blk.bits[sy] = layer;
                            any |= layer != 0;
                        }
                        if (any) {
                            blk.x = header.minX + bx;
                            blk.y = header.minY + by;
                            blk.z = header.minZ + bz;
                            blocks.push_back(blk);
                        }
                    }
            header.count = std::uint32_t(blocks.size());
            return send(proto::kColRegion, header, blocks.data(), blocks.size() * sizeof(proto::ColBlock));
        }
    }

    void update(double mcX, double mcY, double mcZ) {
        const auto signature = Engine::getBSPSignature();
        if (!signature)
            return;
        if (signature != bspSignature) {  // a new level or BSP: start over
            bspSignature = signature;
            ++epoch;
            const std::uint32_t e = epoch;
            Link::writeCollision(proto::kColClear, &e, sizeof(e));  // ponytail: assumes the ring has room for 8 bytes
            loadBsp();
        }

        // Blocks dug or filled in: the regions they touch (and their half-block margins) go again.
        static std::vector<skycraft::Clip::Cube> changed;
        changed.clear();
        Dig::takeChanged(changed);
        for (const auto& c : changed)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz)
                        sent.erase(key(floorDiv(float(c[0] + dx)), floorDiv(float(c[1] + dy)), floorDiv(float(c[2] + dz))));

        // Nearest regions first, until this frame's budget is spent.
        const int px = floorDiv(float(mcX)), py = floorDiv(float(mcY)), pz = floorDiv(float(mcZ));
        const auto deadline = Clock::now() + kFrameBudget;
        for (int ring = 0; ring <= kRadius; ++ring)
            for (int dy = -kBelow; dy <= kAbove; ++dy)
                for (int dx = -ring; dx <= ring; ++dx)
                    for (int dz = -ring; dz <= ring; ++dz) {
                        if (std::max(std::abs(dx), std::abs(dz)) != ring)
                            continue;
                        const int rx = px + dx, ry = py + dy, rz = pz + dz;
                        const auto k = key(rx, ry, rz);
                        if (sent.contains(k))
                            continue;
                        if (!sendRegion(rx, ry, rz))
                            return;  // ring full: Minecraft is behind
                        sent.insert(k);
                        if (Clock::now() > deadline)
                            return;
                    }
    }
}
