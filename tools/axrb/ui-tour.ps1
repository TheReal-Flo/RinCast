<#
  Screenshot tour of the headset UI without a Quest.

  Requires RinCast Streamer to be running (trusting "sim-device"). Starts the
  headset simulator in VR mode, runs the app in AXRB on the RinCast XR runtime,
  steps through the windows with the app's debug.rincast.* properties and logs
  the simulator frame index for each state. Afterwards run
  tools\axrb\extract-shots.mjs to decode one PNG per state.
#>
param(
    [string]$Apk = (Join-Path $PSScriptRoot '..\..\headset\app\build\outputs\apk\debug\app-debug.apk'),
    [int]$SecondsPerState = 22,
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = "$repo\build\axrb-test"
$managed = "$env:LOCALAPPDATA\AXRB Runtime"
New-Item -ItemType Directory -Force "$out\sim" | Out-Null
Remove-Item "$out\sim\stream0.h265", "$out\states.log", "$out\sim.log" -ErrorAction SilentlyContinue

$sim = Start-Process "$repo\build\pc\bin\Release\headset-sim.exe" -WindowStyle Hidden -PassThru `
    -ArgumentList '--pc', '127.0.0.1', '--vr', '--still', '--seconds', '1800', '--out', "`"$out\sim`"" `
    -RedirectStandardOutput "$out\sim.log" -RedirectStandardError "$out\sim.err"
$axrb = Start-Process powershell -WindowStyle Hidden -PassThru `
    -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\run-in-axrb.ps1`"", '-Apk', "`"$((Resolve-Path $Apk).Path)`"" `
    -RedirectStandardOutput "$out\axrb.log" -RedirectStandardError "$out\axrb.err"

function Wait-For([scriptblock]$Condition, [int]$TimeoutSeconds, [string]$What) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (& $Condition)) {
        if ((Get-Date) -gt $deadline) { throw "Timed out waiting for $What" }
        if ($axrb.HasExited) { throw "AXRB exited early; see $out\axrb.log" }
        Start-Sleep -Seconds 2
    }
}
Wait-For { (Get-Content "$out\axrb.log" -Raw -ErrorAction SilentlyContinue) -match 'is running' } 300 'the AXRB session'
Wait-For { (Get-Content "$out\sim.log" -Raw -ErrorAction SilentlyContinue) -match 'frames=[1-9]' } 120 'video frames'

$env:ANDROID_ADB_SERVER_PORT = '5038'
$adb = "$managed\sdk\platform-tools\adb.exe"
function Prop([string]$Name, [string]$Value) { & $adb -s emulator-5584 shell setprop $Name $Value | Out-Null }

$states = @(
    @{ Name = 'pcs'; Ui = 'home'; Pc = '10.0.2.2'; Tab = 0 },
    @{ Name = 'vr-quality'; Ui = 'home'; Tab = 2 },
    @{ Name = 'desktop'; Ui = 'home'; Tab = 3 },
    @{ Name = 'environment'; Ui = 'home'; Tab = 4 },
    @{ Name = 'general'; Ui = 'home'; Tab = 5 },
    @{ Name = 'account'; Ui = 'home'; Tab = 6 },
    @{ Name = 'help'; Ui = 'home'; Tab = 7 },
    @{ Name = 'dock-keyboard-stats'; Ui = 'dock,keyboard,stats' },
    @{ Name = 'games'; Ui = 'home,dock'; Tab = 1 },
    @{ Name = 'vr-overlay'; Ui = 'vr-overlay' },
    @{ Name = 'pcs-again'; Ui = 'home'; Tab = 0 }
)
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Only.Count) { $states = @($states | Where-Object { $Only -contains $_.Name }) }
foreach ($s in $states) {
    if ($s.Pc) { Prop 'debug.rincast.pc' $s.Pc }
    if ($null -ne $s.Tab) { Prop 'debug.rincast.tab' "$($s.Tab)" }
    Prop 'debug.rincast.ui' $s.Ui
    Start-Sleep -Seconds $SecondsPerState
    $ms = [DateTimeOffset]::Now.ToUnixTimeMilliseconds()
    "$($s.Name) $ms" | Add-Content "$out\states.log"
    Write-Output "captured state $($s.Name)"
}
Start-Sleep -Seconds 3
Get-Process axrb-host-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
Stop-Process -Id $sim.Id -Force -ErrorAction SilentlyContinue
Write-Output 'tour done'
