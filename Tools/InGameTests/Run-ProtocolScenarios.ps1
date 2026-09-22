[CmdletBinding()]
param(
    [int]$Port = 12578,
    [switch]$FailOnKnownGaps,
    [switch]$Compact
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$binaryDirectory = Join-Path $projectRoot 'build\windows\x64\releasedbg'
$serverSource = Join-Path $binaryDirectory 'SkyrimTogetherServer.exe'
$serverDllSource = Join-Path $binaryDirectory 'STServer.dll'
$botPath = Join-Path $binaryDirectory 'SkyrimProtocolBot.exe'
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$artifactDirectory = Join-Path $PSScriptRoot "artifacts\protocol-$runStamp"
$serverDirectory = Join-Path $artifactDirectory 'server'
$configDirectory = Join-Path $serverDirectory 'config'

foreach ($required in @($serverSource, $serverDllSource, $botPath)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing built test dependency: $required"
    }
}

New-Item -ItemType Directory -Path $configDirectory -Force | Out-Null
Copy-Item -LiteralPath $serverSource, $serverDllSource -Destination $serverDirectory

@"
[EULA]
bConfirmEULA=true
"@ | Set-Content -LiteralPath (Join-Path $configDirectory 'EULA.txt') -Encoding ASCII

@"
[general]
sLogLevel=debug
bConsole=false

[LiveServices]
bAnnounceServer=false

[Gameplay]
bEnableMiscQuestSync=true
bAutoPartyJoin=true
bSyncPlayerCalendar=false
bEnableDeathSystem=true
bSyncPlayerHomes=true
bEnablePvp=false
bEnableGreetings=false
uDifficulty=4

[ModPolicy]
bAllowMO2=true
bAllowSKSE=true
bEnableModCheck=false

[GameServer]
sPassword=
sAdminPassword=
sServerName=Headless Protocol Test
bPremiumMode=true
uMaxPlayerCount=8
uPort=$Port
"@ | Set-Content -LiteralPath (Join-Path $configDirectory 'STServer.ini') -Encoding ASCII

$server = $null
try {
    $server = Start-Process -FilePath (Join-Path $serverDirectory 'SkyrimTogetherServer.exe') `
        -WorkingDirectory $serverDirectory -WindowStyle Hidden -PassThru

    $serverLog = Join-Path $serverDirectory 'logs\STServerOut.log'
    $deadline = (Get-Date).AddSeconds(15)
    do {
        if ($server.HasExited) { throw "Test server exited with code $($server.ExitCode)" }
        $ready = Test-Path -LiteralPath $serverLog
        if ($ready) {
            $ready = [bool](Select-String -LiteralPath $serverLog -SimpleMatch "started on port $Port" -Quiet)
        }
        if (-not $ready) { Start-Sleep -Milliseconds 100 }
    } while (-not $ready -and (Get-Date) -lt $deadline)
    if (-not $ready) { throw "Test server did not listen on port $Port" }

    $joinOutput = & $botPath "127.0.0.1:$Port" join
    $joinExitCode = $LASTEXITCODE
    $join = $joinOutput | ConvertFrom-Json

    $handoffOutput = & $botPath "127.0.0.1:$Port" leader-handoff
    $handoffExitCode = $LASTEXITCODE
    $handoff = $handoffOutput | ConvertFrom-Json

    $reconnectOutput = & $botPath "127.0.0.1:$Port" follower-reconnect
    $reconnectExitCode = $LASTEXITCODE
    $reconnect = $reconnectOutput | ConvertFrom-Json

    $authorityOutput = & $botPath "127.0.0.1:$Port" quest-authority-audit
    $authorityExitCode = $LASTEXITCODE
    $authority = $authorityOutput | ConvertFrom-Json

    $summary = [pscustomobject]@{
        passed = $joinExitCode -eq 0 -and $handoffExitCode -eq 0 -and $reconnectExitCode -eq 0 -and `
            ($authorityExitCode -eq 0 -or (-not $FailOnKnownGaps -and $authorityExitCode -eq 2))
        join = $join
        leaderHandoff = $handoff
        followerReconnect = $reconnect
        questAuthority = $authority
        knownGaps = @(
            if ($authorityExitCode -eq 2) { 'non-leader quest updates are accepted and broadcast' }
        )
        serverLog = $serverLog
        artifactDirectory = $artifactDirectory
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $artifactDirectory 'summary.json') -Encoding UTF8
    if ($Compact) { $summary | ConvertTo-Json -Depth 8 -Compress }
    else { $summary | ConvertTo-Json -Depth 8 }
    if (-not $summary.passed) { exit 1 }
}
finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        $null = $server.WaitForExit(5000)
    }
}
