[CmdletBinding()]
param(
    [string]$RemoteHost = '192.168.50.103',
    [string]$RemoteUser = 'eflem',
    [int]$ReferenceLimit = 50,
    [uint64]$AtTick = 0,
    [int]$MotionIntervalMs = 0,
    [uint64]$MotionSecondTick = 0,
    [switch]$UseLastPoseCapture,
    [switch]$RequireParity
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$graphVariableNames = @{}
foreach ($descriptor in @(
    @{ Count = 301; File = 'AnimationGraphDescriptor_Master_Behavior.cpp' },
    @{ Count = 76; File = 'AnimationGraphDescriptor_HorseRootBehavior.cpp' }
)) {
    $names = @{}
    $sourcePath = Join-Path $projectRoot "Code\encoding\Structs\Skyrim\$($descriptor.File)"
    foreach ($sourceLine in [IO.File]::ReadAllLines($sourcePath)) {
        if ($sourceLine -match '^\s*(k\w+)\s*=\s*(\d+),') {
            $names[[int]$Matches[2]] = $Matches[1]
        }
    }
    $graphVariableNames[[int]$descriptor.Count] = $names
}
$keyPath = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$sshOptions = @('-i', $keyPath, '-o', "UserKnownHostsFile=$knownHosts", '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')

function Request-LocalSnapshot([uint64]$DelayMs) {
    $request = @{ id = 9100; command = 'request_game_snapshot'; delay_ms = [string]$DelayMs } | ConvertTo-Json -Compress
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.AutoFlush = $true
    try {
        $writer.WriteLine($request)
        $response = $reader.ReadLine() | ConvertFrom-Json
        if (-not $response.ok) { throw "Local snapshot request failed: $($response.error)" }
        return [uint64]$response.targetTick
    }
    finally { $writer.Dispose(); $reader.Dispose(); $pipe.Dispose() }
}

function Request-RemoteSnapshot([uint64]$TargetTick) {
    $request = @{ id = 9100; command = 'request_game_snapshot'; tick = [string]$TargetTick } | ConvertTo-Json -Compress
    $remoteScript = @"
`$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
`$pipe.Connect(10000)
`$reader = [IO.StreamReader]::new(`$pipe)
`$writer = [IO.StreamWriter]::new(`$pipe)
`$writer.AutoFlush = `$true
try { `$writer.WriteLine('$request'); `$reader.ReadLine() }
finally { `$writer.Dispose(); `$reader.Dispose(); `$pipe.Dispose() }
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remoteScript))
    $output = & ssh.exe @sshOptions "$RemoteUser@$RemoteHost" powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Remote snapshot request failed.' }
    $response = ($output | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $response.ok) { throw "Remote snapshot request failed: $($response.error)" }
}

function Read-LocalSnapshot([uint64]$TargetTick = 0) {
    $request = if ($UseLastPoseCapture) {
        @{ id = 9101; command = 'game_pose_snapshot' } | ConvertTo-Json -Compress
    } elseif ($TargetTick -eq 0) {
        @{ id = 9101; command = 'game_snapshot' } | ConvertTo-Json -Compress
    } else {
        @{ id = 9101; command = 'game_snapshot_at'; tick = [string]$TargetTick } | ConvertTo-Json -Compress
    }
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.AutoFlush = $true
    try {
        $writer.WriteLine($request)
        $response = $reader.ReadLine() | ConvertFrom-Json
        if (-not $response.ok) { throw "Local bridge failed: $($response.error)" }
        return $response.game
    }
    finally {
        $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
    }
}

function Read-RemoteSnapshot([uint64]$TargetTick) {
    $request = if ($UseLastPoseCapture) {
        @{ id = 9102; command = 'game_pose_snapshot' } | ConvertTo-Json -Compress
    } else {
        @{ id = 9102; command = 'game_snapshot_at'; tick = [string]$TargetTick } | ConvertTo-Json -Compress
    }
    $remoteScript = @"
`$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
`$pipe.Connect(10000)
`$reader = [IO.StreamReader]::new(`$pipe)
`$writer = [IO.StreamWriter]::new(`$pipe)
`$writer.AutoFlush = `$true
try { `$writer.WriteLine('$request'); `$reader.ReadLine() }
finally { `$writer.Dispose(); `$reader.Dispose(); `$pipe.Dispose() }
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remoteScript))
    $output = & ssh.exe @sshOptions "$RemoteUser@$RemoteHost" powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Remote bridge request failed.' }
    $response = ($output | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $response.ok) { throw "Remote bridge failed: $($response.error)" }
    return $response.game
}

function Get-VectorDistance($Left, $Right) {
    if ($null -eq $Left -or $null -eq $Right -or $Left.Count -lt 3 -or $Right.Count -lt 3) { return $null }
    $x = [double]$Left[0] - [double]$Right[0]
    $y = [double]$Left[1] - [double]$Right[1]
    $z = [double]$Left[2] - [double]$Right[2]
    [Math]::Sqrt(($x * $x) + ($y * $y) + ($z * $z))
}

function Resolve-ActorTarget([uint32]$FormId, [hashtable]$FormToNetwork) {
    if ($FormId -eq 0) { return 'none' }
    if ($FormToNetwork.ContainsKey($FormId)) {
        return 'network:{0}' -f $FormToNetwork[$FormId]
    }
    if ($FormId -lt 0xFF000000) { return 'form:{0:X8}' -f $FormId }
    # Untracked temporary forms cannot be matched by their local FF IDs.
    return 'unresolved-temp'
}

function Get-PoseError($Left, $Right) {
    $leftTransforms = @($Left.transforms)
    $rightTransforms = @($Right.transforms)
    if ($leftTransforms.Count -eq 0 -or $leftTransforms.Count -ne $rightTransforms.Count) { return $null }
    $translationSum = 0.0
    $translationMax = 0.0
    $rotationSum = 0.0
    $rotationMax = 0.0
    for ($i = 0; $i -lt $leftTransforms.Count; $i++) {
        $translation = Get-VectorDistance $leftTransforms[$i].t $rightTransforms[$i].t
        if ($null -eq $translation) { return $null }
        $translationSum += $translation
        $translationMax = [math]::Max($translationMax, $translation)
        $dot = 0.0
        $leftLength = 0.0
        $rightLength = 0.0
        for ($j = 0; $j -lt 4; $j++) {
            $leftValue = [double]$leftTransforms[$i].q[$j]
            $rightValue = [double]$rightTransforms[$i].q[$j]
            $dot += $leftValue * $rightValue
            $leftLength += $leftValue * $leftValue
            $rightLength += $rightValue * $rightValue
        }
        if ($leftLength -le 0 -or $rightLength -le 0) { return $null }
        $cosine = [math]::Min(1.0, [math]::Abs($dot / [math]::Sqrt($leftLength * $rightLength)))
        $degrees = 2.0 * [math]::Acos($cosine) * 180.0 / [math]::PI
        $rotationSum += $degrees
        $rotationMax = [math]::Max($rotationMax, $degrees)
    }
    [pscustomobject]@{
        sampledBones = $leftTransforms.Count
        meanLocalTranslationError = $translationSum / $leftTransforms.Count
        maxLocalTranslationError = $translationMax
        meanLocalRotationErrorDeg = $rotationSum / $leftTransforms.Count
        maxLocalRotationErrorDeg = $rotationMax
    }
}

