[CmdletBinding()]
param(
    [string]$RemoteHost = '192.168.50.103',
    [string]$RemoteUser = 'eflem',
    [switch]$Launch
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$key = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$source = Join-Path $projectRoot 'build\windows\x64\releasedbg\SkyrimTogether.exe'
$helperSource = Join-Path $projectRoot 'build\windows\x64\releasedbg\GameTestKeyHelper.exe'
$serverSource = Join-Path $projectRoot 'build\windows\x64\releasedbg\STServer.dll'
$install = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\SkyrimTogetherReborn'
$destination = Join-Path $install 'SkyrimTogether.exe'
$helperDestination = Join-Path $install 'GameTestKeyHelper.exe'
$serverDestination = Join-Path $install 'STServer.dll'
$remoteTemp = 'C:/Users/eflem/AppData/Local/Temp/SkyrimTogether.next.exe'
$remoteHelperTemp = 'C:/Users/eflem/AppData/Local/Temp/GameTestKeyHelper.next.exe'
$remoteServerTemp = 'C:/Users/eflem/AppData/Local/Temp/STServer.next.dll'
$sshOptions = @('-i', $key, '-o', "UserKnownHostsFile=$knownHosts", '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes')

if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
    -not (Test-Path -LiteralPath $helperSource -PathType Leaf) -or
    -not (Test-Path -LiteralPath $destination -PathType Leaf) -or
    -not (Test-Path -LiteralPath $serverSource -PathType Leaf) -or
    -not (Test-Path -LiteralPath $serverDestination -PathType Leaf)) {
    throw 'Expected build or local game binary is missing.'
}
$expectedHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
$expectedHelperHash = (Get-FileHash -LiteralPath $helperSource -Algorithm SHA256).Hash
$expectedServerHash = (Get-FileHash -LiteralPath $serverSource -Algorithm SHA256).Hash

& scp.exe @sshOptions $source "${RemoteUser}@${RemoteHost}:$remoteTemp"
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the follower binary.' }
& scp.exe @sshOptions $helperSource "${RemoteUser}@${RemoteHost}:$remoteHelperTemp"
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the follower test helper.' }
& scp.exe @sshOptions $serverSource "${RemoteUser}@${RemoteHost}:$remoteServerTemp"
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the follower server DLL.' }

$remoteScript = @'
$ErrorActionPreference = 'Stop'
$install = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\SkyrimTogetherReborn'
$destination = Join-Path $install 'SkyrimTogether.exe'
$helperDestination = Join-Path $install 'GameTestKeyHelper.exe'
$serverDestination = Join-Path $install 'STServer.dll'
$staged = 'C:\Users\eflem\AppData\Local\Temp\SkyrimTogether.next.exe'
$helperStaged = 'C:\Users\eflem\AppData\Local\Temp\GameTestKeyHelper.next.exe'
$serverStaged = 'C:\Users\eflem\AppData\Local\Temp\STServer.next.dll'
if (-not (Test-Path -LiteralPath $destination -PathType Leaf) -or
    -not (Test-Path -LiteralPath $staged -PathType Leaf) -or
    -not (Test-Path -LiteralPath $helperStaged -PathType Leaf) -or
    -not (Test-Path -LiteralPath $serverDestination -PathType Leaf) -or
    -not (Test-Path -LiteralPath $serverStaged -PathType Leaf)) { throw 'Follower install or staged binary missing.' }
Get-Process SkyrimTogether, SkyrimTogetherServer -ErrorAction SilentlyContinue |
    Stop-Process -Force
Start-Sleep -Seconds 2
$backup = Join-Path $install ('SkyrimTogether.pre-authority-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.exe')
Copy-Item -LiteralPath $destination -Destination $backup
Copy-Item -LiteralPath $serverDestination -Destination (Join-Path $install ('STServer.pre-authority-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dll'))
for ($attempt = 0; $attempt -lt 6; $attempt++) {
    try { Copy-Item -LiteralPath $staged -Destination $destination -Force; break }
    catch {
        if ($attempt -eq 5) { throw }
        Start-Sleep -Seconds 2
    }
}
Copy-Item -LiteralPath $serverStaged -Destination $serverDestination -Force
Copy-Item -LiteralPath $helperStaged -Destination $helperDestination -Force
(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
(Get-FileHash -LiteralPath $serverDestination -Algorithm SHA256).Hash
(Get-FileHash -LiteralPath $helperDestination -Algorithm SHA256).Hash
'@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remoteScript))
$remoteOutput = & ssh.exe @sshOptions "$RemoteUser@$RemoteHost" powershell.exe -NoProfile -NonInteractive -EncodedCommand $encoded 2>$null
if ($LASTEXITCODE -ne 0) { throw 'Follower deployment failed.' }
$remoteHashes = @($remoteOutput | Where-Object { $_ -match '^[0-9A-F]{64}$' })
if ($remoteHashes.Count -ne 3 -or $remoteHashes[0] -ne $expectedHash -or
    $remoteHashes[1] -ne $expectedServerHash -or
    $remoteHashes[2] -ne $expectedHelperHash) { throw 'Follower deployment hash mismatch.' }

Get-Process SkyrimTogether, SkyrimTogetherServer -ErrorAction SilentlyContinue |
    Stop-Process -Force
Start-Sleep -Seconds 2
$backup = Join-Path $install ('SkyrimTogether.pre-authority-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.exe')
Copy-Item -LiteralPath $destination -Destination $backup
Copy-Item -LiteralPath $serverDestination -Destination (Join-Path $install ('STServer.pre-authority-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.dll'))
for ($attempt = 0; $attempt -lt 6; $attempt++) {
    try { Copy-Item -LiteralPath $source -Destination $destination -Force; break }
    catch {
        if ($attempt -eq 5) { throw }
        Start-Sleep -Seconds 2
    }
}
Copy-Item -LiteralPath $serverSource -Destination $serverDestination -Force
Copy-Item -LiteralPath $helperSource -Destination $helperDestination -Force
$localHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
$localServerHash = (Get-FileHash -LiteralPath $serverDestination -Algorithm SHA256).Hash
$localHelperHash = (Get-FileHash -LiteralPath $helperDestination -Algorithm SHA256).Hash
if ($localHash -ne $expectedHash -or $localServerHash -ne $expectedServerHash -or
    $localHelperHash -ne $expectedHelperHash) {
    throw 'Host deployment hash mismatch.'
}

if ($Launch) {
    $launchRemote = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes(
        "Start-ScheduledTask -TaskName 'SkyrimSeamlessCoop-InteractiveLaunch'"))
    & ssh.exe @sshOptions "$RemoteUser@$RemoteHost" powershell.exe -NoProfile -NonInteractive -EncodedCommand $launchRemote 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Follower interactive launch failed.' }
    Start-Process -FilePath $destination -WorkingDirectory $install
}

[pscustomobject]@{ hash = $expectedHash; serverHash = $expectedServerHash; helperHash = $expectedHelperHash; host = $destination; follower = "$RemoteUser@$RemoteHost"; launched = $Launch.IsPresent }
