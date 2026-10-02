#include "Combat.hpp"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>
#include "Coords.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Player.hpp"
#include "engine/entity/entity_list.hpp"
#include "engine/player.hpp"
#include "engine/tag.hpp"
#include "engine/types/damage_event.hpp"
#include "spark/hook/Hooks.hpp"

namespace Combat {
    namespace {
        namespace proto = skycraft::proto;

        constexpr float kRangeBlocks = 64.0f;  // stand-ins exist this far around the player
        // Halo keeps shield and health as fractions of each character's maximum. Each bar counts as
        // this much Minecraft damage: a Grunt (health only) falls to three diamond-sword hits, an
        // Elite (shield + health) to six.
        constexpr float kMcHpPerBar = 20.0f;
        // Chief's whole vitality (shield + health = 2.0) is Steve's 20 health.
        constexpr float kMcHpPerChiefVitality = 10.0f;
        constexpr float kSkyrimToMcDamage = 5.0f;  // SkyCombat.SKYRIM_TO_MC_DAMAGE divides what we send
        constexpr std::uint32_t kInstantKill = 0x1 | 0x4;  // DamageEvent flags: single target, instant kill

        std::vector<proto::ActorRecord> actors;

        // Any damage effect works for a kill; the first one this map has.
        Engine::Tag* killTag() {
            static constexpr const char* kPaths[] = { "weapons\\assault rifle\\melee", "weapons\\pistol\\melee", "weapons\\frag grenade\\explosion",
                "weapons\\plasma grenade\\explosion" };
            for (const char* path : kPaths)
                if (auto* tag = Engine::findTag(path, "jpt!"))
                    return tag;
            return nullptr;
        }

        // Halo's own damage path (death animation, ragdoll, checkpoint/respawn), bypassing every
        // DamageEntity handler including ours.
        void kill(std::uint32_t handle, Engine::Entity* entity, std::uint32_t attacker) {
            auto* tag = killTag();
            if (!tag || !Spark::DamageEntity::original) {
                entity->shield = 0.0f;
                entity->health = 0.0f;  // ponytail: no damage tag on this map; it dies on its next hit
                return;
            }
            Engine::DamageEvent ev{};
            ev.damageTypeTagHandle = tag->tagID;
            ev.flags = kInstantKill;
            ev.interactorHandle = 0xFFFFFFFF;
            ev.attackerHandle = attacker;
            ev.sourceTypeIndex = 0xFFFF;
            ev.hitPosition = entity->pos;
            ev.hitDirection = { 0.0f, 0.0f, -1.0f };
            ev.baseDamage = 1.0f;
            ev.damageMultiplier = 1.0f;
            Spark::DamageEntity::original(&ev, handle, 0, 0, -1, 0);
        }

        // Minecraft hit a stand-in for `damage` (after its own armour, crit and enchantment maths).
        void onHit(std::uint32_t handle, float damage) {
            auto* e = Engine::entityValid(handle) ? Engine::getEntityPointer(handle) : nullptr;
            if (!e || e->entityCategory != Engine::EntityCategory_Biped || e->health <= 0.0f)
                return;
            float left = damage / kMcHpPerBar;  // shield first, the rest to health
            if (e->shield > 0.0f) {
                const float taken = std::min(e->shield, left);
                e->shield -= taken;
                left -= taken;
            }
            e->health -= left;
            if (e->health <= 0.0f)
                kill(handle, e, Engine::getPlayerHandle());
            // ponytail: non-lethal hits only lower the bars; Halo plays no flinch for them.
        }

        std::string_view baseName(const char* path) {
            if (!path)
                return {};
            std::string_view p(path);
            const auto slash = p.find_last_of('\\');
            return slash == std::string_view::npos ? p : p.substr(slash + 1);
        }

