<#
  Test-signs the RinCast audio driver package for development machines.

      powershell -ExecutionPolicy Bypass -File scripts\audio\test-sign.ps1 [-Package build\audio\package]

  Creates (once) a self-signed code-signing certificate "RinCast Test Signing"
  in CurrentUser\My, signs rincastaudio.sys and rincastaudio.cat, and exports
  the public certificate as RinCastTest.cer next to them. install.ps1 trusts
  that certificate on the target machine.

  Windows only loads test-signed kernel drivers with test signing enabled:
      bcdedit /set testsigning on     (elevated, then reboot; Secure Boot must be off)
  Never distribute a test-signed package; build-all.ps1 -Package refuses one.
  Shipping builds must be attestation-signed through the Microsoft Partner Center.
#>
[CmdletBinding()]
param([string]$Package)

$ErrorActionPreference = 'Stop'
# Not as the parameter's default: $PSScriptRoot can be empty there (seen on GitHub's runners).
if (-not $Package) { $Package = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..\build\audio\package' }
$Package = (Resolve-Path $Package).Path

$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object Subject -eq 'CN=RinCast Test Signing' | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Subject 'CN=RinCast Test Signing' -Type CodeSigningCert `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $cert -FilePath (Join-Path $Package 'RinCastTest.cer') | Out-Null

$signtool = $null
foreach ($r in @((Join-Path $PSScriptRoot '..\..\deps\packages'), "${env:ProgramFiles(x86)}\Windows Kits\10\bin")) {
    if (Test-Path $r) {
        $signtool = Get-ChildItem $r -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -Last 1
        if ($signtool) { break }
    }
}
if (-not $signtool) { throw 'signtool.exe not found. Run scripts\fetch-deps.ps1 or install the Windows SDK.' }

foreach ($f in 'rincastaudio.sys', 'rincastaudio.cat') {
    & $signtool.FullName sign /fd SHA256 /sha1 $cert.Thumbprint /t http://timestamp.digicert.com (Join-Path $Package $f)
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f" }
}
Write-Host "Signed the driver package in $Package" -ForegroundColor Green
