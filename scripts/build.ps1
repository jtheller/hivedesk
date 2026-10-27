param([switch]$SkipTests, [switch]$Package, [switch]$NoPublish)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildCommit=''
if($Package -and -not $NoPublish){
    $buildCommit=git -C $projectRoot rev-parse HEAD
    if($LASTEXITCODE -or (git -C $projectRoot status --porcelain)){throw 'Commit the source before release packaging (use -NoPublish only for local verification).'}
    $buildCommit=$buildCommit.Trim()
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with C++ x64, Windows SDK and CMake first.' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'MSVC C++ x64 tools were not found.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
cmake -S $projectRoot -B (Join-Path $projectRoot 'build') -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
cmake --build (Join-Path $projectRoot 'build') --parallel 4
if ($LASTEXITCODE) { throw 'Build failed.' }
$dotnet = Join-Path $env:LOCALAPPDATA 'TRG\Toolchains\dotnet10\dotnet.exe'
if (-not (Test-Path -LiteralPath $dotnet)) { $dotnet = (Get-Command dotnet -ErrorAction Stop).Source }
$settingsOutput = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build\settings'))
$expectedOutput = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (-not $settingsOutput.StartsWith($expectedOutput, [StringComparison]::OrdinalIgnoreCase)) { throw 'Settings output must be within the project build directory.' }
if (Test-Path -LiteralPath $settingsOutput) { Remove-Item -LiteralPath $settingsOutput -Recurse -Force }
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
& $dotnet publish (Join-Path $projectRoot 'settings\HiveDesk.Settings.csproj') -c Release -r win-x64 --self-contained true -o $settingsOutput -p:Platform=x64
if ($LASTEXITCODE) { throw 'WinUI settings build failed.' }
# App-local VC runtime for WinUI's native dependencies; the agent itself uses /MT.
$crtRoot = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Redist\MSVC') -Directory | Where-Object Name -Match '^\d' | Sort-Object Name -Descending | Select-Object -First 1
if (-not $crtRoot) { throw 'Visual C++ redistributable directory was not found.' }
Copy-Item -Path (Join-Path $crtRoot.FullName 'x64\Microsoft.VC143.CRT\*.dll') -Destination $settingsOutput -Force
$licenseDir = Join-Path $settingsOutput 'licenses'
New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null
$assets = Get-Content -LiteralPath (Join-Path $projectRoot 'settings\obj\project.assets.json') -Raw | ConvertFrom-Json
$packageRoot = ($assets.packageFolders.PSObject.Properties | Select-Object -First 1).Name
foreach ($libraryEntry in $assets.libraries.PSObject.Properties) {
    $packageDir = Join-Path $packageRoot $libraryEntry.Name.ToLowerInvariant()
    if (Test-Path -LiteralPath $packageDir) {
        Get-ChildItem -LiteralPath $packageDir -File | Where-Object Name -Match '^(license|notice|third-party-notices)' | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $licenseDir ($libraryEntry.Name.Replace('/','-') + '-' + $_.Name)) -Force
        }
    }
}
if (-not $SkipTests) {
    ctest --test-dir (Join-Path $projectRoot 'build') --output-on-failure
    if ($LASTEXITCODE) { throw 'Verification failed.' }
    & $dotnet run --project (Join-Path $projectRoot 'tests\settings_update\HiveDesk.Settings.UpdateCheck.csproj') -c Release --artifacts-path (Join-Path $projectRoot 'build\settings-update-tests')
    if ($LASTEXITCODE) { throw 'Settings update bridge verification failed.' }
    foreach ($theme in @('light','dark')) {
        $report = Join-Path $projectRoot ('build\winui-' + $theme + '-test.txt')
        $probe = Start-Process -FilePath (Join-Path $settingsOutput 'HiveDesk.Settings.exe') -ArgumentList @('--self-test',('--'+$theme),('--report="'+$report+'"')) -PassThru
        if (-not $probe.WaitForExit(20000)) { throw 'WinUI settings verification timed out.' }
        if ($probe.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $report) -or -not ((Get-Content -LiteralPath $report -Raw).StartsWith('PASS'))) { throw ('WinUI '+$theme+' verification failed.') }
        Get-Content -LiteralPath $report
    }
}
if ($Package) {
    $distDir = Join-Path $projectRoot 'dist'
    New-Item -ItemType Directory -Path $distDir -Force | Out-Null
    cpack --config (Join-Path $projectRoot 'build\CPackConfig.cmake') -B $distDir
    if ($LASTEXITCODE) { throw 'ZIP packaging failed.' }
    $innoCompiler = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'
    if (-not (Test-Path -LiteralPath $innoCompiler)) {
        $innoCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if (-not $innoCommand) { throw 'Install Inno Setup 7 or make ISCC.exe available on PATH.' }
        $innoCompiler = $innoCommand.Source
    }
    & $innoCompiler --quiet (Join-Path $projectRoot 'installer\HiveDesk.iss')
    if ($LASTEXITCODE) { throw 'Installer packaging failed.' }
    Get-ChildItem -LiteralPath $distDir -File | Where-Object Extension -in @('.zip','.exe') | ForEach-Object {
        $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
        $hash.Hash.ToLowerInvariant() + '  ' + $_.Name
    } | Set-Content -LiteralPath (Join-Path $distDir 'SHA256SUMS.txt') -Encoding ASCII
    # A clean handoff directory contains only this release's installer, guide
    # and checksum, without historical builds or the developer ZIP.
    $packageConfig = Get-Content -LiteralPath (Join-Path $projectRoot 'build\CPackConfig.cmake') -Raw
    $packageVersion = [regex]::Match($packageConfig, 'set\(CPACK_PACKAGE_VERSION "([0-9A-Za-z.\-]+)"\)').Groups[1].Value
    if (-not $packageVersion) { throw 'Could not read the packaged version.' }
    $releaseDir = Join-Path $distDir ('HiveDesk-' + $packageVersion)
    New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
    $setupName = 'HiveDesk-' + $packageVersion + '-windows-x64-Setup.exe'
    Copy-Item -LiteralPath (Join-Path $distDir $setupName) -Destination $releaseDir -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\INSTALL.zh-CN.md') -Destination $releaseDir -Force
    $setupHash = Get-FileHash -LiteralPath (Join-Path $releaseDir $setupName) -Algorithm SHA256
    ($setupHash.Hash.ToLowerInvariant() + '  ' + $setupName) | Set-Content -LiteralPath (Join-Path $releaseDir 'SHA256SUMS.txt') -Encoding ASCII
    if(-not $NoPublish){
        if((git -C $projectRoot rev-parse HEAD).Trim() -ne $buildCommit -or (git -C $projectRoot status --porcelain)){throw 'Source changed during packaging; rebuild from the committed release.'}
        $buildCommit | Set-Content -LiteralPath (Join-Path $releaseDir 'build-commit.txt') -Encoding ASCII
        & (Join-Path $PSScriptRoot 'publish-release.ps1') -ReleaseDirectory $releaseDir
    }else{Write-Output 'Local verification package only (-NoPublish); no GitHub Release was created.'}
}
