// The shared memory Minecraft opens. Ported from SkyCraft's skse/src/Link.cpp (MIT, chasmlol);
// Halo plays Skyrim's role in protocol/skycraft_protocol.h.
#pragma once
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
    // Collision ring message; false when the ring is full (try again next frame).
    bool writeCollision(proto::ColType type, const void* payload, std::uint32_t bytes);
    // Single producer: only the window thread (Input.cpp) pushes.
    void pushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);

    // Overlay triple buffer: true when Minecraft published a frame since the last call.
    bool acquireOverlayFrame();
    const std::uint8_t* frontPixels();
    const proto::OverlaySlotHdr* frontHeader();
}
