-- One workspace for Spark (the Halo CE MCC mod loader, vendor/spark) and our Halo mod.
-- Generate with `premake5 vs2022`, then build halocraft.sln.
workspace "halocraft"
   architecture "x64"
   configurations { "Debug", "Release" }
   platforms { "Win64" }
   startproject "halocraft"
   defines { "ZYDIS_STATIC_BUILD", "ASMJIT_STATIC" }

outputdir = "%{cfg.buildcfg}-%{cfg.platform}"

group "Dependencies"
   include "vendor/spark/vendor/minhook"
   include "vendor/spark/vendor/zydis"
group ""

include "vendor/spark/spark/premake5.lua"
include "vendor/spark/spark-launcher/premake5.lua"
include "halo/premake5.lua"
include "launcher/premake5.lua"