$compareTick = $AtTick
if (-not $UseLastPoseCapture -and $compareTick -eq 0) {
    $compareTick = Request-LocalSnapshot -DelayMs 3000
    Request-RemoteSnapshot -TargetTick $compareTick
    Start-Sleep -Milliseconds 3400
}
$hostGame = Read-LocalSnapshot -TargetTick $compareTick
if ($null -eq $hostGame -or $null -eq $hostGame.worldTick) {
    throw 'The host has no active game snapshot; load a campaign before comparing.'
}
$followerTick = if ($compareTick) { $compareTick } else { [uint64]$hostGame.worldTick }
$followerGame = Read-RemoteSnapshot -TargetTick $followerTick
function Convert-TriggerEvents($Game) {
    $byFormId = @{}
    foreach ($actor in @($Game.networkEntities)) {
        if ($null -ne $actor.formId -and $null -ne $actor.networkId) {
            $byFormId[[uint32]$actor.formId] = [uint32]$actor.networkId
        }
    }
    foreach ($event in @($Game.triggerEvents)) {
        $actorFormId = [uint32]$event.actorFormId
        $triggerFormId = [uint32]$event.triggerFormId
        [pscustomobject]@{
            sequence = $event.sequence
            enter = $event.enter
            triggerFormId = ('0x{0:X8}' -f $triggerFormId)
            triggerFormStable = ($triggerFormId -band 0xFF000000) -ne 0xFF000000
            actorFormId = ('0x{0:X8}' -f $actorFormId)
            actorNetworkId = if ($byFormId.ContainsKey($actorFormId)) { $byFormId[$actorFormId] } else { $null }
            relativeToSampleMs = [long]$event.timeMs - [long]$Game.sampleTimeMs
        }
    }
}
$hostMotionGame = $null
$followerMotionGame = $null
if ($MotionIntervalMs -ne 0 -and $MotionSecondTick -ne 0) {
    throw 'Choose MotionIntervalMs or MotionSecondTick, not both.'
}
if ($MotionIntervalMs -ne 0) {
    if ($UseLastPoseCapture -or $AtTick -ne 0) {
        throw 'MotionIntervalMs requires a new live capture, not a historical tick or pose capture.'
    }
    if ($MotionIntervalMs -lt 2500 -or $MotionIntervalMs -gt 30000) {
        throw 'MotionIntervalMs must be between 2500 and 30000.'
    }
    $motionTick = Request-LocalSnapshot -DelayMs ([uint64]$MotionIntervalMs)
    Request-RemoteSnapshot -TargetTick $motionTick
    Start-Sleep -Milliseconds ($MotionIntervalMs + 400)
    $hostMotionGame = Read-LocalSnapshot -TargetTick $motionTick
    $followerMotionGame = Read-RemoteSnapshot -TargetTick $motionTick
} elseif ($MotionSecondTick -ne 0) {
    if ($AtTick -eq 0) {
        throw 'MotionSecondTick requires AtTick for the first historical sample.'
    }
    $hostMotionGame = Read-LocalSnapshot -TargetTick $MotionSecondTick
    $followerMotionGame = Read-RemoteSnapshot -TargetTick $MotionSecondTick
}
$followerReferences = @{}
foreach ($reference in @($followerGame.nearbyReferences)) { $followerReferences[[uint32]$reference.formId] = $reference }
$referenceDifferences = foreach ($hostReference in @($hostGame.nearbyReferences)) {
    $formId = [uint32]$hostReference.formId
    if (-not $followerReferences.ContainsKey($formId)) { continue }
    $followerReference = $followerReferences[$formId]
    $positionError = Get-VectorDistance $hostReference.position $followerReference.position
    $rotationError = Get-VectorDistance $hostReference.rotation $followerReference.rotation
    $hostTargetError = if ($followerReference.hasHostPhysics) {
        Get-VectorDistance $hostReference.position $followerReference.hostPhysicsPosition
    } else { $null }
    if ($positionError -gt 0.1 -or $rotationError -gt 0.001) {
        [pscustomobject]@{
            formId = ('0x{0:X8}' -f $formId)
            positionError = $positionError
            rotationError = $rotationError
            hasHostPhysics = $followerReference.hasHostPhysics
            hostTargetError = $hostTargetError
            hostTargetAgeMs = $followerReference.hostPhysicsAgeMs
            hostTargetTick = $followerReference.hostPhysicsTick
        }
    }
}
$followerCarts = @{}
foreach ($cart in @($followerGame.introCartReferences)) { $followerCarts[[uint32]$cart.formId] = $cart }
$cartDifferences = foreach ($hostCart in @($hostGame.introCartReferences)) {
    if (-not $followerCarts.ContainsKey([uint32]$hostCart.formId)) { continue }
    $followerCart = $followerCarts[[uint32]$hostCart.formId]
    [pscustomobject]@{
        formId = ('0x{0:X8}' -f [uint32]$hostCart.formId)
        hostReferencePosition = $hostCart.position
        followerReferencePosition = $followerCart.position
        referenceError = Get-VectorDistance $hostCart.position $followerCart.position
        hostNodePosition = $hostCart.nodeWorldPosition
        followerNodePosition = $followerCart.nodeWorldPosition
        nodeError = Get-VectorDistance $hostCart.nodeWorldPosition $followerCart.nodeWorldPosition
        hostReferenceNodeError = Get-VectorDistance $hostCart.position $hostCart.nodeWorldPosition
        followerReferenceNodeError = Get-VectorDistance $followerCart.position $followerCart.nodeWorldPosition
        hostCollisionPresent = $hostCart.collisionObjectPresent
        followerCollisionPresent = $followerCart.collisionObjectPresent
        hostCollisionFlags = $hostCart.collisionFlags
        followerCollisionFlags = $followerCart.collisionFlags
        hostHavokBodyReadable = $hostCart.havokBodyReadable
        followerHavokBodyReadable = $followerCart.havokBodyReadable
        hostHavokWorldPresent = $hostCart.havokWorldPresent
        followerHavokWorldPresent = $followerCart.havokWorldPresent
        hostHavokMotionType = $hostCart.havokMotionType
        followerHavokMotionType = $followerCart.havokMotionType
        hostHavokTransformPosition = $hostCart.havokTransformPosition
        followerHavokTransformPosition = $followerCart.havokTransformPosition
        havokTransformError = Get-VectorDistance $hostCart.havokTransformPosition $followerCart.havokTransformPosition
        hostHavokLinearVelocity = $hostCart.havokLinearVelocity
        followerHavokLinearVelocity = $followerCart.havokLinearVelocity
        hostMotionSamples = $hostCart.motionSamples
        followerMotionSamples = $followerCart.motionSamples
        hostPeakFrameStep = $hostCart.peakFrameStep
        followerPeakFrameStep = $followerCart.peakFrameStep
        hostPeakFrameSpeed = $hostCart.peakFrameSpeed
        followerPeakFrameSpeed = $followerCart.peakFrameSpeed
        hostLargeFrameSteps = $hostCart.largeFrameSteps
        followerLargeFrameSteps = $followerCart.largeFrameSteps
    }
}

