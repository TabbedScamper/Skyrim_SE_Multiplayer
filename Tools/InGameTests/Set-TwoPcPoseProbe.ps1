[CmdletBinding()]
param([bool]$Enabled = $true, [int]$DelayMs = 3000, [uint32]$FormId = 0)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$key = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
if ($Enabled -and $DelayMs -lt 1000) { throw 'DelayMs must be at least 1000 for a two-PC scheduled capture.' }
$selectionRequest = @{ id = 9199; command = 'set_pose_probe_actor'; form_id = [string]$FormId } |
    ConvertTo-Json -Compress
$selectionPipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
$selectionPipe.Connect(10000)
$selectionReader = [IO.StreamReader]::new($selectionPipe)
$selectionWriter = [IO.StreamWriter]::new($selectionPipe)
$selectionWriter.AutoFlush = $true
try {
    $selectionWriter.WriteLine($selectionRequest)
    $selection = $selectionReader.ReadLine() | ConvertFrom-Json
    if (-not $selection.ok -or $selection.formId -ne $FormId) {
        throw "Host actor selection failed: $($selection.error)"
    }
} finally {
    $selectionWriter.Dispose(); $selectionReader.Dispose(); $selectionPipe.Dispose()
}
$targetTick = [uint64]0
if ($Enabled) {
    $snapshotPipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $snapshotPipe.Connect(10000)
    $snapshotReader = [IO.StreamReader]::new($snapshotPipe)
    $snapshotWriter = [IO.StreamWriter]::new($snapshotPipe)
    $snapshotWriter.AutoFlush = $true
    try {
        $snapshotWriter.WriteLine((@{ id = 9200; command = 'request_game_snapshot'; delay_ms = [string]$DelayMs } | ConvertTo-Json -Compress))
        $snapshot = $snapshotReader.ReadLine() | ConvertFrom-Json
        if (-not $snapshot.ok -or $null -eq $snapshot.targetTick) {
            throw 'Host shared clock is unavailable; load a campaign first.'
        }
        $targetTick = [uint64]$snapshot.targetTick
    } finally {
        $snapshotWriter.Dispose(); $snapshotReader.Dispose(); $snapshotPipe.Dispose()
    }
}
$request = @{ id = 9201; command = 'set_pose_probe'; enabled = $Enabled.ToString().ToLowerInvariant(); tick = [string]$targetTick } |
    ConvertTo-Json -Compress

$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(10000)
$reader = [IO.StreamReader]::new($pipe)
$writer = [IO.StreamWriter]::new($pipe)
$writer.AutoFlush = $true
try {
    $writer.WriteLine($request)
    $local = $reader.ReadLine() | ConvertFrom-Json
    if (-not $local.ok) { throw "Host probe failed: $($local.error)" }
} finally {
    $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
}

$snapshotRequest = @{ id = 9202; command = 'request_game_snapshot'; tick = [string]$targetTick } | ConvertTo-Json -Compress
$remoteScript = @"
function Invoke-Bridge(`$line) {
    `$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    `$pipe.Connect(10000)
    `$reader = [IO.StreamReader]::new(`$pipe)
    `$writer = [IO.StreamWriter]::new(`$pipe)
    `$writer.AutoFlush = `$true
    try { `$writer.WriteLine(`$line); `$reader.ReadLine() }
    finally { `$writer.Dispose(); `$reader.Dispose(); `$pipe.Dispose() }
}
`$selection = Invoke-Bridge '$selectionRequest' | ConvertFrom-Json
if (-not `$selection.ok -or `$selection.formId -ne $FormId) { throw 'Follower actor selection failed.' }
`$pose = Invoke-Bridge '$request'
if ('$Enabled' -eq 'True') { Invoke-Bridge '$snapshotRequest' | Out-Null }
`$pose
"@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remoteScript))
$remoteOutput = & ssh.exe -i $key -o "UserKnownHostsFile=$knownHosts" -o StrictHostKeyChecking=yes -o BatchMode=yes `
    eflem@192.168.50.103 powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded 2>$null
if ($LASTEXITCODE -ne 0) { throw 'Follower probe request failed.' }
$remote = ($remoteOutput | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1) | ConvertFrom-Json
if (-not $remote.ok) { throw "Follower probe failed: $($remote.error)" }
[pscustomobject]@{ enabled = $Enabled; targetTick = $targetTick; host = $local.ok; follower = $remote.ok }
