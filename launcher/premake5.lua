-- HaloCraft.exe: the one-click launcher (MCC, the bundled Minecraft, Spark). No console window.
project "launcher"
    kind "WindowedApp"
    language "C++"
    cppdialect "C++20"
    systemversion "latest"
    staticruntime "on"  -- runs on any PC, no VC++ redistributable needed
    targetname "HaloCraft"
    characterset "Unicode"

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../obj/" .. outputdir .. "/%{prj.name}")

    files { "src/**.cpp" }
    links { "advapi32", "shell32", "user32" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
