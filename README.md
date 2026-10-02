# HaloCraft

Play Halo: Combat Evolved (Steam MCC) as a Minecraft player.

> **Status: Phase 0 — setup.** Nothing playable yet.

HaloCraft is a port of [SkyCraft](https://github.com/chasmlol/SkyCraft) (Minecraft in Skyrim) to
Halo CE. Same idea: real Minecraft Java runs hidden in the background with a Fabric mod, a mod
inside Halo talks to it through shared memory, and Halo draws everything — Minecraft's hand, HUD and
inventory in Halo's frame, and blocks and Steve's body in Halo's world.

- `fabric/`, `protocol/`: SkyCraft's Minecraft half, kept close to upstream.
- `halo/` (coming): the Halo side, a mod for [Spark](https://github.com/KodyJKing/spark), the Halo CE
  MCC mod loader (`vendor/spark`).
- `skse/`: SkyCraft's original Skyrim plugin, kept as reference for now.

How SkyCraft works: [docs/SKYCRAFT.md](docs/SKYCRAFT.md) and [docs/DESIGN.md](docs/DESIGN.md).

## Plan

| Phase | Done when |
|---|---|
| 0. Setup | Our mod DLL loads inside MCC |
| 1. Link + HUD | The real Minecraft hotbar, hearts, hand and inventory show in Halo |
| 2. Movement | Minecraft physics on Halo's levels, camera follows Minecraft |
| 3. Combat | Halo enemies take Minecraft hits, and hit back |
| 4. World | Blocks and Steve's body drawn in Halo's world |

## Requirements

- Halo: The Master Chief Collection on Steam, with Halo: CE installed. Play **offline, with
  anti-cheat disabled** — never use mods online.
- Minecraft: Java Edition.

## Credits

- [SkyCraft](https://github.com/chasmlol/SkyCraft) by chasmlol (MIT) — the whole two-games design
  and the Minecraft mod.
- [Spark](https://github.com/KodyJKing/spark) by KodyJKing — Halo CE MCC mod loader and SDK
  (linked as a submodule, not copied).

Fan project, not affiliated with Mojang, Microsoft, 343 Industries or Halo Studios. You need to own
both games.
