param(
    [string] $Port = "COM5",
    [string] $Uf2  = "C:\projects\side-projects\k_line_kwp\build_linux\k_line_kwp.uf2",
    [int]    $TimeoutSec = 30
)

if (-not (Test-Path $Uf2)) { Write-Host "ERROR: no such file: $Uf2"; exit 1 }

function Get-Rp2Drive {
    Get-CimInstance Win32_LogicalDisk |
        Where-Object { $_.VolumeName -eq 'RPI-RP2' } |
        Select-Object -First 1 -ExpandProperty DeviceID
}

# If it's already sitting in BOOTSEL, skip the touch.
$drive = Get-Rp2Drive
if (-not $drive) {
    Write-Host "Rebooting $Port into BOOTSEL (1200 baud touch)..."
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port, 1200, None, 8, one
        $sp.DtrEnable = $false      # pico stdio_usb resets on 1200 baud with DTR low
        $sp.Open()
        Start-Sleep -Milliseconds 200
        $sp.Close()
    } catch {
        Write-Host "  (touch reported: $($_.Exception.Message) -- continuing, this is often harmless)"
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $drive -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        Start-Sleep -Milliseconds 400
        $drive = Get-Rp2Drive
    }
}

if (-not $drive) { Write-Host "ERROR: RPI-RP2 drive never appeared - board may need the BOOTSEL button"; exit 1 }
Write-Host "BOOTSEL volume at $drive - copying firmware..."

# The board reboots the instant the UF2 finishes writing, so the copy itself
# frequently reports an I/O error. That is expected, not a failure.
try {
    Copy-Item -Path $Uf2 -Destination "$drive\" -Force -ErrorAction Stop
    Write-Host "Copy completed cleanly."
} catch {
    Write-Host "Copy ended with: $($_.Exception.Message)"
    Write-Host "  (normal - device resets mid-write)"
}

Write-Host "Waiting for $Port to come back..."
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Milliseconds 500
    if ([System.IO.Ports.SerialPort]::getportnames() -contains $Port) {
        Start-Sleep -Milliseconds 800       # let USB enumeration settle
        Write-Host "$Port is back - flash OK."
        exit 0
    }
}
Write-Host "WARNING: $Port did not reappear within $TimeoutSec s"
exit 1
