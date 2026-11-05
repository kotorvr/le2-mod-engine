# Lone Echo II Mod Engine

A mod loader, a set of gameplay switches, and a performance fix for **Lone Echo II** (PC VR, `loneecho2.exe`),
with a small desktop app to turn them on and off.

It started as a replacement for **VEGA Patch**, the discontinued community "texture pop-in" patch, which is known to
crash and to leave NVIDIA settings broken, especially on Windows 11. See [What VEGA Patch actually did](#what-vega-patch-actually-did).

## What's in it

**Mods** (the switches in the app)

| Switch | Status | What it does |
|---|---|---|
| God mode | ready | Jack can't die. Switches on and off while the game runs. |
| Cure Gun: permanent neutralization | ready | Cured creep never comes back, and cured cysts stay deflated. Live. |
| All gadgets | in progress | The tools the story hands out later, from the start. |
| Maximum upgrades | in progress | Every suit and tool upgrade. |
| Boost, brake, movement | ready | Boost ×2 (acceleration and burst), brake ×2, hand thrusters ×1.5, boost cooldown 2 s → 0.05 s. |
| Unlimited thrusters | ready | Boost, brake and thrusters never drain suit power. Live. |
| Signal and boundary limit | ready | No "signal low/lost" warning, no out-of-bounds death. |
| Web receivers stay disabled | ready | A web receiver you disable doesn't switch itself back on after 5 s. Live. |
| Tool range 1000 | experimental | Magehand, cutter and faraday projector reach set to 1000 when their tool data loads. IO scanner and cure gun not covered yet. |
| Any four digit keypad code | ready | Keypad doors accept any code. |

**Performance**

- **Texture streaming budget – the real pop-in fix.** The game gives its texture cache a fixed budget from the
  Texture quality setting: 768 MiB (Low), 1.5 GiB (Medium), **3 GiB (High)**, no matter how much VRAM the card has
  (`SetTextureBudget`, `0x1405998e0`, fed from `0x140d8a9c0`). When the cache gets within 16 MiB of that budget it
  drops high-res mips and **blocks every mip upgrade for 5 seconds** (`0x141e54d48`), and each frame it only
  re-evaluates the mip needs of a quarter of the objects. That is the "textures stuck at low detail" problem.
  The app offers:
  - *Boosted* (12 GB+ GPUs): budget ×2 (High = 6 GiB), half the objects re-evaluated per frame.
  - *Maximum* (16 GB+ GPUs): budget ×4 (High = 12 GiB), every object every frame, upgrades resume after 1 s.
- **Default-quality caps removed.** When the game builds its default settings it silently forces texture quality to
  Medium on GPUs under 8000 MB, to Low under 6000 MB, and lowers the preset on PCs under 16 GB RAM
  (`0x140d8af60`). The loader removes those caps.
- **No Windows power throttling** (EcoQoS) for the game process on Windows 11.
- **NVIDIA profile, optional.** One button sets *Power management: Prefer maximum performance* on the driver's own
  "Lone Echo II" profile through NVAPI; another removes it again. Nothing replaces the driver database.

**Looks** – the app edits the game's own `settings.json` for options the in-game menu doesn't show: shadow map and
sun-cascade size, shadow distance, MSAA, the low-res periphery (`multires`), adaptive-resolution floor, sharpening.
Your original file is backed up the first time and can be restored with one click.

## Install

1. Download the release and unzip it anywhere.
2. Run `Lone Echo II Mod Engine` (`engine\le2_mod_engine.pyw`, needs Python 3.10+ from python.org).
3. It finds the game in the usual Oculus library folders. If not, press **Browse...** and pick the
   `ready-at-dawn-lone-echo-2` folder.
4. Press **Install**, tick what you want, start the game as usual (Oculus app, Virtual Desktop, Steam Link, …).

To uninstall, press **Uninstall** (it deletes `bin\win10\winmm.dll`). Starting the game with `-nomods` also skips
every mod for that run.

## How it works

- `winmm.dll` next to `loneecho2.exe`. The game imports WINMM statically and winmm isn't a Windows KnownDLL, so this
  copy loads before any game code runs. All 181 winmm functions jump straight to the real one in System32.
- Each switch is a list of small byte patches (`patches/*.json`, compiled in). Every patch first checks that the bytes
  it replaces are exactly what it expects: on any other game build it skips and writes a log line, it never writes
  blind.
- The game's logic largely lives in 5,223 compiled script DLLs (`bin\win10\scripts\*.dll`). The loader hooks the
  game's `GetProcAddress` import: when the engine asks a freshly loaded script for `setup_bindings`, that script is
  patched before any of its code runs.
- `le2mods.ini` (next to the exe) says which switches are on. Live switches are re-read four times a second.
- Everything is logged to `bin\win10\le2mods.log`.

Supported build: `loneecho2.exe` PE timestamp `0x61578052` (the final Oculus release, 1.1.561021.2).

## What VEGA Patch actually did

VEGA Patch 1.2.0 (installer unpacked, not run):

- Its injector and DLL are never used. The launcher md5-checks them, and then the injection step is an empty label.
  The DLL is 32-bit anyway, and the game is 64-bit.
- What it *did* do: at every launch it renamed your NVIDIA driver profile database
  (`C:\ProgramData\NVIDIA Corporation\Drs\nvdrsdb0/1.bin`) aside and swapped in a whole database exported from driver
  526.98 (December 2022), then swapped them back when the game closed. If the game crashed or the window was closed,
  your real driver profiles stayed replaced. Running a stale whole-driver database under a newer driver is a likely
  source of the crashes people saw.
- The only Lone Echo II changes in that database are four texture-filtering settings: LOD bias +0.5 (driver-controlled
  off), texture filtering "High performance", transparency supersampling on. The game renders with **Direct3D 12**,
  where the driver doesn't apply LOD-bias or transparency-AA overrides, so VEGA's main setting most likely did
  nothing.

## Building

```powershell
winget install MartinStorsjo.LLVM-MinGW.UCRT
pip install pefile
.\build.ps1        # -> engine\winmm.dll
```

Patches are data: edit or add `patches/<feature>.json` (format in [docs/PATCH_SPEC.md](docs/PATCH_SPEC.md)) and
rebuild. `research/` holds features that were found but need more than byte patches.

## Credits

- [echo-symbols](https://github.com/Dualgame/echo-symbols) for the CSymbol64 names that make the script DLLs readable.
- [Lone Echo Mod Loader](https://github.com/heisthecat31/loneechomodloader) for the original Lone Echo approach.

No game files are included or modified on disk. Lone Echo II is © Ready At Dawn / Meta; this project is not affiliated
with them.
