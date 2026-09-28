[CmdletBinding()]
param(
    [switch]$Deploy,
    [switch]$ContinueSession,
    [string]$Scenario = 'helgen-intro',
    [int]$TimeoutMinutes = 35,
    [string]$Worktree = 'C:\Users\mwalt\SkyrimSeamlessCoop-ui',
    [string]$CaptureRoot = 'C:\Users\mwalt\.claude\jobs\77da3be6\tmp\round6\capture'
)
$ErrorActionPreference = 'Stop'
if ($ContinueSession -and $Deploy) { throw 'Diagnostic continuation cannot deploy or restart the games' }
if ($Scenario -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Invalid scenario name' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$began = Get-Date
$out = Join-Path $CaptureRoot "harness-$stamp"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$gameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
$install = Join-Path $gameRoot 'Data\SkyrimTogetherReborn'
$bridge = 'C:\Users\mwalt\SkyrimSeamlessCoop\Tools\InGameTests\Invoke-TwoPcTestBridge.ps1'
$access = 'C:\Users\mwalt\SkyrimSeamlessCoop\runtime\remote-access'
$sshArgs = @('-i', "$access\eriana_deploy_ed25519", '-o', "UserKnownHostsFile=$access\known_hosts", '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')
$script:sequence = 0
$result = 'failed'
$reason = ''
$last = @{}
$script:processIds = @{}
$script:diagPaths = @{}
$script:exitedPcs = @{}

function Arm-Diagnostics($Pc) {
    $command = @'
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath('C:\Tools\diag\sessions')
$game = Get-Process SkyrimTogether -ErrorAction SilentlyContinue | Select-Object -First 1
if ($game) {
    # End only the bounded diagnostic supervisors before detaching their debugger.
    Get-ChildItem -LiteralPath $root -Directory | ForEach-Object {
        'stop' | Set-Content -LiteralPath (Join-Path $_.FullName 'monitor.stop')
    }
    Start-Sleep -Seconds 1
    & C:/Tools/diag/procdump/procdump64.exe -cancel $game.Id *> $null
    Start-Sleep -Seconds 2
    if (Get-Process procdump64 -ErrorAction SilentlyContinue) { throw 'Prior debugger did not detach; refusing forceful disarm' }
}
Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    if (-not ([IO.Path]::GetFullPath($_.FullName).StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase))) { throw 'Diagnostic cleanup path escaped sessions root' }
    if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Diagnostic session is a reparse point' }
}
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Tools\diag\diag-disarm.ps1
$armLog = Join-Path $root ('arm-' + [guid]::NewGuid().ToString('N'))
# Redirect both streams: ProcDump inherits stderr, which otherwise holds the
# invoking PowerShell/SSH pipe open after diag-arm itself has already exited.
$arm = Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','C:\Tools\diag\diag-arm.ps1') -RedirectStandardOutput ($armLog + '.out') -RedirectStandardError ($armLog + '.err')
if (-not $arm.WaitForExit(30000)) { throw 'Diagnostic arm script timed out' }
if ($null -ne $arm.ExitCode -and $arm.ExitCode -ne 0) { throw 'Diagnostic arm script failed' }
Get-Content -LiteralPath ($armLog + '.out')
Start-Sleep -Seconds 1
if (-not (Get-Process procdump64 -ErrorAction SilentlyContinue) -or -not (Get-Process PresentMon -ErrorAction SilentlyContinue)) { throw 'Diagnostic helper exited during arm' }
$armed = Get-Content -LiteralPath ($armLog + '.out') | Where-Object { $_ -match '^armed: ' } | Select-Object -Last 1
if ($armed) {
    $sessionPath = $armed.Substring(7).Trim()
    Start-Process powershell.exe -WindowStyle Hidden -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File','C:\Tools\diag\harness-watch.ps1','-SessionPath',$sessionPath) -RedirectStandardOutput (Join-Path $sessionPath 'watch.out') -RedirectStandardError (Join-Path $sessionPath 'watch.err')
}
'@
    $watcher = Join-Path $Worktree 'Tools\InGameTests\Watch-HarnessDump.ps1'
    if ($Pc -eq 'Host') { Copy-Item -LiteralPath $watcher -Destination C:/Tools/diag/harness-watch.ps1 -Force }
    else {
        & scp.exe @sshArgs $watcher 'eflem@192.168.50.103:C:/Tools/diag/harness-watch.ps1'
        if ($LASTEXITCODE -ne 0) { throw 'Follower diagnostic watcher copy failed' }
    }
    if ($Pc -eq 'Host') { $reply = & ([scriptblock]::Create($command)) }
    else {
        # SSH's job tears down child diagnostics when the remote command ends.
        # Use the same interactive account as the existing game launch task.
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
        $schedule = @'
$ErrorActionPreference='Stop'
$code=[Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('__CODE__'))
$scriptPath='C:\Tools\diag\harness-arm-__STAMP__.ps1'
$resultPath='C:\Tools\diag\sessions\arm-__STAMP__.result'
$wrapper="try { `$reply = & {`n" + $code + "`n}; `$reply | Set-Content -LiteralPath '$resultPath.tmp'; Move-Item -LiteralPath '$resultPath.tmp' -Destination '$resultPath' -Force; while (Get-Process procdump64,PresentMon -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 5 } } catch { `$_ | Out-String | Set-Content -LiteralPath '$resultPath.tmp'; Move-Item -LiteralPath '$resultPath.tmp' -Destination '$resultPath' -Force }"
Set-Content -LiteralPath $scriptPath -Value $wrapper -Encoding UTF8
$taskName='SkyrimSeamlessCoop-HarnessDiagnostics'
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) { Stop-ScheduledTask -TaskName $taskName }
$account=(Get-ScheduledTask -TaskName SkyrimSeamlessCoop-InteractiveLaunch).Principal.UserId
$principal=New-ScheduledTaskPrincipal -UserId $account -LogonType Interactive -RunLevel Highest
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $scriptPath + '"')
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 2) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
$deadline=(Get-Date).AddSeconds(45)
while (-not (Test-Path -LiteralPath $resultPath) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
Get-Content -LiteralPath $resultPath
'@
        $localSchedule = Join-Path $out 'arm-follower.ps1'
        Set-Content -LiteralPath $localSchedule -Value $schedule.Replace('__CODE__',$encoded).Replace('__STAMP__',$stamp) -Encoding UTF8
        & scp.exe @sshArgs $localSchedule 'eflem@192.168.50.103:C:/Tools/diag/harness-schedule.ps1'
        if ($LASTEXITCODE -ne 0) { throw 'Follower diagnostic task script copy failed' }
        $reply = & C:/Tools/skyrim_re/follower.ps1 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:/Tools/diag/harness-schedule.ps1'
    }
    $line = @($reply | Where-Object { $_ -match '^armed: ' }) | Select-Object -Last 1
    if (-not $line) { throw "$Pc diagnostics did not arm: $reply" }
    $script:diagPaths[$Pc] = $line.Substring(7).Trim()
    $script:diagPaths | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'diagnostics.json')
}
function Collect-Diagnostics {
    $failures = @()
    foreach ($pc in 'Host','Follower') {
        if (-not $script:diagPaths[$pc]) { continue }
        try {
        $path = $script:diagPaths[$pc]
        # ProcDump keeps an in-progress dump open. Do not kill its writer or copy
        # a partial full-memory dump. Bound collection independently of gameplay.
        $check = @'
$dumps = @(Get-ChildItem -LiteralPath '__PATH__' -Filter '*.dmp' -File -Recurse)
$ready = $true
if (__PID__ -gt 0 -and -not $dumps.Count -and -not (Get-Process -Id __PID__ -ErrorAction SilentlyContinue) -and
    (Get-Process procdump64 -ErrorAction SilentlyContinue)) { $ready = $false }
foreach ($dump in $dumps) {
    try { $f = [IO.File]::Open($dump.FullName,'Open','Read','None'); $f.Dispose() }
    catch { $ready = $false }
}
if ($ready -and $dumps.Count) {
    $log = (Get-ChildItem -LiteralPath '__PATH__' -Filter 'procdump.log' -File -Recurse | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw -Encoding Unicode }) -join "`n"
    $ready = ([regex]::Matches($log, 'Dump \d+ complete').Count -ge $dumps.Count)
}
$ready
'@
        $check = $check.Replace('__PATH__',$path).Replace('__PID__',[string]$(if ($script:processIds[$pc]) { $script:processIds[$pc] } else { -1 }))
        Wait-Condition {
            $answer = if ($pc -eq 'Host') { & ([scriptblock]::Create($check)) } else { & C:/Tools/skyrim_re/follower.ps1 $check }
            (($answer -join '').Trim() -eq 'True')
        } 1200 "$pc full dump did not finish; diagnostics left armed"
        $cancel = @'
'stop' | Set-Content -LiteralPath '__PATH__\monitor.stop'
$game=Get-Process SkyrimTogether -ErrorAction SilentlyContinue | Select-Object -First 1
@{gameAlive=[bool]$game;dumpMonitorAlive=[bool](Get-Process procdump64 -ErrorAction SilentlyContinue);frameMonitorAlive=[bool](Get-Process PresentMon -ErrorAction SilentlyContinue)} | ConvertTo-Json | Set-Content -LiteralPath '__PATH__\coverage-at-stop.json'
if ($game) {
    & C:/Tools/diag/procdump/procdump64.exe -cancel $game.Id *> $null
    Start-Sleep -Seconds 2
} else { Get-Process procdump64 -ErrorAction SilentlyContinue | Stop-Process -Force }
Get-Process PresentMon -ErrorAction SilentlyContinue | Stop-Process -Force
'@
        $cancel = $cancel.Replace('__PATH__',$path)
        if ($pc -eq 'Host') { & ([scriptblock]::Create($cancel)) }
        else { & C:/Tools/skyrim_re/follower.ps1 $cancel | Out-Null }
        $dest = Join-Path $out "$pc\Diagnostics"
        New-Item -ItemType Directory -Path $dest -Force | Out-Null
        if ($pc -eq 'Host') { Get-ChildItem -LiteralPath $path | Copy-Item -Destination $dest -Recurse -Force }
        else {
            $remotePath = $path.Replace('\','/')
            & scp.exe @sshArgs -r "eflem@192.168.50.103:$remotePath/." $dest
            if ($LASTEXITCODE -ne 0) { throw 'Follower diagnostics copy failed' }
        }
        } catch { $failures += "$pc diagnostics: $_" }
    }
    if ($failures.Count) { throw ($failures -join '; ') }
}

