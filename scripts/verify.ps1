param([switch]$SkipBuild, [switch]$Hardware, [switch]$InstalledBroker, [ValidateRange(1,20)][int]$Repeat=3)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$build=Join-Path $root 'build'
$report=Join-Path $root ('.local\validation\'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $report -Force | Out-Null
$results=New-Object 'System.Collections.Generic.List[object]'
$commit=(git -C $root rev-parse HEAD).Trim()
$dirty=[bool](git -C $root status --porcelain)
$version=[regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')),'set\(HIVEDESK_VERSION "([^"]+)"\)').Groups[1].Value
$finished=$false
function SaveReport {
    [pscustomobject]@{version=$version;commit=$commit;dirty=$dirty;completed=$finished;buildSkipped=[bool]$SkipBuild;repeat=$Repeat;
        hardware=[bool]$Hardware;installedBroker=[bool]$InstalledBroker;results=@($results.ToArray())} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $report 'results.json') -Encoding UTF8
}
function Check([string]$Name,[string]$File,[string[]]$Arguments=@(),[int]$Seconds=120) {
    $stdout=Join-Path $report ($Name+'.stdout.txt');$stderr=Join-Path $report ($Name+'.stderr.txt')
    $clock=[Diagnostics.Stopwatch]::StartNew();$code=-1;$status='failed';$problem='';$process=$null;$outTask=$null;$errTask=$null
    Write-Output ('RUN '+$Name)
    try {
        $process=New-Object Diagnostics.Process
        $process.StartInfo.FileName=$File;$process.StartInfo.Arguments=$Arguments -join ' '
        $process.StartInfo.WorkingDirectory=$root;$process.StartInfo.UseShellExecute=$false
        $process.StartInfo.CreateNoWindow=$true;$process.StartInfo.WindowStyle='Hidden'
        $process.StartInfo.RedirectStandardOutput=$true;$process.StartInfo.RedirectStandardError=$true
        if(-not $process.Start()){throw 'Process did not start'}
        $outTask=$process.StandardOutput.ReadToEndAsync();$errTask=$process.StandardError.ReadToEndAsync()
        while(-not $process.WaitForExit(1000)) {
            if($clock.Elapsed.TotalSeconds -ge $Seconds){$process.Kill();throw ('Deadline exceeded: '+$Seconds+' seconds')}
        }
        $process.WaitForExit();$code=$process.ExitCode
        if($code -eq 0){$status='passed'}else{$problem='Exit '+$code}
    }catch{$problem=$_.Exception.Message}
    finally{
        if($outTask -and $outTask.Wait(5000)){[IO.File]::WriteAllText($stdout,$outTask.Result)}
        if($errTask -and $errTask.Wait(5000)){[IO.File]::WriteAllText($stderr,$errTask.Result)}
        if($process){$process.Dispose()}
    }
    $results.Add([pscustomobject]@{name=$Name;status=$status;exitCode=$code;seconds=[math]::Round($clock.Elapsed.TotalSeconds,2);error=$problem;stdout=$stdout;stderr=$stderr})
    Write-Output ($status.ToUpperInvariant()+' '+$Name+' '+$problem)
    if($status -ne 'passed'){foreach($path in @($stdout,$stderr)){if(Test-Path -LiteralPath $path){Get-Content -LiteralPath $path -Tail 12}}}
    SaveReport
}
if(-not $SkipBuild){
    Check 'build-native-managed-winui' (Join-Path $PSHOME 'powershell.exe') @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+(Join-Path $PSScriptRoot 'build.ps1')+'"')) 600
    if($results[-1].status -ne 'passed'){throw ('Baseline build/test failure; see '+$report)}
}
# Repeat the actual isolated UI/network processes. No installed process is driven.
$ctest=[regex]::Match([IO.File]::ReadAllText((Join-Path $build 'CMakeCache.txt')),'(?m)^CMAKE_CTEST_COMMAND:INTERNAL=(.+)$').Groups[1].Value.Trim()
if(-not (Test-Path -LiteralPath $ctest)){throw 'Configured CTest executable is missing; run the build first.'}
Check 'repeat-ui-input-lifecycle' $ctest @('--test-dir',('"'+$build+'"'),'-R','"^(clipboard|waiting_screen|shell_video|app_sessions|input_capture_worker|input_routing|host_input|recovery|taskbar_peek|reconnect|session_control)$"','--repeat',('until-fail:'+$Repeat),'--output-on-failure') (120*$Repeat)
if($Hardware){
    # Serial GPU checks avoid creating artificial cross-test encoder contention.
    foreach($mode in @('capture','snapshot','render','throughput','pipeline','auto-quality','motion','lan')){
        Check ('hardware-'+$mode) (Join-Path $build 'HiveDeskCheck.exe') @('--'+$mode) 180
    }
    foreach($probe in @('MediaReady','MediaFreshness','Presentation','TaskbarPresentation')){
        Check ('hardware-'+$probe) (Join-Path $build ('HiveDesk'+$probe+'Check.exe')) @() 90
    }
    Check 'loopback-automatic' (Join-Path $build 'HiveDeskLoopback.exe') @('--background-automatic') 90
    Check 'loopback-delayed-feedback' (Join-Path $build 'HiveDeskLoopback.exe') @('--background-feedback-delay') 90
    Check 'vendor-capabilities' (Join-Path $build 'HiveDeskMediaProbe.exe') @() 45
}
if($InstalledBroker){
    $diagnostics=Join-Path $env:LOCALAPPDATA 'HiveDesk\diagnostics.json'
    $state=Get-Content -LiteralPath $diagnostics -Raw | ConvertFrom-Json
    if($state.status -ne 'idle' -or $state.view -or $state.host){throw 'Installed broker checks require an idle agent; existing sessions were left alone.'}
    # Trusted installed app, read-only capture/activation; no input or clipboard.
    Check 'installed-warm-broker' (Join-Path $env:ProgramFiles 'HiveDesk\HiveDesk.exe') @('--warm-broker-check') 30
    Check 'installed-warm-network' (Join-Path $build 'HiveDeskCheck.exe') @('--warm') 30
}
$failed=@($results | Where-Object status -ne 'passed')
if((git -C $root rev-parse HEAD).Trim() -ne $commit -or (-not $dirty -and (git -C $root status --porcelain))){throw 'Source changed during verification; rerun on the final source.'}
$finished=$true;SaveReport
Write-Output ('Report: '+$report)
Write-Output ('Checks: '+$results.Count+'; passed: '+($results.Count-$failed.Count)+'; failed: '+$failed.Count)
if($failed.Count){exit 1}
