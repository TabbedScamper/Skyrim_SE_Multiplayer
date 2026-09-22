[CmdletBinding()]
param(
    [string]$RemoteHost = '192.168.50.103',
    [string]$RemoteUser = 'eflem',
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition',
    [switch]$FailOnMismatch
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$keyPath = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$inventoryScript = Join-Path $PSScriptRoot 'Get-DeploymentInventory.ps1'
$artifactDirectory = Join-Path $PSScriptRoot ('artifacts\deployment-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$sshPath = Join-Path $env:WINDIR 'System32\OpenSSH\ssh.exe'
$scpPath = Join-Path $env:WINDIR 'System32\OpenSSH\scp.exe'
$remoteInventoryScript = 'C:/Users/eflem/AppData/Local/Temp/Get-DeploymentInventory.ps1'
$remoteInventoryWindowsPath = 'C:\Users\eflem\AppData\Local\Temp\Get-DeploymentInventory.ps1'
$sshOptions = @('-i', $keyPath, '-o', "UserKnownHostsFile=$knownHosts", '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')

foreach ($required in @($keyPath, $knownHosts, $inventoryScript, $sshPath, $scpPath)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing deployment-audit dependency: $required" }
}

& $scpPath @sshOptions $inventoryScript "${RemoteUser}@${RemoteHost}:$remoteInventoryScript"
if ($LASTEXITCODE -ne 0) { throw 'Could not deploy the read-only inventory helper.' }

$localJob = Start-Job -ScriptBlock {
    param($Script, $Root)
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Script -GameRoot $Root
} -ArgumentList $inventoryScript, $GameRoot

$remotePowerShell = "& '$remoteInventoryWindowsPath' -GameRoot '$($GameRoot.Replace("'", "''"))'"
$encodedRemoteCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remotePowerShell))
$remoteOutput = & $sshPath @sshOptions "${RemoteUser}@${RemoteHost}" `
    "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encodedRemoteCommand" 2>$null
if ($LASTEXITCODE -ne 0) { throw 'Follower deployment scan failed.' }

Wait-Job $localJob | Out-Null
$localOutput = Receive-Job $localJob
Remove-Job $localJob
$local = ($localOutput | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1) | ConvertFrom-Json
$remote = ($remoteOutput | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1) | ConvertFrom-Json

function Compare-Ordered($Left, $Right, [string]$Kind) {
    $differences = @()
    $maximum = [Math]::Max(@($Left).Count, @($Right).Count)
    for ($index = 0; $index -lt $maximum; ++$index) {
        $leftEntry = if ($index -lt @($Left).Count) { @($Left)[$index] } else { $null }
        $rightEntry = if ($index -lt @($Right).Count) { @($Right)[$index] } else { $null }
        if (-not $leftEntry -or -not $rightEntry -or $leftEntry.Name -ne $rightEntry.Name -or $leftEntry.Enabled -ne $rightEntry.Enabled) {
            $differences += [pscustomobject]@{ Kind=$Kind; Index=$index; Host=$leftEntry; Follower=$rightEntry }
        }
    }
    return @($differences)
}

$differences = @()
$differences += Compare-Ordered @($local.Plugins) @($remote.Plugins) 'plugin-order'
$differences += Compare-Ordered @($local.Creations) @($remote.Creations) 'creation-order'
$localFiles = @{}; foreach ($file in @($local.Files)) { $localFiles[[string]$file.Path] = $file }
$remoteFiles = @{}; foreach ($file in @($remote.Files)) { $remoteFiles[[string]$file.Path] = $file }
foreach ($path in @($localFiles.Keys + $remoteFiles.Keys | Sort-Object -Unique)) {
    $left = $localFiles[$path]; $right = $remoteFiles[$path]
    if (-not $left -or -not $right -or $left.Length -ne $right.Length -or $left.Hash -ne $right.Hash) {
        $differences += [pscustomobject]@{ Kind='file'; Path=$path; Host=$left; Follower=$right }
    }
}

$summary = [pscustomobject]@{
    passed = $differences.Count -eq 0
    hostFileCount = @($local.Files).Count
    followerFileCount = @($remote.Files).Count
    differenceCount = $differences.Count
    differences = @($differences)
    limitation = 'Physical Data view only; launch-time in-process scan remains authoritative for MO2/USVFS profiles.'
    artifactDirectory = $artifactDirectory
}
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $artifactDirectory 'summary.json') -Encoding UTF8
$summary | ConvertTo-Json -Depth 8
if ($FailOnMismatch -and -not $summary.passed) { exit 1 }
