[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ArtifactPath
)

$ErrorActionPreference = 'Stop'
$report = Get-Content -LiteralPath $ArtifactPath -Raw | ConvertFrom-Json
$rows = [Collections.Generic.List[object]]::new()
foreach ($side in @('host', 'follower')) {
    foreach ($actor in @($report.playerAnimation."${side}Pose")) {
        $transforms = @($actor.evaluatedLocalPose.transforms)
        foreach ($sample in @($actor.renderLocalSamples)) {
            $index = [int]$sample.index
            if ($index -lt 0 -or $index -ge $transforms.Count) { continue }
            $pose = $transforms[$index]
            $x = [double]$pose.q[0]; $y = [double]$pose.q[1]
            $z = [double]$pose.q[2]; $w = [double]$pose.q[3]
            $matrix = @(
                (1 - 2 * ($y*$y + $z*$z)), (2 * ($x*$y - $z*$w)), (2 * ($x*$z + $y*$w)),
                (2 * ($x*$y + $z*$w)), (1 - 2 * ($x*$x + $z*$z)), (2 * ($y*$z - $x*$w)),
                (2 * ($x*$z - $y*$w)), (2 * ($y*$z + $x*$w)), (1 - 2 * ($x*$x + $y*$y))
            )
            $translationError = 0.0
            for ($i = 0; $i -lt 3; $i++) {
                $translationError = [Math]::Max($translationError,
                    [Math]::Abs([double]$pose.t[$i] - [double]$sample.t[$i]))
            }
            $rotationError = 0.0
            for ($i = 0; $i -lt 9; $i++) {
                $rotationError = [Math]::Max($rotationError,
                    [Math]::Abs([double]$matrix[$i] - [double]$sample.r[$i]))
            }
            $scaleError = [Math]::Abs([double]$pose.s[0] - [double]$sample.s)
            $bestIndex = $index
            $bestCost = [double]::PositiveInfinity
            $bestTranslationError = [double]::PositiveInfinity
            $bestRotationError = [double]::PositiveInfinity
            for ($candidateIndex = 0; $candidateIndex -lt $transforms.Count; $candidateIndex++) {
                $candidate = $transforms[$candidateIndex]
                $cx = [double]$candidate.q[0]; $cy = [double]$candidate.q[1]
                $cz = [double]$candidate.q[2]; $cw = [double]$candidate.q[3]
                $candidateMatrix = @(
                    (1 - 2 * ($cy*$cy + $cz*$cz)), (2 * ($cx*$cy - $cz*$cw)), (2 * ($cx*$cz + $cy*$cw)),
                    (2 * ($cx*$cy + $cz*$cw)), (1 - 2 * ($cx*$cx + $cz*$cz)), (2 * ($cy*$cz - $cx*$cw)),
                    (2 * ($cx*$cz - $cy*$cw)), (2 * ($cy*$cz + $cx*$cw)), (1 - 2 * ($cx*$cx + $cy*$cy))
                )
                $candidateTranslationError = 0.0
                for ($i = 0; $i -lt 3; $i++) {
                    $candidateTranslationError = [Math]::Max($candidateTranslationError,
                        [Math]::Abs([double]$candidate.t[$i] - [double]$sample.t[$i]))
                }
                $candidateRotationError = 0.0
                for ($i = 0; $i -lt 9; $i++) {
                    $candidateRotationError = [Math]::Max($candidateRotationError,
                        [Math]::Abs([double]$candidateMatrix[$i] - [double]$sample.r[$i]))
                }
                $cost = $candidateRotationError + $candidateTranslationError / 100.0
                if ($cost -lt $bestCost) {
                    $bestCost = $cost
                    $bestIndex = $candidateIndex
                    $bestTranslationError = $candidateTranslationError
                    $bestRotationError = $candidateRotationError
                }
                if ($candidateTranslationError -le 0.001 -and
                    $candidateRotationError -le 0.001) { break }
            }
            $rows.Add([pscustomobject]@{
                side = $side
                formId = ('0x{0:X8}' -f [uint32]$actor.formId)
                index = $index
                translationError = $translationError
                rotationMatrixElementError = $rotationError
                scaleError = $scaleError
                bestPoseIndex = $bestIndex
                bestTranslationError = $bestTranslationError
                bestRotationError = $bestRotationError
            })
        }
    }
}

[pscustomobject]@{
    artifact = (Resolve-Path -LiteralPath $ArtifactPath).Path
    samples = $rows.Count
    exactTranslationWithin1e3 = @($rows | Where-Object translationError -le 0.001).Count
    exactRotationWithin1e3 = @($rows | Where-Object rotationMatrixElementError -le 0.001).Count
    exactScaleWithin1e3 = @($rows | Where-Object scaleError -le 0.001).Count
    maxTranslationError = ($rows | Measure-Object translationError -Maximum).Maximum
    maxRotationMatrixElementError = ($rows | Measure-Object rotationMatrixElementError -Maximum).Maximum
    maxScaleError = ($rows | Measure-Object scaleError -Maximum).Maximum
    byIndex = @($rows | Group-Object index | ForEach-Object {
        [pscustomobject]@{
            index = [int]$_.Name
            samples = $_.Count
            translationMatches = @($_.Group | Where-Object translationError -le 0.001).Count
            rotationMatches = @($_.Group | Where-Object rotationMatrixElementError -le 0.001).Count
            exactRemapMatches = @($_.Group | Where-Object {
                $_.bestTranslationError -le 0.001 -and $_.bestRotationError -le 0.001
            }).Count
            bestPoseIndexModes = @($_.Group | Group-Object bestPoseIndex |
                Sort-Object Count -Descending | Select-Object -First 3 |
                ForEach-Object { "{0}:{1}" -f $_.Name,$_.Count })
            maxTranslationError = ($_.Group | Measure-Object translationError -Maximum).Maximum
            maxRotationError = ($_.Group | Measure-Object rotationMatrixElementError -Maximum).Maximum
        }
    } | Sort-Object index)
    worst = @($rows | Sort-Object rotationMatrixElementError -Descending | Select-Object -First 8)
}
