# Fetches the third-party dependencies that are not committed to this repo and
# generates lv_conf.h from LVGL's template with the settings this project needs.
#
# Prerequisites you must install yourself (not handled here):
#   - CMake, Ninja (or make)
#   - arm-none-eabi-gcc toolchain
#   - The Raspberry Pi Pico SDK, with PICO_SDK_PATH set in your environment
#   - picotool (for flashing)
#
# Run from the repo root:  pwsh ./scripts/setup_deps.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

New-Item -ItemType Directory -Force -Path "lib" | Out-Null

if (-not (Test-Path "lib/lvgl")) {
    Write-Host "Cloning LVGL (v8.3)..."
    git clone --depth 1 -b release/v8.3 https://github.com/lvgl/lvgl lib/lvgl
}
if (-not (Test-Path "lib/cmsis-dsp")) {
    Write-Host "Cloning CMSIS-DSP..."
    git clone --depth 1 https://github.com/ARM-software/CMSIS-DSP lib/cmsis-dsp
}

# Generate lv_conf.h from the template and apply project settings.
$tpl = "lib/lvgl/lv_conf_template.h"
$out = "lv_conf.h"
if (-not (Test-Path $tpl)) { throw "LVGL template not found at $tpl" }

Write-Host "Generating lv_conf.h ..."
$c = Get-Content $tpl -Raw
# Enable the config (template ships disabled behind `#if 0`).
$c = $c -replace '#if 0 /\*Set it to "1" to enable content\*/', '#if 1 /*Set it to "1" to enable content*/'
# 16-bit colour to match the panel; swap bytes is common on QSPI panels (verify on HW).
$c = $c -replace '#define LV_COLOR_DEPTH\s+\d+', '#define LV_COLOR_DEPTH 16'
$c = $c -replace '#define LV_COLOR_16_SWAP\s+\d+', '#define LV_COLOR_16_SWAP 1'
# Tick is driven by a repeating timer calling lv_tick_inc() (see lv_port_disp.c),
# so LV_TICK_CUSTOM stays at its default (0).
# Fonts we use in the UI.
$c = $c -replace '#define LV_FONT_MONTSERRAT_14\s+\d+', '#define LV_FONT_MONTSERRAT_14 1'
$c = $c -replace '#define LV_FONT_MONTSERRAT_20\s+\d+', '#define LV_FONT_MONTSERRAT_20 1'
Set-Content -Path $out -Value $c -NoNewline
Write-Host "Wrote $out"

Write-Host ""
Write-Host "Done. Next:"
Write-Host "  1) Download the Waveshare RP2350-Touch-LCD-3.49 demo and copy its AXS15231B"
Write-Host "     driver + pin map into src/display/ (see TODOs in axs15231b.c)."
Write-Host "  2) cmake -B build -G Ninja  (with PICO_SDK_PATH set)"
Write-Host "  3) cmake --build build"
