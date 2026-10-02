#pragma once
#include <atomic>

// Halo's keyboard and mouse, forwarded to Minecraft from MCC's window procedure.
namespace Input {
    bool install();
    void uninstall();

    // Written by the render thread (Overlay.cpp) every frame.
    inline std::atomic<bool> mcScreenOpen{ false };  // inventory, chat, crafting... Halo gets no input
    inline std::atomic<bool> haloPaused{ false };    // Halo's pause menu owns input
    inline std::atomic<int> overlayW{ 0 }, overlayH{ 0 };

    // Minecraft's cursor in overlay pixels while a screen is open (drawn by Overlay.cpp).
    inline std::atomic<int> cursorX{ 0 }, cursorY{ 0 };
}
