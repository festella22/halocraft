#include "Player.hpp"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <mutex>
#include <numbers>
#include <string>
#include "Collision.hpp"
#include "Coords.hpp"
#include "Log.hpp"
#include "engine/halo1.hpp"
#include "engine/map.hpp"
#include "engine/player.hpp"
#include "spark/hook/Hooks.hpp"

namespace Player {
    namespace {
        namespace proto = skycraft::proto;

        // Halo's player position is the biped's origin; Minecraft's is the feet. Calibration knob:
        // the first frame of each level logs how high Halo's origin sits above the ground under it.
        // How far Halo's biped origin sits above its feet. It depends on the biped: 0.0 for the
        // multiplayer cyborg, ~0.09 for the campaign one. Measured on every level (see below).
        float feetOffset = 0.0f;

        // Never equal to a teleport Minecraft already acknowledged before this copy of the mod
        // loaded, or we'd think it arrived and drag Chief to wherever Steve was left.
        std::uint32_t teleportSeq = std::uint32_t(GetTickCount64());
        std::uint32_t lastHandle = 0;
        char lastMap[33] = {};
        bool freshStartSent = false;

        // %LOCALAPPDATA%\HaloCraft: patch.txt counts patches ever used; session.txt says which patch the
        // level that's running right now plays in.
        std::filesystem::path stateFile(const wchar_t* name) {
            wchar_t dir[MAX_PATH];
            if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH))
                return {};
            const std::filesystem::path folder = std::filesystem::path(dir) / L"HaloCraft";
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            return folder / name;
        }

        unsigned currentPatch = 0;

        // The same MCC, map and Chief as when the session was written: this copy of the mod was
        // hot-reloaded into a level that kept running (a restarted mission has a new Chief).
        void saveSession() {
            std::ofstream(stateFile(L"session.txt")) << GetCurrentProcessId() << ' ' << lastMap << ' ' << lastHandle << ' ' << currentPatch;
        }

        bool sameSession(std::uint32_t handle) {
            DWORD pid = 0;
            std::string map;
            std::uint32_t savedHandle = 0;
            unsigned patch = 0;
            std::ifstream in(stateFile(L"session.txt"));
            if (!(in >> pid >> map >> savedHandle >> patch) || pid != GetCurrentProcessId() || map != lastMap || savedHandle != handle)
                return false;
            currentPatch = patch;
            return true;
        }

        unsigned nextPatch() {
            unsigned n = 0;
            if (std::ifstream in(stateFile(L"patch.txt")); in)
                in >> n;
            std::ofstream(stateFile(L"patch.txt")) << n + 1;
            return n;
        }
        bool loggedGround = false;
        float teleportOriginZ = 0.0f;

        // Render thread -> game thread.
        std::atomic<bool> puppet{ false };
        std::mutex feetLock;
        Coords::V3 mcFeetHalo{};  // Minecraft's feet, in Halo coordinates
        Coords::V3 mcEyeHalo{};   // Minecraft's camera (eye, or F5's third person), Halo's camera goes there
        bool lookingBack = false; // F5's second mode: the camera faces Steve
        std::atomic<float> zoomNow{ 1.0f };  // render thread -> game thread
    }

    void frame(proto::SkyState& sky, const proto::McState* mc) {
        if (!Engine::isGameLoaded())
            return;
        const auto pos = Engine::getPlayerPosition();
        auto* controller = Engine::getPlayerControllerPointer();
        if (!pos || !controller) {
            puppet = false;
            return;
        }

        // Every level load: a fresh, empty patch of the Minecraft world, and Steve starts over. A hot
        // reload of this mod into a level that kept running carries on where it was instead.
        const char* map = Engine::getMapName();
        const auto handle = Engine::getPlayerHandle();
        const bool newMap = map && strncmp(map, lastMap, 32) != 0;
        if (newMap) {
            strncpy_s(lastMap, map, 32);
            const bool reloaded = sameSession(handle);
            if (!reloaded)
                currentPatch = nextPatch();
            freshStartSent = reloaded;
            Coords::usePatch(currentPatch);
            log(std::string("map ") + lastMap + ": Minecraft patch " + std::to_string(currentPatch) + (reloaded ? " (hot reload: kept)" : " (fresh)"));
        }

        // A new player object (level start, respawn) or map: put Minecraft's player where Chief is.
        if (handle != lastHandle || newMap) {
            lastHandle = handle;
            saveSession();
            ++teleportSeq;
            loggedGround = false;
            teleportOriginZ = pos->z;
            log("teleporting Minecraft to Chief (seq " + std::to_string(teleportSeq) + ")");
        }

        const auto feet = Coords::toMc(pos->x, pos->y, pos->z - feetOffset);
        sky.posX = feet.x;
        sky.posY = feet.y;
        sky.posZ = feet.z;
        sky.yaw = Coords::toMcYaw(controller->yaw);
        sky.pitch = Coords::toMcPitch(controller->pitch);
        sky.teleportSeq = teleportSeq;

        // Minecraft has arrived (acks the teleport only once its hold is released): it drives Chief.
        const bool drive = mc && mc->teleportAck == teleportSeq && !Engine::isPlayerInVehicle();
        if (drive) {
            std::lock_guard lock(feetLock);
            mcFeetHalo = Coords::toHalo(mc->x, mc->y, mc->z);
            mcFeetHalo.z += feetOffset;
            // F5: Minecraft's camera sits cameraDistance behind the eye (mode 1) or in front of it
            // looking back (mode 2); WorldRender draws Steve's body there.
            double back = 0.0;
            if (mc->cameraMode == 1)
                back = -mc->cameraDistance;
            else if (mc->cameraMode == 2)
                back = mc->cameraDistance;
            const double yaw = mc->yaw * std::numbers::pi / 180.0, pitch = mc->pitch * std::numbers::pi / 180.0;
            const double look[3] = { -std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
            mcEyeHalo = Coords::toHalo(mc->eyeX + look[0] * back, mc->eyeY + look[1] * back, mc->eyeZ + look[2] * back);
            lookingBack = mc->cameraMode == 2;
            // Only in first person: the scope is Steve's eye.
            zoomNow = mc->cameraMode == 0 ? 1.0f + (kMaxZoom - 1.0f) * std::clamp(mc->bowDraw, 0.0f, 1.0f) : 1.0f;
        }
        if (mc && !freshStartSent) {
            freshStartSent = true;
            Link::pushInput(proto::kInFreshStart, 0);
            log("Minecraft: fresh start (kits, health, food)");
        }

        if (!drive)
            zoomNow = 1.0f;
        if (drive != puppet.exchange(drive))
            log(drive ? "Minecraft is driving Chief" : "Halo is driving Chief");

        // Chief was standing when we teleported; once Steve lands, the height difference is the offset.
        if (drive && !loggedGround && (mc->flags & proto::kMcOnGround)) {
            loggedGround = true;
            const float measured = teleportOriginZ - float(mc->y / Coords::kBlocksPerUnit);
            // More than 0.15 means Chief wasn't standing on the level's own ground (mid-air, on a crate).
            if (measured >= 0.0f && measured < 0.15f)
                feetOffset = measured;
            log("calibration: Halo origin was " + std::to_string(measured) + " units above Minecraft's feet (offset now " +
                std::to_string(feetOffset) + ")");
        }

        if (mc)
            Collision::update(mc->x, mc->y, mc->z);
        else
            Collision::update(feet.x, feet.y, feet.z);
    }

    bool driving() { return puppet.load(std::memory_order_relaxed); }

    float zoom() { return zoomNow.load(std::memory_order_relaxed); }

    void install(Spark::ModId owner) {
        // While Minecraft drives, Halo keeps only the look: no walking, jumping, crouching, shooting,
        // melee or grenades. Halo has already read the input into the controller by now and this call
        // hands it to Chief, so blank it for the call and put it back after (clearing it after the
        // call let Chief's invisible gun keep firing).
        Spark::UpdatePlayerControls::addHandler(owner, +[](void*, Spark::UpdatePlayerControls::Cursor next, float* a, float* b) {
            auto* pc = puppet ? Engine::getPlayerControllerPointer() : nullptr;
            if (!pc) {
                next(a, b);
                return;
            }
            const Engine::PlayerController saved = *pc;
            pc->walkX = pc->walkY = 0.0f;
            pc->actions = 0;
            pc->gunTrigger = 0.0f;
            next(a, b);
            *pc = saved;
        }, nullptr);

        // Halo's camera sits at Minecraft's eye, so Halo's crosshair is exactly Minecraft's aim.
        Spark::UpdateCamera::addHandler(owner, +[](void*, Spark::UpdateCamera::Cursor next, float dt) {
            next(dt);
            if (!puppet)
                return;
            if (auto* cam = Engine::getPlayerCameraPointer()) {
                std::lock_guard lock(feetLock);
                cam->pos = { mcEyeHalo.x, mcEyeHalo.y, mcEyeHalo.z };
                if (lookingBack)  // ponytail: up is left alone, fine while the pitch is small
                    cam->fwd = { -cam->fwd.x, -cam->fwd.y, -cam->fwd.z };
                if (const float z = zoomNow; z > 1.001f)  // the bow's scope
                    cam->fov = 2.0f * std::atan(std::tan(cam->fov * 0.5f) / z);
            }
        }, nullptr);

        // Zoomed in, the mouse turns slower by the same factor, like Halo's own scopes.
        Spark::UpdatePlayerControlsAndLook::addHandler(owner, +[](void*, Spark::UpdatePlayerControlsAndLook::Cursor next, float dt, uint32_t budget) {
            auto* pc = puppet ? Engine::getPlayerControllerPointer() : nullptr;
            const float z = zoomNow;
            if (!pc || z <= 1.001f) {
                next(dt, budget);
                return;
            }
            const float yaw = pc->yaw, pitch = pc->pitch;
            next(dt, budget);
            float dYaw = pc->yaw - yaw;
            if (dYaw > std::numbers::pi_v<float>)
                dYaw -= 2.0f * std::numbers::pi_v<float>;
            else if (dYaw < -std::numbers::pi_v<float>)
                dYaw += 2.0f * std::numbers::pi_v<float>;
            pc->yaw = yaw + dYaw / z;
            pc->pitch = pitch + (pc->pitch - pitch) / z;
        }, nullptr);

        // After Halo moves everything, put Chief where Minecraft's player is.
        Spark::UpdateAllEntities::addHandler(owner, +[](void*, Spark::UpdateAllEntities::Cursor next) {
            next();
            if (!puppet)
                return;
            auto* entity = Engine::getPlayerEntity();
            if (!entity)
                return;
            std::lock_guard lock(feetLock);
            entity->pos = { mcFeetHalo.x, mcFeetHalo.y, mcFeetHalo.z };
            entity->vel = { 0.0f, 0.0f, 0.0f };
        }, nullptr);
    }
}
