// HaloCraft's Halo side: a Spark mod. Spark LoadLibrary-s every DLL in MCC\Binaries\Win64\mods\
// and calls spark_modLoad.
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>
#include "spark/SparkAPI.h"
#include "spark/RenderBuses.hpp"
#include "spark/mod/IMod.hpp"
#include "spark/mod/ImGuiBridge.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
    using Bus = std::remove_reference_t<decltype(Spark::onRenderPauseMenuTabs)>;

    // Spark's console, plus halocraft.log next to this DLL (readable while the game runs).
    void log(const std::string& msg) {
        static std::ofstream file = [] {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
            return std::ofstream(std::filesystem::path(path).replace_extension(".log"));
        }();
        std::cout << "[HaloCraft] " << msg << std::endl;
        file << msg << std::endl;
    }

    class HaloCraftMod : public Spark::IMod {
    public:
        void init() override {
            log("Minecraft Mode activated. I am Steve.");

            // ponytail: Phase 0 proof of life only. Phase 1 replaces this with Minecraft's frame.
            Spark::onRenderPauseMenuTabs.addHandler(modId_, +[](void*, Bus::Cursor next) {
                Spark::Mod::syncImGuiContext();
                if (ImGui::BeginTabItem("HaloCraft")) {
                    ImGui::TextUnformatted("Minecraft Mode activated");
                    ImGui::TextUnformatted("I am Steve");
                    ImGui::EndTabItem();
                }
                next();
            }, nullptr);
        }
    };
}

extern "C" __declspec(dllexport) void spark_modLoad() {
    spark_registerMod(new HaloCraftMod());
}