$followerPoses = @{}
foreach ($pose in @($followerGame.actorPoseDiagnostics)) { $followerPoses[[uint32]$pose.formId] = $pose }
$poseComparisons = foreach ($hostPose in @($hostGame.actorPoseDiagnostics)) {
    $formId = [uint32]$hostPose.formId
    if (-not $followerPoses.ContainsKey($formId)) { continue }
    $followerPose = $followerPoses[$formId]
    $poseError = Get-PoseError $hostPose.evaluatedLocalPose $followerPose.evaluatedLocalPose
    $rootError = if ($hostPose.renderRoot.readable -and $followerPose.renderRoot.readable) {
        Get-VectorDistance $hostPose.renderRoot.worldT $followerPose.renderRoot.worldT
    } else { $null }
    $followerLocalSamples = @{}
    foreach ($sample in @($followerPose.renderLocalSamples)) { $followerLocalSamples[[int]$sample.index] = $sample }
    $followerWorldSamples = @{}
    foreach ($sample in @($followerPose.renderWorldSamples)) { $followerWorldSamples[[int]$sample.index] = $sample }
    $matchingRenderLocals = 0
    $comparedRenderLocals = 0
    $maxRenderWorldTranslationError = 0.0
    $maxRootRelativeRenderWorldTranslationError = 0.0
    $comparedRootRelativeRenderWorldSamples = 0
    foreach ($sample in @($hostPose.renderLocalSamples)) {
        $index = [int]$sample.index
        if (-not $followerLocalSamples.ContainsKey($index)) { continue }
        $other = $followerLocalSamples[$index]
        $comparedRenderLocals++
        $localError = [double](Get-VectorDistance $sample.t $other.t)
        for ($j = 0; $j -lt 9; $j++) {
            $localError = [math]::Max($localError, [math]::Abs([double]$sample.r[$j] - [double]$other.r[$j]))
        }
        $localError = [math]::Max($localError, [math]::Abs([double]$sample.s - [double]$other.s))
        if ($localError -le 0.001) { $matchingRenderLocals++ }
    }
    foreach ($sample in @($hostPose.renderWorldSamples)) {
        $index = [int]$sample.index
        if (-not $followerWorldSamples.ContainsKey($index)) { continue }
        $other = $followerWorldSamples[$index]
        $maxRenderWorldTranslationError = [math]::Max($maxRenderWorldTranslationError,
            [double](Get-VectorDistance $sample.t $other.t))
        if ($hostPose.renderRoot.readable -and $followerPose.renderRoot.readable) {
            $hostRelative = for ($j = 0; $j -lt 3; $j++) {
                [double]$sample.t[$j] - [double]$hostPose.renderRoot.worldT[$j]
            }
            $followerRelative = for ($j = 0; $j -lt 3; $j++) {
                [double]$other.t[$j] - [double]$followerPose.renderRoot.worldT[$j]
            }
            $maxRootRelativeRenderWorldTranslationError = [math]::Max(
                $maxRootRelativeRenderWorldTranslationError,
                [double](Get-VectorDistance $hostRelative $followerRelative))
            $comparedRootRelativeRenderWorldSamples++
        }
    }
    [pscustomobject]@{
        formId = ('0x{0:X8}' -f $formId)
        graphDescriptorMatches = $hostPose.graphDescriptor -eq $followerPose.graphDescriptor
        graphTransitionMatches = $hostPose.transitionObservables.signature -eq $followerPose.transitionObservables.signature
        evaluatedPoseReadable = $hostPose.evaluatedLocalPose.readable -and $followerPose.evaluatedLocalPose.readable
        evaluatedPoseMatches = $hostPose.evaluatedLocalPose.readable -and $followerPose.evaluatedLocalPose.readable -and
            $hostPose.evaluatedLocalPose.count -eq $followerPose.evaluatedLocalPose.count -and
            $hostPose.evaluatedLocalPose.checksum -eq $followerPose.evaluatedLocalPose.checksum
        skeletonChecksumMatches = $hostPose.skeleton.checksum -eq $followerPose.skeleton.checksum
        ragdollChecksumMatches = $hostPose.ragdoll.bodyChecksum -eq $followerPose.ragdoll.bodyChecksum
        hostBoneCount = $hostPose.skeleton.boneCount
        followerBoneCount = $followerPose.skeleton.boneCount
        hostEvaluatedPoseCount = $hostPose.evaluatedLocalPose.count
        followerEvaluatedPoseCount = $followerPose.evaluatedLocalPose.count
        evaluatedPoseError = $poseError
        renderRootPositionError = $rootError
        matchingRenderLocalSamples = $matchingRenderLocals
        comparedRenderLocalSamples = $comparedRenderLocals
        maxRenderWorldTranslationError = $maxRenderWorldTranslationError
        maxRootRelativeRenderWorldTranslationError = $maxRootRelativeRenderWorldTranslationError
        comparedRootRelativeRenderWorldSamples = $comparedRootRelativeRenderWorldSamples
        hostRagdollBodyCount = $hostPose.ragdoll.bodyCount
        followerRagdollBodyCount = $followerPose.ragdoll.bodyCount
    }
}
$followerNetworkEntities = @{}
$followerVisualSamples = @{}
$followerEntityIndex = 0
foreach ($entity in @($followerGame.networkEntities)) {
    if ($entity.authority -eq 'remote') {
        # Temporary FF form IDs are allocated independently by each game.
        # Server network IDs identify the same actor across peers.
        $followerNetworkEntities[[uint32]$entity.networkId] = $entity
        $followerVisualSamples[[uint32]$entity.networkId] = $followerEntityIndex -lt 48
    }
    $followerEntityIndex++
}
$hostVisualSamples = @{}
$hostFormToNetwork = @{}
$hostEntityIndex = 0
foreach ($entity in @($hostGame.networkEntities)) {
    $hostVisualSamples[[uint32]$entity.networkId] = $hostEntityIndex -lt 48
    $hostFormToNetwork[[uint32]$entity.formId] = [uint32]$entity.networkId
    $hostEntityIndex++
}
$followerFormToNetwork = @{}
foreach ($entity in @($followerGame.networkEntities)) {
    $followerFormToNetwork[[uint32]$entity.formId] = [uint32]$entity.networkId
}
$hostMotionByNetwork = @{}
$followerMotionByNetwork = @{}
foreach ($entity in @($hostMotionGame.networkEntities)) {
    if ($entity.authority -eq 'local') {
        $hostMotionByNetwork[[uint32]$entity.networkId] = $entity
    }
}
foreach ($entity in @($followerMotionGame.networkEntities)) {
    if ($entity.authority -eq 'remote') {
        $followerMotionByNetwork[[uint32]$entity.networkId] = $entity
    }
}
$poseTransportComparisons = foreach ($hostEntity in @($hostGame.networkEntities)) {
    if ($hostEntity.authority -ne 'local' -or $hostEntity.poseBoneCount -le 0) { continue }
    $followerEntity = $followerNetworkEntities[[uint32]$hostEntity.networkId]
    if ($null -eq $followerEntity -or $followerEntity.authority -ne 'remote') { continue }
    [pscustomobject]@{
        formId = ('0x{0:X8}' -f [uint32]$hostEntity.formId)
        followerFormId = ('0x{0:X8}' -f [uint32]$followerEntity.formId)
        networkId = $hostEntity.networkId
        hostSourceTick = $hostEntity.poseSourceTick
        followerSourceTick = $followerEntity.poseSourceTick
        hostBoneCount = $hostEntity.poseBoneCount
        followerBoneCount = $followerEntity.poseBoneCount
        sameSourceTick = $hostEntity.poseSourceTick -eq $followerEntity.poseSourceTick
        checksumMatches = $hostEntity.poseSourceTick -eq $followerEntity.poseSourceTick -and
            $hostEntity.poseBoneCount -eq $followerEntity.poseBoneCount -and
            $hostEntity.poseChecksum -eq $followerEntity.poseChecksum
    }
}
$visualTransportComparisons = foreach ($hostEntity in @($hostGame.networkEntities)) {
    if ($hostEntity.authority -ne 'local' -or $hostEntity.visualBoneCount -le 0) { continue }
    $followerEntity = $followerNetworkEntities[[uint32]$hostEntity.networkId]
    if ($null -eq $followerEntity -or $followerEntity.authority -ne 'remote') { continue }
    [pscustomobject]@{
        formId = ('0x{0:X8}' -f [uint32]$hostEntity.formId)
        followerFormId = ('0x{0:X8}' -f [uint32]$followerEntity.formId)
        networkId = $hostEntity.networkId
        hostSourceTick = $hostEntity.visualSourceTick
        followerSourceTick = $followerEntity.visualSourceTick
        hostBoneCount = $hostEntity.visualBoneCount
        followerBoneCount = $followerEntity.visualBoneCount
        hostRootPresent = $hostEntity.visualRootPresent
        followerRootPresent = $followerEntity.visualRootPresent
        hostRootWorldT = $hostEntity.visualRootWorldT
        followerRootWorldT = $followerEntity.visualRootWorldT
        rootPacketPositionError = if ($hostEntity.visualRootPresent -and $followerEntity.visualRootPresent -and
            $hostEntity.visualSourceTick -eq $followerEntity.visualSourceTick) {
            Get-VectorDistance $hostEntity.visualRootWorldT $followerEntity.visualRootWorldT
        } else { $null }
        sameSourceTick = $hostEntity.visualSourceTick -eq $followerEntity.visualSourceTick
        checksumMatches = $hostEntity.visualSourceTick -eq $followerEntity.visualSourceTick -and
            $hostEntity.visualBoneCount -eq $followerEntity.visualBoneCount -and
            $hostEntity.visualChecksum -eq $followerEntity.visualChecksum
    }
}
$followerScenes = @{}
foreach ($scene in @($followerGame.quests.scenes)) { $followerScenes[[uint32]$scene.formId] = $scene }
$sceneComparisons = foreach ($hostScene in @($hostGame.quests.scenes)) {
    $formId = [uint32]$hostScene.formId
    if (-not $followerScenes.ContainsKey($formId)) { continue }
    $followerScene = $followerScenes[$formId]
    if ($hostScene.playing -or $followerScene.playing) {
        [pscustomobject]@{
            formId = ('0x{0:X8}' -f $formId)
            hostPlaying = $hostScene.playing
            followerPlaying = $followerScene.playing
            hostPhase = $hostScene.rawPhaseWord
            followerPhase = $followerScene.rawPhaseWord
            phaseMatches = $hostScene.rawPhaseWord -eq $followerScene.rawPhaseWord
            hostPhaseEligibleActions = @($hostScene.phaseEligibleActions)
            followerPhaseEligibleActions = @($followerScene.phaseEligibleActions)
        }
    }
}

$followerActiveScenes = @{}
foreach ($scene in @($followerGame.activeScenes)) { $followerActiveScenes[[uint32]$scene.sceneId] = $scene }
$activeSceneComparisons = foreach ($hostScene in @($hostGame.activeScenes)) {
    $sceneId = [uint32]$hostScene.sceneId
    $followerScene = $followerActiveScenes[$sceneId]
    [pscustomobject]@{
        sceneId = ('0x{0:X8}' -f $sceneId)
        questId = ('0x{0:X8}' -f [uint32]$hostScene.questId)
        presentOnFollower = $null -ne $followerScene
        hostPhase = $hostScene.rawPhaseWord
        followerPhase = if ($null -ne $followerScene) { $followerScene.rawPhaseWord } else { $null }
        phaseMatches = $null -ne $followerScene -and $hostScene.rawPhaseWord -eq $followerScene.rawPhaseWord
        actionCountMatches = $null -ne $followerScene -and $hostScene.actionCount -eq $followerScene.actionCount
        actionSignatureMatches = $null -ne $followerScene -and
            $hostScene.actionSignature -eq $followerScene.actionSignature
        hostReadableActions = $hostScene.readableActions
        followerReadableActions = if ($null -ne $followerScene) { $followerScene.readableActions } else { $null }
    }
}
$hostActiveSceneIds = @{}
foreach ($scene in @($hostGame.activeScenes)) { $hostActiveSceneIds[[uint32]$scene.sceneId] = $true }
$followerOnlyScenes = @($followerGame.activeScenes | Where-Object { -not $hostActiveSceneIds.ContainsKey([uint32]$_.sceneId) } |
    ForEach-Object { '0x{0:X8}' -f [uint32]$_.sceneId })

