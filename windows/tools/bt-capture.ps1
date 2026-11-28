# EveryPods: record a Bluetooth (HCI) trace of an AirPods idle drop on Windows.
# Needs Microsoft's Bluetooth Test Platform (BTP) installed. From an admin PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\bt-capture.ps1
#
# It saves system details (Windows build, Bluetooth driver, Intel audio offload, clock offset), downloads
# Microsoft's Bluetooth trace profile, records while you reproduce the drop, converts the recording for
# Wireshark, adds the EveryPods traces and zips it all on your Desktop.

$ErrorActionPreference = 'Stop'
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an admin PowerShell (right-click > Run as administrator).'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path ([Environment]::GetFolderPath('Desktop')) "bt-capture-$stamp"
New-Item -ItemType Directory -Path $out | Out-Null
Set-Location -LiteralPath $out
$notes = Join-Path $out 'notes.txt'
function Note([string]$m) {
    $line = '{0:yyyy-MM-dd HH:mm:ss.fff} {1}' -f (Get-Date), $m
    Write-Host $line
    Add-Content -LiteralPath $notes -Value $line
}

# 1. BTETLParse from the BTP package
Write-Host 'Looking for btetlparse.exe...'
$parse = Get-ChildItem -Path 'C:\BTP' -Recurse -Filter 'btetlparse.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $parse) {
    $parse = Get-ChildItem -Path $env:ProgramFiles, ${env:ProgramFiles(x86)} -Recurse -Filter 'btetlparse.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
}
if ($parse) { Note "btetlparse: $($parse.FullName)" } else { Note 'btetlparse.exe not found: the .etl is kept for converting later.' }

# 2. System details
Write-Host 'Collecting system details (about 15 s)...'
& {
    $ErrorActionPreference = 'Continue'
    '== Windows'
    Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' |
        Select-Object ProductName, DisplayVersion, CurrentBuild, UBR | Format-List | Out-String
    '== Bluetooth radios'
    Get-PnpDevice -Class Bluetooth -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^(USB|PCI)\\' } | ForEach-Object {
            $ver = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
            "$($_.FriendlyName) | driver $ver | $($_.InstanceId)"
        }
    '== Intel Smart Sound / Bluetooth audio offload'
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match 'Smart Sound' -or $_.InstanceId -match 'HCIBYPASS' } |
        Format-Table -AutoSize Status, FriendlyName, InstanceId | Out-String -Width 300
    '== AirPods devices'
    Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match 'AirPods' } |
        Format-Table -AutoSize Status, Class, FriendlyName, InstanceId | Out-String -Width 300
    '== Clock vs time.apple.com'
    w32tm /stripchart /computer:time.apple.com /dataonly /samples:5
} *>&1 | Out-File -LiteralPath (Join-Path $out 'inventory.txt') -Encoding utf8
Note 'System details saved.'

# 3. Microsoft's Bluetooth trace profile
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$wprp = Join-Path $out 'BluetoothStack.wprp'
Invoke-WebRequest -UseBasicParsing -OutFile $wprp `
    -Uri 'https://raw.githubusercontent.com/microsoft/busiotools/master/bluetooth/tracing/BluetoothStack.wprp'

# 4. Record while the drop is reproduced
$etl = Join-Path $out 'BthTracing.etl'
wpr.exe -cancel 2>$null | Out-Null
$started = $false
foreach ($profile in 'BluetoothStack.Verbose', 'BluetoothStack') {
    wpr.exe -start "$wprp!$profile" -filemode
    if ($LASTEXITCODE -eq 0) { Note "Recording started (profile $profile)."; $started = $true; break }
}
if (-not $started) { throw 'wpr -start failed. Paste the error above to Claude.' }
$stopCode = $null
try {
    Write-Host ''
    Write-Host 'Now, with EveryPods running as usual:'
    Write-Host '  1. Get the AirPods onto this PC: start playing something (YouTube) and wait until the sound is on the AirPods.'
    Read-Host  '  2. Stop the playback, note the time, then press Enter' | Out-Null
    Note 'Playback stopped.'
    Write-Host '  3. Leave the PC silent: no music, no videos, no calls. Keep the AirPods in your ears.'
    $a = Read-Host '     When the AirPods drop, press Enter. If 5 minutes pass with no drop, type n and press Enter'
    if ($a -match '^n') { Note 'No drop within 5 minutes.' } else { Note 'AirPods dropped (when noticed).' }
    Start-Sleep -Seconds 10
} finally {
    Write-Host 'Stopping the recording (can take a minute)...'
    wpr.exe -stop $etl
    $stopCode = $LASTEXITCODE
}
if ($stopCode -ne 0) { throw "wpr -stop failed ($stopCode)." }
Note ('Recording saved: {0:N1} MB' -f ((Get-Item -LiteralPath $etl).Length / 1MB))

# 5. Convert for Wireshark
if ($parse) {
    Push-Location -LiteralPath $parse.DirectoryName
    try {
        & $parse.FullName -pcapng (Join-Path $out 'BthTracing.pcapng') -hci (Join-Path $out 'BthTracing.hci.txt') $etl
    } catch { Note "btetlparse failed: $_" } finally { Pop-Location }
}
$hasPcap = Test-Path -LiteralPath (Join-Path $out 'BthTracing.pcapng')
if ($hasPcap) { Note 'Converted to BthTracing.pcapng.' } else { Note 'WARNING: no pcapng; the .etl goes into the zip instead.' }

# 6. EveryPods traces from the last 3 hours
Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'EveryPods\traces') -Filter 'windows-*.log' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt (Get-Date).AddHours(-3) } |
    Copy-Item -Destination $out

# 7. Zip (the raw .etl only when it could not be converted)
$zip = "$out.zip"
Note "Zipping to $zip"
$skip = if ($hasPcap) { '.etl', '.wprp' } else { '.wprp' }
$files = Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -notin $skip }
Compress-Archive -LiteralPath $files.FullName -DestinationPath $zip -Force
Write-Host ''
Write-Host "Done. Upload this file to Claude: $zip"
explorer.exe "/select,$zip"
