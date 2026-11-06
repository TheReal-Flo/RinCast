<#
  Builds every RinCast component and stages the PC package in dist\RinCast.

      powershell -ExecutionPolicy Bypass -File scripts\build-all.ps1 [-Config Release] [-Skip pc,audio,headset,server]
                 [-WdkRoot <path>] [-Deps <folder>] [-TestSign] [-SignedAudio <folder>] [-OpenXR]
                 [-Package] [-DebugApk] [-BuildDir <folder>] [-OutDir <folder>]

  Outputs
    dist\RinCast\            PC package; run install.ps1 inside it
    dist\RinCast.apk         headset app (adb install -r dist\RinCast.apk)
    dist\rincast-server.exe  account server (also see server\Dockerfile)
    dist\rincast-server-linux-amd64, -linux-arm64   static Linux builds (systemd unit in server\deploy)
    dist\LICENSE, dist\THIRD_PARTY_NOTICES.md       licence texts for the server binaries

  With -Package, also the files to publish, named after the VERSION file:
    dist\RinCast-<version>-windows-x64.zip
    dist\RinCast-<version>.apk (or RinCast-<version>-debug.apk with -DebugApk)
    dist\rincast-server-<version>-windows-amd64.exe, -linux-amd64, -linux-arm64
    dist\SHA256SUMS.txt

  The audio driver is only staged when Windows can load it: from -SignedAudio (a folder
  with the Microsoft-signed rincastaudio.sys, .cat and RinCastAudio.inf), or with -TestSign
  for development machines. -Package refuses test-signed files and RinCast XR.

  Release APKs need the RINCAST_KEYSTORE variables (docs\BUILDING.md). Without them a plain
  build falls back to a debug APK, and -Package stops unless you pass -DebugApk.

  Requirements: Visual Studio 2022 (C++ desktop workload), CMake 3.22+,
  Android Studio (JBR, SDK, NDK 27.3.13750724), Go 1.26+, and for the audio
  driver the WDK (scripts\fetch-deps.ps1).
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [string[]]$Skip = @(),
    [string]$WdkRoot = '',
    [string]$Deps = '',
    [switch]$TestSign,
    [string]$SignedAudio = '',
    [switch]$OpenXR,
    [switch]$Package,
    [switch]$DebugApk,
    [string]$BuildDir = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$Skip = @($Skip | ForEach-Object { $_ -split ',' })  # powershell -File passes "a,b" as one string
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# Absolute: later steps run from other folders.
$build = if ($BuildDir) { [IO.Path]::GetFullPath((Join-Path (Get-Location) $BuildDir)) } else { Join-Path $root 'build' }
$dist = if ($OutDir) { [IO.Path]::GetFullPath((Join-Path (Get-Location) $OutDir)) } else { Join-Path $root 'dist' }
$stage = Join-Path $dist 'RinCast'
$version = (Get-Content (Join-Path $root 'VERSION') -TotalCount 1).Trim()
$notices = "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.md"
$signingVars = 'RINCAST_KEYSTORE', 'RINCAST_KEYSTORE_PASSWORD', 'RINCAST_KEY_ALIAS', 'RINCAST_KEY_PASSWORD'
$missingSigning = @($signingVars | Where-Object { -not [Environment]::GetEnvironmentVariable($_) })

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Run($exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}
function Find-Package($pattern) {
    $roots = @($Deps, (Join-Path $root 'deps')) | Where-Object { $_ -and (Test-Path $_) }
    foreach ($r in $roots) {
        $hit = Get-ChildItem (Join-Path $r 'packages') -Directory -Filter $pattern -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}
# Windows loads a driver for ordinary users only with Microsoft's signature on its catalog.
function Assert-MicrosoftSigned($dir) {
    $cat = Join-Path $dir 'rincastaudio.cat'
    if (-not (Test-Path $cat)) { throw "$cat is missing, so the audio driver in $dir isn't signed." }
    $sig = Get-AuthenticodeSignature $cat
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft Windows Hardware') {
        throw "$cat isn't signed by Microsoft (status $($sig.Status), signer $($sig.SignerCertificate.Subject))."
    }
}

