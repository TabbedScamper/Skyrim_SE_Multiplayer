[CmdletBinding()]
param(
    [string]$RemoteHost = '192.168.50.103',
    [string]$RemoteUser = 'eflem',
    [string]$HostSteamId = '76561198212993360',
    [int]$JoinWaitSeconds = 6,
    [ValidateSet('new', 'continue')][string]$CampaignMode = 'new'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$keyPath = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$sshOptions = @(
    '-i', $keyPath,
    '-o', "UserKnownHostsFile=$knownHosts",
    '-o', 'StrictHostKeyChecking=yes',
    '-o', 'BatchMode=yes'
)
$script:requestId = 0

function New-Request([string]$Command, [hashtable]$Arguments = @{}) {
    $script:requestId++
    $request = @{ id = $script:requestId; command = $Command }
    foreach ($entry in $Arguments.GetEnumerator()) {
        $request[$entry.Key] = [string]$entry.Value
    }
    return ($request | ConvertTo-Json -Compress)
}

function Invoke-Pipe([string]$Json) {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.AutoFlush = $true
    try {
        $writer.WriteLine($Json)
        $line = $reader.ReadLine()
        if (-not $line) { throw 'The local game returned no debug-bridge response.' }
        $response = $line | ConvertFrom-Json
        if (-not $response.ok) { throw "Local command failed: $($response.error)" }
        return $response
    } finally {
        $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
    }
}

function Invoke-RemotePipe([string]$Json) {
    $escaped = $Json.Replace("'", "''")
    $remoteScript = @"
`$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
`$pipe.Connect(10000)
`$reader = [IO.StreamReader]::new(`$pipe)
`$writer = [IO.StreamWriter]::new(`$pipe)
`$writer.AutoFlush = `$true
try {
    `$writer.WriteLine('$escaped')
    `$line = `$reader.ReadLine()
    if (-not `$line) { throw 'The remote game returned no debug-bridge response.' }
    `$line
} finally {
    `$writer.Dispose(); `$reader.Dispose(); `$pipe.Dispose()
}
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remoteScript))
    $line = & ssh.exe @sshOptions "$RemoteUser@$RemoteHost" powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'The remote debug-bridge command failed.' }
    $response = ($line | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $response.ok) { throw "Remote command failed: $($response.error)" }
    return $response
}

# The executable can expose its pipe before Steam has created the lobby or
# the local server has assigned party leadership. The settings request is a
# no-op before that transition, so explicitly wait for it.
$hostReady = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    $hostParty = Invoke-Pipe (New-Request 'party_state')
    if ($hostParty.inParty -and $hostParty.leader -and
        $hostParty.memberCount -ge 1 -and $hostParty.sessionState -eq 0) {
        $hostReady = $true
        break
    }
    Start-Sleep -Seconds 1
}
if (-not $hostReady) { throw 'Host Steam lobby and party did not become ready.' }
Invoke-Pipe (New-Request 'set_session_open' @{ open = 'true' }) | Out-Null
# Steam applies lobby metadata asynchronously. QueueJoinFriend merely accepts
# the request, so do not mistake that acknowledgement for a successful join.
$joined = $false
for ($attempt = 0; $attempt -lt 8; $attempt++) {
    Invoke-RemotePipe (New-Request 'join_friend' @{ steamId = $HostSteamId }) | Out-Null
    Start-Sleep -Seconds $JoinWaitSeconds
    $hostParty = Invoke-Pipe (New-Request 'party_state')
    $followerParty = Invoke-RemotePipe (New-Request 'party_state')
    if ($hostParty.memberCount -ge 2 -and $followerParty.memberCount -ge 2 -and
        $hostParty.leader -and -not $followerParty.leader) {
        $joined = $true
        break
    }
}
if (-not $joined) { throw 'The follower did not join the host party; campaign was not started.' }
Invoke-RemotePipe (New-Request 'set_ready' @{ ready = 'true' }) | Out-Null
Invoke-Pipe (New-Request 'set_ready' @{ ready = 'true' }) | Out-Null
$allReady = $false
for ($attempt = 0; $attempt -lt 10; $attempt++) {
    Start-Sleep -Seconds 1
    $hostParty = Invoke-Pipe (New-Request 'party_state')
    if ($hostParty.readyCount -eq $hostParty.memberCount -and $hostParty.memberCount -ge 2) {
        $allReady = $true
        break
    }
}
if (-not $allReady) { throw 'Not every party member reached ready state; campaign was not started.' }
Invoke-Pipe (New-Request $(if ($CampaignMode -eq 'continue') {
    'start_continue_campaign'
} else {
    'start_new_campaign'
})) | Out-Null
$launched = $false
for ($attempt = 0; $attempt -lt 15; $attempt++) {
    Start-Sleep -Seconds 1
    $hostParty = Invoke-Pipe (New-Request 'party_state')
    $followerParty = Invoke-RemotePipe (New-Request 'party_state')
    if ($hostParty.startEpoch -gt 0 -and
        $hostParty.startEpoch -eq $followerParty.startEpoch -and
        $hostParty.sessionState -gt 0 -and $followerParty.sessionState -gt 0) {
        $launched = $true
        break
    }
}
if (-not $launched) { throw 'The party did not enter a shared campaign on both PCs.' }

[pscustomobject]@{
    launched = $launched
    campaignMode = $CampaignMode
    hostSteamId = $HostSteamId
    remote = "$RemoteUser@$RemoteHost"
}