$report = [pscustomobject]@{
    capturedAt = (Get-Date).ToString('o')
    sampling = [pscustomobject]@{
        hostWorldTick = $hostGame.worldTick
        followerWorldTick = $followerGame.worldTick
        tickDifferenceMs = [int64]$followerGame.worldTick - [int64]$hostGame.worldTick
        hostSampleTimeMs = $hostGame.sampleTimeMs
        followerSampleTimeMs = $followerGame.sampleTimeMs
    }
    campaign = [pscustomobject]@{
        matches = $hostGame.session.campaignId -eq $followerGame.session.campaignId
        host = $hostGame.session.campaignId; follower = $followerGame.session.campaignId
        hostAuthorityEpoch = $hostGame.session.authorityEpoch; followerAuthorityEpoch = $followerGame.session.authorityEpoch
    }
    poseTransport = [pscustomobject]@{
        hostOwnedSamples = @($poseTransportComparisons).Count
        sameTickSamples = @($poseTransportComparisons | Where-Object sameSourceTick).Count
        matchingChecksums = @($poseTransportComparisons | Where-Object checksumMatches).Count
        comparisons = @($poseTransportComparisons)
    }
    visualTransport = [pscustomobject]@{
        hostOwnedSamples = @($visualTransportComparisons).Count
        sameTickSamples = @($visualTransportComparisons | Where-Object sameSourceTick).Count
        matchingChecksums = @($visualTransportComparisons | Where-Object checksumMatches).Count
        comparisons = @($visualTransportComparisons)
    }
    graphUpdateTrace = [pscustomobject]@{
        host = $hostGame.graphUpdateTrace
        follower = $followerGame.graphUpdateTrace
        hostActorHoldersObserved = @($hostGame.networkEntities | Where-Object { $_.graphPostCallMs -gt 0 }).Count
        followerActorHoldersObserved = @($followerGame.networkEntities | Where-Object { $_.graphPostCallMs -gt 0 }).Count
        hostActorHoldersRecent = @($hostGame.networkEntities | Where-Object { $_.graphPostCallMs -gt 0 -and $_.graphPostAgeMs -le 200 }).Count
        followerActorHoldersRecent = @($followerGame.networkEntities | Where-Object { $_.graphPostCallMs -gt 0 -and $_.graphPostAgeMs -le 200 }).Count
        hostThreadIds = @($hostGame.networkEntities | Where-Object { $_.graphPostCallMs -gt 0 } |
            Select-Object -ExpandProperty graphPostThreadId -Unique)
        followerThreadIds = @($followerGame.networkEntities | Where-Object { $_.graphPostCallMs -gt 0 } |
            Select-Object -ExpandProperty graphPostThreadId -Unique)
    }
    visualPoseMailbox = [pscustomobject]@{
        host = $hostGame.visualPoseMailbox
        follower = $followerGame.visualPoseMailbox
    }
    presentationDelayMs = [pscustomobject]@{
        host = $hostGame.presentationDelayMs
        follower = $followerGame.presentationDelayMs
    }
    mountDiagnostic = [pscustomobject]@{
        host = $hostGame.mountDiagnostic
        follower = $followerGame.mountDiagnostic
    }
    mountRelations = [pscustomobject]@{
        host = @($hostGame.mountRelations)
        follower = @($followerGame.mountRelations)
    }
    combatTargetAuthorityTrial = [pscustomobject]@{
        host = $hostGame.combatTargetAuthorityTrial
        follower = $followerGame.combatTargetAuthorityTrial
    }
    cinematicSpawnActors = @($hostGame.networkEntities | Where-Object {
        $_.authority -eq 'local' -and $_.has3D
    } | ForEach-Object {
        $remote = $followerNetworkEntities[[uint32]$_.networkId]
        if ($null -eq $remote) { return }
        # NiTObjectArray::length includes vacant slots left by detach/re-equip.
        # Compare actual present geometry nodes, independent of insertion order.
        $hostChildNames = @($_.rootChildren | Where-Object present | ForEach-Object name | Sort-Object)
        $followerChildNames = @($remote.rootChildren | Where-Object present | ForEach-Object name | Sort-Object)
        $hostVisualSampled = if ($null -ne $_.visualGeometrySampled) {
            [bool]$_.visualGeometrySampled
        } else { $hostVisualSamples[[uint32]$_.networkId] }
        $followerVisualSampled = if ($null -ne $remote.visualGeometrySampled) {
            [bool]$remote.visualGeometrySampled
        } else { $followerVisualSamples[[uint32]$_.networkId] }
        $hostArmorSampled = if ($null -ne $_.wornArmorSampled) {
            [bool]$_.wornArmorSampled
        } else { $hostVisualSampled }
        $followerArmorSampled = if ($null -ne $remote.wornArmorSampled) {
            [bool]$remote.wornArmorSampled
        } else { $followerVisualSampled }
        $rootGeometryObserved = $hostVisualSampled -and $followerVisualSampled -and
            @($_.rootChildren).Count -gt 0 -and @($remote.rootChildren).Count -gt 0
        $hostNestedSignatures = @($_.rootChildren | Where-Object present | ForEach-Object {
            '{0}|{1}|{2}' -f $_.name, $_.nestedPresentCount,
                (@($_.nestedNames | Sort-Object) -join ';')
        } | Sort-Object)
        $followerNestedSignatures = @($remote.rootChildren | Where-Object present | ForEach-Object {
            '{0}|{1}|{2}' -f $_.name, $_.nestedPresentCount,
                (@($_.nestedNames | Sort-Object) -join ';')
        } | Sort-Object)
        $nestedGeometryObserved = $rootGeometryObserved -and
            @($_.rootChildren | Where-Object nestedReadable).Count -gt 0 -and
            @($remote.rootChildren | Where-Object nestedReadable).Count -gt 0
        [pscustomobject]@{
            formId = ('0x{0:X8}' -f [uint32]$_.formId)
            followerFormId = ('0x{0:X8}' -f [uint32]$remote.formId)
            networkId = $_.networkId
            hostVisualSampled = $hostVisualSampled
            followerVisualSampled = $followerVisualSampled
            wornArmorObserved = $hostArmorSampled -and $followerArmorSampled
            hostCellId = $_.cellId
            followerCellId = $remote.cellId
            hostPosition = @($_.position)
            followerPosition = @($remote.position)
            hostPackageFormId = $_.packageFormId
            followerPackageFormId = $remote.packageFormId
            hostCombatTargetFormId = $_.combatTargetFormId
            followerCombatTargetFormId = $remote.combatTargetFormId
            hostAiFollowFormId = $_.aiFollowFormId
            followerAiFollowFormId = $remote.aiFollowFormId
            hostAiTargetFormId = $_.aiTargetFormId
            followerAiTargetFormId = $remote.aiTargetFormId
            hostHeadtrackFormId = $_.headtrackFormId
            followerHeadtrackFormId = $remote.headtrackFormId
            hostHeadtrackReadable = $_.headtrackReadable
            followerHeadtrackReadable = $remote.headtrackReadable
            hostDialogueTargetFormId = $_.dialogueTargetFormId
            followerDialogueTargetFormId = $remote.dialogueTargetFormId
            hostProcessLevel = $_.processLevel
            followerProcessLevel = $remote.processLevel
            hostLastActionId = $_.lastActionId
            followerLastActionId = $remote.lastActionId
            hostLastActionTargetId = $_.lastActionTargetId
            followerLastActionTargetId = $remote.lastActionTargetId
            hostLastActionEvent = $_.lastActionEvent
            followerLastActionEvent = $remote.lastActionEvent
            hostLastActionTick = $_.lastActionTick
            followerLastActionTick = $remote.lastActionTick
            hostLastActionType = $_.lastActionType
            followerLastActionType = $remote.lastActionType
            hostOwnerProcessedTick = $_.ownerProcessedTick
            followerRemoteRanTick = $remote.remoteRanTick
            followerRemoteProcessedTick = $remote.remoteProcessedTick
            hostOwnerProcessedEvent = $_.ownerProcessedEvent
            followerRemoteRanEvent = $remote.remoteRanEvent
            hostNativeGraphReady = $_.nativeGraphReady
            followerNativeGraphReady = $remote.nativeGraphReady
            hostNativeStateId = $_.nativeStateId
            followerNativeStateId = $remote.nativeStateId
            hostNativeTimeInState = $_.nativeTimeInState
            followerNativeTimeInState = $remote.nativeTimeInState
            hostNativePoseCount = $_.nativePoseCount
            followerNativePoseCount = $remote.nativePoseCount
            hostNativePoseChecksum = $_.nativePoseChecksum
            followerNativePoseChecksum = $remote.nativePoseChecksum
            hostGraphVariables = $_.graphVariables
            followerGraphVariables = $remote.graphVariables
            hostActionPipeline = $_.actionPipeline
            followerActionPipeline = $remote.actionPipeline
            followerCombatTargetPipeline = $remote.combatTargetPipeline
            followerMovementGraphInputs = $remote.movementGraphInputs
            hostRenderBoneCount = $_.renderBoneCount
            followerRenderBoneCount = $remote.renderBoneCount
            hostRenderBoneChecksum = $_.renderBoneChecksum
            followerRenderBoneChecksum = $remote.renderBoneChecksum
            hostRenderWorldBoneChecksum = $_.renderWorldBoneChecksum
            followerRenderWorldBoneChecksum = $remote.renderWorldBoneChecksum
            hostGraphPostCallMs = $_.graphPostCallMs
            followerGraphPostCallMs = $remote.graphPostCallMs
            hostGraphPostAgeMs = $_.graphPostAgeMs
            followerGraphPostAgeMs = $remote.graphPostAgeMs
            hostGraphPostThreadId = $_.graphPostThreadId
            followerGraphPostThreadId = $remote.graphPostThreadId
            hostMailbox = $_.mailbox
            followerMailbox = $remote.mailbox
            presentRootChildCountHost = $hostChildNames.Count
            presentRootChildCountFollower = $followerChildNames.Count
            rootGeometryObserved = $rootGeometryObserved
            rootChildNamesMatch = $rootGeometryObserved -and (($hostChildNames -join "`n") -ceq ($followerChildNames -join "`n"))
            nestedGeometryObserved = $nestedGeometryObserved
            nestedGeometryMatches = $nestedGeometryObserved -and (($hostNestedSignatures -join "`n") -ceq ($followerNestedSignatures -join "`n"))
            host = [pscustomobject]@{ has3D = $_.has3D; rootChildCount = $_.rootChildCount; rootChildren = @($_.rootChildren); wornArmorCount = $_.wornArmorCount; nativeMountFormId = $_.nativeMountFormId; horseExtra = $_.horseExtra; horseHandle = $_.horseHandle; interactionExtra = $_.interactionExtra; interactionPointerPresent = $_.interactionPointerPresent; interactionActorHandle = $_.interactionActorHandle; interactionTargetHandle = $_.interactionTargetHandle; waitingFor3D = $_.waitingFor3D }
            follower = [pscustomobject]@{ has3D = $remote.has3D; rootChildCount = $remote.rootChildCount; rootChildren = @($remote.rootChildren); wornArmorCount = $remote.wornArmorCount; nativeMountFormId = $remote.nativeMountFormId; horseExtra = $remote.horseExtra; horseHandle = $remote.horseHandle; interactionExtra = $remote.interactionExtra; interactionPointerPresent = $remote.interactionPointerPresent; interactionActorHandle = $remote.interactionActorHandle; interactionTargetHandle = $remote.interactionTargetHandle; waitingFor3D = $remote.waitingFor3D }
        }
    })
    loading = [pscustomobject]@{
        matches = $hostGame.ui.loading -eq $followerGame.ui.loading
        host = $hostGame.ui.loading; follower = $followerGame.ui.loading
        hostPresentation = $hostGame.loadingPresentation; followerPresentation = $followerGame.loadingPresentation
    }
    window = [pscustomobject]@{ host = $hostGame.window; follower = $followerGame.window }
    camera = [pscustomobject]@{
        host = $hostGame.camera; follower = $followerGame.camera
        rootPositionError = Get-VectorDistance $hostGame.camera.rootWorldPosition $followerGame.camera.rootWorldPosition
        hostAuthority = $hostGame.cameraAuthority
        followerAuthority = $followerGame.cameraAuthority
    }
    playerAnimation = [pscustomobject]@{
        descriptorMatches = $hostGame.playerAnimation.graphDescriptor -eq $followerGame.playerAnimation.graphDescriptor
        eventMatches = $hostGame.playerAnimation.event -eq $followerGame.playerAnimation.event
        hostEvent = $hostGame.playerAnimation.event; followerEvent = $followerGame.playerAnimation.event
        hostPose = $hostGame.actorPoseDiagnostics; followerPose = $followerGame.actorPoseDiagnostics
        hostPoseSampleTick = $hostGame.actorPoseSampleTick
        followerPoseSampleTick = $followerGame.actorPoseSampleTick
        poseSampleTickDifferenceMs = if ($hostGame.actorPoseSampleTick -and $followerGame.actorPoseSampleTick) {
            [math]::Abs([long]$hostGame.actorPoseSampleTick - [long]$followerGame.actorPoseSampleTick)
        } else { $null }
        hostPoseProbeDurationUs = $hostGame.actorPoseProbeDurationUs
        followerPoseProbeDurationUs = $followerGame.actorPoseProbeDurationUs
        overlapCount = @($poseComparisons).Count
        graphDescriptorMatchCount = @($poseComparisons | Where-Object graphDescriptorMatches).Count
        graphTransitionMatchCount = @($poseComparisons | Where-Object graphTransitionMatches).Count
        evaluatedPoseReadableCount = @($poseComparisons | Where-Object evaluatedPoseReadable).Count
        evaluatedPoseMatchCount = @($poseComparisons | Where-Object evaluatedPoseMatches).Count
        skeletonChecksumMatchCount = @($poseComparisons | Where-Object skeletonChecksumMatches).Count
        ragdollChecksumMatchCount = @($poseComparisons | Where-Object ragdollChecksumMatches).Count
        poseComparisons = @($poseComparisons)
    }
    bodyPlaybackProbe = [pscustomobject]@{
        host = $hostGame.bodyPlaybackProbe
        follower = $followerGame.bodyPlaybackProbe
    }
    referencePhaseProbe = [pscustomobject]@{
        host = $hostGame.referencePhaseProbe
        follower = $followerGame.referencePhaseProbe
    }
    nativeCrashGuard = [pscustomobject]@{
        host = $hostGame.nativeCrashGuard
        follower = $followerGame.nativeCrashGuard
    }
    collisionSyncProbe = [pscustomobject]@{
        host = $hostGame.collisionSyncProbe
        follower = $followerGame.collisionSyncProbe
    }
    collisionWorldProbe = [pscustomobject]@{
        host = $hostGame.collisionWorldProbe
        follower = $followerGame.collisionWorldProbe
    }
    worldUpdateProbe = [pscustomobject]@{
        host = $hostGame.worldUpdateProbe
        follower = $followerGame.worldUpdateProbe
    }
    gameLoopTiming = [pscustomobject]@{
        host = $hostGame.gameLoopTiming
        follower = $followerGame.gameLoopTiming
    }
    snapshotProfileUs = [pscustomobject]@{
        host = $hostGame.snapshotProfileUs
        follower = $followerGame.snapshotProfileUs
    }
    preStepPlaybackProbe = [pscustomobject]@{
        host = $hostGame.preStepPlaybackProbe
        follower = $followerGame.preStepPlaybackProbe
    }
    renderNodePhaseProbe = [pscustomobject]@{
        host = $hostGame.renderNodePhaseProbe
        follower = $followerGame.renderNodePhaseProbe
    }
    quest = [pscustomobject]@{
        host = $hostGame.quests; follower = $followerGame.quests
        activeSceneComparisons = @($sceneComparisons)
        activeScenes = @($activeSceneComparisons)
        followerOnlyActiveScenes = $followerOnlyScenes
    }
    playerState = [pscustomobject]@{
        host = $hostGame.player
        follower = $followerGame.player
        hostAnimation = $hostGame.playerAnimation
        followerAnimation = $followerGame.playerAnimation
        hostInventory = $hostGame.playerInventory
        followerInventory = $followerGame.playerInventory
        hostControls = $hostGame.controls
        followerControls = $followerGame.controls
    }
    triggerEvents = [pscustomobject]@{
        host = @(Convert-TriggerEvents $hostGame)
        follower = @(Convert-TriggerEvents $followerGame)
    }
    nativeCameraUpdateTrace = [pscustomobject]@{
        host = @($hostGame.nativeCameraUpdateTrace)
        follower = @($followerGame.nativeCameraUpdateTrace)
    }
    papyrusNativeDispatch = [pscustomobject]@{
        host = $hostGame.papyrusNativeDispatch
        follower = $followerGame.papyrusNativeDispatch
    }
    references = [pscustomobject]@{
        hostObserved = @($hostGame.nearbyReferences).Count; followerObserved = @($followerGame.nearbyReferences).Count
        divergentCount = @($referenceDifferences).Count
        largestDifferences = @($referenceDifferences | Sort-Object positionError -Descending | Select-Object -First $ReferenceLimit)
    }
    introCarts = @($cartDifferences)
    recentHitchMotionEvents = [pscustomobject]@{
        host = @($hostGame.recentHitchMotionEvents)
        follower = @($followerGame.recentHitchMotionEvents)
    }
}

