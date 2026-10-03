#include "Dig.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include "Link.hpp"

namespace Dig {
    namespace {
        namespace proto = skycraft::proto;
        using Bits = std::array<std::uint64_t, 64>;  // bit x + 16z + 256y

        std::unordered_map<std::uint64_t, Bits> sections;
        std::uint32_t dugCount = 0;
        std::vector<skycraft::Clip::Cube> changed;
        constexpr std::size_t kMaxChanged = 65536;

        std::uint64_t sectionKey(std::int32_t x, std::int32_t y, std::int32_t z) {
            return (std::uint64_t(std::uint32_t(x) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(z) & 0x3FFFFF) << 20) |
                   std::uint64_t(std::uint32_t(y) & 0xFFFFF);
        }

        std::array<int, 3> unpackKey(std::uint64_t key) {
            auto sext = [](std::uint64_t v, int bits) { return int(std::int64_t(v << (64 - bits)) >> (64 - bits)); };
            return { sext(key >> 42, 22), sext(key & 0xFFFFF, 20), sext((key >> 20) & 0x3FFFFF, 22) };
        }

        std::uint32_t count(const Bits& bits) {
            std::uint32_t n = 0;
            for (auto word : bits)
                n += std::uint32_t(std::popcount(word));
            return n;
        }

        // Every block of a section whose bit differs between old and new (null: all clear).
        void noteChanged(int sx, int sy, int sz, const Bits* oldBits, const Bits* newBits) {
            for (int word = 0; word < 64; ++word) {
                std::uint64_t diff = (oldBits ? (*oldBits)[word] : 0) ^ (newBits ? (*newBits)[word] : 0);
                while (diff && changed.size() < kMaxChanged) {
                    const int bit = word * 64 + std::countr_zero(diff);
                    diff &= diff - 1;
                    changed.push_back({ sx * 16 + (bit & 15), sy * 16 + (bit >> 8), sz * 16 + ((bit >> 4) & 15) });
                }
            }
        }
    }

    // ponytail: SkyState.worldId is always 0 (every level load has its own patch instead), so the
    // message's world isn't checked.
    void onDug(const std::uint8_t* data, std::uint32_t bytes) {
        if (bytes < sizeof(proto::RenDug))
            return;
        proto::RenDug hdr{};
        std::memcpy(&hdr, data, sizeof(hdr));
        const auto key = sectionKey(hdr.sx, hdr.sy, hdr.sz);
        const auto it = sections.find(key);
        if (hdr.count == 0 || bytes < sizeof(proto::RenDug) + 512) {
            if (it != sections.end()) {
                dugCount -= count(it->second);
                noteChanged(hdr.sx, hdr.sy, hdr.sz, &it->second, nullptr);
                sections.erase(it);
            }
            return;
        }
        Bits bits;
        std::memcpy(bits.data(), data + sizeof(proto::RenDug), 512);
        if (it != sections.end()) {
            if (it->second == bits)
                return;
            dugCount -= count(it->second);
        }
        dugCount += count(bits);
        noteChanged(hdr.sx, hdr.sy, hdr.sz, it != sections.end() ? &it->second : nullptr, &bits);
        sections[key] = bits;
    }

    void clear() {
        for (const auto& [key, bits] : sections) {
            const auto s = unpackKey(key);
            noteChanged(s[0], s[1], s[2], &bits, nullptr);
        }
        sections.clear();
        dugCount = 0;
    }

    bool any() { return dugCount != 0; }

    void collect(const float lo[3], const float hi[3], std::vector<skycraft::Clip::Cube>& out) {
        if (!any())
            return;
        int l[3], h[3];
        for (int i = 0; i < 3; ++i) {
            l[i] = int(std::floor(lo[i] - skycraft::Clip::kSlop));
            h[i] = int(std::floor(hi[i] + skycraft::Clip::kSlop));
        }
        for (int sy = l[1] >> 4; sy <= h[1] >> 4; ++sy)
            for (int sz = l[2] >> 4; sz <= h[2] >> 4; ++sz)
                for (int sx = l[0] >> 4; sx <= h[0] >> 4; ++sx) {
                    const auto it = sections.find(sectionKey(sx, sy, sz));
                    if (it == sections.end())
                        continue;
                    const auto& bits = it->second;
                    for (int y = std::max(l[1], sy * 16); y <= std::min(h[1], sy * 16 + 15); ++y)
                        for (int z = std::max(l[2], sz * 16); z <= std::min(h[2], sz * 16 + 15); ++z)
                            for (int x = std::max(l[0], sx * 16); x <= std::min(h[0], sx * 16 + 15); ++x) {
                                const int bit = (x & 15) + 16 * (z & 15) + 256 * (y & 15);
                                if ((bits[bit >> 6] >> (bit & 63)) & 1)
                                    out.push_back({ x, y, z });
                            }
                }
    }

    void takeChanged(std::vector<skycraft::Clip::Cube>& out) {
        out.insert(out.end(), changed.begin(), changed.end());
        changed.clear();
    }
}