# Fail before the long builds, not after them.
if ($Package) {
    if ($Config -ne 'Release') { throw '-Package needs -Config Release.' }
    if ($TestSign) { throw '-Package refuses -TestSign: a test-signed driver must never be published.' }
    if ($OpenXR) { throw '-Package refuses -OpenXR: RinCast XR contains components that cannot be redistributed.' }
    if ($Skip -notcontains 'headset' -and -not $DebugApk -and $missingSigning) {
        throw "-Package needs a signed APK. Set $($missingSigning -join ', ') (see docs\BUILDING.md), or pass -DebugApk."
    }
}
if ($TestSign -and $SignedAudio) { throw 'Use either -TestSign or -SignedAudio, not both.' }

New-Item -ItemType Directory -Force $dist | Out-Null

# ---- PC: streamer, SteamVR driver, tools --------------------------------------
if ($Skip -notcontains 'pc') {
    Step 'PC components'
    $xrOption = if ($OpenXR) { 'ON' } else { 'OFF' }
    Run cmake @('-S', "$root\pc", '-B', "$build\pc", '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DRINCAST_BUILD_OPENXR=$xrOption")
    Run cmake @('--build', "$build\pc", '--config', $Config, '--parallel')
    # SteamVR may have the staged driver loaded (or the streamer may be running from dist):
    # files in use can't be deleted or overwritten, but they can be renamed aside.
    function Copy-Replacing([string]$from, [string]$toDir) {
        if (Test-Path $from -PathType Container) {
            $target = Join-Path $toDir (Split-Path $from -Leaf)
            New-Item -ItemType Directory -Force $target | Out-Null
            Get-ChildItem $from | ForEach-Object { Copy-Replacing $_.FullName $target }
            return
        }
        $dest = Join-Path $toDir (Split-Path $from -Leaf)
        try {
            Copy-Item $from $dest -Force -ErrorAction Stop
        } catch {
            $aside = "$dest.in-use-$(Get-Date -Format yyyyMMddHHmmss)"
            Rename-Item $dest $aside -ErrorAction Stop
            Copy-Item $from $dest -Force
            Write-Warning "$dest was in use; the old file was renamed to $(Split-Path $aside -Leaf)"
        }
    }
    if (Test-Path $stage) {
        Get-ChildItem $stage -Recurse -File -Filter '*.in-use-*' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
        Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue  # whatever is not locked
    }
    New-Item -ItemType Directory -Force $stage | Out-Null
    $bin = Join-Path $build "pc\bin\$Config"
    $items = @('RinCast Streamer.exe', 'rincast-audio-setup.exe', 'ui', 'driver')
    # Only when asked for: an older build folder may still hold a runtime from before.
    if ($OpenXR -and (Test-Path "$bin\openxr")) { $items += 'openxr' }
    foreach ($item in $items) { Copy-Replacing (Join-Path $bin $item) $stage }
    # The setup program without files in it is the uninstaller.
    Copy-Item (Join-Path $bin 'RinCast Setup.exe') (Join-Path $stage 'Uninstall RinCast.exe') -Force
    Copy-Item "$root\pc\installer\install.ps1", "$root\pc\installer\uninstall.ps1", "$root\VERSION" $stage -Force
    Copy-Item $notices $stage -Force
    New-Item -ItemType Directory -Force "$stage\LICENSES" | Out-Null
    $licenses = @{
        'Inter.txt'            = 'third_party\fonts\Inter-LICENSE.txt'
        'MaterialSymbols.txt'  = 'third_party\fonts\MaterialSymbols-LICENSE.txt'
        'Monocypher.md'        = 'third_party\monocypher\LICENCE.md'
        'OpenVR.txt'           = 'third_party\openvr\LICENSE'
        'OpenXR.txt'           = 'third_party\openxr\LICENSE'
        'Opus.txt'             = 'third_party\opus\COPYING'
        'WebView2.txt'         = 'third_party\webview2\LICENSE.txt'
        'SimpleAudioSample.txt' = 'pc\audio-driver\LICENSE.ms-pl'
    }
    if ($items -contains 'openxr') {
        $licenses['VDXR.txt'] = 'pc\runtime-openxr\LICENSE.vdxr'
        $licenses['VDXR-third-party.txt'] = 'pc\runtime-openxr\THIRD_PARTY.vdxr'
    }
    foreach ($k in $licenses.Keys) { Copy-Item (Join-Path $root $licenses[$k]) (Join-Path "$stage\LICENSES" $k) -Force }
}

# ---- Audio driver -------------------------------------------------------------
if (Test-Path "$stage\audio") { Remove-Item "$stage\audio" -Recurse -Force }
$audioToStage = $null
if ($SignedAudio) {
    Assert-MicrosoftSigned $SignedAudio
    $audioToStage = $SignedAudio
}
if ($Skip -notcontains 'audio' -and -not $SignedAudio) {
    Step 'Audio driver (RinCast Speaker / RinCast Microphone)'
    if (-not $WdkRoot) {
        $pkg = Find-Package 'Microsoft.Windows.WDK.x64.*'
        if ($pkg) { $WdkRoot = Join-Path $pkg 'c' }
    }
    if (-not $WdkRoot) {
        if ($TestSign) { throw 'WDK not found. Run scripts\fetch-deps.ps1 or pass -WdkRoot.' }
        Write-Host 'WDK not found; skipping the audio driver build. Release packages use Steam''s streaming devices.'
    } else {
        # CMake reads these two when it first configures a build folder.
        $env:CXXFLAGS = "/experimental:deterministic /pathmap:$root=rincast"
        $env:LDFLAGS = '/PDBALTPATH:%_PDB%'
        try {
            Run cmake @('-S', "$root\pc\audio-driver", '-B', "$build\audio", '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DWDK_ROOT=$WdkRoot")
        } finally { $env:CXXFLAGS = $null; $env:LDFLAGS = $null }
        Run cmake @('--build', "$build\audio", '--config', $Config)
        $pkgDir = Join-Path $build 'audio\package'
        New-Item -ItemType Directory -Force $pkgDir | Out-Null
        Remove-Item "$pkgDir\*.cer" -Force -ErrorAction SilentlyContinue
        Copy-Item "$build\audio\$Config\rincastaudio.sys", "$root\pc\audio-driver\RinCastAudio.inf" $pkgDir -Force
        # Stamp the INF date/version and create the catalog.
        $inf2cat = Get-ChildItem $WdkRoot -Recurse -Filter Inf2Cat.exe | Select-Object -First 1
        Run $inf2cat.FullName @("/driver:$pkgDir", '/os:10_X64,10_RS5_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64', '/uselocaltime')
        if ($TestSign) {
            & "$PSScriptRoot\audio\test-sign.ps1" -Package $pkgDir
            $audioToStage = $pkgDir
        } else {
            Write-Host "The unsigned driver is in $pkgDir and isn't staged: Windows won't load it. See docs\BUILDING.md."
        }
    }
}
if ($audioToStage -and (Test-Path $stage)) {
    New-Item -ItemType Directory -Force "$stage\audio" | Out-Null
    Copy-Item "$audioToStage\*" "$stage\audio" -Force
}

# ---- Headset app --------------------------------------------------------------
if ($Skip -notcontains 'headset') {
    Step 'Headset app'
    if (-not $env:JAVA_HOME) {
        $jbr = Join-Path $env:ProgramFiles 'Android\Android Studio\jbr'
        if (Test-Path $jbr) { $env:JAVA_HOME = $jbr }
    }
    $flavor = $Config.ToLower()
    if ($flavor -eq 'release' -and ($DebugApk -or $missingSigning)) {
        if (-not $DebugApk) { Write-Warning "$($missingSigning -join ', ') not set; building a debug APK. See docs\BUILDING.md." }
        $flavor = 'debug'
    }
    $task = if ($flavor -eq 'release') { 'assembleRelease' } else { 'assembleDebug' }
    Push-Location "$root\headset"
    try { Run '.\gradlew.bat' @($task, '--console=plain') } finally { Pop-Location }
    Copy-Item "$root\headset\app\build\outputs\apk\$flavor\app-$flavor.apk" "$dist\RinCast.apk" -Force
}

# ---- Server -------------------------------------------------------------------
if ($Skip -notcontains 'server') {
    Step 'Account server'
    Push-Location "$root\server"
    try {
        Run go @('test', './...')
        Run go @('build', '-trimpath', '-ldflags', '-s -w', '-o', "$dist\rincast-server.exe", '.')
        # Static Linux builds for servers (x86-64 and ARM64, e.g. Ampere/Graviton).
        $saved = @{ GOOS = $env:GOOS; GOARCH = $env:GOARCH; CGO_ENABLED = $env:CGO_ENABLED }
        try {
            $env:GOOS = 'linux'; $env:CGO_ENABLED = '0'
            foreach ($arch in 'amd64', 'arm64') {
                $env:GOARCH = $arch
                Run go @('build', '-trimpath', '-ldflags', '-s -w', '-o', "$dist\rincast-server-linux-$arch", '.')
            }
        } finally {
            $env:GOOS = $saved.GOOS; $env:GOARCH = $saved.GOARCH; $env:CGO_ENABLED = $saved.CGO_ENABLED
        }
    } finally { Pop-Location }
    Copy-Item $notices $dist -Force
}

# ---- Release package ----------------------------------------------------------
if ($Package) {
    Step "Release package $version"
    if (-not (Test-Path "$stage\RinCast Streamer.exe")) { throw "$stage has no streamer. Don't skip pc when packaging." }

    $cer = @(Get-ChildItem $stage -Recurse -File -Filter '*.cer')
    if ($cer) { throw "Refusing to package a test certificate: $($cer.FullName -join ', ')" }
    if (Test-Path "$stage\openxr") { throw "Refusing to package RinCast XR: $stage\openxr exists." }
    if (Test-Path "$stage\audio") { Assert-MicrosoftSigned "$stage\audio" }
    $inUse = @(Get-ChildItem $stage -Recurse -File -Filter '*.in-use-*')
    if ($inUse) { throw "Close whatever is running from $stage and build again: $($inUse.FullName -join ', ')" }
    foreach ($f in $notices) {
        if (-not (Test-Path (Join-Path $stage (Split-Path $f -Leaf)))) { throw "$f is missing from $stage." }
    }

    $published = @()
    $zip = Join-Path $dist "RinCast-$version-windows-x64.zip"
    # Entry by entry: Windows PowerShell's Compress-Archive and CreateFromDirectory write
    # backslashes into the entry names, which other unzip tools don't treat as folders.
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    if (Test-Path $zip) { Remove-Item $zip -Force }
    $stageFull = (Resolve-Path $stage).Path
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem $stageFull -Recurse -File -Force) {
            $entry = 'RinCast/' + $file.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $archive.Dispose() }
    $published += $zip
    # The same files in one program that installs them: what most people download.
    $setup = Join-Path $dist "RinCast-$version-Setup.exe"
    & (Join-Path $bin 'RinCast Setup.exe') --pack $stageFull $setup | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $setup)) { throw "Packing $setup failed." }
    $published += $setup
    if ($Skip -notcontains 'headset') {
        $apkName = if ($flavor -eq 'release') { "RinCast-$version.apk" } else { "RinCast-$version-debug.apk" }
        Copy-Item "$dist\RinCast.apk" (Join-Path $dist $apkName) -Force
        $published += Join-Path $dist $apkName
    }
    if ($Skip -notcontains 'server') {
        $servers = @{ 'rincast-server.exe' = "rincast-server-$version-windows-amd64.exe"
            'rincast-server-linux-amd64' = "rincast-server-$version-linux-amd64"
            'rincast-server-linux-arm64' = "rincast-server-$version-linux-arm64" }
        foreach ($k in $servers.Keys) {
            Copy-Item (Join-Path $dist $k) (Join-Path $dist $servers[$k]) -Force
            $published += Join-Path $dist $servers[$k]
        }
    }
    $published | Sort-Object | ForEach-Object {
        "$((Get-FileHash $_ -Algorithm SHA256).Hash.ToLower())  $(Split-Path $_ -Leaf)"
    } | Set-Content (Join-Path $dist 'SHA256SUMS.txt') -Encoding Ascii
    $published | ForEach-Object { Write-Host "  $_" }
}

Write-Host "`nDone. Output in $dist" -ForegroundColor Green
