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
}
# Injecting during MCC's startup crashes Spark. PartyWin.dll loads late; Spark's own import path
# waits for it too.
do { Start-Sleep 2; $p = Get-Process $proc -ErrorAction SilentlyContinue }
until ($p -and ($p.Modules | Where-Object ModuleName -eq 'PartyWin.dll'))
Start-Sleep 5

if (-not ($p.Modules | Where-Object ModuleName -eq 'spark.dll')) {
    & "$root\vendor\spark\bin\$Config-Win64\spark-launcher\spark-launcher.exe"
}
"Spark injected. Mods load once a Halo CE level is running; log: $mods\halocraft.log"
