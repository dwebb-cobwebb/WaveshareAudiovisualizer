# Reboot the 1U Visualizer into the RP2350 UF2 bootloader (BOOTSEL) over its
# CDC serial port — no physical button press needed. Optionally flash a UF2.
#
#   .\reboot_bootsel.ps1                    # reboot, report the UF2 drive
#   .\reboot_bootsel.ps1 -Uf2 build\av.uf2  # reboot, then copy the UF2 (flash)
#
# Copying a UF2 to the bootloader drive flashes it; the device then reboots
# straight back into the new firmware. After flashing, the PC's local time is
# pushed to the device (the wall clock is lost on reboot).

param(
    [string]$Uf2
)

$ErrorActionPreference = 'Stop'

function Find-VisualizerCom {
    $port = (Get-CimInstance Win32_PnPEntity |
             Where-Object { $_.PNPDeviceID -like '*VID_CAFE&PID_4001*' -and $_.Name -match 'COM(\d+)' } |
             Select-Object -First 1).Name
    if ($port -and $port -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

if ($Uf2 -and -not (Test-Path $Uf2)) {
    Write-Error "UF2 file not found: $Uf2"
}

function Find-BootloaderDrive {
    Get-CimInstance Win32_LogicalDisk |
        Where-Object { $_.VolumeName -like 'RP*' -and
                       (Test-Path (Join-Path $_.DeviceID 'INFO_UF2.TXT')) } |
        Select-Object -First 1 -ExpandProperty DeviceID
}

$drive = Find-BootloaderDrive
if (-not $drive) {
    $com = Find-VisualizerCom
    if (-not $com) {
        Write-Error "1U Visualizer serial port not found and no bootloader drive present. Is the device plugged in?"
    }

    $sp = New-Object System.IO.Ports.SerialPort $com, 115200
    $sp.NewLine = "`n"
    $sp.ReadTimeout = 1000
    $sp.Open()
    # The device detaches the instant it acts on BOOT; every step after the
    # write — including Close — can fail with an IO error. That's success.
    try {
        $sp.WriteLine("BOOT")
        try { [void]$sp.ReadLine() } catch {}
    } catch {} finally {
        try { $sp.Close() } catch {}
    }
    Write-Host "Sent BOOT to $com - waiting for the UF2 bootloader drive..."

    $deadline = [DateTime]::Now.AddSeconds(15)
    while (-not ($drive = Find-BootloaderDrive)) {
        if ([DateTime]::Now -gt $deadline) {
            Write-Error "Bootloader drive did not appear within 15 s."
        }
        Start-Sleep -Milliseconds 250
    }
} else {
    Write-Host "Device is already in the bootloader."
}

Write-Host "UF2 bootloader drive: $drive"

if ($Uf2) {
    Copy-Item $Uf2 "$drive\" -Confirm:$false
    Write-Host "Flashed $Uf2 - device is rebooting into the new firmware."

    # The reboot wiped the wall clock; wait for the CDC port to come back and
    # push the PC's local time (same protocol as set_clock.ps1).
    Write-Host "Waiting for the serial port to reappear to set the clock..."
    $deadline = [DateTime]::Now.AddSeconds(20)
    $com = $null
    while (-not ($com = Find-VisualizerCom)) {
        if ([DateTime]::Now -gt $deadline) {
            Write-Warning "Serial port did not reappear within 20 s - clock not set. Run set_clock.ps1 manually."
            exit 0
        }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Milliseconds 500   # let the CDC interface finish enumerating

    $now = [int64]([DateTime]::Now - [DateTime]'1970-01-01').TotalSeconds
    $sp = New-Object System.IO.Ports.SerialPort $com, 115200
    $sp.NewLine = "`n"
    $sp.ReadTimeout = 2000
    $sp.Open()
    try {
        $sp.WriteLine("T$now")
        try { $reply = $sp.ReadLine() } catch { $reply = "(no reply)" }
        Write-Host "Clock set on $com - device replied: $reply"
    } finally {
        $sp.Close()
    }
}
