// Ported unchanged from SkyCraft's skse/src/Clip.h (MIT, chasmlol); only the includes are added.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

// Cutting dug Minecraft blocks (unit cubes) out of Skyrim triangles: what's left of a triangle
// outside every cube, as convex polygons. Used for Skyrim's collision sent to Minecraft and for
// the meshes Skyrim draws. Minecraft coordinates.
namespace skycraft::Clip
{
	struct Vert
	{
		float p[3];  // position
		float b[3];  // weights of the source triangle's three corners (for the other vertex data)
	};
	using Poly = std::vector<Vert>;  // convex, in winding order
	using Cube = std::array<int, 3>;  // min corner of a dug block

	// Dug blocks merged into boxes (a TNT crater is a few boxes, not hundreds of cubes: far fewer
	// cuts, far fewer pieces). Float bounds, slop and reach included.
	struct Box
	{
		float lo[3];
		float hi[3];
	};

	// Faces lying exactly on a dug block's face go with the block: the ground's surface at a whole
	// block height belongs to the block under it.
	inline constexpr float kSlop = 1.0e-4f;

	inline Vert Lerp(const Vert& a_a, const Vert& a_b, float a_t)
	{
		Vert v;
		for (int i = 0; i < 3; ++i) {
			v.p[i] = a_a.p[i] + (a_b.p[i] - a_a.p[i]) * a_t;
			v.b[i] = a_a.b[i] + (a_b.b[i] - a_a.b[i]) * a_t;
		}
		return v;
	}

