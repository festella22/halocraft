// The shared memory Minecraft opens. Ported from SkyCraft's skse/src/Link.cpp (MIT, chasmlol);
// Halo plays Skyrim's role in protocol/skycraft_protocol.h.
#pragma once
#include <atomic>
#include <cstdint>
#include "skycraft_protocol.h"

namespace Link {
    namespace proto = skycraft::proto;

    bool create();
    bool valid();
    bool mcAlive();
    void heartbeat();
    void writeSkyState(const proto::SkyState& state);
    bool readMcState(proto::McState& out);
    // Dropped items, arrows, block cracks and the targeted block's outline (seqlock copy).
    bool readWorldEntities(proto::WorldEntities& out);
    // Render ring (Minecraft -> Halo): calls fn(type, payload, bytes) for each message, until
    // maxBytes have been consumed this call.
    template <class Fn> void drainRender(Fn&& fn, std::uint64_t maxBytes);
    std::uint8_t* renderRing();

    // Collision ring message; false when the ring is full (try again next frame).
    bool writeCollision(proto::ColType type, const void* payload, std::uint32_t bytes);
    // Single producer: only the window thread (Input.cpp) pushes.
    void pushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);

    // Overlay triple buffer: true when Minecraft published a frame since the last call.
    bool acquireOverlayFrame();
    const std::uint8_t* frontPixels();
    const proto::OverlaySlotHdr* frontHeader();

    template <class Fn> void drainRender(Fn&& fn, std::uint64_t maxBytes) {
        auto* ring = renderRing();
        if (!ring)
            return;
        auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff);
        auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff);
        const auto head = std::atomic_ref(headRef).load(std::memory_order_acquire);
        auto tail = std::atomic_ref(tailRef).load(std::memory_order_relaxed);
        auto* data = ring + proto::kRenRingDataOff;
        constexpr auto size = proto::kRenRingDataBytes;
        std::uint64_t done = 0;
        while (tail < head && done < maxBytes) {
            const auto pos = tail % size;
            const auto* hdr = reinterpret_cast<const proto::ColMsgHeader*>(data + pos);
            if (hdr->type == proto::kRenPad) {
                tail += size - pos;
                continue;
            }
            fn(hdr->type, data + pos + sizeof(proto::ColMsgHeader), hdr->payloadBytes);
            const auto msgBytes = (sizeof(proto::ColMsgHeader) + hdr->payloadBytes + 7) & ~7ull;
            tail += msgBytes;
            done += msgBytes;
        }
        std::atomic_ref(tailRef).store(tail, std::memory_order_release);
    }
}
