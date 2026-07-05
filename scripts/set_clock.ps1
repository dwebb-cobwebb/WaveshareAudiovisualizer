# Push the PC's local time to the 1U Visualizer's clock over its CDC serial
# port. Run manually, or register as a scheduled task so the clock is set
# automatically at logon/unlock:
#
#   schtasks /Create /TN "1U Visualizer clock" /SC ONLOGON /TR `
#     "powershell -WindowStyle Hidden -ExecutionPolicy Bypass -File <path>\set_clock.ps1"
#
# The device keeps time from its internal timer between runs (drift is a few
# seconds/day at most) and forgets it on power loss.

$ErrorActionPreference = 'Stop'

$port = (Get-CimInstance Win32_PnPEntity |
         Where-Object { $_.PNPDeviceID -like '*VID_CAFE&PID_4001*' -and $_.Name -match 'COM(\d+)' } |
         Select-Object -First 1).Name
if (-not $port -or $port -notmatch '\((COM\d+)\)') {
    Write-Error "1U Visualizer serial port not found. Is the device plugged in?"
}
$com = $Matches[1]

# Local-time epoch seconds: the device applies no timezone math of its own.
$now = [int64]([DateTime]::Now - [DateTime]'1970-01-01').TotalSeconds

$sp = New-Object System.IO.Ports.SerialPort $com, 115200
$sp.NewLine = "`n"
$sp.ReadTimeout = 1000
$sp.Open()
try {
    $sp.WriteLine("T$now")
    try { $reply = $sp.ReadLine() } catch { $reply = "(no reply)" }
    Write-Host "Sent T$now to $com - device replied: $reply"
} finally {
    $sp.Close()
}
