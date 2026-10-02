#include "Player.hpp"
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include "Collision.hpp"
#include "Coords.hpp"
#include "Log.hpp"
#include "engine/halo1.hpp"
#include "engine/player.hpp"
#include "spark/hook/Hooks.hpp"

namespace Player {
    namespace {
        namespace proto = skycraft::proto;

        // Halo's player position is the biped's origin; Minecraft's is the feet. Calibration knob:
        // the first frame of each level logs how high Halo's origin sits above the ground under it.
        constexpr float kFeetOffsetUnits = 0.0f;

        std::uint32_t teleportSeq = 0;
        std::uint32_t lastHandle = 0;
        bool loggedGround = false;
        float teleportOriginZ = 0.0f;

        // Render thread -> game thread.
        std::atomic<bool> puppet{ false };
        std::mutex feetLock;
        Coords::V3 mcFeetHalo{};  // Minecraft's feet, in Halo coordinates
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

        // A new player object (level start, respawn): put Minecraft's player where Chief is.
        const auto handle = Engine::getPlayerHandle();
        if (handle != lastHandle) {
            lastHandle = handle;
            ++teleportSeq;
            loggedGround = false;
            teleportOriginZ = pos->z;
            log("teleporting Minecraft to Chief (seq " + std::to_string(teleportSeq) + ")");
        }

        const auto feet = Coords::toMc(pos->x, pos->y, pos->z - kFeetOffsetUnits);
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
            mcFeetHalo.z += kFeetOffsetUnits;
        }
        if (drive != puppet.exchange(drive))
            log(drive ? "Minecraft is driving Chief" : "Halo is driving Chief");

        // Chief was standing when we teleported; once Steve lands, the height difference is the offset.
        if (drive && !loggedGround && (mc->flags & proto::kMcOnGround)) {
            loggedGround = true;
            const float feetZ = float(mc->y / Coords::kBlocksPerUnit);
            log("calibration: Halo origin was " + std::to_string(teleportOriginZ - feetZ) + " units above Minecraft's feet (offset now " +
                std::to_string(kFeetOffsetUnits) + ")");
        }

        if (mc)
            Collision::update(mc->x, mc->y, mc->z);
        else
            Collision::update(feet.x, feet.y, feet.z);
    }

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
