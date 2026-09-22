[CmdletBinding()]
param(
    [string[]]$ScenarioPath,
    [int]$Port = 12579,
    [switch]$Compact
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$binaryDirectory = Join-Path $projectRoot 'build\windows\x64\releasedbg'
$serverSource = Join-Path $binaryDirectory 'SkyrimTogetherServer.exe'
$serverDllSource = Join-Path $binaryDirectory 'STServer.dll'
$botPath = Join-Path $binaryDirectory 'SkyrimProtocolBot.exe'
$papyrusSimPath = Join-Path $binaryDirectory 'SkyrimPapyrusSim.exe'

if (-not $ScenarioPath -or $ScenarioPath.Count -eq 0) {
    $ScenarioPath = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'scenarios') `
        -Filter '*.json' -File -Recurse | Sort-Object FullName | ForEach-Object FullName)
}

foreach ($required in @($serverSource, $serverDllSource, $botPath)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing built ModSim dependency: $required"
    }
}

$adapterModes = @{
    'protocol.join' = 'join'
    'protocol.leader-handoff' = 'leader-handoff'
    'protocol.follower-reconnect' = 'follower-reconnect'
    'protocol.quest-authority' = 'quest-authority-audit'
    'model.player-lifecycle' = 'player-lifecycle-model'
    'pex.mq101-fragment' = 'mq101-fragment'
}
$adapterAssertions = @{
    'protocol.join' = @('clients.converged', 'party.singleLeader')
    'protocol.leader-handoff' = @('party.singleLeader', 'party.leaderEquals')
    'protocol.follower-reconnect' = @('clients.converged', 'party.leaderEquals')
    'protocol.quest-authority' = @(
        'quest.committedStageEquals',
        'transaction.rejected',
        'clients.converged'
    )
    'model.player-lifecycle' = @(
        'player.stateEquals',
        'inventory.quantityEquals',
        'transaction.effectCountEquals'
    )
    'pex.mq101-fragment' = @(
        'quest.stagesEqual',
        'inventory.quantityEquals',
        'controls.enabledEquals',
        'trace.contains'
    )
}

function Read-ModSimScenario {
    param([Parameter(Mandatory)][string]$Path)

    $resolved = Resolve-Path -LiteralPath $Path
    $scenario = Get-Content -Raw -LiteralPath $resolved | ConvertFrom-Json
    foreach ($property in @('schemaVersion', 'name', 'adapter', 'seed', 'participants', 'actions', 'assertions')) {
        if ($null -eq $scenario.$property) { throw "$resolved is missing '$property'" }
    }
    if ($scenario.schemaVersion -ne 1) { throw "$resolved uses unsupported schema version $($scenario.schemaVersion)" }
    if (-not $adapterModes.ContainsKey([string]$scenario.adapter)) {
        throw "$resolved uses unsupported adapter '$($scenario.adapter)'"
    }
    if ($scenario.participants.Count -lt 2 -or $scenario.participants.Count -gt 8) {
        throw "$resolved must declare two to eight participants"
    }
    $participantIds = @($scenario.participants | ForEach-Object { [string]$_.id })
    if (($participantIds | Sort-Object -Unique).Count -ne $participantIds.Count) {
        throw "$resolved contains duplicate participant IDs"
    }
    if (@($scenario.participants | Where-Object role -eq 'leader').Count -ne 1) {
        throw "$resolved must declare exactly one initial leader"
    }
    foreach ($action in $scenario.actions) {
        if ([string]$action.actor -notin $participantIds) {
            throw "$resolved action '$($action.type)' refers to unknown actor '$($action.actor)'"
        }
    }
    foreach ($assertion in $scenario.assertions) {
        if ([string]$assertion.type -notin $adapterAssertions[[string]$scenario.adapter]) {
            throw "$resolved assertion '$($assertion.type)' is not implemented by adapter '$($scenario.adapter)'"
        }
    }
    if ($scenario.faults) {
        if ([int]$scenario.faults.maxDelayTicks -ne 0 -or
            [double]$scenario.faults.dropProbability -ne 0 -or
            [double]$scenario.faults.duplicateProbability -ne 0 -or
            [bool]$scenario.faults.allowReorder) {
            throw "$resolved requests network faults; the first production adapter does not implement them yet"
        }
    }

    if ($scenario.adapter -eq 'protocol.quest-authority') {
        $questActions = @($scenario.actions | Where-Object type -eq 'quest.setStage')
        if ($questActions.Count -ne 2 -or
            [string]$questActions[0].data.quest -ne 'Skyrim.esm:0003372B' -or
            [int]$questActions[0].data.stage -ne 10 -or
            [int]$questActions[1].data.stage -ne 20) {
            throw "$resolved does not match the quest parameters supported by the current production bot"
        }
    }

    [pscustomobject]@{ Path = [string]$resolved; Definition = $scenario }
}

$loadedScenarios = @($ScenarioPath | ForEach-Object { Read-ModSimScenario -Path $_ })
if (@($loadedScenarios | Where-Object { $_.Definition.adapter -eq 'pex.mq101-fragment' }).Count -gt 0 -and
    -not (Test-Path -LiteralPath $papyrusSimPath)) {
    throw "Missing built Papyrus simulation dependency: $papyrusSimPath"
}
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$artifactDirectory = Join-Path $PSScriptRoot "artifacts\modsim-$runStamp"
$serverDirectory = Join-Path $artifactDirectory 'server'
$configDirectory = Join-Path $serverDirectory 'config'
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
sServerName=ModSim Production Adapter
bPremiumMode=true
uMaxPlayerCount=8
uPort=$Port
"@ | Set-Content -LiteralPath (Join-Path $configDirectory 'STServer.ini') -Encoding ASCII

$server = $null
$results = @()
try {
    $server = Start-Process -FilePath (Join-Path $serverDirectory 'SkyrimTogetherServer.exe') `
        -WorkingDirectory $serverDirectory -WindowStyle Hidden -PassThru
    $serverLog = Join-Path $serverDirectory 'logs\STServerOut.log'
    $deadline = (Get-Date).AddSeconds(15)
    do {
        if ($server.HasExited) { throw "ModSim server exited with code $($server.ExitCode)" }
        $ready = Test-Path -LiteralPath $serverLog
        if ($ready) {
            $ready = [bool](Select-String -LiteralPath $serverLog -SimpleMatch "started on port $Port" -Quiet)
        }
        if (-not $ready) { Start-Sleep -Milliseconds 100 }
    } while (-not $ready -and (Get-Date) -lt $deadline)
    if (-not $ready) { throw "ModSim server did not listen on port $Port" }

    foreach ($loaded in $loadedScenarios) {
        $scenario = $loaded.Definition
        $mode = $adapterModes[[string]$scenario.adapter]
        if ($scenario.adapter -eq 'model.player-lifecycle') {
            $engineText = & node (Join-Path $PSScriptRoot 'src\player-lifecycle-model.mjs') $loaded.Path
        }
        elseif ($scenario.adapter -eq 'pex.mq101-fragment') {
            $execute = @($scenario.actions | Where-Object type -eq 'papyrus.execute')
            if ($execute.Count -ne 1) { throw "$($loaded.Path) must contain one papyrus.execute action" }
            $pexDirectory = Join-Path $env:LOCALAPPDATA 'SkyrimSEMultiplayer\AnalysisTools\vanilla-1.7.104\misc-bsa\scripts'
            if (-not (Test-Path -LiteralPath $pexDirectory)) { throw "Installed PEX corpus is missing: $pexDirectory" }
            $engineText = & $papyrusSimPath $pexDirectory `
                ([string]$execute[0].data.script) ([string]$execute[0].data.function)
        }
        else {
            $engineText = & $botPath "127.0.0.1:$Port" $mode
        }
        $engineExitCode = $LASTEXITCODE
        $engineResult = $engineText | ConvertFrom-Json
        $passed = $engineExitCode -eq 0 -and [bool]$engineResult.passed
        if ($engineResult.assertionResults) {
            $assertionResults = @($engineResult.assertionResults)
        }
        elseif ($scenario.adapter -eq 'pex.mq101-fragment') {
            $assertionResults = @($scenario.assertions | ForEach-Object {
                $assertion = $_
                $assertionPassed = $false
                switch ([string]$assertion.type) {
                    'quest.stagesEqual' {
                        $actual = [int[]]@($engineResult.questStages)
                        $expected = [int[]]@($assertion.data.stages)
                        $assertionPassed = ($actual -join ',') -ceq ($expected -join ',')
                    }
                    'inventory.quantityEquals' {
                        $property = $engineResult.playerInventory.PSObject.Properties[[string]$assertion.data.item]
                        $actual = if ($property) { [int]$property.Value } else { 0 }
                        $assertionPassed = $actual -eq [int]$assertion.data.quantity
                    }
                    'controls.enabledEquals' {
                        $assertionPassed = [bool]$engineResult.playerControlsEnabled -eq [bool]$assertion.data.enabled
                    }
                    'trace.contains' {
                        $assertionPassed = @($engineResult.events) -contains [string]$assertion.data.event
                    }
                }
                [ordered]@{
                    type = [string]$assertion.type
                    after = if ($null -eq $assertion.after) { $null } else { [int]$assertion.after }
                    passed = $assertionPassed
                    evidence = [string]$engineResult.detail
                }
            })
        }
        else {
            $assertionResults = @($scenario.assertions | ForEach-Object {
                [ordered]@{
                    type = [string]$_.type
                    after = if ($null -eq $_.after) { $null } else { [int]$_.after }
                    passed = $passed
                    evidence = [string]$engineResult.detail
                }
            })
        }
        $trace = [ordered]@{
            schemaVersion = 1
            scenario = [string]$scenario.name
            source = $loaded.Path
            seed = [long]$scenario.seed
            adapter = [string]$scenario.adapter
            productionMode = $mode
            actions = @($scenario.actions | Sort-Object at)
            assertions = $assertionResults
            engine = $engineResult
            passed = $passed -and @($assertionResults | Where-Object { -not $_.passed }).Count -eq 0
        }
        $safeName = ([string]$scenario.name -replace '[^A-Za-z0-9._-]', '-').Trim('-')
        $tracePath = Join-Path $artifactDirectory "$safeName.trace.json"
        $trace | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $tracePath -Encoding UTF8
        $results += [pscustomobject]@{
            name = [string]$scenario.name
            adapter = [string]$scenario.adapter
            seed = [long]$scenario.seed
            passed = [bool]$trace.passed
            detail = [string]$engineResult.detail
            trace = $tracePath
        }
    }
}
finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        $null = $server.WaitForExit(5000)
    }
}

$summary = [ordered]@{
    schemaVersion = 1
    passed = @($results | Where-Object { -not $_.passed }).Count -eq 0
    scenarioCount = $results.Count
    results = $results
    artifactDirectory = $artifactDirectory
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $artifactDirectory 'summary.json') -Encoding UTF8
if ($Compact) { $summary | ConvertTo-Json -Depth 10 -Compress }
else { $summary | ConvertTo-Json -Depth 10 }
if (-not $summary.passed) { exit 1 }
