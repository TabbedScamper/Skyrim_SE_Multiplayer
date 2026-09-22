$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$processHandler = Join-Path $repositoryRoot 'Code\tp_process\ProcessHandler.cpp'
$overlayClient = Join-Path $repositoryRoot 'Code\client\Services\Generic\OverlayClient.cpp'

$exported = Select-String -Path $processHandler -Pattern '(?:CreateFunction\(|SetValue\()"([A-Za-z][A-Za-z0-9_]*)"' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } |
    Sort-Object -Unique

$handled = Select-String -Path $overlayClient -Pattern 'eventName == "([A-Za-z][A-Za-z0-9_]*)"' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } |
    Sort-Object -Unique

$missing = Compare-Object $exported $handled |
    Where-Object SideIndicator -eq '=>' |
    Select-Object -ExpandProperty InputObject

if ($missing) {
    throw "UI commands handled by OverlayClient but missing from TPProcess exports: $($missing -join ', ')"
}

Write-Output "UI binding check passed: all $($handled.Count) native commands are exported to CEF."
