[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
)

$ErrorActionPreference = 'Stop'

function Get-OrderedEntries([string]$Path, [switch]$StarsMeanEnabled) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $entries = @()
    foreach ($line in Get-Content -LiteralPath $Path) {
        $value = $line.Trim()
        if (-not $value -or $value.StartsWith('#')) { continue }
        $entries += [pscustomobject]@{
            Name = $value.TrimStart('*').ToLowerInvariant()
            Enabled = -not $StarsMeanEnabled -or $value.StartsWith('*')
        }
    }
    return @($entries)
}

function Test-SkippedPath([string]$Path) {
    if (-not $Path -or $Path.StartsWith('.git/')) { return $true }
    if ($Path -match '\.(log|tmp|dmp|sqlite3|sqlite3-wal|sqlite3-shm|bak)$') { return $true }
    if ($Path.StartsWith('skyrimsemultiplayerbackups/')) { return $true }
    if ($Path -match '^skyrimtogetherreborn/(backups|cache|data|debug-feedback|logs)/') { return $true }
    if ($Path -eq 'skyrimtogetherreborn/config/stserver.ini') { return $true }
    if ($Path -in @('skyrimtogetherreborn/crashpad_handler.exe', 'skyrimtogetherreborn/crashpad_wer.dll')) { return $true }
    if ($Path.StartsWith('skyrimtogetherreborn/') -and $Path.Contains('.pre-')) { return $true }
    return $Path -eq 'skyrimsemultiplayer.plugins.manifest'
}

$dataRoot = Join-Path $GameRoot 'Data'
$files = @()
foreach ($file in Get-ChildItem -LiteralPath $dataRoot -File -Recurse -ErrorAction SilentlyContinue) {
    $relative = $file.FullName.Substring($dataRoot.Length).TrimStart('\').Replace('\', '/').ToLowerInvariant()
    if (Test-SkippedPath $relative) { continue }
    $files += [pscustomobject]@{
        Path = $relative
        Length = $file.Length
        Hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
}

[pscustomobject]@{
    Plugins = @(Get-OrderedEntries (Join-Path $env:LOCALAPPDATA 'Skyrim Special Edition\plugins.txt') -StarsMeanEnabled)
    Creations = @(Get-OrderedEntries (Join-Path $GameRoot 'Skyrim.ccc'))
    Files = @($files | Sort-Object Path)
} | ConvertTo-Json -Depth 5 -Compress
