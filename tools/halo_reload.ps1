# Hot-reload the Halo side while MCC keeps running: build halocraft.dll, unload Spark (fake F9,
# Spark's own dev trick), install the new DLL, inject Spark again. Mods load once a level runs.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot
$mods = "C:\Program Files (x86)\Steam\steamapps\common\Halo The Master Chief Collection\MCC\Binaries\Win64\mods"
$env:Path = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin;" + $env:Path

$build = & MSBuild.exe "$root\halocraft.sln" /t:halocraft /p:Configuration=$Config /p:Platform=Win64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { $build | Select-String "error"; exit 1 }

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class HaloCraftKeys { [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra); }
'@
$p = Get-Process MCC-Win64-Shipping -ErrorAction SilentlyContinue
if ($p -and ($p.Modules | Where-Object ModuleName -eq 'spark.dll')) {
    [HaloCraftKeys]::keybd_event(0x78, 0, 0, [UIntPtr]::Zero)  # F9: Spark un-injects itself
    Start-Sleep -Milliseconds 80
    [HaloCraftKeys]::keybd_event(0x78, 0, 2, [UIntPtr]::Zero)
    $t = Get-Date
    do { Start-Sleep 1; $p.Refresh() } while (($p.Modules | Where-Object ModuleName -in 'spark.dll', 'halocraft.dll') -and ((Get-Date) - $t).TotalSeconds -lt 20)
}
Copy-Item "$root\bin\$Config-Win64\halocraft\halocraft.dll" $mods -Force
if ($p) { & "$root\vendor\spark\bin\$Config-Win64\spark-launcher\spark-launcher.exe" }
Start-Sleep 6
Get-Content "$mods\halocraft.log" -ErrorAction SilentlyContinue