# One compact, server-ID-keyed actor audit for triage. Local FF form IDs are
# retained only as per-machine labels; they are never used to join peers.
$actorStateAudit = @($report.cinematicSpawnActors | Sort-Object networkId | ForEach-Object {
    $actor = $_
    $hostRootNames = @($actor.host.rootChildren | Where-Object present | ForEach-Object name | Sort-Object)
    $followerRootNames = @($actor.follower.rootChildren | Where-Object present | ForEach-Object name | Sort-Object)
    $rootDifference = if ($actor.rootGeometryObserved) {
        @(Compare-Object -ReferenceObject $hostRootNames -DifferenceObject $followerRootNames)
    } else { @() }
    $hostNested = @($actor.host.rootChildren | Where-Object present | ForEach-Object {
        '{0}|{1}|{2}' -f $_.name, $_.nestedPresentCount,
            (@($_.nestedNames | Sort-Object) -join ';')
    } | Sort-Object)
    $followerNested = @($actor.follower.rootChildren | Where-Object present | ForEach-Object {
        '{0}|{1}|{2}' -f $_.name, $_.nestedPresentCount,
            (@($_.nestedNames | Sort-Object) -join ';')
    } | Sort-Object)
    $nestedDifference = if ($actor.nestedGeometryObserved) {
        @(Compare-Object -ReferenceObject $hostNested -DifferenceObject $followerNested)
    } else { @() }
    $positionError = Get-VectorDistance $actor.hostPosition $actor.followerPosition
    $hostCombatTarget = Resolve-ActorTarget ([uint32]$actor.hostCombatTargetFormId) $hostFormToNetwork
    $followerCombatTarget = Resolve-ActorTarget ([uint32]$actor.followerCombatTargetFormId) $followerFormToNetwork
    $hostAiFollow = Resolve-ActorTarget ([uint32]$actor.hostAiFollowFormId) $hostFormToNetwork
    $followerAiFollow = Resolve-ActorTarget ([uint32]$actor.followerAiFollowFormId) $followerFormToNetwork
    $hostAiTarget = Resolve-ActorTarget ([uint32]$actor.hostAiTargetFormId) $hostFormToNetwork
    $followerAiTarget = Resolve-ActorTarget ([uint32]$actor.followerAiTargetFormId) $followerFormToNetwork
    $hostHeadtrack = Resolve-ActorTarget ([uint32]$actor.hostHeadtrackFormId) $hostFormToNetwork
    $followerHeadtrack = Resolve-ActorTarget ([uint32]$actor.followerHeadtrackFormId) $followerFormToNetwork
    $hostDialogueTarget = Resolve-ActorTarget ([uint32]$actor.hostDialogueTargetFormId) $hostFormToNetwork
    $followerDialogueTarget = Resolve-ActorTarget ([uint32]$actor.followerDialogueTargetFormId) $followerFormToNetwork
    $hostActionTarget = Resolve-ActorTarget ([uint32]$actor.hostLastActionTargetId) $hostFormToNetwork
    $followerActionTarget = Resolve-ActorTarget ([uint32]$actor.followerLastActionTargetId) $followerFormToNetwork
    $hostLater = $hostMotionByNetwork[[uint32]$actor.networkId]
    $followerLater = $followerMotionByNetwork[[uint32]$actor.networkId]
    $motionObserved = $null -ne $hostLater -and $null -ne $followerLater -and
        $null -ne $actor.hostNativePoseChecksum -and $null -ne $actor.followerNativePoseChecksum
    $hostNativePoseChanged = $motionObserved -and $actor.hostNativePoseCount -gt 0 -and
        $hostLater.nativePoseCount -eq $actor.hostNativePoseCount -and
        $hostLater.nativePoseChecksum -ne $actor.hostNativePoseChecksum
    $followerNativePoseChanged = $motionObserved -and $actor.followerNativePoseCount -gt 0 -and
        $followerLater.nativePoseCount -eq $actor.followerNativePoseCount -and
        $followerLater.nativePoseChecksum -ne $actor.followerNativePoseChecksum
    $hostRenderPoseChanged = $motionObserved -and $actor.hostRenderBoneCount -gt 0 -and
        $hostLater.renderBoneCount -eq $actor.hostRenderBoneCount -and
        $hostLater.renderBoneChecksum -ne $actor.hostRenderBoneChecksum
    $followerRenderPoseChanged = $motionObserved -and $actor.followerRenderBoneCount -gt 0 -and
        $followerLater.renderBoneCount -eq $actor.followerRenderBoneCount -and
        $followerLater.renderBoneChecksum -ne $actor.followerRenderBoneChecksum
    $hostRenderWorldChanged = $motionObserved -and $actor.hostRenderBoneCount -gt 0 -and
        $hostLater.renderWorldBoneChecksum -ne $actor.hostRenderWorldBoneChecksum
    $followerRenderWorldChanged = $motionObserved -and $actor.followerRenderBoneCount -gt 0 -and
        $followerLater.renderWorldBoneChecksum -ne $actor.followerRenderWorldBoneChecksum
    $hostGraphPostAdvanced = $motionObserved -and
        $hostLater.graphPostCallMs -gt $actor.hostGraphPostCallMs
    $followerGraphPostAdvanced = $motionObserved -and
        $followerLater.graphPostCallMs -gt $actor.followerGraphPostCallMs
    $graphVariableDifferences = [System.Collections.Generic.List[object]]::new()
    $receivedGraphInputDifferences = [System.Collections.Generic.List[object]]::new()
    $variableNames = $graphVariableNames[[int]$actor.hostGraphVariables.count]
    if ($actor.hostGraphVariables -and $actor.followerGraphVariables -and
        $actor.hostGraphVariables.count -eq $actor.followerGraphVariables.count) {
        $hostValues = @($actor.hostGraphVariables.values)
        $followerValues = @($actor.followerGraphVariables.values)
        for ($variableIndex = 0;
            $variableIndex -lt [Math]::Min($hostValues.Count, $followerValues.Count);
            $variableIndex++) {
            if ($hostValues[$variableIndex] -ne $followerValues[$variableIndex]) {
                $graphVariableDifferences.Add([pscustomobject]@{
                    index = $variableIndex
                    name = if ($variableNames -and $variableNames.ContainsKey($variableIndex)) {
                        $variableNames[$variableIndex]
                    } else { 'unknown' }
                    hostRaw = $hostValues[$variableIndex]
                    followerRaw = $followerValues[$variableIndex]
                })
            }
        }
    }
    if ($actor.followerMovementGraphInputs -and $actor.followerGraphVariables) {
        $followerNativeValues = @($actor.followerGraphVariables.values)
        foreach ($inputValue in @($actor.followerMovementGraphInputs.values)) {
            $inputIndex = [int]$inputValue.index
            if ($inputIndex -ge 0 -and $inputIndex -lt $followerNativeValues.Count -and
                [uint32]$inputValue.raw -ne [uint32]$followerNativeValues[$inputIndex]) {
                $receivedGraphInputDifferences.Add([pscustomobject]@{
                    index = $inputIndex
                    name = if ($variableNames -and $variableNames.ContainsKey($inputIndex)) {
                        $variableNames[$inputIndex]
                    } else { 'unknown' }
                    receivedRaw = [uint32]$inputValue.raw
                    followerNativeRaw = [uint32]$followerNativeValues[$inputIndex]
                })
            }
        }
    }
    $hostMotionUnits = if ($motionObserved) {
        Get-VectorDistance $actor.hostPosition $hostLater.position
    } else { $null }
    $followerMotionUnits = if ($motionObserved) {
        Get-VectorDistance $actor.followerPosition $followerLater.position
    } else { $null }
    $mailboxPublishDelta = if ($motionObserved -and $actor.followerMailbox -and
        $followerLater.mailbox -and $null -ne $followerLater.mailbox.ownerPublishCount) {
        [long]$followerLater.mailbox.ownerPublishCount - [long]$actor.followerMailbox.ownerPublishCount
    } else { $null }
    $mailboxInspectDelta = if ($motionObserved -and $actor.followerMailbox -and
        $followerLater.mailbox -and $null -ne $followerLater.mailbox.ownerInspectCount) {
        [long]$followerLater.mailbox.ownerInspectCount - [long]$actor.followerMailbox.ownerInspectCount
    } else { $null }
    $mailboxApplyDelta = if ($motionObserved -and $actor.followerMailbox -and
        $followerLater.mailbox -and $null -ne $followerLater.mailbox.ownerApplyCount) {
        [long]$followerLater.mailbox.ownerApplyCount - [long]$actor.followerMailbox.ownerApplyCount
    } else { $null }
    $mailboxEvictionDelta = if ($motionObserved -and $actor.followerMailbox -and
        $followerLater.mailbox -and $null -ne $followerLater.mailbox.slotOwnerEvictions) {
        [long]$followerLater.mailbox.slotOwnerEvictions - [long]$actor.followerMailbox.slotOwnerEvictions
    } else { $null }
    $mailboxMissDelta = if ($motionObserved -and $actor.followerMailbox -and
        $followerLater.mailbox -and $null -ne $followerLater.mailbox.slotInspectMisses) {
        [long]$followerLater.mailbox.slotInspectMisses - [long]$actor.followerMailbox.slotInspectMisses
    } else { $null }
    $domains = [System.Collections.Generic.List[string]]::new()
    if ($actor.host.has3D -ne $actor.follower.has3D) { $domains.Add('root-3d') }
    if ($actor.wornArmorObserved -and
        $actor.host.wornArmorCount -ne $actor.follower.wornArmorCount) { $domains.Add('worn-armor') }
    if ($actor.rootGeometryObserved -and -not $actor.rootChildNamesMatch) { $domains.Add('root-mesh') }
    if ($actor.nestedGeometryObserved -and -not $actor.nestedGeometryMatches) { $domains.Add('nested-mesh') }
    if ($actor.host.nativeMountFormId -ne $actor.follower.nativeMountFormId) { $domains.Add('native-mount') }
    if ($actor.host.interactionExtra -ne $actor.follower.interactionExtra) { $domains.Add('interaction') }
    if ($null -ne $actor.hostCellId -and $null -ne $actor.followerCellId -and
        $actor.hostCellId -ne $actor.followerCellId) { $domains.Add('cell') }
    if ($null -ne $positionError -and $positionError -gt 5) { $domains.Add('position') }
    if ($null -ne $actor.hostPackageFormId -and $null -ne $actor.followerPackageFormId -and
        $actor.hostPackageFormId -ne $actor.followerPackageFormId) { $domains.Add('package-diagnostic') }
    if ($hostCombatTarget -ne 'unresolved-temp' -and $followerCombatTarget -ne 'unresolved-temp' -and
        $hostCombatTarget -ne $followerCombatTarget) { $domains.Add('combat-target') }
    if ($hostAiFollow -ne 'unresolved-temp' -and $followerAiFollow -ne 'unresolved-temp' -and
        $hostAiFollow -ne $followerAiFollow) { $domains.Add('ai-follow-target') }
    if ($hostAiTarget -ne 'unresolved-temp' -and $followerAiTarget -ne 'unresolved-temp' -and
        $hostAiTarget -ne $followerAiTarget) { $domains.Add('ai-target') }
    if ($actor.hostHeadtrackReadable -and $actor.followerHeadtrackReadable -and
        $hostHeadtrack -ne 'unresolved-temp' -and $followerHeadtrack -ne 'unresolved-temp' -and
        $hostHeadtrack -ne $followerHeadtrack) { $domains.Add('headtrack-target') }
    if ($hostDialogueTarget -ne 'unresolved-temp' -and $followerDialogueTarget -ne 'unresolved-temp' -and
        $hostDialogueTarget -ne $followerDialogueTarget) { $domains.Add('dialogue-target') }
    if ($null -ne $actor.hostProcessLevel -and $null -ne $actor.followerProcessLevel -and
        $actor.hostProcessLevel -ne $actor.followerProcessLevel) { $domains.Add('process-level-diagnostic') }
    if ($hostActionTarget -ne 'unresolved-temp' -and $followerActionTarget -ne 'unresolved-temp' -and
        $hostActionTarget -ne $followerActionTarget) { $domains.Add('action-target-diagnostic') }
    if ($actor.hostNativeGraphReady -and $actor.followerNativeGraphReady -and
        $actor.hostNativeStateId -ne $actor.followerNativeStateId) { $domains.Add('native-state-diagnostic') }
    if ($graphVariableDifferences.Count -gt 0) { $domains.Add('graph-input-diagnostic') }
    if ($actor.hostNativePoseCount -gt 0 -and
        $actor.hostNativePoseCount -eq $actor.followerNativePoseCount -and
        $actor.hostNativePoseChecksum -ne $actor.followerNativePoseChecksum) {
        $domains.Add('native-pose-diagnostic')
    }
    if ($actor.hostRenderBoneCount -gt 0 -and
        $actor.hostRenderBoneCount -eq $actor.followerRenderBoneCount -and
        $actor.hostRenderBoneChecksum -ne $actor.followerRenderBoneChecksum) {
        $domains.Add('render-pose-diagnostic')
    }
    if ($null -ne $actor.hostRenderWorldBoneChecksum -and
        $actor.hostRenderBoneCount -gt 0 -and
        $actor.hostRenderBoneCount -eq $actor.followerRenderBoneCount -and
        $actor.hostRenderWorldBoneChecksum -ne $actor.followerRenderWorldBoneChecksum) {
        $domains.Add('render-world-pose-diagnostic')
    }
    if ($actor.followerMailbox -and $actor.followerNativeGraphReady) {
        if (-not $actor.followerMailbox.slotOwnerMatches -or
            -not $actor.followerMailbox.frameMatchesActor) {
            $domains.Add('mailbox-frame-missing-diagnostic')
        } elseif ($actor.followerMailbox.receiptAgeMs -gt 1500 -or
            $actor.followerMailbox.lastSkipReason -in 1,5) {
            $domains.Add('mailbox-receipt-stale-diagnostic')
        }
        if ($actor.followerMailbox.frameMatchesActor -and
            -not $actor.followerMailbox.presentationBracketed) {
            $domains.Add('mailbox-no-bracket-diagnostic')
        }
        if ($report.visualPoseMailbox.follower.applyEnabled -and
            $actor.followerMailbox.frameMatchesActor -and
            $actor.followerMailbox.lastWrittenBones -eq 0) {
            $domains.Add('mailbox-no-write-diagnostic')
        }
    }
    if ($hostNativePoseChanged -and -not $followerNativePoseChanged) {
        $domains.Add('motion-stagnant-native-pose-diagnostic')
    }
    if ($hostRenderPoseChanged -and -not $followerRenderPoseChanged) {
        $domains.Add('motion-stagnant-render-pose-diagnostic')
    }
    if ($hostRenderWorldChanged -and -not $followerRenderWorldChanged) {
        $domains.Add('motion-stagnant-render-world-diagnostic')
    }
    if ($null -ne $hostMotionUnits -and $null -ne $followerMotionUnits -and
        $hostMotionUnits -gt 5 -and $followerMotionUnits -lt 0.5) {
        $domains.Add('motion-stagnant-position-diagnostic')
    }
    [pscustomobject]@{
        networkId = $actor.networkId
        hostFormId = $actor.formId
        followerFormId = $actor.followerFormId
        mismatchDomains = @($domains.ToArray())
        hostHas3D = $actor.host.has3D
        followerHas3D = $actor.follower.has3D
        hostWornArmorCount = $actor.host.wornArmorCount
        followerWornArmorCount = $actor.follower.wornArmorCount
        wornArmorObserved = $actor.wornArmorObserved
        hostCellId = $actor.hostCellId
        followerCellId = $actor.followerCellId
        hostPosition = $actor.hostPosition
        followerPosition = $actor.followerPosition
        positionError = $positionError
        hostPackageFormId = $actor.hostPackageFormId
        followerPackageFormId = $actor.followerPackageFormId
        hostCombatTarget = $hostCombatTarget
        followerCombatTarget = $followerCombatTarget
        hostAiFollow = $hostAiFollow
        followerAiFollow = $followerAiFollow
        hostAiTarget = $hostAiTarget
        followerAiTarget = $followerAiTarget
        hostHeadtrack = $hostHeadtrack
        followerHeadtrack = $followerHeadtrack
        hostDialogueTarget = $hostDialogueTarget
        followerDialogueTarget = $followerDialogueTarget
        hostProcessLevel = $actor.hostProcessLevel
        followerProcessLevel = $actor.followerProcessLevel
        hostActionTarget = $hostActionTarget
        followerActionTarget = $followerActionTarget
        hostLastActionId = $actor.hostLastActionId
        followerLastActionId = $actor.followerLastActionId
        hostLastActionEvent = $actor.hostLastActionEvent
        followerLastActionEvent = $actor.followerLastActionEvent
        hostLastActionTick = $actor.hostLastActionTick
        followerLastActionTick = $actor.followerLastActionTick
        hostOwnerProcessedTick = $actor.hostOwnerProcessedTick
        followerRemoteRanTick = $actor.followerRemoteRanTick
        followerRemoteProcessedTick = $actor.followerRemoteProcessedTick
        hostOwnerProcessedEvent = $actor.hostOwnerProcessedEvent
        followerRemoteRanEvent = $actor.followerRemoteRanEvent
        hostNativeGraphReady = $actor.hostNativeGraphReady
        followerNativeGraphReady = $actor.followerNativeGraphReady
        hostNativeStateId = $actor.hostNativeStateId
        followerNativeStateId = $actor.followerNativeStateId
        hostNativeTimeInState = $actor.hostNativeTimeInState
        followerNativeTimeInState = $actor.followerNativeTimeInState
        hostNativePoseCount = $actor.hostNativePoseCount
        followerNativePoseCount = $actor.followerNativePoseCount
        hostNativePoseChecksum = $actor.hostNativePoseChecksum
        followerNativePoseChecksum = $actor.followerNativePoseChecksum
        hostGraphVariableCount = $actor.hostGraphVariables.count
        followerGraphVariableCount = $actor.followerGraphVariables.count
        hostGraphVariableChecksum = $actor.hostGraphVariables.checksum
        followerGraphVariableChecksum = $actor.followerGraphVariables.checksum
        graphVariableDifferenceCount = $graphVariableDifferences.Count
        graphVariableDifferences = @($graphVariableDifferences | Select-Object -First 16)
        hostActionPipeline = $actor.hostActionPipeline
        followerActionPipeline = $actor.followerActionPipeline
        followerCombatTargetPipeline = $actor.followerCombatTargetPipeline
        receivedGraphInputSourceTick = $actor.followerMovementGraphInputs.sourceTick
        receivedGraphInputPresentationTick = $actor.followerMovementGraphInputs.presentationTick
        receivedGraphInputDifferenceCount = $receivedGraphInputDifferences.Count
        receivedGraphInputDifferences = @($receivedGraphInputDifferences | Select-Object -First 16)
        hostRenderBoneCount = $actor.hostRenderBoneCount
        followerRenderBoneCount = $actor.followerRenderBoneCount
        hostRenderBoneChecksum = $actor.hostRenderBoneChecksum
        followerRenderBoneChecksum = $actor.followerRenderBoneChecksum
        hostRenderWorldBoneChecksum = $actor.hostRenderWorldBoneChecksum
        followerRenderWorldBoneChecksum = $actor.followerRenderWorldBoneChecksum
        hostGraphPostCallMs = $actor.hostGraphPostCallMs
        followerGraphPostCallMs = $actor.followerGraphPostCallMs
        hostGraphPostAgeMs = $actor.hostGraphPostAgeMs
        followerGraphPostAgeMs = $actor.followerGraphPostAgeMs
        hostGraphPostThreadId = $actor.hostGraphPostThreadId
        followerGraphPostThreadId = $actor.followerGraphPostThreadId
        hostMailbox = $actor.hostMailbox
        followerMailbox = $actor.followerMailbox
        followerMailboxPublishDelta = $mailboxPublishDelta
        followerMailboxInspectDelta = $mailboxInspectDelta
        followerMailboxApplyDelta = $mailboxApplyDelta
        followerMailboxSlotEvictionDelta = $mailboxEvictionDelta
        followerMailboxSlotMissDelta = $mailboxMissDelta
        motionObserved = $motionObserved
        hostNativePoseChanged = $hostNativePoseChanged
        followerNativePoseChanged = $followerNativePoseChanged
        hostRenderPoseChanged = $hostRenderPoseChanged
        followerRenderPoseChanged = $followerRenderPoseChanged
        hostRenderWorldChanged = $hostRenderWorldChanged
        followerRenderWorldChanged = $followerRenderWorldChanged
        hostGraphPostAdvanced = $hostGraphPostAdvanced
        followerGraphPostAdvanced = $followerGraphPostAdvanced
        hostMotionUnits = $hostMotionUnits
        followerMotionUnits = $followerMotionUnits
        rootGeometryObserved = $actor.rootGeometryObserved
        hostPresentRootChildren = $actor.presentRootChildCountHost
        followerPresentRootChildren = $actor.presentRootChildCountFollower
        rootChildNamesMatch = $actor.rootChildNamesMatch
        rootMissingOnFollower = @($rootDifference | Where-Object SideIndicator -eq '<=' | ForEach-Object InputObject)
        rootExtraOnFollower = @($rootDifference | Where-Object SideIndicator -eq '=>' | ForEach-Object InputObject)
        nestedGeometryObserved = $actor.nestedGeometryObserved
        nestedGeometryMatches = $actor.nestedGeometryMatches
        nestedMissingOnFollower = @($nestedDifference | Where-Object SideIndicator -eq '<=' | ForEach-Object InputObject)
        nestedExtraOnFollower = @($nestedDifference | Where-Object SideIndicator -eq '=>' | ForEach-Object InputObject)
        hostNativeMountFormId = $actor.host.nativeMountFormId
        followerNativeMountFormId = $actor.follower.nativeMountFormId
    }
})
$report | Add-Member -NotePropertyName actorStateAudit -NotePropertyValue $actorStateAudit
$hostUntrackedActors = @($hostGame.nearbyActorAudit | Where-Object { -not $_.networked })
$followerUntrackedActors = @($followerGame.nearbyActorAudit | Where-Object { -not $_.networked })
$hostDuplicateIds = @($hostGame.nearbyActorAudit | Where-Object { $_.networked -and $_.networkId -gt 0 } |
    Group-Object networkId | Where-Object Count -gt 1 | ForEach-Object {
        [pscustomobject]@{
            networkId = [uint32]$_.Name
            formIds = @($_.Group | ForEach-Object { '0x{0:X8}' -f [uint32]$_.formId })
            baseIds = @($_.Group | ForEach-Object { '0x{0:X8}' -f [uint32]$_.baseId })
        }
    })
