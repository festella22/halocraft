// Forwards Halo's keyboard and mouse to Minecraft through the input ring. Both games get every key
// while you play (Halo still drives the player until Phase 2); while a Minecraft screen is open
// (inventory, chat, crafting) Halo gets nothing and the mouse moves Minecraft's cursor instead.
#include "Input.hpp"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include "Link.hpp"
#include "Log.hpp"

namespace Input {
    namespace {
        namespace proto = skycraft::proto;

        // DirectInput / set-1 scan code (0x80 = extended) -> SDL scancode, what Minecraft 26 uses.
        // From SkyCraft's skse/src/Input.cpp (MIT, chasmlol).
        constexpr auto kDikToSdl = [] {
            std::array<std::uint16_t, 256> t{};
            t[0x01] = 41;  // Esc
            for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
            t[0x0B] = 39;  // 0
            t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
            t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
            t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
            t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
            t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
            t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
            t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
            t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
            t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
            t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
            t[0x3C] = 59, t[0x3D] = 60, t[0x3F] = 62;  // F2 F3 F5 only: F1 F8 F9 F10 are Spark's keys
            t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
            t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
            t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
            t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
            t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
            t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB8] = 230;               // KPEnter RCtrl KP/ RAlt
            t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;                 // Home Up PgUp Left
            t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
            t[0xD3] = 76;                                                           // Delete
            return t;
        }();
        constexpr std::uint32_t kDikEscape = 0x01;

        HWND window = nullptr;
        WNDPROC original = nullptr;
        bool rawKeyboard = false, rawMouse = false;  // MCC uses raw input: then ignore legacy messages
        bool wasScreenOpen = false;

        bool screenOpen() { return mcScreenOpen.load(std::memory_order_relaxed); }

        // Returns true when Halo must not see this key.
        bool key(std::uint32_t dik, bool down) {
            if (dik == kDikEscape) {
                if (!screenOpen())
                    return false;  // Esc is Halo's pause menu...
                Link::pushInput(proto::kInKey, 41, down ? 1 : 0);  // ...or closes Minecraft's screen
                return true;
            }
            if (const auto sdl = kDikToSdl[dik & 0xFF])
                Link::pushInput(proto::kInKey, sdl, down ? 1 : 0);
            return screenOpen();
        }

        bool button(int sdlButton, bool down) {
            Link::pushInput(proto::kInMouseButton, std::uint16_t(sdlButton), down ? 1 : 0);
            return screenOpen();
        }

        bool scroll(int delta) {
            Link::pushInput(proto::kInScroll, 0, delta);
            return screenOpen();
        }

        void moveCursor(int dx, int dy) {
            const int w = overlayW.load(), h = overlayH.load();
            cursorX = std::clamp(cursorX.load() + dx, 0, std::max(w - 1, 0));
            cursorY = std::clamp(cursorY.load() + dy, 0, std::max(h - 1, 0));
            Link::pushInput(proto::kInCursor, 0, cursorX.load(), cursorY.load());
        }

        bool rawInput(HRAWINPUT handle) {
            RAWINPUT ri{};
            UINT size = sizeof(ri);
            if (GetRawInputData(handle, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
                return false;
            if (ri.header.dwType == RIM_TYPEKEYBOARD) {
                const auto& kb = ri.data.keyboard;
                if (!rawKeyboard) {
                    rawKeyboard = true;
                    log("input: raw keyboard");
                }
                if (kb.VKey == 0xFF || (kb.Flags & RI_KEY_E1))
                    return false;  // fake shift sequences, Pause
                return key((kb.MakeCode & 0x7F) | ((kb.Flags & RI_KEY_E0) ? 0x80 : 0), !(kb.Flags & RI_KEY_BREAK));
            }
            if (ri.header.dwType != RIM_TYPEMOUSE)
                return false;
            const auto& m = ri.data.mouse;
            if (!rawMouse) {
                rawMouse = true;
                log("input: raw mouse");
            }
            bool swallow = screenOpen();
            const USHORT f = m.usButtonFlags;
            static constexpr struct { USHORT down, up; int sdl; } kButtons[] = {
                { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1 },
                { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2 },
                { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 3 },
                { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4 },
                { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5 },
            };
            for (const auto& b : kButtons) {
                if (f & b.down)
                    button(b.sdl, true);
                if (f & b.up)
                    button(b.sdl, false);
            }
            if (f & RI_MOUSE_WHEEL)
                scroll(static_cast<SHORT>(m.usButtonData));
            if (screenOpen() && !(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY))
                moveCursor(m.lLastX, m.lLastY);
            return swallow;
        }

        // Returns true when the message is Minecraft's alone.
        bool handle(UINT msg, WPARAM wp, LPARAM lp) {
            const bool open = screenOpen();
            if (open && !wasScreenOpen) {  // a screen just opened: start the cursor in the middle
                cursorX = overlayW.load() / 2;
                cursorY = overlayH.load() / 2;
                Link::pushInput(proto::kInCursor, 0, cursorX.load(), cursorY.load());
            }
            wasScreenOpen = open;

            switch (msg) {
            case WM_INPUT:
                return rawInput(reinterpret_cast<HRAWINPUT>(lp));
            case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
                if (rawKeyboard)
                    return open;
                return key(((lp >> 16) & 0x7F) | ((lp & (1 << 24)) ? 0x80 : 0), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: return !rawMouse && button(1, msg == WM_LBUTTONDOWN);
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: return !rawMouse && button(2, msg == WM_MBUTTONDOWN);
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: return !rawMouse && button(3, msg == WM_RBUTTONDOWN);
            case WM_MOUSEWHEEL: return !rawMouse && scroll(GET_WHEEL_DELTA_WPARAM(wp));
            case WM_CHAR:
                if (open && wp >= 32)
                    Link::pushInput(proto::kInText, 0, std::int32_t(wp));  // ponytail: no UTF-16 surrogate pairs
                return open;
            case WM_KILLFOCUS:
                Link::pushInput(proto::kInReleaseAll, 0);
                return false;
            default:
                break;
            }
            return false;
        }

        LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
            if (Link::mcAlive() && !haloPaused.load(std::memory_order_relaxed) && handle(msg, wp, lp))
                return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wp, lp) : 0;  // WM_INPUT needs DefWindowProc's cleanup
            return CallWindowProcW(original, hwnd, msg, wp, lp);
        }

        BOOL CALLBACK findMainWindow(HWND hwnd, LPARAM out) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd) || hwnd == GetConsoleWindow())
                return TRUE;
            *reinterpret_cast<HWND*>(out) = hwnd;
            return FALSE;
        }
    }

    bool install() {
        if (window)
            return true;
        EnumWindows(findMainWindow, reinterpret_cast<LPARAM>(&window));
        if (!window) {
            log("input: MCC's window not found");
            return false;
        }
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
        log("input: hooked MCC's window");
        return true;
    }

    void uninstall() {
        if (!window)
            return;
        // ponytail: assumes nothing subclassed the window after us (Spark hooks before mods load).
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        Link::pushInput(proto::kInReleaseAll, 0);
        window = nullptr;
    }
}
