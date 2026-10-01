[CmdletBinding()]
param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition',
    [string]$PluginsPath = (Join-Path $env:LOCALAPPDATA 'Skyrim Special Edition\plugins.txt'),
    [string]$SkyrimPrefsPath = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition\SkyrimPrefs.ini'),
    [string]$BackupStamp = (Get-Date -Format 'yyyyMMdd-HHmmss')
)

$ErrorActionPreference = 'Stop'

$togetherRoot = Join-Path $GameRoot 'Data\SkyrimTogetherReborn'
$serverData = Join-Path $togetherRoot 'Data'
$configPath = Join-Path $togetherRoot 'config\STServer.ini'
$creationPath = Join-Path $GameRoot 'Skyrim.ccc'
$backupPath = Join-Path $togetherRoot "backups\$BackupStamp-pre-strict-modcheck"

foreach ($requiredPath in @($configPath, $creationPath, $PluginsPath, $SkyrimPrefsPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required deployment file is missing: $requiredPath"
    }
}

$requiredMultiplayerPlugins = @('SkyrimSEMultiplayer.esp', 'SkyrimSEMultiplayerQuestPatches.esp')
$pluginLines = [Collections.Generic.List[string]]::new()
$pluginLines.AddRange([string[]](Get-Content -LiteralPath $PluginsPath))
$preservedPluginLines = [Collections.Generic.List[string]]::new()
foreach ($pluginLine in $pluginLines) {
    if ($pluginLine.TrimStart('*').Trim() -notin $requiredMultiplayerPlugins) {
        $preservedPluginLines.Add($pluginLine)
    }
}
$pluginLines = $preservedPluginLines
foreach ($requiredPlugin in $requiredMultiplayerPlugins) {
    $pluginLines.Add("*$requiredPlugin")
}

$utf8NoBom = [Text.UTF8Encoding]::new($false)
$pluginsBackupPath = "$PluginsPath.$BackupStamp.bak"
$prefsBackupPath = "$SkyrimPrefsPath.$BackupStamp.bak"
Copy-Item -LiteralPath $PluginsPath -Destination $pluginsBackupPath -Force
Copy-Item -LiteralPath $SkyrimPrefsPath -Destination $prefsBackupPath -Force
[IO.File]::WriteAllLines($PluginsPath, $pluginLines, $utf8NoBom)

$prefsText = [IO.File]::ReadAllText($SkyrimPrefsPath)
if ($prefsText -match '(?m)^bEnableFileSelection\s*=') {
    $prefsText = [regex]::Replace($prefsText, '(?m)^bEnableFileSelection\s*=.*$', 'bEnableFileSelection=1')
} elseif ($prefsText -match '(?m)^\[Launcher\]\s*$') {
    $prefsText = [regex]::Replace($prefsText, '(?m)^(\[Launcher\]\s*\r?\n)', "`$1bEnableFileSelection=1`r`n")
} else {
    $prefsText += "`r`n[Launcher]`r`nbEnableFileSelection=1`r`n"
}
[IO.File]::WriteAllText($SkyrimPrefsPath, $prefsText, $utf8NoBom)

$enabledPlugins = Get-Content -LiteralPath $PluginsPath |
    Where-Object { $_ -match '^\*' } |
    ForEach-Object { $_.Substring(1).Trim() } |
    Where-Object { $_ }

function Test-LightPlugin([string]$PluginName) {
    if ([IO.Path]::GetExtension($PluginName) -ieq '.esl') {
        return $true
    }
    $pluginPath = Join-Path (Join-Path $GameRoot 'Data') $PluginName
    if (-not (Test-Path -LiteralPath $pluginPath -PathType Leaf)) {
        throw "Enabled plugin is missing from Data: $pluginPath"
    }
    $stream = [IO.File]::Open($pluginPath, 'Open', 'Read', 'ReadWrite')
    try {
        $header = [byte[]]::new(12)
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length -or [Text.Encoding]::ASCII.GetString($header, 0, 4) -ne 'TES4') {
            throw "Enabled plugin has an invalid TES4 header: $pluginPath"
        }
        return ([BitConverter]::ToUInt32($header, 8) -band 0x200) -ne 0
    } finally {
        $stream.Dispose()
    }
}

$loadOrderNames = @(
    'Skyrim.esm'
    'Update.esm'
    'Dawnguard.esm'
    'HearthFires.esm'
    'Dragonborn.esm'
) + @(Get-Content -LiteralPath $creationPath | ForEach-Object { $_.Trim() } | Where-Object { $_ }) + @($enabledPlugins)

$loadOrder = @($loadOrderNames | ForEach-Object {
    if ((Test-LightPlugin $_) -and [IO.Path]::GetExtension($_) -ine '.esl') { "light:$_" } else { $_ }
})

if (@($loadOrderNames | Select-Object -Unique).Count -ne $loadOrderNames.Count) {
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
    RequiredPluginsEnabled = [bool](@($requiredMultiplayerPlugins | Where-Object { "*$_" -notin $pluginLines }).Count -eq 0)
    FileSelectionEnabled = [bool]((Get-Content -LiteralPath $SkyrimPrefsPath) -match '^bEnableFileSelection=1$')
    PluginsBackupPath = $pluginsBackupPath
    SkyrimPrefsBackupPath = $prefsBackupPath
    BackupPath = $backupPath
}
