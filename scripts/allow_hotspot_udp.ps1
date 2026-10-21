param(
    [string]$HotspotAddress = '192.168.137.1',
    [string]$HotspotSubnet = '192.168.137.0/24',
    [ValidateRange(1, 65535)][int]$Port = 5005
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Windows requires an Administrator PowerShell window to add this firewall rule.'
}
$python = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
if (-not (Test-Path $python)) { throw 'Windows PlatformIO Python was not found.' }
$ruleName = "HAMR-Caster-Encoder-UDP-$Port-Hotspot"
$settings = @{
    Direction = 'Inbound'
    Action = 'Allow'
    Enabled = 'True'
    Protocol = 'UDP'
    LocalPort = $Port
    LocalAddress = $HotspotAddress
    RemoteAddress = $HotspotSubnet
    Program = $python
    Profile = 'Any'
}
if ($null -eq (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -Name $ruleName -DisplayName "HAMR caster encoder UDP $Port on hotspot" @settings | Out-Null
} else {
    Set-NetFirewallRule -Name $ruleName @settings
}
Write-Host "Allowed UDP $Port to Windows Python at $HotspotAddress from $HotspotSubnet."
