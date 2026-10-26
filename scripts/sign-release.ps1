param([Parameter(Mandatory=$true)][string]$ReleaseDirectory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security
$projectRoot=Split-Path -Parent $PSScriptRoot
$version=[regex]::Match((Get-Content (Join-Path $projectRoot 'CMakeLists.txt') -Raw),'set\(HIVEDESK_VERSION "([^"]+)"\)').Groups[1].Value
if($version -notmatch '^\d+\.\d+\.\d+(-(alpha|beta|rc)\.\d+)?$'){throw 'Invalid release version'}
$setup=Get-Item -LiteralPath (Join-Path $ReleaseDirectory ('HiveDesk-'+$version+'-windows-x64-Setup.exe'))
if($setup.VersionInfo.ProductVersion.Trim() -ne $version){throw 'Installer version differs from source'}
$hash=(Get-FileHash -LiteralPath $setup.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest="HiveDesk-Update-1`nversion=$version`nfile=$($setup.Name)`nsize=$($setup.Length)`nsha256=$hash`n"
$bytes=[Text.Encoding]::UTF8.GetBytes($manifest)
$keyPath=Join-Path $env:LOCALAPPDATA 'HiveDeskRelease\signing-key.dpapi'
if(-not (Test-Path -LiteralPath $keyPath)){throw 'Release signing key is missing. Restore the existing key; do not rotate it automatically.'}
$entropy=[Text.Encoding]::UTF8.GetBytes('jtheller/hivedesk release signing v1')
$plain=[Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($keyPath),$entropy,[Security.Cryptography.DataProtectionScope]::CurrentUser)
$rsa=New-Object Security.Cryptography.RSACryptoServiceProvider
$rsa.PersistKeyInCsp=$false
try {
    $rsa.FromXmlString([Text.Encoding]::UTF8.GetString($plain))
    $expected=Get-Content (Join-Path $projectRoot 'resources\update-public-key.json') -Raw | ConvertFrom-Json
    $actual=$rsa.ExportParameters($false)
    if([Convert]::ToBase64String($actual.Modulus) -ne $expected.modulus -or [Convert]::ToBase64String($actual.Exponent) -ne $expected.exponent){throw 'Signing key does not match the pinned updater public key'}
    $signature=$rsa.SignData($bytes,[Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'))
    [IO.File]::WriteAllBytes((Join-Path $ReleaseDirectory 'update.manifest'),$bytes)
    [IO.File]::WriteAllBytes((Join-Path $ReleaseDirectory 'update.manifest.sig'),$signature)
} finally {$rsa.Dispose();[Array]::Clear($plain,0,$plain.Length)}
& (Join-Path $projectRoot 'build\HiveDeskUpdater.exe') --verify-release $ReleaseDirectory
if($LASTEXITCODE){throw 'Native updater rejected the signed release'}
