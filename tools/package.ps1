# Builds HaloCraft and packs it into dist\:
#   HaloCraft\                 ready to run: double-click HaloCraft.exe
#     HaloCraft.exe             the launcher (Minecraft, MCC without anti-cheat, Spark)
#     spark.dll, halocraft.dll  Spark (the Halo CE MCC mod loader) and the HaloCraft mod it loads
#     HaloCraft-Minecraft.zip   a portable Prism Launcher with a ready "HaloCraft" instance (Minecraft
#                               26.3, Fabric, Fabric API, the mod). HaloCraft.exe unpacks it to
#                               %LOCALAPPDATA%\HaloCraft; Prism asks for a Microsoft account that owns
#                               Minecraft: Java Edition once, then downloads Minecraft and Java itself.
#   HaloCraft-<version>.zip     the same folder, zipped
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-NoBuild]    (JAVA_HOME: JDK 25)
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$root\fabric\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()

# Pinned downloads (checked against these hashes).
$prismVersion = "11.1.1"
$prismZip = "PrismLauncher-Windows-MSVC-Portable-$prismVersion.zip"
$prismUrl = "https://github.com/PrismLauncher/PrismLauncher/releases/download/$prismVersion/$prismZip"
$prismSha256 = "ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7"
$prismLicenseUrl = "https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/$prismVersion/LICENSE"
$fabricApiJar = "fabric-api-0.161.0+26.3.jar"
$fabricApiUrl = "https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar"
$e4mcJar = "e4mc-fabric-6.2.2-modern.jar"
$e4mcUrl = "https://cdn.modrinth.com/data/qANg5Jrr/versions/AouleFRY/e4mc-fabric-6.2.2-modern.jar"
$e4mcSha512 = "01ef0a8c5b76e2cb0effd337bad3350d8807d100d0ec661e01b2ffb20af7b652f756c5eaa11bee233c37905bfd7b573f7a85f3d15bfd2833962c76f02cd59a86"
$fabricApiSha512 = "ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d"

function Get-Pinned([string]$url, [string]$path, [string]$algorithm, [string]$hash) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory (Split-Path $path) -Force | Out-Null
        Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
    }
    if ($hash -and (Get-FileHash $path -Algorithm $algorithm).Hash -ne $hash.ToUpper()) {
        Remove-Item $path
        throw "$path doesn't match its pinned $algorithm hash"
    }
}

# Zip entries named explicitly with forward slashes, as the zip format (and every mod manager)
# expects; Windows PowerShell's own zipping writes backslashes.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function New-Zip([string]$path, [System.Collections.IDictionary]$entries) {
    $zip = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $entries.Keys) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $entries[$name], $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
}
function New-ZipFromFolder([string]$path, [string]$folder) {
    $entries = [ordered]@{}
    $base = (Resolve-Path $folder).Path.TrimEnd('\') + '\'
    Get-ChildItem $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entries[$_.FullName.Substring($base.Length).Replace('\', '/')] = $_.FullName
    }
    New-Zip $path $entries
}

if (-not $NoBuild) {
    $premake = (Get-Command premake5 -ErrorAction SilentlyContinue).Source
    if (-not $premake) { $premake = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Premake.*\premake5.exe" | Select-Object -First 1 -ExpandProperty FullName }
    Push-Location $root
    try {
        & $premake vs2022 | Out-Null
        $env:Path = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin;" + $env:Path
        MSBuild.exe halocraft.sln /t:spark /t:halocraft /t:launcher /p:Configuration=Release /p:Platform=Win64 /m /v:minimal /nologo
        if ($LASTEXITCODE) { throw "the Halo side didn't build" }
    } finally { Pop-Location }
    Push-Location "$root\fabric"
    try {
        .\gradlew.bat build --no-configuration-cache
        if ($LASTEXITCODE) { throw "the Fabric mod didn't build" }
    } finally { Pop-Location }
}

$exe = "$root\bin\Release-Win64\launcher\HaloCraft.exe"
$mod = "$root\bin\Release-Win64\halocraft\halocraft.dll"
$spark = "$root\vendor\spark\bin\Release-Win64\spark\spark.dll"
$jar = "$root\fabric\build\libs\skycraft-$version.jar"
foreach ($f in @($exe, $mod, $spark, $jar)) {
    if (-not (Test-Path $f)) { throw "missing $f (build first, or drop -NoBuild)" }
}
$cache = "$root\.tools\prism"
Get-Pinned $prismUrl "$cache\$prismZip" SHA256 $prismSha256
Get-Pinned $fabricApiUrl "$cache\$fabricApiJar" SHA512 $fabricApiSha512
Get-Pinned $e4mcUrl "$cache\$e4mcJar" SHA512 $e4mcSha512
Get-Pinned $prismLicenseUrl "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "" ""

$dist = "$root\dist"
New-Item -ItemType Directory $dist -Force | Out-Null
Get-ChildItem $dist | Remove-Item -Recurse -Force
$out = "$dist\HaloCraft"
New-Item -ItemType Directory $out | Out-Null

# The bundled Minecraft: Prism (portable), the HaloCraft instance, its mods, Prism's default settings.
$bundle = "$dist\bundle"
Copy-Item -Recurse "$root\tools\minecraft-bundle" $bundle
Expand-Archive "$cache\$prismZip" "$bundle\Prism" -Force
Copy-Item "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "$bundle\Prism\LICENSE-PrismLauncher.txt"
(Get-Content "$bundle\Prism\THIRD-PARTY.txt" -Raw).Replace("{PRISM_VERSION}", $prismVersion) | Set-Content "$bundle\Prism\THIRD-PARTY.txt" -NoNewline
$mods = "$bundle\Prism\instances\HaloCraft\.minecraft\mods"
New-Item -ItemType Directory $mods -Force | Out-Null
Copy-Item "$cache\$fabricApiJar" $mods
Copy-Item "$cache\$e4mcJar" $mods
Copy-Item $jar "$mods\skycraft-$version.jar"
Set-Content "$bundle\bundle-version.txt" "HaloCraft $version, Prism Launcher $prismVersion, $fabricApiJar, $e4mcJar" -NoNewline
New-ZipFromFolder "$out\HaloCraft-Minecraft.zip" $bundle
Remove-Item -Recurse -Force $bundle

Copy-Item $exe, $mod, $spark $out
Copy-Item "$root\LICENSE" "$out\LICENSE.txt"
Copy-Item "$root\THIRD-PARTY-NOTICES.md" $out
New-ZipFromFolder "$dist\HaloCraft-$version.zip" $out

Get-ChildItem $out, "$dist\HaloCraft-$version.zip" | ForEach-Object { "{0,-40} {1,12:N0} bytes" -f $_.Name, $_.Length }
