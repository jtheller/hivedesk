$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$root=Join-Path $projectRoot '.local\ffmpeg-build'
$destination=Join-Path $projectRoot 'build\ffmpeg'
$bash='C:\Program Files\Git\bin\bash.exe'
$sevenZip='C:\Program Files\7-Zip\7z.exe'
$recipe=Join-Path $PSScriptRoot 'build-codec.sh'
$recipeHash=(Get-FileHash -LiteralPath $recipe -Algorithm SHA256).Hash
$stamp=Join-Path $root 'complete.txt'
New-Item -ItemType Directory -Path $root -Force | Out-Null
function Fetch-Verified([string]$name,[string]$uri,[string]$sha256){
    $path=Join-Path $root $name
    if(!(Test-Path -LiteralPath $path)){Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $path}
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $sha256){throw "Checksum mismatch: $name"}
}
Fetch-Verified 'ffmpeg-n9.0.2.tar.gz' 'https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n9.0.2.tar.gz' '6E374ED621E48FAA40639307DFF48BA6FE574A509977956D2CCE9669B7CC27E9'
Fetch-Verified 'make.pkg.tar.zst' 'https://repo.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst' 'AF0BDBA17F06FE037F0194069ADAA31A8FE45F1A11381501896AEA1FAE37BD5D'
Fetch-Verified 'nasm.zip' 'https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip' '3EE4782247BCB874378D02F7EAB4E294A84D3D15F3F6EE2DE2F47A46AA7226E6'
if(!(Test-Path -LiteralPath $stamp) -or (Get-Content -LiteralPath $stamp -Raw).Trim() -ne $recipeHash -or !(Test-Path -LiteralPath (Join-Path $root 'install\bin\avcodec-63.dll'))){
    if(!(Test-Path -LiteralPath $bash) -or !(Test-Path -LiteralPath $sevenZip)){throw 'Building the pinned HEVC decoder needs Git for Windows and 7-Zip.'}
    tar.exe -xf (Join-Path $root 'ffmpeg-n9.0.2.tar.gz') -C $root
    if($LASTEXITCODE){throw 'FFmpeg source extraction failed'}
    & $sevenZip x (Join-Path $root 'make.pkg.tar.zst') ('-o'+$root) -y | Out-Null
    if($LASTEXITCODE){throw 'GNU make extraction failed'}
    tar.exe -xf (Join-Path $root 'make.pkg.tar') -C $root
    if($LASTEXITCODE){throw 'GNU make tar extraction failed'}
    Expand-Archive -LiteralPath (Join-Path $root 'nasm.zip') -DestinationPath $root -Force
    & $bash --noprofile --norc $recipe $root
    if($LASTEXITCODE){throw 'HEVC decoder build failed'}
    Set-Content -LiteralPath $stamp -Value $recipeHash -Encoding ASCII
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'install\include') -Destination $destination -Recurse -Force
foreach($name in @('avcodec-63.dll','avutil-61.dll','avcodec.lib','avutil.lib')){
    Copy-Item -LiteralPath (Join-Path $root ('install\bin\'+$name)) -Destination (Join-Path $destination $name) -Force
}
# Include the exact corresponding source and rebuild recipe with every package.
Copy-Item -LiteralPath (Join-Path $root 'ffmpeg-n9.0.2.tar.gz') -Destination $destination -Force
Copy-Item -LiteralPath (Join-Path $root 'FFmpeg-n9.0.2\COPYING.LGPLv2.1') -Destination $destination -Force
Write-Output 'Pinned HEVC software decoder ready (FFmpeg 9.0.2, LGPL, HEVC only).'
