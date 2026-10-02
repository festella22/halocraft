// HaloCraft's Halo side: a Spark mod. Spark LoadLibrary-s every DLL in MCC\Binaries\Win64\mods\
// and calls spark_modLoad once a Halo CE level is running.
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "Combat.hpp"
#include "Input.hpp"
#include "Link.hpp"
#include "Log.hpp"
#include "Overlay.hpp"
#include "Player.hpp"
#include "WorldRender.hpp"
#include "engine/scripting/Scripting.hpp"
#include "spark/SparkAPI.h"
#include "spark/hook/Hooks.hpp"
#include "spark/mod/IMod.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

void log(const std::string& msg) {
    static std::ofstream file = [] {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
        return std::ofstream(std::filesystem::path(path).replace_extension(".log"));
    }();
    std::cout << "[HaloCraft] " << msg << std::endl;
    file << msg << std::endl;
}

namespace {
    bool hudHidden = false;

    class HaloCraftMod : public Spark::IMod {
    public:
        void init() override {
            log("Minecraft Mode activated. I am Steve.");
            Link::create();
            Overlay::install();
            Input::install();
            Player::install(modId_);
            Combat::install(modId_);
            Spark::RenderBSPAlbedo::addHandler(modId_, +[](void*, Spark::RenderBSPAlbedo::Cursor next) {
                next();
                WorldRender::onSceneRendered();
            }, nullptr);

            // A Minecraft screen (inventory, crafting, chat) owns the mouse and keys: freeze Halo's
            // movement and look until it closes.
            Spark::UpdatePlayerControlsAndLook::addHandler(modId_, +[](void*, Spark::UpdatePlayerControlsAndLook::Cursor next, float dt, uint32_t budget) {
                if (!Input::mcScreenOpen)
                    next(dt, budget);
            }, nullptr);

            // While Minecraft is connected, its hand and HUD replace Halo's.
            Spark::RenderFPVModel::addHandler(modId_, +[](void*, Spark::RenderFPVModel::Cursor next) {
                if (!Link::mcAlive())
                    next();
            }, nullptr);
            Spark::UpdateCamera::addHandler(modId_, +[](void*, Spark::UpdateCamera::Cursor next, float dt) {
                next(dt);
                if (Link::mcAlive() != hudHidden) {
                    hudHidden = !hudHidden;
                    Engine::Scripting::submit(hudHidden ? "(show_hud false)" : "(show_hud true)");
                    log(hudHidden ? "Minecraft connected: Halo HUD hidden" : "Minecraft gone: Halo HUD back");
                }
            }, nullptr);
        }

        // ponytail: Halo's HUD isn't restored here; Spark unloads mods when the level unloads.
        void free() override {
            Input::uninstall();
            Overlay::uninstall();
        }
    };
}

extern "C" __declspec(dllexport) void spark_modLoad() {
    spark_registerMod(new HaloCraftMod());
}
