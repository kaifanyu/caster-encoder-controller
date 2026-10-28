# Run in the ESP-IDF v5.4.2 PowerShell terminal. Defaults to build only.
param(
    [Parameter(Mandatory=$true, Position=0)]
    [ValidateSet('front_left', 'front_right')][string]$Wheel,
    [Parameter(Mandatory=$true, Position=1)]
    [ValidateSet('SWIVEL', 'WHEEL')][string]$Role,
    [ValidatePattern('^COM[1-9][0-9]*$')][string]$Port,
    [switch]$Flash,
    [switch]$Monitor
)
$ErrorActionPreference = 'Stop'
$Wheel = $Wheel.ToLowerInvariant()
$Role = $Role.ToUpperInvariant()
if (($Flash -or $Monitor) -and !$Port) { throw 'Specify -Port COMx for flash/monitor.' }
if (!(Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw 'Open the ESP-IDF v5.4.2 PowerShell terminal first.'
}
$version = (& idf.py --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne 'ESP-IDF v5.4.2') { throw "Expected ESP-IDF v5.4.2; found $version" }
$casterRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path.Replace('\', '/')
$header = "$casterRoot/ordinary_caster/config/$Wheel.local.h"
if (!(Test-Path -LiteralPath $header)) {
    throw "Copy ordinary_caster/config/$Wheel.example.h to $Wheel.local.h in the same folder and configure Wi-Fi/Pi IP."
}
# Use Windows-specific build trees: WSL/Docker CMake caches contain Linux paths.
$build = "$casterRoot/build/windows/ordinary_caster/$Wheel/$Role"
$idfArgs = @('-B', $build, "-DSDKCONFIG=$build/sdkconfig",
             "-DCASTER_WHEEL=$Wheel", "-DCASTER_ROLE=$Role", '-DIDF_TARGET=esp32c6')
if ($Port) { $idfArgs += @('-p', $Port) }
$idfArgs += 'build'
if ($Flash) { $idfArgs += 'flash' }
if ($Monitor) { $idfArgs += 'monitor' }
Push-Location $casterRoot
try {
    & idf.py @idfArgs
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
