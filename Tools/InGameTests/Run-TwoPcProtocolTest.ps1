[CmdletBinding()]
param(
    [string]$RemoteHost = '192.168.50.103',
    [string]$RemoteUser = 'eflem',
    [string]$HostAddress = '192.168.50.173',
    [int]$Port = 12579,
    [switch]$EnableModCheck
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$binaryDirectory = Join-Path $projectRoot 'build\windows\x64\releasedbg'
$serverSource = Join-Path $binaryDirectory 'SkyrimTogetherServer.exe'
$serverDllSource = Join-Path $binaryDirectory 'STServer.dll'
$botPath = Join-Path $binaryDirectory 'SkyrimProtocolBot.exe'
$keyPath = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$artifactDirectory = Join-Path $PSScriptRoot "artifacts\two-pc-$stamp"
$serverDirectory = Join-Path $artifactDirectory 'server'
$configDirectory = Join-Path $serverDirectory 'config'
$dataDirectory = Join-Path $serverDirectory 'Data'
$remoteBot = 'C:/Users/eflem/AppData/Local/Temp/SkyrimProtocolBot.exe'

foreach ($required in @($serverSource, $serverDllSource, $botPath, $keyPath, $knownHosts)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing two-PC test dependency: $required" }
}
if (-not (Test-NetConnection -ComputerName $RemoteHost -Port 22 -InformationLevel Quiet -WarningAction SilentlyContinue)) {
    throw "The second PC answers on the LAN but SSH port 22 is closed. On that PC run elevated PowerShell: Set-Service sshd -StartupType Automatic; Start-Service sshd"
}

New-Item -ItemType Directory -Path $configDirectory -Force | Out-Null
Copy-Item -LiteralPath $serverSource, $serverDllSource -Destination $serverDirectory
$modCheckValue = $EnableModCheck.IsPresent.ToString().ToLowerInvariant()
$manifestArgument = if ($EnableModCheck) { 'synthetic-plugin' } else { 'none' }
if ($EnableModCheck) {
    New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
    'ProtocolTest.esp' | Set-Content -LiteralPath (Join-Path $dataDirectory 'loadorder.txt') -Encoding ASCII
}
"[EULA]`r`nbConfirmEULA=true" | Set-Content -LiteralPath (Join-Path $configDirectory 'EULA.txt') -Encoding ASCII
@"
[general]
sLogLevel=debug
bConsole=false
[LiveServices]
bAnnounceServer=false
[Gameplay]
bAutoPartyJoin=true
bEnableDeathSystem=true
[ModPolicy]
bAllowMO2=true
bAllowSKSE=true
bEnableModCheck=$modCheckValue
[GameServer]
sPassword=
sAdminPassword=
sServerName=Two PC Headless Test
bPremiumMode=true
uMaxPlayerCount=2
uPort=$Port
"@ | Set-Content -LiteralPath (Join-Path $configDirectory 'STServer.ini') -Encoding ASCII

$sshOptions = @('-i', $keyPath, '-o', "UserKnownHostsFile=$knownHosts", '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')
$server = $null
$hostBot = $null
try {
    & scp @sshOptions $botPath "${RemoteUser}@${RemoteHost}:$remoteBot"
    if ($LASTEXITCODE -ne 0) { throw 'Could not deploy the headless protocol bot to the second PC.' }

    $server = Start-Process -FilePath (Join-Path $serverDirectory 'SkyrimTogetherServer.exe') -WorkingDirectory $serverDirectory -WindowStyle Hidden -PassThru
    $serverLog = Join-Path $serverDirectory 'logs\STServerOut.log'
    $deadline = (Get-Date).AddSeconds(15)
    do {
        if ($server.HasExited) { throw "Test server exited with code $($server.ExitCode)" }
        $ready = Test-Path -LiteralPath $serverLog
        if ($ready) { $ready = [bool](Select-String -LiteralPath $serverLog -SimpleMatch "started on port $Port" -Quiet) }
        if (-not $ready) { Start-Sleep -Milliseconds 100 }
    } while (-not $ready -and (Get-Date) -lt $deadline)
    if (-not $ready) { throw 'Two-PC test server did not become ready.' }

    $hostOutput = Join-Path $artifactDirectory 'host.json'
    $hostError = Join-Path $artifactDirectory 'host.stderr.log'
    $hostBot = Start-Process -FilePath $botPath -ArgumentList @("127.0.0.1:$Port", 'distributed-host', $manifestArgument) `
        -RedirectStandardOutput $hostOutput -RedirectStandardError $hostError -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 750

    $remoteOutput = & ssh @sshOptions "${RemoteUser}@${RemoteHost}" `
        "& '$remoteBot' '${HostAddress}:$Port' 'distributed-follower' '$manifestArgument'"
    $remoteExit = $LASTEXITCODE
    $remoteOutput | Set-Content -LiteralPath (Join-Path $artifactDirectory 'follower.json') -Encoding UTF8
    $hostBot.WaitForExit(45000) | Out-Null
    if (-not $hostBot.HasExited) { throw 'Host protocol bot timed out.' }

    $hostResult = Get-Content -LiteralPath $hostOutput -Raw | ConvertFrom-Json
    $followerResult = ($remoteOutput -join "`n") | ConvertFrom-Json
    $summary = [pscustomobject]@{
        passed = $hostBot.ExitCode -eq 0 -and $remoteExit -eq 0 -and $hostResult.passed -and $followerResult.passed
        host = $hostResult
        follower = $followerResult
        artifactDirectory = $artifactDirectory
    }
    $summary | ConvertTo-Json -Depth 6
    if (-not $summary.passed) { exit 1 }
}
finally {
    if ($hostBot -and -not $hostBot.HasExited) { Stop-Process -Id $hostBot.Id -Force }
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}
