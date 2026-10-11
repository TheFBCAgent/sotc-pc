# Build the boot image from a dumped game folder.
# Usage: $env:SOTC_GAME_DIR = 'D:\CUSA08809\uroot'; scripts\convert.ps1
$ErrorActionPreference = 'Stop'
if (-not $env:SOTC_GAME_DIR) { throw 'Set SOTC_GAME_DIR to the game folder (the one with eboot.bin)' }
$shad = if ($env:SOTC_SHADPS4) { $env:SOTC_SHADPS4 } else { '..\shadps4' }
Set-Location (Join-Path $PSScriptRoot '..')
$names = @()
if (Test-Path (Join-Path $shad 'src')) { $names = @('--shadps4', $shad) }
# Optional firmware folder with the user's own decrypted libSceNgs2.sprx (e.g. 'D:\'): Sony's
# Ngs2 mixer is linked for sound. Without it the game runs silent. ('\.' so a trailing
# backslash never escapes the quote PowerShell adds around a path with spaces.)
$fw = @()
if ($env:SOTC_FW_DIR) { $fw = @('--fw-dir', (Join-Path $env:SOTC_FW_DIR '.')) }
python tools/sotcconv/sotcconv.py --game $env:SOTC_GAME_DIR @names @fw -o build/game/boot.img --report build/game/boot.txt