$followerDuplicateIds = @($followerGame.nearbyActorAudit | Where-Object { $_.networked -and $_.networkId -gt 0 } |
    Group-Object networkId | Where-Object Count -gt 1 | ForEach-Object {
        [pscustomobject]@{
            networkId = [uint32]$_.Name
            formIds = @($_.Group | ForEach-Object { '0x{0:X8}' -f [uint32]$_.formId })
            baseIds = @($_.Group | ForEach-Object { '0x{0:X8}' -f [uint32]$_.baseId })
        }
    })
$report | Add-Member -NotePropertyName nearbyActorCoverage -NotePropertyValue ([pscustomobject]@{
    scope = 'loaded current-cell actors within 8000 game units; capped at 256 per PC'
    hostObserved = @($hostGame.nearbyActorAudit).Count
    followerObserved = @($followerGame.nearbyActorAudit).Count
    hostUntrackedCount = $hostUntrackedActors.Count
    followerUntrackedCount = $followerUntrackedActors.Count
    hostUntrackedFormIds = @($hostUntrackedActors | ForEach-Object { '0x{0:X8}' -f [uint32]$_.formId })
    followerUntrackedFormIds = @($followerUntrackedActors | ForEach-Object { '0x{0:X8}' -f [uint32]$_.formId })
    hostDuplicateNetworkIds = $hostDuplicateIds
    followerDuplicateNetworkIds = $followerDuplicateIds
})
$deadActorAudit = @($hostGame.nearbyActorAudit | Where-Object { $_.dead -and $_.networked } | ForEach-Object {
    $hostDead = $_
    $followerDead = $followerGame.nearbyActorAudit | Where-Object { $_.networkId -eq $hostDead.networkId } | Select-Object -First 1
    [pscustomobject]@{
        networkId = $hostDead.networkId
        hostFormId = '0x{0:X8}' -f [uint32]$hostDead.formId
        followerFormId = if ($followerDead) { '0x{0:X8}' -f [uint32]$followerDead.formId } else { $null }
        followerPresent = $null -ne $followerDead
        followerDead = if ($followerDead) { [bool]$followerDead.dead } else { $null }
        nativePoseMatches = if ($followerDead) { $hostDead.nativePoseChecksum -eq $followerDead.nativePoseChecksum } else { $null }
        renderPoseMatches = if ($followerDead) { $hostDead.renderBoneChecksum -eq $followerDead.renderBoneChecksum } else { $null }
        hostNativePoseChecksum = $hostDead.nativePoseChecksum
        followerNativePoseChecksum = if ($followerDead) { $followerDead.nativePoseChecksum } else { $null }
    }
})
$report | Add-Member -NotePropertyName deadActorAudit -NotePropertyValue $deadActorAudit
$report | Add-Member -NotePropertyName motionSampling -NotePropertyValue ([pscustomobject]@{
    requested = $MotionIntervalMs -gt 0 -or $MotionSecondTick -gt 0
    hostFirstTick = $hostGame.worldTick
    followerFirstTick = $followerGame.worldTick
    hostSecondTick = if ($hostMotionGame) { $hostMotionGame.worldTick } else { $null }
    followerSecondTick = if ($followerMotionGame) { $followerMotionGame.worldTick } else { $null }
    hostFirstCameraRoot = $hostGame.camera.rootWorldPosition
    hostSecondCameraRoot = if ($hostMotionGame) { $hostMotionGame.camera.rootWorldPosition } else { $null }
    followerFirstCameraRoot = $followerGame.camera.rootWorldPosition
    followerSecondCameraRoot = if ($followerMotionGame) { $followerMotionGame.camera.rootWorldPosition } else { $null }
    hostFirstPlayerPosition = $hostGame.player.position
    hostSecondPlayerPosition = if ($hostMotionGame) { $hostMotionGame.player.position } else { $null }
    followerFirstPlayerPosition = $followerGame.player.position
    followerSecondPlayerPosition = if ($followerMotionGame) { $followerMotionGame.player.position } else { $null }
    hostCameraTravel = if ($hostMotionGame) { Get-VectorDistance $hostGame.camera.rootWorldPosition $hostMotionGame.camera.rootWorldPosition } else { $null }
    followerCameraTravel = if ($followerMotionGame) { Get-VectorDistance $followerGame.camera.rootWorldPosition $followerMotionGame.camera.rootWorldPosition } else { $null }
    hostPlayerTravel = if ($hostMotionGame) { Get-VectorDistance $hostGame.player.position $hostMotionGame.player.position } else { $null }
    followerPlayerTravel = if ($followerMotionGame) { Get-VectorDistance $followerGame.player.position $followerMotionGame.player.position } else { $null }
    actorPairsObserved = @($actorStateAudit | Where-Object motionObserved).Count
    hostPoseChangedFollowerStagnant = @($actorStateAudit | Where-Object {
        $_.motionObserved -and $_.hostNativePoseChanged -and -not $_.followerNativePoseChanged
    }).Count
    hostRenderChangedFollowerStagnant = @($actorStateAudit | Where-Object {
        $_.motionObserved -and $_.hostRenderPoseChanged -and -not $_.followerRenderPoseChanged
    }).Count
    hostRenderWorldChangedFollowerStagnant = @($actorStateAudit | Where-Object {
        $_.motionObserved -and $_.hostRenderWorldChanged -and -not $_.followerRenderWorldChanged
    }).Count
})

