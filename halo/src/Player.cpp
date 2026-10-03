#include "Player.hpp"
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <cstring>
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
        bool loggedGround = false;
        float teleportOriginZ = 0.0f;

        // Render thread -> game thread.
        std::atomic<bool> puppet{ false };
        std::mutex feetLock;
        Coords::V3 mcFeetHalo{};  // Minecraft's feet, in Halo coordinates
        Coords::V3 mcEyeHalo{};   // Minecraft's camera (eye, or F5's third person), Halo's camera goes there
        bool lookingBack = false; // F5's second mode: the camera faces Steve
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

        // A new map gets its own patch of the Minecraft world (Coords::setMap).
        const char* map = Engine::getMapName();
        const bool newMap = map && strncmp(map, lastMap, 32) != 0;
        if (newMap) {
            strncpy_s(lastMap, map, 32);
            Coords::setMap(lastMap);
            log(std::string("map ") + lastMap + ": Minecraft patch at x " + std::to_string(int(Coords::offsetX)) + ", z " +
                std::to_string(int(Coords::offsetZ)));
        }

        // A new player object (level start, respawn) or map: put Minecraft's player where Chief is.
        const auto handle = Engine::getPlayerHandle();
        if (handle != lastHandle || newMap) {
            lastHandle = handle;
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
        }
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

    void install(Spark::ModId owner) {
        // While Minecraft drives, Halo keeps only the look: no walking, jumping, crouching or shooting.
        Spark::UpdatePlayerControls::addHandler(owner, +[](void*, Spark::UpdatePlayerControls::Cursor next, float* a, float* b) {
            next(a, b);
            if (!puppet)
                return;
            if (auto* pc = Engine::getPlayerControllerPointer()) {
                pc->walkX = pc->walkY = 0.0f;
                pc->actions = 0;
                pc->gunTrigger = 0.0f;
            }
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
            }
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
