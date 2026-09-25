[CmdletBinding()]
param(
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
$bridge = Join-Path $PSScriptRoot 'Invoke-TwoPcTestBridge.ps1'
$script:requestId = 4000

function Invoke-Game([string]$Target, [string]$Command, [hashtable]$Arguments = @{}) {
    $script:requestId++
    $request = @{ id = $script:requestId; command = $Command }
    foreach ($entry in $Arguments.GetEnumerator()) { $request[$entry.Key] = $entry.Value }
    $result = & $bridge -Target $Target -RequestJson ($request | ConvertTo-Json -Compress)
    if (-not $result.response.ok) {
        throw "$Target $Command failed: $($result.response.error)"
    }
    return $result.response
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
do {
    $hostMenu = Invoke-Game Host 'race_menu_state'
    $followerMenu = Invoke-Game Follower 'race_menu_state'
    if ($hostMenu.raceMenuOpen -and $followerMenu.raceMenuOpen) { break }
    if ((Get-Date) -ge $deadline) { throw 'Both character creators did not open before the deadline.' }
    Start-Sleep -Seconds 5
} while ($true)

foreach ($target in 'Host', 'Follower') {
    Invoke-Game $target 'race_menu_key' @{ key = 'done' } | Out-Null
}
Start-Sleep -Seconds 1
foreach ($target in 'Host', 'Follower') {
    $menu = Invoke-Game $target 'race_menu_state'
    if (-not $menu.messageBoxOpen) { throw "$target confirmation dialog did not open." }
    Invoke-Game $target 'confirm_character_native' | Out-Null
}
Start-Sleep -Seconds 1
foreach ($target in 'Host', 'Follower') {
    $status = Invoke-Game $target 'confirm_character_native_status'
    if (-not $status.complete -or -not $status.nativeSelected) {
        throw "$target native character confirmation was not selected."
    }
    $name = if ($target -eq 'Host') { 'Host' } else { 'Follower 1' }
    Invoke-Game $target 'race_menu_key' @{ key = 'type'; name = $name } | Out-Null
    Start-Sleep -Seconds 1
    $accepted = $false
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        Invoke-Game $target 'race_menu_key' @{ key = 'accept_name' } | Out-Null
        Start-Sleep -Seconds 1
        $current = Invoke-Game $target 'race_menu_state'
        if (-not $current.raceMenuOpen -and $current.playerName -eq $name) {
            $accepted = $true
            break
        }
    }
    if (-not $accepted) { throw "$target did not accept the character name." }
}
Start-Sleep -Seconds 1
$hostFinal = Invoke-Game Host 'race_menu_state'
$followerFinal = Invoke-Game Follower 'race_menu_state'
if ($hostFinal.raceMenuOpen -or $followerFinal.raceMenuOpen -or
    $hostFinal.playerName -ne 'Host' -or $followerFinal.playerName -ne 'Follower 1') {
    throw 'Character creator did not close with the expected names on both PCs.'
}
[pscustomobject]@{
    hostName = $hostFinal.playerName
    followerName = $followerFinal.playerName
    hostRaceMenuOpen = $hostFinal.raceMenuOpen
    followerRaceMenuOpen = $followerFinal.raceMenuOpen
}
