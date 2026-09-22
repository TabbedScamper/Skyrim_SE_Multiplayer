[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$InputDirectory,

    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'out')
)

$ErrorActionPreference = 'Stop'

$resolvedInput = (Resolve-Path -LiteralPath $InputDirectory).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null

# This is a triage pass, not a proof that a script is broken. We record only
# script names, operation classes, counts, and line numbers; decompiled game
# source never becomes part of the report.
$rules = @(
    [pscustomobject]@{ Name = 'local_player_identity'; Weight = 4; Pattern = '\bGame\.GetPlayer\s*\(' }
    [pscustomobject]@{ Name = 'quest_stage_write'; Weight = 5; Pattern = '\.(?:SetStage|SetObjectiveDisplayed|SetObjectiveCompleted|CompleteQuest|FailAllObjectives)\s*\(' }
    [pscustomobject]@{ Name = 'scene_control'; Weight = 4; Pattern = '\.(?:Start|Stop|ForceStart)\s*\(' }
    [pscustomobject]@{ Name = 'alias_mutation'; Weight = 5; Pattern = '\.(?:ForceRefTo|Clear|TryToEnable|TryToDisable|TryToMoveTo)\s*\(' }
    [pscustomobject]@{ Name = 'world_reference_mutation'; Weight = 3; Pattern = '\.(?:MoveTo|Enable|EnableNoWait|Disable|DisableNoWait|Delete|Kill|Resurrect|Activate|BlockActivation|SetOpen)\s*\(' }
    [pscustomobject]@{ Name = 'player_control_or_camera'; Weight = 6; Pattern = '\bGame\.(?:ShowRaceMenu|SetInChargen|EnablePlayerControls|DisablePlayerControls|SetPlayerAIDriven|ShowFirstPersonGeometry|ForceFirstPerson|ForceThirdPerson)\s*\(|\.SetVehicle\s*\(' }
    [pscustomobject]@{ Name = 'event_entry'; Weight = 2; Pattern = '^\s*Event\s+On(?:Activate|TriggerEnter|TriggerLeave|Hit|Death|Dying|ItemAdded|ItemRemoved|CombatStateChanged|LocationChange|CellAttach|CellDetach)\b' }
    [pscustomobject]@{ Name = 'dialogue_control'; Weight = 4; Pattern = '\.(?:Say|StartConversation|ForceGreet|SetDialogueWithPlayer)\s*\(' }
    [pscustomobject]@{ Name = 'latent_or_timer'; Weight = 1; Pattern = '\bUtility\.Wait(?:GameTime)?\s*\(|\bRegisterFor(?:Single)?Update(?:GameTime)?\s*\(' }
    [pscustomobject]@{ Name = 'save_request'; Weight = 6; Pattern = '\bGame\.Request(?:Auto)?Save\s*\(' }
)

$rows = [System.Collections.Generic.List[object]]::new()
$files = Get-ChildItem -LiteralPath $resolvedInput -Filter '*.psc' -File -Recurse | Sort-Object FullName

foreach ($file in $files) {
    $relativePath = $file.FullName.Substring($resolvedInput.Length).TrimStart('\', '/') -replace '\\', '/'
    $lines = Get-Content -LiteralPath $file.FullName
    $categoryResults = [ordered]@{}
    $score = 0

    foreach ($rule in $rules) {
        $matchingLines = [System.Collections.Generic.List[int]]::new()
        for ($index = 0; $index -lt $lines.Count; $index++) {
            if ($lines[$index] -match $rule.Pattern) {
                $matchingLines.Add($index + 1)
            }
        }

        if ($matchingLines.Count -gt 0) {
            $categoryResults[$rule.Name] = [ordered]@{
                count = $matchingLines.Count
                lines = @($matchingLines)
            }
            $score += $rule.Weight * [Math]::Min($matchingLines.Count, 5)
        }
    }

    if ($categoryResults.Count -eq 0) {
        continue
    }

    $riskBand = if ($score -ge 45) { 'critical' } elseif ($score -ge 25) { 'high' } elseif ($score -ge 12) { 'medium' } else { 'low' }
    $rows.Add([pscustomobject]@{
        script = $relativePath
        score = $score
        risk_band = $riskBand
        categories = $categoryResults
    })
}

$orderedRows = @($rows | Sort-Object @{ Expression = 'score'; Descending = $true }, script)
$jsonPath = Join-Path $resolvedOutput 'quest-script-risk.json'
$csvPath = Join-Path $resolvedOutput 'quest-script-risk.csv'
$summaryPath = Join-Path $resolvedOutput 'quest-script-summary.json'

$orderedRows | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
$orderedRows | ForEach-Object {
    [pscustomobject]@{
        script = $_.script
        score = $_.score
        risk_band = $_.risk_band
        categories = ($_.categories.Keys -join ';')
    }
} | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8

$summary = [ordered]@{
    schema_version = 1
    input_directory = $resolvedInput
    scanned_at_utc = [DateTime]::UtcNow.ToString('o')
    scripts_scanned = $files.Count
    scripts_flagged = $orderedRows.Count
    risk_bands = [ordered]@{
        critical = @($orderedRows | Where-Object risk_band -eq 'critical').Count
        high = @($orderedRows | Where-Object risk_band -eq 'high').Count
        medium = @($orderedRows | Where-Object risk_band -eq 'medium').Count
        low = @($orderedRows | Where-Object risk_band -eq 'low').Count
    }
    note = 'Static triage only. A finding identifies a multiplayer-sensitive seam and does not prove a defect.'
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

$summary | ConvertTo-Json -Depth 4

