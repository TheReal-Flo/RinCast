<#
  Downloads the build-time dependencies that are not vendored in third_party/:
  the Windows Driver Kit and Windows SDK NuGet packages (for the audio driver).

      powershell -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1 [-Dest <folder>]

  Everything else (Monocypher, Opus, OpenXR/OpenVR headers, NVENC/AMF headers,
  WebView2, stb, nlohmann/json, fonts) is vendored in third_party/. Android
  builds need Android Studio (or the SDK + NDK 27.3.13750724 + CMake 3.22.1).
#>
[CmdletBinding()]
param([string]$Dest)

$ErrorActionPreference = 'Stop'
# Not as the parameter's default: $PSScriptRoot can be empty there (seen on GitHub's runners).
if (-not $Dest) { $Dest = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\deps' }
$wdkVersion = '10.0.28000.2526'
$sdkVersion = '10.0.28000.1721'

New-Item -ItemType Directory -Force $Dest | Out-Null
$Dest = (Resolve-Path $Dest).Path
$nuget = Join-Path $Dest 'nuget.exe'
if (-not (Test-Path $nuget)) {
    Write-Host 'Downloading nuget.exe'
    Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget
}
$packages = Join-Path $Dest 'packages'
foreach ($p in @(
        @{ Id = 'Microsoft.Windows.WDK.x64'; Version = $wdkVersion },
        @{ Id = 'Microsoft.Windows.SDK.CPP'; Version = $sdkVersion },
        @{ Id = 'Microsoft.Windows.SDK.CPP.x64'; Version = $sdkVersion })) {
    if (-not (Test-Path (Join-Path $packages "$($p.Id).$($p.Version)"))) {
        Write-Host "Fetching $($p.Id) $($p.Version)"
        & $nuget install $p.Id -Version $p.Version -OutputDirectory $packages -NonInteractive | Out-Null
    }
}
Write-Host "Dependencies are in $packages"
Write-Host "WDK_ROOT = $(Join-Path $packages "Microsoft.Windows.WDK.x64.$wdkVersion\c")"
