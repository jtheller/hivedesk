param(
    [string]$SourceAddress,
    [string]$ExpectedComputerName,
    [switch]$InventoryOnly
)

# Run on the Intel computer in its logged-in desktop session. Uses installed,
# signed probes; does not change drivers, settings, services or firewall rules.
$ErrorActionPreference = 'Stop'
if ($ExpectedComputerName -and $env:COMPUTERNAME -ne $ExpectedComputerName) {
    throw "Run on $ExpectedComputerName, not $env:COMPUTERNAME."
}
if (!$InventoryOnly) {
    $parsed = $null
    if (![Net.IPAddress]::TryParse($SourceAddress, [ref]$parsed) -or
        $parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'Supply -SourceAddress with the IPv4 address of an idle NVIDIA HiveDesk computer.'
    }
}
$run = Join-Path $env:LOCALAPPDATA ('HiveDesk\intel-tests\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
New-Item -ItemType Directory -Path $run -Force | Out-Null
$install = Join-Path $env:ProgramFiles 'HiveDesk'
$manifest = [ordered]@{
    machine = $env:COMPUTERNAME
    startedUtc = [DateTime]::UtcNow.ToString('o')
    sourceAddress = $SourceAddress
    status = 'running'
    tests = @()
}
function Save-Manifest {
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'manifest.json') -Encoding UTF8
}
function Invoke-Probe([string]$label, [string]$exe, [string[]]$probeArguments, [int]$timeoutSeconds) {
    $file = Join-Path $install $exe
    $record = [ordered]@{ label=$label; arguments=$probeArguments; exitCode=$null; timedOut=$false }
    $started = [DateTime]::UtcNow
    try {
        if (!(Test-Path -LiteralPath $file)) { throw "Missing installed probe: $exe" }
        $record.sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        $options = @{
            FilePath=$file; WorkingDirectory=$install; WindowStyle='Hidden'; PassThru=$true
            RedirectStandardOutput=(Join-Path $run ($label+'.stdout.log'))
            RedirectStandardError=(Join-Path $run ($label+'.stderr.log'))
        }
        if ($probeArguments.Count) { $options.ArgumentList = $probeArguments }
        Write-Host "Running $label ..."
        $process = Start-Process @options
        $nativeHandle = $process.Handle
        $record.timedOut = !$process.WaitForExit($timeoutSeconds * 1000)
        if ($record.timedOut -and !$process.HasExited) { $process.Kill() }
        $process.WaitForExit()
        $record.exitCode = $process.ExitCode
    } catch { $record.error = $_.Exception.Message }
    $record.elapsedSeconds = ([DateTime]::UtcNow - $started).TotalSeconds
    $manifest.tests += $record
    Save-Manifest
    Write-Host "$label : exit=$($record.exitCode), timeout=$($record.timedOut)"
}
try {
    Save-Manifest
    $inventory = [ordered]@{
        machine=$env:COMPUTERNAME
        os=@(Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber)
        cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
        video=@(Get-CimInstance Win32_VideoController | Select-Object Name,PNPDeviceID,DriverVersion,DriverDate,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate,VideoModeDescription)
        hiveDesk=@(Get-Item -LiteralPath (Join-Path $install 'HiveDesk.exe') -ErrorAction SilentlyContinue | ForEach-Object { $_.VersionInfo.FileVersion })
    }
    $inventory | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'hardware.json') -Encoding UTF8
    if (!$InventoryOnly) {
        Write-Host 'Intel encoding/decoding qualification and three read-only receiving sessions.'
        Write-Host 'Small previews may appear. Keep the source computer free of other remote viewers.'
        Write-Host 'No keyboard, mouse or clipboard is forwarded. Static desktops do not measure motion performance.'
        Invoke-Probe 'capabilities' 'HiveDeskMediaProbe.exe' @() 30
        Invoke-Probe 'intel-roundtrip' 'HiveDeskMediaProbe.exe' @('--intel-roundtrip') 60
        foreach ($cycle in 1..3) {
            Invoke-Probe ('receive-'+$cycle) 'HiveDeskRemoteVideoCheck.exe' @($SourceAddress,'20000') 45
        }
    }
    $failures = @($manifest.tests | Where-Object { $_.timedOut -or $null -eq $_.exitCode -or $_.exitCode -ne 0 })
    $manifest.status = if ($InventoryOnly) { 'inventory-only' } elseif ($failures.Count) { 'completed-with-failures' } else { 'completed' }
} catch {
    $manifest.status = 'failed'
    $manifest.error = $_.Exception.Message
} finally {
    # Intentionally excludes configuration/keys, screenshots and clipboard data.
    foreach ($name in @('log.txt','diagnostics.json','diagnostics.jsonl')) {
        $source = Join-Path $env:LOCALAPPDATA ('HiveDesk\'+$name)
        if (Test-Path -LiteralPath $source) {
            try { Copy-Item -LiteralPath $source -Destination (Join-Path $run $name) -ErrorAction Stop }
            catch { Write-Warning "Could not collect $name : $($_.Exception.Message)" }
        }
    }
    $manifest.finishedUtc = [DateTime]::UtcNow.ToString('o')
    Save-Manifest
    $zip = $run + '.zip'
    Compress-Archive -LiteralPath $run -DestinationPath $zip
    Write-Host "Finished: $($manifest.status)"
    Write-Host "Report: $zip"
}
