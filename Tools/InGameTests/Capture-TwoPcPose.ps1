[CmdletBinding()]
param([int]$DelayMs = 3000, [uint32]$FormId = 0)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = (Resolve-Path (Join-Path $scriptRoot '..\..')).Path
$key = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$armed = & (Join-Path $scriptRoot 'Set-TwoPcPoseProbe.ps1') -DelayMs $DelayMs -FormId $FormId
if (-not $armed.enabled -or -not $armed.host -or -not $armed.follower) {
    throw 'Pose capture did not arm on both PCs.'
}

function Read-HostGameSnapshot {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.AutoFlush = $true
    try {
        $writer.WriteLine('{"id":9305,"command":"game_snapshot"}')
        $response = $reader.ReadLine() | ConvertFrom-Json
        if (-not $response.ok) { throw "Host snapshot failed: $($response.error)" }
        return $response.game
    } finally {
        $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
    }
}

function Read-FollowerGameSnapshot {
    $remoteScript = @'
$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(10000)
$reader = [IO.StreamReader]::new($pipe)
$writer = [IO.StreamWriter]::new($pipe)
$writer.AutoFlush = $true
try { $writer.WriteLine('{"id":9306,"command":"game_snapshot"}'); $reader.ReadLine() }
finally { $writer.Dispose(); $reader.Dispose(); $pipe.Dispose() }
'@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remoteScript))
    $output = & ssh.exe -i $key -o "UserKnownHostsFile=$knownHosts" -o StrictHostKeyChecking=yes -o BatchMode=yes `
        eflem@192.168.50.103 powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Follower snapshot request failed.' }
    $response = ($output | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $response.ok) { throw "Follower snapshot failed: $($response.error)" }
    return $response.game
}

$deadline = (Get-Date).AddSeconds([math]::Max(12, [math]::Ceiling($DelayMs / 1000) + 8))
$sampleTick = [uint64]0
do {
    $snapshot = Read-HostGameSnapshot
    if ($snapshot.actorPoseLastSampleTick -ge $armed.targetTick) {
        $sampleTick = [uint64]$snapshot.actorPoseLastSampleTick
        break
    }
    Start-Sleep -Milliseconds 100
} while ((Get-Date) -lt $deadline)
if ($sampleTick -eq 0) { throw 'Host never produced the scheduled pose snapshot.' }

$followerDeadline = (Get-Date).AddSeconds(20)
do {
    $followerSnapshot = Read-FollowerGameSnapshot
    if ($followerSnapshot.actorPoseLastSampleTick -ge $armed.targetTick) { break }
    Start-Sleep -Milliseconds 100
} while ((Get-Date) -lt $followerDeadline)
if ($followerSnapshot.actorPoseLastSampleTick -lt $armed.targetTick) {
    throw 'Follower never produced the scheduled pose snapshot.'
}

$comparison = & (Join-Path $scriptRoot 'Compare-TwoPcAuthority.ps1') -UseLastPoseCapture
if (-not $comparison) { throw 'Two-PC comparison produced no result.' }
$result = ($comparison -join "`n") | ConvertFrom-Json
if ($result.playerAnimation.hostPoseSampleTick -ne $sampleTick) {
    throw 'Comparator missed the host pose snapshot.'
}
if (-not $result.playerAnimation.followerPoseSampleTick) {
    throw 'Follower did not produce a pose snapshot at the matching world tick.'
}
if ($result.playerAnimation.poseSampleTickDifferenceMs -gt 200) {
    throw "Pose sample ticks differ by $($result.playerAnimation.poseSampleTickDifferenceMs) ms."
}
[pscustomobject]@{
    artifact = $result.artifactPath
    formId = $FormId
    targetTick = $armed.targetTick
    hostPoseTick = $result.playerAnimation.hostPoseSampleTick
    followerPoseTick = $result.playerAnimation.followerPoseSampleTick
    sampleTickDifferenceMs = $result.playerAnimation.poseSampleTickDifferenceMs
    overlapCount = $result.playerAnimation.overlapCount
    matchingSkeletons = $result.playerAnimation.skeletonChecksumMatchCount
    readableEvaluatedPoses = $result.playerAnimation.evaluatedPoseReadableCount
    matchingEvaluatedPoses = $result.playerAnimation.evaluatedPoseMatchCount
    worstLocalTranslationError = @($result.playerAnimation.poseComparisons |
        Where-Object evaluatedPoseError | ForEach-Object { $_.evaluatedPoseError.maxLocalTranslationError } |
        Measure-Object -Maximum)[0].Maximum
    worstLocalRotationErrorDeg = @($result.playerAnimation.poseComparisons |
        Where-Object evaluatedPoseError | ForEach-Object { $_.evaluatedPoseError.maxLocalRotationErrorDeg } |
        Measure-Object -Maximum)[0].Maximum
    matchingRagdolls = $result.playerAnimation.ragdollChecksumMatchCount
    hostProbeDurationUs = $result.playerAnimation.hostPoseProbeDurationUs
    followerProbeDurationUs = $result.playerAnimation.followerPoseProbeDurationUs
    parity = $result.parityGate.passed
}
