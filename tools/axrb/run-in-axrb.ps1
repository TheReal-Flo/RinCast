<#
  Visual test of the headset app without a Quest: runs the RinCast APK in AXRB
  (Android XR on Windows, emulator backend, which supports GLES) and presents it
  through the RinCast XR OpenXR runtime instead of SteamVR, so no headset is
  needed. AXRB's diagnostic capture writes the submitted layers as .ppm files.

      powershell -ExecutionPolicy Bypass -File tools\axrb\run-in-axrb.ps1 [-Apk dist\RinCast.apk] [-Seconds 120]
#>
param(
    [string]$Apk = (Join-Path $PSScriptRoot '..\..\dist\RinCast.apk'),
    [int]$CaptureDelay = 30,
    [string]$Out = (Join-Path $PSScriptRoot '..\..\build\axrb-test')
)
$ErrorActionPreference = 'Stop'
$runtime = "$env:LOCALAPPDATA\Programs\axrb-launcher\resources\runtime"
$managed = "$env:LOCALAPPDATA\AXRB Runtime"
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Force "$Out\cap", "$Out\data" | Out-Null
$Out = (Resolve-Path $Out).Path

$env:PATH = "$runtime\tools\python;$env:PATH"
$env:AXRB_DATA_HOME = "$Out\data"
$env:ANDROID_AVD_HOME = "$managed\avd"
$env:ANDROID_USER_HOME = "$managed\android"
$env:XR_RUNTIME_JSON = "$repo\build\pc\bin\Release\openxr\rincast-openxr.json"
$env:AXRB_CAPTURE_PREFIX = "$Out\cap\frame"
$env:AXRB_CAPTURE_DELAY_SECONDS = "$CaptureDelay"
$env:AXRB_CAPTURE_FIRST_SEQUENCE = '1'
$env:AXRB_CAPTURE_ALL_LAYERS = '1'

& "$runtime\scripts\run\run_windows_game.ps1" -Avd axrb-managed-api36 -Port 5584 -Sdk "$managed\sdk" `
    -MemoryMB 8192 -CpuCores 4 -OwnsEmulator -CaptureGuestLog -GuestLogTags @('RinCast:V', 'AXRB.Perf:I', 'AXRB.GPU:I') `
    -RuntimeApk "$runtime\out\android\runtime-arm64-v8a\axrb-openxr-runtime-debug.apk" `
    -AppApk (Resolve-Path $Apk).Path -Package app.rincast.vr -Activity app.rincast.vr/.MainActivity -GameName RinCast
