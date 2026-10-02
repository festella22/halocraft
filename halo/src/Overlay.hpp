#pragma once

// Minecraft's hand + HUD layer, composited into Halo's back buffer at Present.
namespace Overlay {
    bool install();
    void uninstall();
}