        // Every living biped near the player except Chief, in Minecraft space.
        void mirrorActors() {
            actors.clear();
            const auto playerHandle = Engine::getPlayerHandle();
            auto* chief = Engine::getPlayerEntity();
            if (!chief)
                return;
            const float range = kRangeBlocks / Coords::kBlocksPerUnit;
            Engine::foreachEntityRecordIndexed([&](Engine::EntityRecord* rec, std::uint16_t index) {
                if (actors.size() >= proto::kMaxActors)
                    return;
                const std::uint32_t handle = (std::uint32_t(rec->id) << 16) | index;
                auto* e = rec->entity();
                if (!e || handle == playerHandle || e->entityCategory != Engine::EntityCategory_Biped || e->health <= 0.0f)
                    return;
                const float dx = e->pos.x - chief->pos.x, dy = e->pos.y - chief->pos.y, dz = e->pos.z - chief->pos.z;
                if (dx * dx + dy * dy + dz * dz > range * range)
                    return;

                // The skeleton gives the size: highest bone over the feet, widest spread around them.
                float top = 0.7f, reach = 0.15f;
                const auto bones = e->worldBones.count();
                for (std::uint16_t i = 0; i < bones; ++i) {
                    if (const auto* b = e->worldBones.get(e, i)) {
                        top = std::max(top, b->pos.z - e->pos.z + 0.08f);
                        reach = std::max(reach, std::hypot(b->pos.x - e->pos.x, b->pos.y - e->pos.y));
                    }
                }
                proto::ActorRecord a{};
                a.formId = handle;
                a.flags = rec->typeId == Engine::TypeID_Marine ? 0u : std::uint32_t(proto::kActorHostile);
                const auto feet = Coords::toMc(e->pos.x, e->pos.y, e->pos.z);
                a.x = feet.x, a.y = feet.y, a.z = feet.z;
                a.yaw = Coords::toMcYaw(std::atan2(e->fwd.y, e->fwd.x));
                a.height = std::clamp(top * Coords::kBlocksPerUnit, 0.8f, 4.5f);
                a.width = std::clamp(2.0f * reach * Coords::kBlocksPerUnit, 0.5f, 2.5f);
                a.healthFrac = std::clamp(e->health, 0.0f, 1.0f);
                a.level = 1;
                const auto name = baseName(e->getTagResourcePath());
                std::memcpy(a.name, name.data(), std::min(name.size(), sizeof(a.name) - 1));
                actors.push_back(a);
            });
            Link::writeActors(actors.data(), std::uint32_t(actors.size()));
        }

        void drainEvents() {
            proto::McEvent ev{};
            while (Link::popEvent(ev)) {
                if (ev.type == proto::kEvHitActor) {
                    onHit(ev.formId, ev.a);
                } else if (ev.type == proto::kEvPlayerDied) {
                    if (auto* chief = Engine::getPlayerEntity(); chief && chief->health > 0.0f) {
                        log("Steve died: killing Chief");
                        kill(Engine::getPlayerHandle(), chief, ev.formId);
                    }
                }
            }
        }
    }

    void install(Spark::ModId owner) {
        // Game thread, every tick: who's around, and what Minecraft did to them.
        Spark::UpdateAllEntities::addHandler(owner, +[](void*, Spark::UpdateAllEntities::Cursor next) {
            next();
            if (!Link::mcAlive())
                return;
            mirrorActors();
            drainEvents();
        }, nullptr);

        // Halo hitting Chief: Halo works out how much it would take, Minecraft takes it instead.
        Spark::DamageEntity::addHandler(owner, +[](void*, Spark::DamageEntity::Cursor next, Engine::DamageEvent* ev, std::uint32_t handle,
                                                   std::uint16_t p2, std::uint16_t p3, std::int16_t bone, std::uint64_t p5) {
            auto* chief = Player::driving() && handle == Engine::getPlayerHandle() ? Engine::getPlayerEntity() : nullptr;
            if (!chief || !ev) {
                next(ev, handle, p2, p3, bone, p5);
                return;
            }
            // Pad the bars so Halo's own damage code can't kill him, measure, put them back.
            constexpr float kPad = 10.0f;
            const float shield = chief->shield, health = chief->health;
            chief->shield = shield + kPad;
            chief->health = 1.0f + kPad;
            next(ev, handle, p2, p3, bone, p5);
            const float lost = std::max(0.0f, (shield + kPad - chief->shield) + (1.0f + kPad - chief->health));
            chief->shield = shield;
            chief->health = health;
            if (lost < 0.001f)
                return;
            const float mcDamage = lost * kMcHpPerChiefVitality;
            const auto kind = ev->sourceType == 4 ? proto::kHurtMelee : proto::kHurtProjectile;
            Link::pushInput(proto::kInHurt, std::uint16_t(kind), std::int32_t(mcDamage * kSkyrimToMcDamage * 100.0f), std::int32_t(ev->attackerHandle), 0);
            log(std::format("Halo hit Chief for {:.3f} of his vitality: {:.1f} Minecraft damage", lost, mcDamage));
        }, nullptr);
    }
}
