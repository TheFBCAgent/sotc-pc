# Run the game from the boot image. Extra arguments are passed to the runtime.
# Usage: $env:SOTC_GAME_DIR = 'D:\CUSA08809\uroot'; scripts\run.ps1
$ErrorActionPreference = 'Stop'
if (-not $env:SOTC_GAME_DIR) { throw 'Set SOTC_GAME_DIR to the game folder (the one with eboot.bin)' }
Set-Location (Join-Path $PSScriptRoot '..')
if (-not $env:SOTC_USER_DIR) { $env:SOTC_USER_DIR = 'build\user' }
& build\sotc-runtime.exe build\game\boot.img @args
exit $LASTEXITCODE
