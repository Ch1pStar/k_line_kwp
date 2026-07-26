param(
    [string] $Port     = "COM5",
    [string] $Commands = "",      # semicolon-separated: "connect;read-dtcs"
    [double] $Settle   = 2.0,     # seconds to keep reading after each command
    [double] $Warmup   = 0.4
)

# -File passes everything as strings, so split here rather than relying on
# PowerShell array binding (which silently joins the args into one string).
$CommandList = @()
if ($Commands) {
    $CommandList = $Commands.Split(';') | Where-Object { $_.Trim() -ne "" }
}

# Pico presents a USB CDC device: the baud rate is ignored, but DTR must be
# asserted or pico stdio_usb treats the host as disconnected and drops output.
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, None, 8, one
$sp.DtrEnable  = $true
$sp.RtsEnable  = $true
$sp.NewLine    = "`n"
$sp.ReadTimeout = 300

try {
    $sp.Open()
} catch {
    Write-Host "ERROR: could not open $Port -- $($_.Exception.Message)"
    Write-Host "If this says 'Access denied', a terminal still has the port open."
    exit 1
}

# Drain until the line goes idle (no new bytes for $IdleMs), capped at $Settle
# seconds. This keeps command pacing tight - we move on shortly after the reply
# stops - so gaps between commands stay well under the ECU session timeout,
# instead of always waiting a fixed (and too-long) interval.
function Drain([double]$maxSeconds) {
    # 5-baud init has a ~2.4s silent stretch (sending 0x88 at 5 baud produces no
    # console output), so the idle threshold must clear that or connect looks done
    # before it is.
    $idleMs = 2800
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $lastByte = [Diagnostics.Stopwatch]::StartNew()
    $sawAny = $false
    while ($sw.Elapsed.TotalSeconds -lt $maxSeconds) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk) {
                Write-Host -NoNewline $chunk
                $lastByte.Restart()
                $sawAny = $true
            }
        } catch {}
        # Once we've seen output and the line has been quiet for $idleMs, stop.
        if ($sawAny -and $lastByte.Elapsed.TotalMilliseconds -ge $idleMs) { break }
        Start-Sleep -Milliseconds 25
    }
}

Start-Sleep -Milliseconds ($Warmup * 1000)
$sp.DiscardInBuffer()

foreach ($cmd in $CommandList) {
    Write-Host "`n>>> $cmd"
    $sp.Write("$cmd`n")
    Drain $Settle
}

if ($CommandList.Count -eq 0) { Drain $Settle }

$sp.Close()
