# Builds the loader (winmm.dll) and puts it next to the Mod Engine in engine\.
# Needs llvm-mingw (winget install MartinStorsjo.LLVM-MinGW.UCRT) and Python 3.10+ with pefile for the generators.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
python tools/gen_winmm_proxy.py
python tools/gen_patches.py
New-Item -ItemType Directory -Force build | Out-Null
& x86_64-w64-mingw32-clang++ -std=c++17 -O2 -s -shared -static -Wall `
    -o build/winmm.dll loader/src/main.cpp loader/src/winmm_stubs.S loader/src/winmm.def
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
Copy-Item build/winmm.dll engine/winmm.dll -Force
Write-Host "built engine\winmm.dll ($((Get-Item build/winmm.dll).Length) bytes)"
