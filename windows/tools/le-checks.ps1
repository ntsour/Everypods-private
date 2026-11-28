# EveryPods: run the LE check tool (G3, docs/windows-peer-links-le-plan.md) and zip the logs.
# Put this next to le_spike.exe, then from a normal PowerShell in that folder:
#   powershell -ExecutionPolicy Bypass -File .\le-checks.ps1
# Both phones' Bluetooth on, EveryPods open on them. It runs with Propods open, then quits Propods,
# runs again, starts Propods again and zips everything on your Desktop.

param([int]$Tries = 10)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$tool = Join-Path $PSScriptRoot 'le_spike.exe'
if (-not (Test-Path -LiteralPath $tool)) { throw "le_spike.exe not found next to this script ($PSScriptRoot)." }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $PSScriptRoot "le-checks-$stamp"
New-Item -ItemType Directory -Path $out | Out-Null

function Step([string]$name, [string[]]$toolArgs) {
    Write-Host ''
    Write-Host "=== $name : le_spike $($toolArgs -join ' ') ===" -ForegroundColor Cyan
    Push-Location -LiteralPath $out
    try {
        $before = Get-ChildItem -Filter 'le-spike-*.log' -ErrorAction SilentlyContinue | ForEach-Object Name
        & $tool @toolArgs
        $new = Get-ChildItem -Filter 'le-spike-*.log' | Where-Object { $_.Name -notin $before }
        foreach ($f in $new) { Rename-Item -LiteralPath $f.FullName -NewName "$name-$($f.Name)" }
    } finally { Pop-Location }
    Start-Sleep -Seconds 2   # log file names have one-second resolution
}

# 1. With Propods running (classic links up)
$propods = Get-Process -Name 'Propods' -ErrorAction SilentlyContinue | Select-Object -First 1
$propodsPath = if ($propods) { $propods.Path } else { $null }
if (-not $propods) { Write-Host 'Propods is not running: the "with Propods" steps run without its links.' -ForegroundColor Yellow }

Step '1-devices' @('devices')
Step '2-scan' @('scan')
Step '3-connect-propods-on' @('connect', '--tries', "$Tries")

# 2. With Propods quit (no classic links to the phones)
if ($propods) {
    Write-Host ''
    Write-Host 'Quitting Propods...' -ForegroundColor Cyan
    Stop-Process -Id $propods.Id
    Start-Sleep -Seconds 15   # let the classic links to the phones close
}
Step '4-connect-propods-off' @('connect', '--tries', "$Tries")

# Connect by each phone's classic address too (Pixel, Xiaomi)
Step '5-classic-pixel' @('connect', '--classic', '30:E0:44:86:61:2C', '--tries', '3')
Step '6-classic-xiaomi' @('connect', '--classic', '68:4D:B6:A3:8D:7E', '--tries', '3')

# 3. Start Propods again
if ($propodsPath) {
    Write-Host ''
    Write-Host "Starting Propods again ($propodsPath)" -ForegroundColor Cyan
    Start-Process -FilePath $propodsPath
}

$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "le-checks-$stamp.zip"
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -Force
Write-Host ''
Write-Host "Done. Upload this file to Claude: $zip" -ForegroundColor Green
explorer.exe "/select,$zip"
