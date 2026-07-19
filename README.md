# 1U Visualizer

A USB audio visualiser firmware for the **Waveshare RP2350-Touch-LCD-3.49** board.

Plug it into a PC or Mac as a USB sound device and it drives its own 3.49" touch
LCD with real-time, audio-reactive visualisations — spectrum analyser, VU
meters, loudness metering, and a few eye-candy modes — no host-side software
required.

## Hardware
I used: [Waveshare RP2350 3.49inch Touch LCD development board](https://www.waveshare.com/rp2350-touch-lcd-3.49.htm) consisting of:
- **Board:** [Waveshare RP2350-Touch-LCD-3.49](https://www.waveshare.com/rp2350-touch-lcd-3.49.htm)
  (RP2350B, QFN-80 — the variant with extra GPIO; *not* the plain RP2350A "Pico 2").
- **Display:** AXS15231B QSPI panel, 172×640 native (portrait); this firmware
  drives it in landscape (640×172) via software transpose.
- **Touch:** CST816-compatible capacitive touch controller over I2C0.
- **USB:** exposes two interfaces to the host —
  - a **USB Audio Class 1.0** stereo 48 kHz/16-bit sink named "1U Visualizer"
    (appears as a normal playback device — no drivers needed on Windows/macOS/Linux)
  - a **CDC serial** port ("1U Visualizer Serial") used to set the on-device
    clock and to trigger a reboot into the UF2 bootloader

## What it does

Send audio to the device (select it as your playback device) and the display
comes alive, reacting to the stream in real time. Touch gestures switch
between modes:

- **Swipe right** — next mode (wraps around)
- **Swipe left** — previous mode
- **Swipe down** — turn the display off (tap anywhere to wake it)
- The display also auto-sleeps after 5 minutes without an active audio stream

Modes:

| Mode | Description |
|---|---|
| Producer | 31-band FFT spectrum, peak-hold, stereo level meters, clip indicator, phase bar |
| Vibe | Photorealistic analogue-style stereo VU meters |
| LUFS | EBU R128 loudness metering (Momentary/Short/Integrated, LRA, true peak) |
| Tunnel | Audio-reactive infinite neon tunnel (eye candy) |
| Starfield | Warp-speed starfield with motion trails, audio-reactive |
| Plasma | Audio-reactive plasma effect |
| Clock | Large digital clock (time is set over the CDC serial port; lost on power loss) |

Internally, audio RX and FFT/DSP analysis run on core 1 while core 0 handles
USB, LVGL rendering, and touch input, so the display stays smooth regardless
of DSP load.

## Prerequisites

Install these yourself before building:

- [CMake](https://cmake.org/) and [Ninja](https://ninja-build.org/) (or Make)
- The `arm-none-eabi-gcc` toolchain
- The [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (2.2.0),
  with `PICO_SDK_PATH` set in your environment
- [picotool](https://github.com/raspberrypi/picotool) (for flashing/inspecting UF2s)
- PowerShell (for the helper scripts in `scripts/`)

The Pico SDK, LVGL, and CMSIS-DSP are not vendored in this repo — they're
fetched by the setup script below.

## Building

```powershell
# 1. Fetch dependencies (LVGL v8.3, CMSIS-DSP) and generate lv_conf.h
pwsh ./scripts/setup_deps.ps1

# 2. Configure
cmake -B build -G Ninja

# 3. Build
cmake --build build
```

This produces `build/waveshare_audiovisualiser.uf2` (plus `.elf`/`.bin`/`.hex`/`.map`).

## Installing / flashing

**First flash (board not yet running this firmware):** hold the **BOOT**
button while plugging in USB (or while pressing **RESET**) to force the
RP2350 into its UF2 bootloader. It will appear as a USB mass-storage drive
(volume name starting with `RP`). Copy the UF2 onto it:

```powershell
Copy-Item build\waveshare_audiovisualiser.uf2 E:\   # replace E: with the bootloader drive
```

The board reboots automatically into the new firmware once the copy finishes.

**Subsequent updates:** once the firmware is running, it exposes a CDC serial
port that can drop it back into the bootloader without touching the physical
button, so `scripts/reboot_bootsel.ps1` can flash a rebuilt UF2 in one step:

```powershell
pwsh ./scripts/reboot_bootsel.ps1 -Uf2 build\waveshare_audiovisualiser.uf2
```

Run it with no `-Uf2` argument to just reboot into the bootloader and report
the drive letter, without flashing anything.

Flashing resets the RP2350's wall clock, so the script waits for the serial
port to come back and pushes the PC's local time to the device automatically
(same as `scripts/set_clock.ps1`, which can also be run standalone or
registered as a logon scheduled task — see the comments at the top of that
script).
