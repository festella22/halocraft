HaloCraft: Minecraft inside Halo: Combat Evolved
https://github.com/festella22/halocraft

You need
  - Halo: The Master Chief Collection on Steam, with Halo: CE installed
  - a Microsoft account that owns Minecraft: Java Edition
  - Windows 10 or 11

Play
  1. Keep this whole folder together (anywhere you like).
  2. Double-click HaloCraft.exe. It starts the Minecraft that plays inside Halo (you won't see
     its window), starts Halo: MCC through Steam with anti-cheat disabled, and loads HaloCraft
     into it once it's up.
  3. Pick any Halo CE level: a campaign mission or a local custom game.

  The first time, a Prism Launcher window asks you to sign in to Minecraft, then downloads
  Minecraft and Java (a few minutes). After that it starts by itself.

  Minecraft closes by itself when you quit Halo. If something didn't start, run HaloCraft.exe
  again: it only starts what's missing.

Good to know
  - Offline only. HaloCraft runs MCC with anti-cheat disabled; never take mods online.
  - Windows may say "Windows protected your PC" the first time (HaloCraft.exe isn't signed):
    click "More info", then "Run anyway".
  - Some antivirus tools flag mod loaders because they load code into a game. HaloCraft only
    loads spark.dll (which loads halocraft.dll) into MCC.
  - Your Minecraft world and Prism's files live in %LOCALAPPDATA%\HaloCraft.

Controls
  E, 1-9, scroll, Q, T, /   Minecraft, as usual
  F5                        Minecraft's camera: first person, behind, in front
  Esc                       Halo's pause menu (or closes a Minecraft screen)
  F9                        unloads all mods (Spark)

Logs, if something goes wrong
  Halo side:      <MCC folder>\MCC\Binaries\Win64\mods\halocraft.log
  Minecraft side: %LOCALAPPDATA%\HaloCraft\Prism\instances\HaloCraft\.minecraft\logs\latest.log