$parityFailures = [System.Collections.Generic.List[string]]::new()
if ($hostDuplicateIds.Count -gt 0 -or $followerDuplicateIds.Count -gt 0) {
    $parityFailures.Add('two local actors share one network ID')
}
if (@($deadActorAudit | Where-Object {
    $_.followerPresent -and $_.followerDead -and
    (-not $_.nativePoseMatches -or -not $_.renderPoseMatches)
}).Count -gt 0) {
    $parityFailures.Add('dead actor native or rendered poses differ')
}
if ($report.motionSampling.requested -and
    $null -ne $report.motionSampling.followerCameraTravel -and
    $report.motionSampling.followerCameraTravel -gt 1 -and
    $report.motionSampling.followerPlayerTravel -lt 0.1 -and
    $report.motionSampling.hostCameraTravel -lt 0.1) {
    $parityFailures.Add('stationary follower camera moves independently of player root')
}
if (-not $report.campaign.matches -or
    $report.campaign.hostAuthorityEpoch -ne $report.campaign.followerAuthorityEpoch) {
    $parityFailures.Add('campaign or authority epoch differs')
}
if ([Math]::Abs([double]$report.sampling.tickDifferenceMs) -gt 100) {
    $parityFailures.Add('matched samples are more than 100 ms apart')
}
if (@($activeSceneComparisons | Where-Object {
    -not $_.phaseMatches -or -not $_.actionSignatureMatches
}).Count -gt 0 -or $followerOnlyScenes.Count -gt 0) {
    $parityFailures.Add('active scene phase/action state differs')
}
if ($null -eq $report.camera.rootPositionError -or $report.camera.rootPositionError -gt 1) {
    $parityFailures.Add('camera roots differ by more than 1 game unit')
}
if (@($referenceDifferences | Where-Object { $_.positionError -gt 1 }).Count -gt 0) {
    $parityFailures.Add('loaded reference positions differ by more than 1 game unit')
}
if (@($report.cinematicSpawnActors | Where-Object {
    ($_.host.nativeMountFormId -ne 0 -and
        $_.host.nativeMountFormId -ne $_.follower.nativeMountFormId) -or
    ($_.host.interactionExtra -and -not $_.follower.interactionExtra)
}).Count -gt 0) {
    $parityFailures.Add('host mounted/interaction state is missing on follower actors')
}
if (@($report.cinematicSpawnActors | Where-Object {
    $_.rootGeometryObserved -and -not $_.rootChildNamesMatch
}).Count -gt 0) {
    $parityFailures.Add('actor root geometry children differ between host and follower')
}
if (@($report.cinematicSpawnActors | Where-Object {
    $_.nestedGeometryObserved -and -not $_.nestedGeometryMatches
}).Count -gt 0) {
    $parityFailures.Add('actor nested geometry children differ between host and follower')
}
if (@($poseComparisons).Count -eq 0) {
    $parityFailures.Add('actor pose probe has no overlapping actors')
} elseif (@($poseComparisons | Where-Object {
    -not $_.skeletonChecksumMatches -or -not $_.ragdollChecksumMatches
}).Count -gt 0) {
    $parityFailures.Add('actor bone or ragdoll checksums differ')
}
$report | Add-Member -NotePropertyName parityGate -NotePropertyValue ([pscustomobject]@{
    passed = $parityFailures.Count -eq 0
    failures = @($parityFailures)
})

$artifactDirectory = Join-Path $PSScriptRoot 'artifacts'
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$artifactPath = Join-Path $artifactDirectory ("authority-{0}.json" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $artifactPath -Encoding UTF8
$report | Add-Member -NotePropertyName artifactPath -NotePropertyValue $artifactPath
$report | ConvertTo-Json -Depth 12
if ($RequireParity -and $parityFailures.Count -gt 0) { exit 1 }
