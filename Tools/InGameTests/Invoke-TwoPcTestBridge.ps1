[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RequestJson,
    [ValidateSet('Both', 'Host', 'Follower')][string]$Target = 'Both',
    [string]$RemoteHost = '192.168.50.103',
    [string]$RemoteUser = 'eflem'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$key = Join-Path $projectRoot 'runtime\remote-access\eriana_deploy_ed25519'
$knownHosts = Join-Path $projectRoot 'runtime\remote-access\known_hosts'
$parsed = $RequestJson | ConvertFrom-Json
if (-not $parsed.command) { throw 'RequestJson must contain a command.' }

function Invoke-Local([string]$Json) {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.AutoFlush = $true
    try {
        $writer.WriteLine($Json)
        $line = $reader.ReadLine()
        if (-not $line) { throw 'Host bridge returned no response.' }
        return $line | ConvertFrom-Json
    } finally {
        $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
    }
}

function Invoke-Remote([string]$Json) {
    $escaped = $Json.Replace("'", "''")
    $script = @"
`$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
`$pipe.Connect(10000)
`$reader = [IO.StreamReader]::new(`$pipe)
`$writer = [IO.StreamWriter]::new(`$pipe)
`$writer.AutoFlush = `$true
try { `$writer.WriteLine('$escaped'); `$reader.ReadLine() }
finally { `$writer.Dispose(); `$reader.Dispose(); `$pipe.Dispose() }
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
    $output = & ssh.exe -i $key -o "UserKnownHostsFile=$knownHosts" -o StrictHostKeyChecking=yes `
        -o BatchMode=yes "${RemoteUser}@${RemoteHost}" powershell.exe -NoProfile -NonInteractive `
        -EncodedCommand $encoded 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'Follower bridge transport failed.' }
    $line = $output | Where-Object { $_ -match '^\{' } | Select-Object -Last 1
    if (-not $line) { throw 'Follower bridge returned no response.' }
    return $line | ConvertFrom-Json
}

if ($Target -ne 'Follower') {
    [pscustomobject]@{ role = 'Host'; response = (Invoke-Local $RequestJson) }
}
if ($Target -ne 'Host') {
    [pscustomobject]@{ role = 'Follower'; response = (Invoke-Remote $RequestJson) }
}
