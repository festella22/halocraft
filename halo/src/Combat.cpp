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
        // this much Minecraft damage: a Grunt (health only) falls to two sword hits, an Elite
        // (shield + health) to three or four. (20 felt spongy in the first campaign test.)
        constexpr float kMcHpPerBar = 12.0f;
        // A full shield bar of Halo damage is more than Steve's 20 health: Chief's shield breaking
        // is deadly, like it should feel. (10 let Minecraft's regeneration outheal Easy Covenant.)
        constexpr float kMcHpPerChiefVitality = 25.0f;
        constexpr float kSkyrimToMcDamage = 5.0f;  // SkyCombat.SKYRIM_TO_MC_DAMAGE divides what we send
        // DamageEvent flags (Spark's notes): single target, instant kill, head.
        constexpr std::uint32_t kSingle = 0x1, kInstantKill = 0x4, kHead = 0x20;
        constexpr std::uint32_t kSourceMelee = 4;
        // Minecraft knockback (blocks per Minecraft tick, 20/s) -> Halo velocity (units per Halo tick, 30/s).
        constexpr float kKnockbackScale = (20.0f / 30.0f) / Coords::kBlocksPerUnit;

        std::vector<proto::ActorRecord> actors;

        // A melee damage effect (any weapon's), so Halo reacts to a punch, not a bullet or grenade.
        Engine::Tag* meleeTag() {
            static constexpr const char* kPaths[] = { "weapons\\assault rifle\\melee", "weapons\\pistol\\melee", "weapons\\shotgun\\melee",
                "weapons\\plasma rifle\\melee", "weapons\\needler\\melee", "weapons\\sniper rifle\\melee", "weapons\\rocket launcher\\melee" };
            for (const char* path : kPaths)
                if (auto* tag = Engine::findTag(path, "jpt!"))
                    return tag;
            return nullptr;
        }

        // Halo's own damage path (death animation, ragdoll, checkpoint/respawn), bypassing every
        // DamageEntity handler including ours.
        void kill(std::uint32_t handle, Engine::Entity* entity, std::uint32_t attacker) {
            auto* tag = meleeTag();
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

        // Minecraft hit a stand-in: ev.a = damage after Minecraft's own sword/crit/enchantment maths,
        // ev.b/ev.c = knockback direction (Minecraft x/z), ev.d = knockback strength.
        void onHit(const proto::McEvent& ev) {
            const std::uint32_t handle = ev.formId;
            auto* e = Engine::entityValid(handle) ? Engine::getEntityPointer(handle) : nullptr;
            if (!e || e->entityCategory != Engine::EntityCategory_Biped || e->health <= 0.0f)
                return;
            const bool crit = (ev.flags & proto::kHitCritical) != 0;

            // Minecraft decides how much it hurts: shield first, the rest to health.
            float shield = e->shield, health = e->health;
            float left = ev.a / kMcHpPerBar;
            if (shield > 0.0f) {
                const float taken = std::min(shield, left);
                shield -= taken;
                left -= taken;
            }
            health -= left;
            const bool lethal = health <= 0.0f;

            // Halo decides how it looks: a melee hit through its own damage code (flinch, shield flare
            // or blood, impact effects, AI notices). Pad the bar it lands on so Halo's own melee damage
            // can't decide anything, then put Minecraft's numbers back.
            float dir[3] = { ev.b, -ev.c, 0.0f };  // Minecraft x/z -> Halo x/y
            const float len = std::hypot(dir[0], dir[1]);
            if (len > 1e-4f)
                dir[0] /= len, dir[1] /= len;
            auto* tag = meleeTag();
            if (tag && Spark::DamageEntity::original) {
                constexpr float kPad = 10.0f;
                if (!lethal) {
                    if (e->shield > 0.0f)
                        e->shield += kPad;
                    else
                        e->health += kPad;
                }
                Engine::DamageEvent d{};
                d.damageTypeTagHandle = tag->tagID;
                d.sourceType = kSourceMelee;
                d.flags = kSingle | (lethal ? kInstantKill : 0) | (crit ? kHead : 0);  // a crit lands like a headshot
                d.interactorHandle = 0xFFFFFFFF;
                // A mob's hit is nobody's: Marines don't turn on Chief for a zombie's bite.
                d.attackerHandle = (ev.flags & proto::kHitNotPlayer) ? 0xFFFFFFFFu : Engine::getPlayerHandle();
                d.sourceTypeIndex = 0xFFFF;
                d.hitPosition = { e->pos.x, e->pos.y, e->pos.z + 0.4f };
                d.hitDirection = { dir[0], dir[1], 0.0f };
                d.baseDamage = 1.0f;
                d.damageMultiplier = 1.0f;
                Spark::DamageEntity::original(&d, handle, 0, 0, -1, 0);
            } else if (lethal) {
                e->health = 0.0f;  // ponytail: no melee tag on this map; it dies on its next hit
            }
            if (!lethal) {
                e->shield = shield;
                e->health = health;
            }

            // Minecraft's knockback, along the swing (crits shove half again as hard).
            const float push = ev.d * kKnockbackScale * (crit ? 1.5f : 1.0f);
            e->vel = { e->vel.x + dir[0] * push, e->vel.y + dir[1] * push, e->vel.z + push * 0.5f };
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
                    onHit(ev);
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
