[CmdletBinding()]
param(
    [string]$ScriptName = 'QF_MQ101_0003372B',
    [string]$PexDirectory = (Join-Path $env:LOCALAPPDATA 'SkyrimSEMultiplayer\AnalysisTools\vanilla-1.7.104\misc-bsa\scripts'),
    [string]$PscPath = (Join-Path $env:LOCALAPPDATA 'SkyrimSEMultiplayer\AnalysisTools\vanilla-1.7.104\all-psc\qf_mq101_0003372b.psc'),
    [switch]$Compact
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$simulator = Join-Path $projectRoot 'build\windows\x64\releasedbg\SkyrimPapyrusSim.exe'
foreach ($required in @($simulator, $PexDirectory, $PscPath)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing Papyrus sweep dependency: $required" }
}

$functions = @(Select-String -LiteralPath $PscPath -Pattern '^Function (Fragment_[A-Za-z0-9_]+)\(' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
if ($functions.Count -eq 0) { throw "No fragment functions found in $PscPath" }

$results = foreach ($function in $functions) {
    $text = & $simulator $PexDirectory $ScriptName $function
    $exitCode = $LASTEXITCODE
    try { $result = $text | ConvertFrom-Json }
    catch { $result = [pscustomobject]@{ passed = $false; error = 'simulator emitted invalid JSON' } }
    [pscustomobject]@{
        function = $function
        passed = $exitCode -eq 0 -and [bool]$result.passed
        error = [string]$result.error
        eventCount = @($result.events).Count
        confidence = [string]$result.confidence
    }
}

$passedCount = @($results | Where-Object passed).Count
$summary = [ordered]@{
    schemaVersion = 1
    script = $ScriptName
    total = $results.Count
    passed = $passedCount
    failed = $results.Count - $passedCount
    executableCoveragePercent = [math]::Round(100 * $passedCount / $results.Count, 1)
    interpretation = 'Bytecode completed without missing direct calls or Papyrus VM runtime errors; conservative native shims are not engine-parity proof.'
    results = $results
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$artifactDirectory = Join-Path $PSScriptRoot "artifacts\papyrus-sweep-$stamp"
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$summaryPath = Join-Path $artifactDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$output = [ordered]@{ summary = $summary; artifactDirectory = $artifactDirectory }
if ($Compact) { $output | ConvertTo-Json -Depth 10 -Compress }
else { $output | ConvertTo-Json -Depth 10 }
if ($summary.failed -ne 0) { exit 1 }
