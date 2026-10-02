// Halo's level collision -> Minecraft. Halo gives us the whole level's collision BSP up front as
// polygons, so unlike SkyCraft (which harvests Havok shapes near the player) this triangulates the
// level once, buckets triangles by region, and streams regions outward from the player.
// Triangle voxelization (SAT, steep-surface coarsening) and the message layout are ported from
// SkyCraft's skse/src/Collision.cpp (MIT, chasmlol).
#include "Collision.hpp"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Coords.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "engine/bsp/level_bsp.hpp"

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
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets;  // region -> triangles near it
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

        // Every collision surface is a convex polygon walked through the edge table; fan it out.
        void loadBsp() {
            tris.clear();
            buckets.clear();
            sent.clear();
            const auto* verts = Engine::getBSPVertexArray();
            const auto* edges = Engine::getBSPEdgeArray();
            const auto* surfaces = Engine::getBSPSurfaceArray();
            const auto nVerts = Engine::getBSPVertexCount(), nEdges = Engine::getBSPEdgeCount(), nSurfaces = Engine::getBSPSurfaceCount();
            if (!verts || !edges || !surfaces)
                return;
            std::vector<Coords::V3> poly;
            for (std::uint32_t s = 0; s < nSurfaces; ++s) {
                poly.clear();
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
                    tris.push_back({ { poly[0].x, poly[0].y, poly[0].z, poly[i].x, poly[i].y, poly[i].z, poly[i + 1].x, poly[i + 1].y, poly[i + 1].z } });
                }
            }
            // Bucket by every region the triangle comes within half a block of (not its whole
            // bounding box: a long sloped triangle would land in thousands of empty regions).
            constexpr float kHalf = kRegionSize * 0.5f + 0.5f;
            for (std::uint32_t i = 0; i < tris.size(); ++i) {
                const float* v = tris[i].v;
                float e1[3], e2[3], n[3];
                sub(v + 3, v, e1);
                sub(v + 6, v, e2);
                cross(e1, e2, n);
                const float len = std::sqrt(dot(n, n));
                if (len < 1e-9f)
                    continue;  // degenerate: collides with nothing
                n[0] /= len, n[1] /= len, n[2] /= len;
                const int x0 = floorDiv(std::min({ v[0], v[3], v[6] }) - 0.5f), x1 = floorDiv(std::max({ v[0], v[3], v[6] }) + 0.5f);
                const int y0 = floorDiv(std::min({ v[1], v[4], v[7] }) - 0.5f), y1 = floorDiv(std::max({ v[1], v[4], v[7] }) + 0.5f);
                const int z0 = floorDiv(std::min({ v[2], v[5], v[8] }) - 0.5f), z1 = floorDiv(std::max({ v[2], v[5], v[8] }) + 0.5f);
                for (int x = x0; x <= x1; ++x)
                    for (int y = y0; y <= y1; ++y)
                        for (int z = z0; z <= z1; ++z) {
                            const float c[3] = { (x + 0.5f) * kRegionSize, (y + 0.5f) * kRegionSize, (z + 0.5f) * kRegionSize };
                            if (triBoxOverlap(c, kHalf, v, v + 3, v + 6, n))
                                buckets[key(x, y, z)].push_back(i);
                        }
            }
            log("collision: " + std::to_string(nSurfaces) + " BSP surfaces -> " + std::to_string(tris.size()) + " triangles in " +
                std::to_string(buckets.size()) + " regions");
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

            static const std::vector<std::uint32_t> kNone;
            const auto it = buckets.find(key(rx, ry, rz));
            const auto& mine = it != buckets.end() ? it->second : kNone;

            // Exact triangles first (the smooth collider), then the voxel grid (which marks it known).
            std::vector<proto::ColTri> out;
            out.reserve(mine.size());
            for (auto i : mine) {
                proto::ColTri t{};
                std::memcpy(t.v, tris[i].v, sizeof(t.v));
                out.push_back(t);
            }
            header.count = std::uint32_t(out.size());
            if (!send(proto::kColTris, header, out.data(), out.size() * sizeof(proto::ColTri)))
                return false;

            constexpr int G = kGrid;
            std::vector<std::uint64_t> solid(G * G, 0), steep(G * G, 0);
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
                auto& grid = (ny >= kSteepMax || ny < kSteepMin) ? solid : steep;

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
            for (int by = 0; by < kRegionSize; ++by)
                for (int bz = 0; bz < kRegionSize; ++bz)
                    for (int bx = 0; bx < kRegionSize; ++bx) {
                        const std::uint64_t xmask = 0xFFull << (bx * 8);
                        int minY = 99, maxY = -1;
                        for (int y = by * 8; y < by * 8 + 8; ++y)
                            for (int z = bz * 8; z < bz * 8 + 8; ++z)
                                if (steep[y * G + z] & xmask) {
                                    minY = std::min(minY, y);
                                    maxY = std::max(maxY, y);
                                }
                        for (int y = minY; y <= maxY; ++y)
                            for (int z = bz * 8; z < bz * 8 + 8; ++z)
                                solid[y * G + z] |= xmask;
                    }

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
