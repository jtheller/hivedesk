param([Parameter(Mandatory=$true)][string]$ReleaseDirectory)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
$version=[regex]::Match((Get-Content CMakeLists.txt -Raw),'set\(HIVEDESK_VERSION "([^"]+)"\)').Groups[1].Value
$tag='v'+$version
$repository='jtheller/hivedesk'
$commit=(git rev-parse HEAD).Trim()
if($LASTEXITCODE){throw 'Commit the release source before publishing'}
if(git status --porcelain){throw 'Release source must be committed and clean before publishing'}
$built=(Get-Content (Join-Path $ReleaseDirectory 'build-commit.txt') -Raw).Trim()
if($built -ne $commit){throw 'Package was not built from the current commit'}
& .\scripts\sign-release.ps1 -ReleaseDirectory $ReleaseDirectory
if($LASTEXITCODE){throw 'Signing failed'}
$env:GIT_TERMINAL_PROMPT='0';$env:GCM_INTERACTIVE='Never'
$lines=@('protocol=https','host=github.com','') | git credential fill
if($LASTEXITCODE){throw 'GitHub authentication unavailable'}
$credentials=@{};foreach($line in $lines){$parts=$line -split '=',2;if($parts.Length -eq 2){$credentials[$parts[0]]=$parts[1]}}
$headers=@{Authorization=('Bearer '+$credentials.password);Accept='application/vnd.github+json';'X-GitHub-Api-Version'='2022-11-28';'User-Agent'='HiveDesk-release'}
try {
    $account=Invoke-RestMethod https://api.github.com/user -Headers $headers
    if($account.login -ne 'jtheller'){throw 'Release publisher must be jtheller'}
    $repo=Invoke-RestMethod ('https://api.github.com/repos/'+$repository) -Headers $headers
    if($repo.private -or -not $repo.permissions.push){throw 'Public release repository is not writable'}
    # A published tag is immutable from this workflow. Drafts can be resumed
    # after an interrupted upload, but only for the exact original source commit.
    $releases=Invoke-RestMethod ('https://api.github.com/repos/'+$repository+'/releases?per_page=100') -Headers $headers
    $release=$releases | Where-Object tag_name -eq $tag | Select-Object -First 1
    if($release -and -not $release.draft){throw 'This version is already published; increment the version instead of replacing assets'}
    if($release -and $release.target_commitish -ne $commit){throw 'Draft release belongs to another commit'}
    $existingTag=git rev-parse -q --verify ('refs/tags/'+$tag) 2>$null
    if($existingTag){if($existingTag.Trim() -ne $commit){throw 'Release tag points to another commit'}}else{git tag $tag $commit;if($LASTEXITCODE){throw 'Cannot create release tag'}}
    git push --quiet origin HEAD:main
    if($LASTEXITCODE){throw 'Cannot push release source'}
    git push --quiet origin ('refs/tags/'+$tag)
    if($LASTEXITCODE){throw 'Cannot push release tag'}
    if(-not $release){
        $noteFile=Join-Path $projectRoot ('docs\releases\'+$version+'.md')
        $notes=if(Test-Path -LiteralPath $noteFile){[IO.File]::ReadAllText($noteFile)}else{'Windows x64 installer. Automatic updates are applied only while remote sessions are idle.'}
        $body=@{tag_name=$tag;target_commitish=$commit;name=('HiveDesk '+$version);body=$notes;draft=$true;prerelease=$false} | ConvertTo-Json
        $release=Invoke-RestMethod ('https://api.github.com/repos/'+$repository+'/releases') -Method Post -Headers $headers -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($body))
    }
    $names=@(('HiveDesk-'+$version+'-windows-x64-Setup.exe'),'SHA256SUMS.txt','INSTALL.zh-CN.md','update.manifest','update.manifest.sig','build-commit.txt')
    $upload=$release.upload_url -replace '\{.*$',''
    foreach($name in $names){
        $file=Get-Item -LiteralPath (Join-Path $ReleaseDirectory $name)
        $old=$release.assets | Where-Object name -eq $name
        if($old){Invoke-RestMethod $old.url -Method Delete -Headers $headers | Out-Null}
        $asset=Invoke-RestMethod ($upload+'?name='+[Uri]::EscapeDataString($name)) -Method Post -Headers $headers -ContentType 'application/octet-stream' -InFile $file.FullName -TimeoutSec 600
        $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        if($asset.size -ne $file.Length -or ($asset.digest -and $asset.digest -ne ('sha256:'+$hash))){throw ('Uploaded asset verification failed: '+$name)}
    }
    $published=Invoke-RestMethod $release.url -Method Patch -Headers $headers -ContentType 'application/json' -Body '{"draft":false,"prerelease":false,"make_latest":"true"}'
    Write-Output ('Published '+$published.html_url)
} finally {$headers.Clear();$credentials.Clear();$lines=$null}
