[CmdletBinding()]
param(
    [string]$RemoteHost = '192.168.50.103',
    [string]$RemoteUser = 'eflem',
    [ValidateRange(1, 120)][int]$Samples = 1,
    [ValidateRange(250, 10000)][int]$IntervalMs = 1000
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$key = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$request = '{"id":9300,"command":"hitch_snapshot"}'

function Read-LocalHitch {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.AutoFlush = $true
    try {
        $writer.WriteLine($request)
        $response = $reader.ReadLine() | ConvertFrom-Json
        if (-not $response.ok) { throw "Host hitch snapshot failed: $($response.error)" }
        return $response.game
    } finally {
        $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
    }
}

function Read-RemoteHitch {
    $remoteScript = @'
$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(10000)
$reader = [IO.StreamReader]::new($pipe)
$writer = [IO.StreamWriter]::new($pipe)
$writer.AutoFlush = $true
try {
    $writer.WriteLine('{"id":9300,"command":"hitch_snapshot"}')
    $reader.ReadLine()
} finally {
    $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
}
'@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remoteScript))
    $output = & ssh.exe -i $key -o "UserKnownHostsFile=$knownHosts" -o StrictHostKeyChecking=yes -o BatchMode=yes "${RemoteUser}@${RemoteHost}" powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Follower hitch snapshot transport failed.' }
    $response = ($output | Where-Object { $_ -match '^\{' } | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $response.ok) { throw "Follower hitch snapshot failed: $($response.error)" }
    return $response.game
}

function Get-InterpolatedCart([object[]]$History, [long]$Tick, [int]$CartIndex,
    [string]$PositionField = 'position', [string]$PresentField = 'present') {
    # Older deployed clients may not expose the optional paired-horse fields.
    $History = @($History | Where-Object {
        $present = $_.PSObject.Properties[$PresentField]
        $position = $_.PSObject.Properties[$PositionField]
        $null -ne $present -and $null -ne $position -and
            $null -ne $present.Value -and $null -ne $position.Value -and
            @($present.Value).Count -gt $CartIndex -and
            @($position.Value).Count -gt $CartIndex
    })
    $before = $History | Where-Object {
        [long]$_.tick -le $Tick -and $_.PSObject.Properties[$PresentField].Value[$CartIndex]
    } |
        Sort-Object { [long]$_.tick } -Descending | Select-Object -First 1
    $after = $History | Where-Object {
        [long]$_.tick -ge $Tick -and $_.PSObject.Properties[$PresentField].Value[$CartIndex]
    } |
        Sort-Object { [long]$_.tick } | Select-Object -First 1
    if (-not $before -or -not $after) { return $null }
    $span = [long]$after.tick - [long]$before.tick
    if ($span -gt 200) { return $null }
    $fraction = if ($span -gt 0) { ($Tick - [long]$before.tick) / [double]$span } else { 0.0 }
    $position = for ($axis = 0; $axis -lt 3; ++$axis) {
        [double]$before.PSObject.Properties[$PositionField].Value[$CartIndex][$axis] +
            ([double]$after.PSObject.Properties[$PositionField].Value[$CartIndex][$axis] -
                [double]$before.PSObject.Properties[$PositionField].Value[$CartIndex][$axis]) * $fraction
    }
    return [pscustomobject]@{ position = @($position); spanMs = $span }
}

$lastHostTick = [uint64]0
$lastFollowerTick = [uint64]0
for ($sample = 0; $sample -lt $Samples; ++$sample) {
    $hostState = Read-LocalHitch
    $followerState = Read-RemoteHitch
    $followerHistory = @($followerState.history)
    $hostHistory = @($hostState.history)
    $alignedTick = if ($hostHistory.Count -and $followerHistory.Count) {
        [math]::Min([long]$hostHistory[-1].tick, [long]$followerHistory[-1].tick)
    } else { $null }
    $cartPairs = for ($i = 0; $i -lt [math]::Min(@($hostState.carts).Count, @($followerState.carts).Count); ++$i) {
        $hostCart = $hostState.carts[$i]
        $followerCart = $followerState.carts[$i]
        if ($hostCart.formId -ne $followerCart.formId) { continue }
        $hostInterpolated = if ($null -ne $alignedTick) { Get-InterpolatedCart $hostHistory $alignedTick $i } else { $null }
        $followerInterpolated = if ($null -ne $alignedTick) { Get-InterpolatedCart $followerHistory $alignedTick $i } else { $null }
        $hostHorse = if ($null -ne $alignedTick) {
            Get-InterpolatedCart $hostHistory $alignedTick $i 'horsePosition' 'horsePresent'
        } else { $null }
        $followerHorse = if ($null -ne $alignedTick) {
            Get-InterpolatedCart $followerHistory $alignedTick $i 'horsePosition' 'horsePresent'
        } else { $null }
        $aligned = $hostInterpolated -and $followerInterpolated
        if ($aligned) {
            $dx = [double]$hostInterpolated.position[0] - [double]$followerInterpolated.position[0]
            $dy = [double]$hostInterpolated.position[1] - [double]$followerInterpolated.position[1]
            $dz = [double]$hostInterpolated.position[2] - [double]$followerInterpolated.position[2]
        }
        $hostHorseDistance = $null
        $followerHorseDistance = $null
        if ($aligned -and $hostHorse -and $followerHorse) {
            $hostOffset = for ($axis = 0; $axis -lt 3; ++$axis) {
                [double]$hostHorse.position[$axis] - [double]$hostInterpolated.position[$axis]
            }
            $followerOffset = for ($axis = 0; $axis -lt 3; ++$axis) {
                [double]$followerHorse.position[$axis] - [double]$followerInterpolated.position[$axis]
            }
            $hostHorseDistance = [math]::Sqrt(($hostOffset | ForEach-Object { $_ * $_ } | Measure-Object -Sum).Sum)
            $followerHorseDistance = [math]::Sqrt(($followerOffset | ForEach-Object { $_ * $_ } | Measure-Object -Sum).Sum)
        }
        [pscustomobject]@{
            formId = ('0x{0:X8}' -f [uint32]$hostCart.formId)
            error = if ($aligned) { [math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz) } else { $null }
            hostInterpolationSpanMs = if ($aligned) { $hostInterpolated.spanMs } else { $null }
            followerInterpolationSpanMs = if ($aligned) { $followerInterpolated.spanMs } else { $null }
            hostPeakStep = $hostCart.peakStep
            followerPeakStep = $followerCart.peakStep
            followerLargeSteps = $followerCart.largeSteps
            hostHorseDistance = $hostHorseDistance
            followerHorseDistance = $followerHorseDistance
        }
    }
    [pscustomobject]@{
        sample = $sample + 1
        hostTick = $hostState.worldTick
        followerTick = $followerState.worldTick
        tickDifferenceMs = [math]::Abs([long]$hostState.worldTick - [long]$followerState.worldTick)
        alignedTick = $alignedTick
        hostMaxGapUs = $hostState.worldMaxGapUs
        followerMaxGapUs = $followerState.worldMaxGapUs
        hostMaxGameTestUs = $hostState.worldMaxGameTestUs
        followerMaxGameTestUs = $followerState.worldMaxGameTestUs
        hostMaxAppUs = $hostState.vmMaxAppUs
        followerMaxAppUs = $followerState.vmMaxAppUs
        hostScanCount = $hostState.hostScanCount
        hostScanTotalUs = $hostState.hostScanTotalUs
        hostScanMaxUs = $hostState.hostScanMaxUs
        hostScanLastReferencesVisited = $hostState.hostScanLastReferencesVisited
        hostScanLastCandidateCount = $hostState.hostScanLastCandidateCount
        hostPoseSelectedBatches = $hostState.poseSelectedBatches
        hostPoseSelectedActors = $hostState.poseSelectedActors
        hostPoseSelectedTotalUs = $hostState.poseSelectedTotalUs
        hostPoseSelectedMaxActorUs = $hostState.poseSelectedMaxActorUs
        hostPoseSelectedLastBatchUs = $hostState.poseSelectedLastBatchUs
        followerPoseSelectedBatches = $followerState.poseSelectedBatches
        followerPoseSelectedActors = $followerState.poseSelectedActors
        followerPoseSelectedTotalUs = $followerState.poseSelectedTotalUs
        followerPoseSelectedMaxActorUs = $followerState.poseSelectedMaxActorUs
        followerPoseSelectedLastBatchUs = $followerState.poseSelectedLastBatchUs
        hostPhysicsPackets = $hostState.hostPhysicsPackets
        hostPhysicsUpdates = $hostState.hostPhysicsUpdates
        hostBodyOnlyUpdates = $hostState.hostBodyOnlyUpdates
        followerPhysicsPackets = $followerState.followerPhysicsPackets
        hostSelectedBodyPeakStep = $hostState.selectedBodyPeakStep
        followerSelectedBodyPeakStep = $followerState.selectedBodyPeakStep
        hostSelectedBodyStepsOver75 = $hostState.selectedBodyStepsOver75
        followerSelectedBodyStepsOver75 = $followerState.selectedBodyStepsOver75
        hostSelectedBodyPeak = [pscustomobject]@{
            timeMs = $hostState.selectedBodyPeakTimeMs
            dt = $hostState.selectedBodyPeakDt
            preLinearSpeed = $hostState.selectedBodyPeakPreLinearSpeed
            postLinearSpeed = $hostState.selectedBodyPeakPostLinearSpeed
            preAngularSpeed = $hostState.selectedBodyPeakPreAngularSpeed
            postAngularSpeed = $hostState.selectedBodyPeakPostAngularSpeed
            motionType = $hostState.selectedBodyPeakMotionType
            targetApplied = $hostState.selectedBodyPeakTargetApplied
            targetAgeMs = $hostState.selectedBodyPeakTargetAgeMs
            velocityAfterWrite = $hostState.selectedBodyPeakVelocityAfterWrite
        }
        followerSelectedBodyPeak = [pscustomobject]@{
            timeMs = $followerState.selectedBodyPeakTimeMs
            dt = $followerState.selectedBodyPeakDt
            preLinearSpeed = $followerState.selectedBodyPeakPreLinearSpeed
            postLinearSpeed = $followerState.selectedBodyPeakPostLinearSpeed
            preAngularSpeed = $followerState.selectedBodyPeakPreAngularSpeed
            postAngularSpeed = $followerState.selectedBodyPeakPostAngularSpeed
            motionType = $followerState.selectedBodyPeakMotionType
            targetApplied = $followerState.selectedBodyPeakTargetApplied
            targetAgeMs = $followerState.selectedBodyPeakTargetAgeMs
            velocityAfterWrite = $followerState.selectedBodyPeakVelocityAfterWrite
        }
        carts = @($cartPairs)
        hostEvents = @($hostState.events | Where-Object { [uint64]$_.worldTick -gt $lastHostTick })
        followerEvents = @($followerState.events | Where-Object { [uint64]$_.worldTick -gt $lastFollowerTick })
    }
    $lastHostTick = [uint64]$hostState.worldTick
    $lastFollowerTick = [uint64]$followerState.worldTick
    if ($sample + 1 -lt $Samples) { Start-Sleep -Milliseconds $IntervalMs }
}
