# Builds and zips a release: dist\LE2-Mod-Engine-<version>.zip
#   engine\  le2_mod_engine.pyw, nvidia_profile.py, features.json, winmm.dll
#   Start Mod Engine.bat, README.md, LICENSE
param([string]$Version = "0.1.0")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
& "$root\build.ps1"
$stage = Join-Path $root "dist\LE2-Mod-Engine-$Version"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\engine" | Out-Null
Copy-Item engine\le2_mod_engine.pyw, engine\nvidia_profile.py, engine\features.json, engine\winmm.dll "$stage\engine\"
Copy-Item README.md, LICENSE $stage
@"
@echo off
rem Starts the Lone Echo II Mod Engine (needs Python 3.10+ from python.org).
where pythonw >nul 2>nul || (echo Python is not installed: get it from https://www.python.org/downloads/ & pause & exit /b 1)
start "" pythonw "%~dp0engine\le2_mod_engine.pyw"
"@ | Set-Content -Encoding ascii "$stage\Start Mod Engine.bat"
$zip = "$stage.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "release: $zip ($((Get-Item $zip).Length) bytes)"
