<#
  Removes RinCast: streamer, SteamVR driver registration, OpenXR runtime
  registration, audio driver and test certificate, firewall rules and shortcuts.

      powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-KeepSettings]
#>
[CmdletBinding()]
param([switch]$KeepSettings)

$ErrorActionPreference = 'Continue'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" $(if ($KeepSettings) { '-KeepSettings' })"
    return
}

# An elevated PowerShell can run as a different account than the person at the
# desktop, and then HKCU and %LOCALAPPDATA% belong to that admin account.
function Get-DesktopUserSid {
    $session = (Get-Process -Id $PID).SessionId
    $shell = Get-CimInstance Win32_Process -Filter "Name='explorer.exe' AND SessionId=$session" | Select-Object -First 1
    if ($shell) {
        $sid = (Invoke-CimMethod -InputObject $shell -MethodName GetOwnerSid).Sid
        if ($sid -and (Test-Path "Registry::HKEY_USERS\$sid")) { return $sid }
    }
    return $id.User.Value
}
function Get-LocalAppData($sid) {
    $path = (Get-ItemProperty "Registry::HKEY_USERS\$sid\Volatile Environment" -ErrorAction SilentlyContinue).LOCALAPPDATA
    if ($path) { return $path }
    $userProfile = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$sid" -ErrorAction SilentlyContinue).ProfileImagePath
    if ($userProfile) { return Join-Path $userProfile 'AppData\Local' }
    return $null
}
$userSid = Get-DesktopUserSid
$userHive = "Registry::HKEY_USERS\$userSid"

$installDir = $PSScriptRoot
$un = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RinCast'
$loc = (Get-ItemProperty $un -ErrorAction SilentlyContinue).InstallLocation
if ($loc) { $installDir = $loc }
$installDir = $installDir.TrimEnd('\')

$running = Get-Process 'RinCast Streamer' -ErrorAction SilentlyContinue
if ($running) {
    $running | Stop-Process -Force
    # Windows keeps the exe locked until the process has really gone.
    $running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}

# Audio driver
$setup = Join-Path $installDir 'rincast-audio-setup.exe'
if (Test-Path $setup) { & $setup remove | Out-Null }

# Test certificate from a development build (scripts\audio\test-sign.ps1)
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq 'CN=RinCast Test Signing' | Remove-Item
}

# SteamVR driver. vrpathreg writes to the profile of the account it runs as, so when the
# desktop user is another account their SteamVR keeps an entry for a folder that is gone,
# which SteamVR ignores.
$steam = (Get-ItemProperty "$userHive\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
$vrpathreg = if ($steam) { Join-Path $steam 'steamapps\common\SteamVR\bin\win64\vrpathreg.exe' }
if ($userSid -eq $id.User.Value -and $vrpathreg -and (Test-Path $vrpathreg)) { & $vrpathreg removedriver (Join-Path $installDir 'driver\rincast') | Out-Null }

# OpenXR runtime: unregister, and hand the active runtime back to SteamVR if it was ours.
$manifest = Join-Path $installDir 'openxr\rincast-openxr.json'
$xr = 'HKLM:\SOFTWARE\Khronos\OpenXR\1'
Remove-ItemProperty -Path "$xr\AvailableRuntimes" -Name $manifest -ErrorAction SilentlyContinue
$active = (Get-ItemProperty $xr -ErrorAction SilentlyContinue).ActiveRuntime
if ($active -and ($active -ieq $manifest)) {
    $steamXr = if ($steam) { Join-Path $steam 'steamapps\common\SteamVR\steamxr_win64.json' }
    if ($steamXr -and (Test-Path $steamXr)) { Set-ItemProperty $xr -Name ActiveRuntime -Value $steamXr }
    else { Remove-ItemProperty $xr -Name ActiveRuntime -ErrorAction SilentlyContinue }
}

# Firewall, autostart, shortcuts, uninstall entry
Get-NetFirewallRule -Group 'RinCast' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName 'RinCast Streamer' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName 'RinCast Streamer (USB cable)' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Remove-ItemProperty "$userHive\Software\Microsoft\Windows\CurrentVersion\Run" -Name 'RinCast Streamer' -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'RinCast Streamer.lnk') -ErrorAction SilentlyContinue
Remove-Item $un -Recurse -ErrorAction SilentlyContinue
if (-not $KeepSettings) {
    $localAppData = Get-LocalAppData $userSid
    if ($localAppData) { Remove-Item (Join-Path $localAppData 'RinCast') -Recurse -Force -ErrorAction SilentlyContinue }
}

# Files (the script itself lives here, so delete from a detached shell). The installer leaves a
# sentinel file; a custom folder without it may hold someone else's files, so leave it alone.
$isDriveRoot = $installDir -eq [IO.Path]::GetPathRoot($installDir).TrimEnd('\')
$isDefault = $installDir -ieq (Join-Path $env:ProgramFiles 'RinCast')
if (-not $isDriveRoot -and ($isDefault -or (Test-Path (Join-Path $installDir '.rincast-install')))) {
    Start-Process cmd.exe -WindowStyle Hidden -ArgumentList "/c timeout /t 2 >nul & rmdir /s /q `"$installDir`""
    Write-Host 'RinCast was removed.' -ForegroundColor Green
} else {
    Write-Host "RinCast was removed, but $installDir was kept because it has no installer marker. Delete it yourself if you don't need it." -ForegroundColor Yellow
}