	// The parts of a_in below and above the plane p[a_axis] = a_value (either may come back empty).
	inline void Split(const Poly& a_in, int a_axis, float a_value, Poly& a_below, Poly& a_above)
	{
		a_below.clear();
		a_above.clear();
		const std::size_t n = a_in.size();
		for (std::size_t i = 0; i < n; ++i) {
			const Vert& a = a_in[i];
			const Vert& b = a_in[(i + 1) % n];
			const float da = a.p[a_axis] - a_value;
			const float db = b.p[a_axis] - a_value;
			if (da <= 0.0f) {
				a_below.push_back(a);
			}
			if (da >= 0.0f) {
				a_above.push_back(a);
			}
			if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
				Vert m = Lerp(a, b, da / (da - db));
				m.p[a_axis] = a_value;
				a_below.push_back(m);
				a_above.push_back(m);
			}
		}
		if (a_below.size() < 3) {
			a_below.clear();
		}
		if (a_above.size() < 3) {
			a_above.clear();
		}
	}

	inline float Area2(const Poly& a_poly)
	{
		float n[3] = { 0, 0, 0 };
		for (std::size_t i = 1; i + 1 < a_poly.size(); ++i) {
			const float* o = a_poly[0].p;
			const float  u[3] = { a_poly[i].p[0] - o[0], a_poly[i].p[1] - o[1], a_poly[i].p[2] - o[2] };
			const float  w[3] = { a_poly[i + 1].p[0] - o[0], a_poly[i + 1].p[1] - o[1], a_poly[i + 1].p[2] - o[2] };
			n[0] += u[1] * w[2] - u[2] * w[1];
			n[1] += u[2] * w[0] - u[0] * w[2];
			n[2] += u[0] * w[1] - u[1] * w[0];
		}
		return std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	}

	// a_up: how far a dug block reaches above its top (things lying on dug ground go with it).
	inline std::vector<Box> Merge(const std::vector<Cube>& a_cubes, float a_up = 0.0f)
	{
		std::vector<Box> out;
		if (a_cubes.empty()) {
			return out;
		}
		auto key = [](int a_x, int a_y, int a_z) {
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) | (std::uint32_t(a_z) & 0x1FFFFF);
		};
		std::unordered_set<std::uint64_t> left;
		left.reserve(a_cubes.size() * 2);
		for (const auto& c : a_cubes) {
			left.insert(key(c[0], c[1], c[2]));
		}
		std::vector<Cube> order(a_cubes);
		std::ranges::sort(order, [](const Cube& a_a, const Cube& a_b) { return std::tie(a_a[1], a_a[2], a_a[0]) < std::tie(a_b[1], a_b[2], a_b[0]); });
		for (const auto& c : order) {
			if (!left.contains(key(c[0], c[1], c[2]))) {
				continue;
			}
			// Greedy: as far as it goes along x, then z (whole rows), then y (whole layers).
			int x1 = c[0], z1 = c[2], y1 = c[1];
			while (left.contains(key(x1 + 1, c[1], c[2]))) {
				++x1;
			}
			for (bool grow = true; grow;) {
				for (int x = c[0]; x <= x1 && grow; ++x) {
					grow = left.contains(key(x, c[1], z1 + 1));
				}
				z1 += grow ? 1 : 0;
			}
			for (bool grow = true; grow;) {
				for (int z = c[2]; z <= z1 && grow; ++z) {
					for (int x = c[0]; x <= x1 && grow; ++x) {
						grow = left.contains(key(x, y1 + 1, z));
					}
				}
				y1 += grow ? 1 : 0;
			}
			for (int y = c[1]; y <= y1; ++y) {
				for (int z = c[2]; z <= z1; ++z) {
					for (int x = c[0]; x <= x1; ++x) {
						left.erase(key(x, y, z));
					}
				}
			}
			out.push_back({ { float(c[0]) - kSlop, float(c[1]) - kSlop, float(c[2]) - kSlop }, { float(x1 + 1) + kSlop, float(y1 + 1) + a_up + kSlop, float(z1 + 1) + kSlop } });
		}
		return out;
	}

	inline bool Touches(const Poly& a_poly, const Box& a_box)
	{
		for (int axis = 0; axis < 3; ++axis) {
			float lo = FLT_MAX, hi = -FLT_MAX;
			for (const auto& v : a_poly) {
				lo = std::min(lo, v.p[axis]);
				hi = std::max(hi, v.p[axis]);
			}
			if (hi < a_box.lo[axis] || lo > a_box.hi[axis]) {
				return false;
			}
		}
		return true;
	}

	// a_in minus every box, as convex pieces appended to a_out. Returns false if nothing was cut
	// (a_out then holds a_in unchanged).
	inline bool Subtract(const Poly& a_in, const std::vector<Box>& a_boxes, std::vector<Poly>& a_out)
	{
		std::vector<Poly> pieces{ a_in }, next;
		Poly              below, above, rest;
		bool              cut = false;
		for (const auto& box : a_boxes) {
			next.clear();
			for (auto& piece : pieces) {
				if (!Touches(piece, box)) {
					next.push_back(std::move(piece));
					continue;
				}
				cut = true;
				rest = std::move(piece);
				bool inside = true;
				for (int axis = 0; axis < 3 && inside; ++axis) {
					Split(rest, axis, box.lo[axis], below, above);
					if (!below.empty()) {
						next.push_back(below);
					}
					rest.swap(above);
					if (rest.empty()) {
						inside = false;
						break;
					}
					Split(rest, axis, box.hi[axis], below, above);
					if (!above.empty()) {
						next.push_back(above);
					}
					rest.swap(below);
					if (rest.empty()) {
						inside = false;
					}
				}
				// Whatever is left is inside the cube: gone.
			}
			pieces.swap(next);
			if (pieces.empty()) {
				break;
			}
		}
		for (auto& piece : pieces) {
			if (!cut || Area2(piece) > 1.0e-7f) {
				a_out.push_back(std::move(piece));
			}
		}
		return cut;
	}

	inline bool Subtract(const Poly& a_in, const std::vector<Cube>& a_cubes, std::vector<Poly>& a_out, float a_up = 0.0f)
	{
		return Subtract(a_in, Merge(a_cubes, a_up), a_out);
	}

	inline Poly FromTriangle(const float* a_a, const float* a_b, const float* a_c)
	{
		Poly poly(3);
		for (int i = 0; i < 3; ++i) {
			poly[0].p[i] = a_a[i];
			poly[1].p[i] = a_b[i];
			poly[2].p[i] = a_c[i];
			poly[0].b[i] = poly[1].b[i] = poly[2].b[i] = 0.0f;
		}
		poly[0].b[0] = poly[1].b[1] = poly[2].b[2] = 1.0f;
		return poly;
	}
}