function Send($Pc, $Request) {
    $Request.id = ++$script:sequence
    $response = (& $bridge -Target $Pc -RequestJson ($Request | ConvertTo-Json -Compress)).response
    [pscustomobject]@{time=(Get-Date).ToString('o');pc=$Pc;request=$Request;response=$response} |
        ConvertTo-Json -Compress -Depth 12 | Add-Content -LiteralPath (Join-Path $out 'control.jsonl') -Encoding UTF8
    if (-not $response.ok) { throw "$Pc $($Request.command): $($response.error)" }
    return $response
}
function Wait-Condition([scriptblock]$Condition, [int]$Seconds, [string]$Failure) {
    $until = (Get-Date).AddSeconds($Seconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $until)
    throw $Failure
}
function Get-DumpIdentity($Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt32() -ne 0x504d444d) { return $null }
        $null = $reader.ReadUInt32()
        $count = $reader.ReadUInt32(); $directory = $reader.ReadUInt32()
        $stream.Position = 24; $fullMemory = [bool]($reader.ReadUInt64() -band 2)
        $dumpPid = 0; $exception = $false; $exceptionCode = 0
        for ($i=0; $i -lt $count -and $i -lt 128; $i++) {
            $stream.Position = $directory + 12*$i
            $kind = $reader.ReadUInt32(); $size = $reader.ReadUInt32(); $offset = $reader.ReadUInt32()
            if ($kind -eq 6 -and $size -ge 12) {
                $exception = $true; $stream.Position = $offset + 8; $exceptionCode = $reader.ReadUInt32()
            }
            if ($kind -eq 15 -and $size -ge 12) {
                $stream.Position = $offset + 4
                $flags = $reader.ReadUInt32(); $candidate = $reader.ReadUInt32()
                if ($flags -band 1) { $dumpPid = $candidate }
            }
        }
        return @{processId=$dumpPid;exception=$exception;exceptionCode=$exceptionCode;fullMemory=$fullMemory}
    } finally { $reader.Dispose(); $stream.Dispose() }
}
function Read-HarnessStatus($Pc) {
    $path = Join-Path $install "logs\harness-$stamp.status.jsonl"
    $read = @'
if (Test-Path -LiteralPath '__PATH__') {
    $file = [System.IO.File]::Open('__PATH__',[System.IO.FileMode]::Open,[System.IO.FileAccess]::Read,([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
    $null = $file.Seek([math]::Max(0,$file.Length-8192),[System.IO.SeekOrigin]::Begin)
    $reader = New-Object System.IO.StreamReader($file)
    try {
        $text = $reader.ReadToEnd()
        $end = $text.LastIndexOf("`n")
        if ($end -ge 0) { ($text.Substring(0,$end).TrimEnd("`r") -split "`n")[-1].Trim() }
    } finally { $reader.Dispose(); $file.Dispose() }
}

'@
    $read = $read.Replace('__PATH__',$path)
    if ($Pc -eq 'Host') {
        $json = & ([scriptblock]::Create($read))
        if ($json) { return $json | ConvertFrom-Json }
    } else {
        $json = (& C:/Tools/skyrim_re/follower.ps1 $read) -join ''
        if ($json) { return $json | ConvertFrom-Json }
    }
    return $null
}
function Assert-SessionHealth {
    $serverLog = Join-Path $gameRoot 'logs\STServerOut.log'
    try {
        $server = @(Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue)
        if ($server.Count -ne 1) { throw 'session_health_server_pid' }
        $ports = @(Get-NetUDPEndpoint -OwningProcess $server[0].Id -ErrorAction Stop)
        if (-not ($ports | Where-Object LocalPort -eq 10578)) { throw 'session_health_server_port_10578' }
        $serverStart = $server[0].StartTime.ToString('yyyy-MM-dd HH:mm:ss.fff')
        $tagLine = Select-String -LiteralPath $serverLog -Pattern 'Harness server buildTag=' | ForEach-Object Line | Where-Object {
            $_ -match '^\[([^]]+)\].*Harness server buildTag=(.+)$' -and $Matches[1] -ge $serverStart
        } | Select-Object -Last 1
        if (-not $tagLine -or $tagLine -notmatch 'buildTag=(.+)$') { throw 'session_health_server_build_tag_missing' }
        $serverTag = $Matches[1].Trim()
        $evidence = @{serverPid=$server[0].Id;serverStart=$serverStart;ports=@($ports.LocalPort);serverBuildTag=$serverTag;clients=@{}}
        foreach ($pc in 'Host','Follower') {
            $status = (Send $pc @{command='harness_status'}).harness
            if (-not $status.buildTag -or $status.buildTag -cne $serverTag) { throw "session_health_build_mismatch: $pc client=$($status.buildTag) server=$serverTag" }
            $read = @'
$ErrorActionPreference='Stop'
$game=Get-Process SkyrimTogether | Select-Object -First 1
$start=$game.StartTime.ToString('yyyy-MM-dd HH:mm:ss.fff')
$root='C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\SkyrimTogetherReborn\logs'
$found=@(Get-ChildItem -LiteralPath $root -Filter 'tp_client*.log' | Where-Object { $_.Name -match '^tp_client(?:\.[1-3])?\.log$' } | ForEach-Object {
    Select-String -LiteralPath $_.FullName -Pattern 'Effective Data scan complete|Joined shared campaign' | ForEach-Object Line | Where-Object {
        $_ -match '^\[([^]]+)\].*(Effective Data scan complete|Joined shared campaign)' -and $Matches[1] -ge $start
    }
})
@{pid=$game.Id;start=$start;scan=[bool]($found -match 'Effective Data scan complete');joined=[bool]($found -match 'Joined shared campaign');lines=$found} | ConvertTo-Json -Compress
'@
            $raw = if ($pc -eq 'Host') { & ([scriptblock]::Create($read)) } else { & C:/Tools/skyrim_re/follower.ps1 $read }
            $client = ($raw -join '') | ConvertFrom-Json
            if (-not $client.scan -or -not $client.joined) { throw "session_health_fresh_scan_join_missing: $pc" }
            $evidence.clients[$pc] = @{buildTag=$status.buildTag;logs=$client}
        }
        $evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'session-health.json')
    } catch {
        $tail = (Get-Content -LiteralPath $serverLog -Tail 60 -ErrorAction SilentlyContinue) -join "`n"
        $tail | Set-Content -LiteralPath (Join-Path $out 'session-health-server-tail.txt')
        throw "$_`nServer log tail:`n$tail"
    }
}
function Assert-ArtifactIdentity {
    # Tags omit dirty source changes. Compare each artifact against one captured
    # build bundle and verify the process loaded it from the installation.
    $files = @('SkyrimTogether.exe','STServer.dll','SkyrimTogetherServer.exe','TPProcess.exe')
    $expected = @{}
    foreach ($name in $files) {
        $expected[$name] = (Get-FileHash -LiteralPath (Join-Path $Worktree "build\windows\x64\releasedbg\$name") -Algorithm SHA256).Hash
    }
    $read = @'
$ErrorActionPreference='Stop'
$root='C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\SkyrimTogetherReborn'
$game=@(Get-Process SkyrimTogether)
if ($game.Count -ne 1) { throw 'artifact_identity_game_pid' }
$hashes=@{}
foreach ($name in 'SkyrimTogether.exe','STServer.dll','SkyrimTogetherServer.exe','TPProcess.exe') {
    $path=Join-Path $root $name
    if ((Get-Item -LiteralPath $path).LastWriteTimeUtc -gt $game[0].StartTime.ToUniversalTime()) { throw "artifact_changed_after_launch: $name" }
    $hashes[$name]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}
$gameModule=@($game[0].Modules | Where-Object { $_.FileName -ieq (Join-Path $root 'SkyrimTogether.exe') })
if ($gameModule.Count -ne 1) { throw 'artifact_identity_client_module_path' }
$server=Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue
if ('__PC__' -eq 'Host') {
    if (@($server).Count -ne 1) { throw 'artifact_identity_server_pid' }
    $module=@($server.Modules | Where-Object { $_.FileName -ieq (Join-Path $root 'STServer.dll') })
    if ($module.Count -ne 1) { throw 'artifact_identity_server_module_path' }
    if ((Get-Item -LiteralPath (Join-Path $root 'STServer.dll')).LastWriteTimeUtc -gt $server.StartTime.ToUniversalTime()) { throw 'server_artifact_changed_after_launch' }
}
@{hashes=$hashes;gamePid=$game[0].Id;gameStart=$game[0].StartTime.ToUniversalTime().ToString('o');serverPid=$(if($server){$server.Id}else{0})} | ConvertTo-Json -Compress
'@
    $bundle = @{expected=$expected;clients=@{}}
    foreach ($pc in 'Host','Follower') {
        $command = $read.Replace('__PC__',$pc)
        $raw = if ($pc -eq 'Host') { & ([scriptblock]::Create($command)) } else { & C:/Tools/skyrim_re/follower.ps1 $command }
        $observed = ($raw -join '') | ConvertFrom-Json
        if ($observed.gamePid -ne $script:processIds[$pc]) { throw "artifact_identity_pid_changed: $pc" }
        foreach ($name in $files) {
            if ($observed.hashes.$name -cne $expected[$name]) { throw "artifact_identity_hash_mismatch: $pc $name" }
        }
        $bundle.clients[$pc] = $observed
    }
    $bundle | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'artifact-identity.json') -Encoding UTF8
}
function Collect {
    $hostFolder = Join-Path $out 'Host'
    $followerFolder = Join-Path $out 'Follower'
    New-Item -ItemType Directory -Path $hostFolder,$followerFolder -Force | Out-Null
    foreach ($pattern in @("harness-$stamp.jsonl","harness-$stamp.status.jsonl",'tp_client*.log')) {
        Get-ChildItem -LiteralPath (Join-Path $install 'logs') -Filter $pattern -ErrorAction SilentlyContinue | Copy-Item -Destination $hostFolder -Force
    }
    Get-ChildItem -LiteralPath (Join-Path $gameRoot 'logs') -Filter 'STServer*.log' -ErrorAction SilentlyContinue | Copy-Item -Destination $hostFolder -Force
    foreach ($root in @($gameRoot,$install,"$env:LOCALAPPDATA\CrashDumps","$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\SKSE")) {
        if (Test-Path -LiteralPath $root) {
            Get-ChildItem -LiteralPath $root -File | Where-Object { ($_.Extension -eq '.dmp' -or $_.Name -like 'crash-*.log') -and $_.LastWriteTime -ge $began } | Copy-Item -Destination $hostFolder -Force
        }
    }
    $remoteStage = "C:/Users/eflem/AppData/Local/Temp/harness-$stamp"
    $remote = @'
$ErrorActionPreference = 'Stop'
$install = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\SkyrimTogetherReborn'
$dest = '__STAGE__'
New-Item -ItemType Directory -Path $dest -Force | Out-Null
foreach ($pattern in @('harness-__STAMP__.jsonl','harness-__STAMP__.status.jsonl','tp_client*.log')) {
    Get-ChildItem -LiteralPath (Join-Path $install 'logs') -Filter $pattern -ErrorAction SilentlyContinue | Copy-Item -Destination $dest -Force
}
foreach ($root in @((Split-Path (Split-Path $install -Parent) -Parent),$install,"$env:LOCALAPPDATA\CrashDumps","$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\SKSE")) {
    if (Test-Path -LiteralPath $root) {
        Get-ChildItem -LiteralPath $root -File | Where-Object { ($_.Extension -eq '.dmp' -or $_.Name -like 'crash-*.log') -and $_.LastWriteTimeUtc -ge ([datetime]'__BEGAN__').ToUniversalTime() } | Copy-Item -Destination $dest -Force
    }
}
Get-ChildItem -LiteralPath $dest -File | Select-Object Name,Length,LastWriteTimeUtc | ConvertTo-Json -Compress
'@
    $remote = $remote.Replace('__STAGE__',$remoteStage).Replace('__STAMP__',$stamp).Replace('__BEGAN__',$began.ToUniversalTime().ToString('o'))
    & C:/Tools/skyrim_re/follower.ps1 $remote | Set-Content -LiteralPath (Join-Path $out 'follower-manifest.json')
    & scp.exe @sshArgs -r "eflem@192.168.50.103:$remoteStage/." $followerFolder
    if ($LASTEXITCODE -ne 0) { throw 'Follower evidence copy failed' }
    foreach ($pc in 'Host','Follower') {
        $clientLogs = @(Get-ChildItem -LiteralPath (Join-Path $out $pc) -Filter 'tp_client*.log' -File | Where-Object Length -gt 0)
        if (-not $clientLogs.Count) { throw "collection_missing_client_log: $pc" }
    }
    if ((Get-Item -LiteralPath (Join-Path $hostFolder 'STServerOut.log') -ErrorAction Stop).Length -eq 0) { throw 'collection_empty_server_log' }
}

Start-Transcript -Path (Join-Path $out 'runner.txt') | Out-Null
try {
    # Files are prepared before launch. The game owns all scenario progression.
    $scenarioPath = Join-Path $Worktree "Tools\InGameTests\scenarios\$Scenario.json"
    if (-not (Test-Path -LiteralPath $scenarioPath)) { throw "Missing scenario $scenarioPath" }
    $scenarioData = Get-Content -LiteralPath $scenarioPath -Raw | ConvertFrom-Json
    $metric = $scenarioData.metric
    if ($metric -notin 'collision-on','tcl-assisted') { throw 'Scenario must declare a pass metric' }
    if ($metric -eq 'collision-on' -and @($scenarioData.steps | Where-Object { $_.cmd -eq 'tcl' -or $_.fallback }).Count) { throw 'Collision-on scenario contains a TCL command or fallback' }
    Copy-Item -LiteralPath $scenarioPath -Destination (Join-Path $out 'scenario.json')
    $prepare = @'
$ErrorActionPreference = 'Stop'
$root = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
$install = Join-Path $root 'Data\SkyrimTogetherReborn'
Get-Process SkyrimTogether -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
if (Get-Process SkyrimTogether -ErrorAction SilentlyContinue) { throw 'Game did not stop' }
Get-Process SkyrimTogetherServer,TPProcess -ErrorAction SilentlyContinue | Stop-Process -Force
'{"enabled":true,"armor":true}' | Set-Content -LiteralPath (Join-Path $install 'harness.json') -Encoding ASCII
'1' | Set-Content -LiteralPath (Join-Path $install 'harness-armor.enabled') -Encoding ASCII
$ini = Join-Path $env:USERPROFILE 'Documents\My Games\Skyrim Special Edition\Skyrim.ini'
if (Test-Path -LiteralPath $ini) {
    Copy-Item -LiteralPath $ini -Destination ($ini + '.pre-harness-__STAMP__')
    $text = Get-Content -LiteralPath $ini -Raw
    if ($text -match '(?im)^bAlwaysActive\s*=') { $text = [regex]::Replace($text,'(?im)^bAlwaysActive\s*=.*$','bAlwaysActive=1') }
    elseif ($text -match '(?im)^\[General\]') { $text = [regex]::Replace($text,'(?im)^\[General\]','[General]' + "`r`nbAlwaysActive=1") }
    else { $text += "`r`n[General]`r`nbAlwaysActive=1`r`n" }
    Set-Content -LiteralPath $ini -Value $text -Encoding Unicode
}
$serverIni = Join-Path $root 'config\STServer.ini'
if (Test-Path -LiteralPath $serverIni) {
    Copy-Item -LiteralPath $serverIni -Destination ($serverIni + '.pre-harness-__STAMP__')
    $text = Get-Content -LiteralPath $serverIni -Raw
    if ($text -match '(?im)^\[Harness\]') { $text = [regex]::Replace($text,'(?im)(^\[Harness\][\s\S]*?)(?=^\[|\z)',"[Harness]`r`nbEnabled=true`r`n") }
    else { $text += "`r`n[Harness]`r`nbEnabled=true`r`n" }
    Set-Content -LiteralPath $serverIni -Value $text -Encoding UTF8
}
'@
    $prepare = $prepare.Replace('__STAMP__',$stamp)
    if (-not $ContinueSession) {
        & ([scriptblock]::Create($prepare))
        & C:/Tools/skyrim_re/follower.ps1 $prepare | Out-Null
    }
    Copy-Item -LiteralPath $scenarioPath -Destination (Join-Path $install "$Scenario.json") -Force
    & scp.exe @sshArgs $scenarioPath "eflem@192.168.50.103:C:/Users/eflem/AppData/Local/Temp/harness-scenario.json"
    if ($LASTEXITCODE -ne 0) { throw 'Follower scenario copy failed' }
    & C:/Tools/skyrim_re/follower.ps1 ("Copy-Item -LiteralPath C:/Users/eflem/AppData/Local/Temp/harness-scenario.json -Destination '" + (Join-Path $install "$Scenario.json") + "' -Force") | Out-Null
    Arm-Diagnostics Host
    Arm-Diagnostics Follower
    if ($Deploy) { & C:/Tools/skyrim_re/Deploy-Worktree.ps1 -Worktree $Worktree -Tag harness -Launch }
    elseif (-not $ContinueSession) {
        & C:/Tools/skyrim_re/follower.ps1 "Start-ScheduledTask -TaskName 'SkyrimSeamlessCoop-InteractiveLaunch'" | Out-Null
        Start-Process -FilePath (Join-Path $install 'SkyrimTogether.exe') -WorkingDirectory $install
    }
    Wait-Condition {
        $hostGame = Get-Process SkyrimTogether -ErrorAction SilentlyContinue | Select-Object -First 1
        $followerGameId = ((& C:/Tools/skyrim_re/follower.ps1 '(Get-Process SkyrimTogether -ErrorAction SilentlyContinue | Select-Object -First 1).Id') -join '').Trim()
        if ($hostGame) { $script:processIds.Host = $hostGame.Id }
        if ($followerGameId -match '^\d+$') { $script:processIds.Follower = [int]$followerGameId }
        $script:processIds.Host -and $script:processIds.Follower
    } 90 'Both game processes did not launch'
    $script:processIds | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'processes.json')
    Wait-Condition {
        $hostAlive = [bool](Get-Process SkyrimTogether -ErrorAction SilentlyContinue)
        $followerAlive = ((& C:/Tools/skyrim_re/follower.ps1 '[bool](Get-Process SkyrimTogether -ErrorAction SilentlyContinue)') -join '') -eq 'True'
        if (-not $hostAlive -or -not $followerAlive) { $script:result='process-exit'; $script:exitedPcs=@{Host=(-not $hostAlive);Follower=(-not $followerAlive)}; throw "Startup game exit: Host=$hostAlive Follower=$followerAlive" }
        try { (Send Host @{command='ping'}).ok -and (Send Follower @{command='ping'}).ok } catch { $false }
    } 120 'Both game bridges did not start'

    if ($ContinueSession) {
        $h = Send Host @{command='party_state'}; $f = Send Follower @{command='party_state'}
        if (-not (Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue) -or
            -not $h.online -or -not $f.online -or -not $h.leader -or $h.memberCount -lt 2 -or
            $f.memberCount -ne $h.memberCount -or -not $h.startEpoch -or $h.startEpoch -ne $f.startEpoch) {
            throw 'Diagnostic continuation requires the existing healthy party and campaign epoch'
        }
        foreach ($pc in 'Host','Follower') {
            $harness = (Send $pc @{command='harness_status'}).harness
            if (-not $harness.enabled -or $harness.active) { throw "$pc harness must be enabled and inactive for diagnostic continuation" }
        }
    } else {
    $formed = $false
    for ($attempt=1; $attempt -le 3 -and -not $formed; $attempt++) {
        try {
            Send Host @{command='open_coop'} | Out-Null
            Send Host @{command='steam_host'} | Out-Null
            Wait-Condition {
                $h = Send Host @{command='party_state'}
                $server = Get-Process SkyrimTogetherServer -ErrorAction SilentlyContinue
                $h.inParty -and $h.leader -and $h.online -and $server
            } 90 'Host server/authenticated party health check failed'
            $lobby = (Send Host @{command='steam_state'}).steam.lobbyId
            Send Host @{command='steam_invite';steamId='76561198801933478'} | Out-Null
            Wait-Condition {
                $f = Send Follower @{command='steam_state'}
                [bool]($f.steam.invites | Where-Object lobby -eq $lobby)
            } 45 'Follower invite not received'
            Send Follower @{command='steam_answer_invite';lobby=[string]$lobby;accept='true'} | Out-Null
            Wait-Condition {
                $h = Send Host @{command='party_state'}; $f = Send Follower @{command='party_state'}
                $h.online -and $f.online -and $h.memberCount -ge 2 -and $f.memberCount -eq $h.memberCount
            } 90 'Party did not converge'
            $formed = $true
        } catch {
            "Session attempt $attempt failed: $_" | Add-Content -LiteralPath (Join-Path $out 'session-health.txt')
            foreach ($pc in 'Host','Follower') { try { Send $pc @{command='steam_leave'} | Out-Null } catch {} }
            Start-Sleep -Seconds 3
        }
    }
    if (-not $formed) { throw 'Session health failed after three attempts; scenario was not started' }
    foreach ($pc in 'Host','Follower') {
        $harness = (Send $pc @{command='harness_status'}).harness
        if (-not $harness.enabled) { throw "$pc requires a --harness=y build and enabled harness.json" }
        Send $pc @{command='set_ready';ready='true'} | Out-Null
    }
    Wait-Condition { $h=Send Host @{command='party_state'}; $h.readyCount -eq $h.memberCount } 30 'Ready barrier failed'
    Send Host @{command='start_new_campaign'} | Out-Null
    Wait-Condition { (Send Host @{command='party_state'}).startEpoch -gt 0 } 90 'Campaign epoch did not start'
    }
    try {
        Assert-SessionHealth
        Assert-ArtifactIdentity
    } catch {
        Get-Content -LiteralPath (Join-Path $gameRoot 'logs\STServerOut.log') -Tail 60 -ErrorAction SilentlyContinue |
            Set-Content -LiteralPath (Join-Path $out 'session-health-server-tail.txt')
        throw
    }
    Send Host @{command='harness_start';scenario=$Scenario;stamp=$stamp} | Out-Null
    $until = (Get-Date).AddMinutes($TimeoutMinutes)
    while ((Get-Date) -lt $until) {
        $hostAlive = [bool](Get-Process SkyrimTogether -ErrorAction SilentlyContinue)
        $followerAlive = ((& C:/Tools/skyrim_re/follower.ps1 '[bool](Get-Process SkyrimTogether -ErrorAction SilentlyContinue)') -join '') -eq 'True'
        if (-not $hostAlive -or -not $followerAlive) { $result='process-exit'; $script:exitedPcs=@{Host=(-not $hostAlive);Follower=(-not $followerAlive)}; throw "Game exited: Host=$hostAlive Follower=$followerAlive" }
        # Read the writer's last complete status line (at most8192 bytes). A busy game window/bridge must
        # not abort an otherwise progressing native scenario.
        foreach ($pc in 'Host','Follower') {
            try { $status = Read-HarnessStatus $pc; if ($status) { $last[$pc] = $status } }
            catch { "Transient status read: $pc $_" | Add-Content -LiteralPath (Join-Path $out 'status-read-errors.txt') }
        }
        if ($last.Host.state -eq 'failed' -or $last.Follower.state -eq 'failed') { throw ($last | ConvertTo-Json -Compress -Depth 8) }
        if ($last.Host.state -eq 'passed' -and $last.Follower.state -eq 'passed' -and $last.Host.stamp -eq $stamp -and $last.Follower.stamp -eq $stamp) { $result='passed'; break }
        Start-Sleep -Seconds 3
    }
    if ($result -ne 'passed') { throw 'Harness run deadline exceeded' }
} catch {
    $reason = $_.ToString()
    Write-Output "STOP: $reason"
} finally {
    if ($result -ne 'passed') { foreach ($pc in 'Host','Follower') { if ($script:processIds[$pc]) { try { Send $pc @{command='harness_stop'} | Out-Null } catch {} } } }
    Start-Sleep -Seconds 2
    $collectionOk = $true
    try { Collect-Diagnostics } catch { $collectionOk = $false; $reason += "; diagnostics: $_" }
    try { Collect } catch { $collectionOk = $false; $reason += "; collection: $_" }
    if ($result -eq 'passed') {
        $last | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $out 'accepted-status.json') -Encoding UTF8
        try {
            & C:/Users/mwalt/AppData/Local/Programs/Python/Python312/python.exe (Join-Path $Worktree 'Tools\InGameTests\Analyze-Harness.py') --validate $out $stamp
            if ($LASTEXITCODE -ne 0) { throw 'strict_capture_validation_failed (see evidence-validation.json)' }
        } catch { $collectionOk = $false; $reason += "; validation: $_" }
        foreach ($pc in 'Host','Follower') {
            try {
                foreach ($name in @("harness-$stamp.jsonl","harness-$stamp.status.jsonl")) {
                    if ((Get-Item -LiteralPath (Join-Path $out "$pc\$name")).Length -eq 0) { throw "empty $name" }
                }
                $diag = Join-Path $out "$pc\Diagnostics"
                $coverage = Get-Content -LiteralPath (Join-Path $diag 'coverage-at-stop.json') -Raw | ConvertFrom-Json
                if (-not $coverage.dumpMonitorAlive -or -not $coverage.frameMonitorAlive -or (Test-Path (Join-Path $diag 'monitor.failed'))) { throw 'diagnostic monitor coverage failed' }
                $reader = [IO.File]::OpenText((Join-Path $diag 'frames.csv'))
                try { $null=$reader.ReadLine(); if (-not $reader.ReadLine()) { throw 'PresentMon CSV has no frame rows' } } finally { $reader.Dispose() }
            } catch { $collectionOk = $false; $reason += "; $pc evidence: $_" }
        }
    }
    if (-not $collectionOk -and $result -eq 'passed') { $result = 'evidence-incomplete' }
    $dumps = @(Get-ChildItem -LiteralPath $out -Recurse -Filter '*.dmp' -File)
    $verified = @()
    foreach ($dump in $dumps) {
        try {
            $identity = Get-DumpIdentity $dump.FullName
            $pc = if ($dump.FullName.StartsWith((Join-Path $out 'Host') + '\')) { 'Host' } else { 'Follower' }
            if ($script:exitedPcs[$pc] -and $identity.exception -and $identity.exceptionCode -ge 0x80000000L -and $identity.fullMemory -and $script:processIds[$pc] -eq $identity.processId) {
                $crashLog = Join-Path $dump.Directory.FullName 'procdump.log'
                $code = '{0:X8}' -f $identity.exceptionCode
                if ((Test-Path -LiteralPath $crashLog) -and (Get-Content -LiteralPath $crashLog -Raw -Encoding Unicode) -match ('Unhandled: ' + $code)) { $verified += $dump.FullName }
            }
        } catch { $reason += "; could not validate dump $($dump.Name): $_" }
    }
    if ($result -eq 'process-exit' -and $verified.Count -and $collectionOk) { $result = 'crash-captured' }
    if (($ContinueSession -or $metric -eq 'tcl-assisted') -and $result -eq 'passed') { $result = 'diagnostic-passed' }
    $elapsed = ((Get-Date)-$began).TotalSeconds
    @{result=$result;metric=$metric;collisionOnPass=($result -eq 'passed' -and $metric -eq 'collision-on');diagnosticContinuation=[bool]$ContinueSession;reason=$reason;elapsedSeconds=$elapsed;scenario=$Scenario;stamp=$stamp;host=$last.Host;follower=$last.Follower;dumps=@($dumps.FullName)} |
        ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $out 'result.json') -Encoding UTF8
    @("# Harness $stamp",'',"Result: $result","Pass metric: $metric","Wall time: $([math]::Round($elapsed,1)) seconds","Scenario: $Scenario",'',"$reason",'',
      "Diagnostic continuation: $([bool]$ContinueSession). A continuation never counts as a full new-game intro pass.",
      'Scenario actions and barriers, when started, run inside each game. External requests are limited to session setup, start/stop and completion status.',
      'See paired harness JSONL, tp_client logs, host server log and fresh dumps. A timeout or process exit without a dump is not a successful run.') |
        Set-Content -LiteralPath (Join-Path $out 'summary.md') -Encoding UTF8
    Stop-Transcript | Out-Null
    $analyzer = Join-Path $Worktree 'Tools\InGameTests\Analyze-Harness.py'
    if (Test-Path -LiteralPath $analyzer) {
        & C:/Users/mwalt/AppData/Local/Programs/Python/Python312/python.exe $analyzer $out
    }
}
Write-Output "RESULTS: $out"
if ($result -notin 'passed','diagnostic-passed','crash-captured') { exit 1 }
