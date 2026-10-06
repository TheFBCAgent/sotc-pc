# Build the boot image from a dumped game folder.
# Usage: $env:SOTC_GAME_DIR = 'D:\CUSA08809\uroot'; scripts\convert.ps1
$ErrorActionPreference = 'Stop'
if (-not $env:SOTC_GAME_DIR) { throw 'Set SOTC_GAME_DIR to the game folder (the one with eboot.bin)' }
$shad = if ($env:SOTC_SHADPS4) { $env:SOTC_SHADPS4 } else { '..\shadps4' }
Set-Location (Join-Path $PSScriptRoot '..')
$names = @()
if (Test-Path (Join-Path $shad 'src')) { $names = @('--shadps4', $shad) }
python tools/sotcconv/sotcconv.py --game $env:SOTC_GAME_DIR @names -o build/game/boot.img --report build/game/boot.txt
