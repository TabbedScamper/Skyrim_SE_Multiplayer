[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition',
    [string]$PluginsPath = (Join-Path $env:LOCALAPPDATA 'Skyrim Special Edition\plugins.txt'),
    [string]$BackupStamp = (Get-Date -Format 'yyyyMMdd-HHmmss')
)

$ErrorActionPreference = 'Stop'

$togetherRoot = Join-Path $GameRoot 'Data\SkyrimTogetherReborn'
$serverData = Join-Path $togetherRoot 'Data'
$configPath = Join-Path $togetherRoot 'config\STServer.ini'
$creationPath = Join-Path $GameRoot 'Skyrim.ccc'
$backupPath = Join-Path $togetherRoot "backups\$BackupStamp-pre-strict-modcheck"

foreach ($requiredPath in @($configPath, $creationPath, $PluginsPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required deployment file is missing: $requiredPath"
    }
}

$enabledPlugins = Get-Content -LiteralPath $PluginsPath |
    Where-Object { $_ -match '^\*' } |
    ForEach-Object { $_.Substring(1).Trim() } |
    Where-Object { $_ }

$loadOrder = @(
    'Skyrim.esm'
    'Update.esm'
    'Dawnguard.esm'
    'HearthFires.esm'
    'Dragonborn.esm'
) + @(Get-Content -LiteralPath $creationPath | ForEach-Object { $_.Trim() } | Where-Object { $_ }) + @($enabledPlugins)

if (@($loadOrder | Select-Object -Unique).Count -ne $loadOrder.Count) {
    throw 'The canonical server load order contains duplicate plugin names.'
}

New-Item -ItemType Directory -Path $serverData -Force | Out-Null
New-Item -ItemType Directory -Path $backupPath -Force | Out-Null
Copy-Item -LiteralPath $configPath -Destination (Join-Path $backupPath 'STServer.ini') -Force

foreach ($name in @('loadorder.txt', 'SkyrimSEMultiplayer.plugins.manifest')) {
    $existingPath = Join-Path $serverData $name
    if (Test-Path -LiteralPath $existingPath -PathType Leaf) {
        Copy-Item -LiteralPath $existingPath -Destination (Join-Path $backupPath $name) -Force
    }
}

$utf8NoBom = [Text.UTF8Encoding]::new($false)
$loadOrderPath = Join-Path $serverData 'loadorder.txt'
[IO.File]::WriteAllLines($loadOrderPath, $loadOrder, $utf8NoBom)

$configText = [IO.File]::ReadAllText($configPath)
if ($configText -notmatch '(?m)^bEnableModCheck\s*=') {
    throw "The [ModPolicy] bEnableModCheck setting is missing from $configPath"
}
$configText = [regex]::Replace($configText, '(?m)^bEnableModCheck\s*=.*$', 'bEnableModCheck=true')
if ($configText -match '(?m)^bAllowManifestBootstrap\s*=') {
    $configText = [regex]::Replace($configText, '(?m)^bAllowManifestBootstrap\s*=.*$', 'bAllowManifestBootstrap=true')
} else {
    $configText = [regex]::Replace(
        $configText,
        '(?m)^(bEnableModCheck=true\r?\n)',
        "`$1bAllowManifestBootstrap=true`r`n")
}
[IO.File]::WriteAllText($configPath, $configText, $utf8NoBom)

[pscustomobject]@{
    GameRoot = $GameRoot
    LoadOrderEntries = $loadOrder.Count
    LoadOrderSha256 = (Get-FileHash -LiteralPath $loadOrderPath -Algorithm SHA256).Hash
    FirstPlugin = $loadOrder[0]
    LastPlugin = $loadOrder[-1]
    ModCheckEnabled = [bool]((Get-Content -LiteralPath $configPath) -match '^bEnableModCheck=true$')
    ManifestBootstrapEnabled = [bool]((Get-Content -LiteralPath $configPath) -match '^bAllowManifestBootstrap=true$')
    BackupPath = $backupPath
}
