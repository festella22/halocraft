project "halocraft"
    kind "SharedLib"
    language "C++"
    cppdialect "C++20"
    systemversion "latest"
    staticruntime "off" -- share the CRT heap with spark.dll

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/**.hpp",
        "src/**.cpp",
        -- Core ImGui only; spark.dll owns the DX11 backend. Sync its context before ImGui calls.
        "../vendor/spark/vendor/imgui/*.h",
        "../vendor/spark/vendor/imgui/*.cpp",
    }

    includedirs {
        "src",
        "../protocol",
        "../vendor/spark/spark/src",
        "../vendor/spark/vendor/minhook/include",
        "../vendor/spark/vendor/imgui",
    }

    links { "spark", "d3d11", "d3dcompiler", "advapi32" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
