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

    // Overlay triple buffer: true when Minecraft published a frame since the last call.
    bool acquireOverlayFrame();
    const std::uint8_t* frontPixels();
    const proto::OverlaySlotHdr* frontHeader();
}
