# Dev loop for the Halo side: install halocraft.dll into MCC's mods folder, start MCC without
# anti-cheat if it isn't running, inject Spark (which loads the mod), then show the mod's log.
# Build first: premake5 vs2022, then MSBuild halocraft.sln. F9 in game un-injects Spark.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$mods = "C:\Program Files (x86)\Steam\steamapps\common\Halo The Master Chief Collection\MCC\Binaries\Win64\mods"
$proc = "MCC-Win64-Shipping"

New-Item -ItemType Directory -Force $mods | Out-Null
Copy-Item "$root\bin\$Config-Win64\halocraft\halocraft.dll" $mods -Force

if (-not (Get-Process $proc -ErrorAction SilentlyContinue)) {
    Start-Process "steam://launch/976730/option2"  # option2 = Anti-Cheat Disabled
    while (-not (Get-Process $proc -ErrorAction SilentlyContinue)) { Start-Sleep 1 }
}

& "$root\vendor\spark\bin\$Config-Win64\spark-launcher\spark-launcher.exe"
Start-Sleep 3
Get-Content "$mods\halocraft.log" -ErrorAction SilentlyContinue
