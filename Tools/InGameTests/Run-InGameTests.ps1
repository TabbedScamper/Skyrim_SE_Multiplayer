[CmdletBinding()]
param(
    [switch]$NoLaunch,
    [string]$InstallDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data\SkyrimTogetherReborn'
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$artifactRoot = Join-Path $scriptRoot "artifacts\$runStamp"
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SkyrimTestInput {
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion U; }
  [StructLayout(LayoutKind.Explicit)] public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public UIntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public UIntPtr extra; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint count, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@

function Wait-Condition([scriptblock]$Condition, [int]$Seconds, [string]$Failure) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    do {
        try { if (& $Condition) { return } } catch { }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    throw $Failure
}

if (-not $NoLaunch -and -not (Get-Process SkyrimTogether -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath (Join-Path $InstallDirectory 'SkyrimTogether.exe') -WorkingDirectory $InstallDirectory
}

Wait-Condition { Get-Process SkyrimTogether -ErrorAction SilentlyContinue } 30 'SkyrimTogether did not start'
$gameProcesses = @(Get-Process SkyrimTogether -ErrorAction SilentlyContinue)
if ($gameProcesses.Count -ne 1) {
    throw "Expected exactly one SkyrimTogether process, found $($gameProcesses.Count)"
}
Wait-Condition { Test-Path '\\.\pipe\SkyrimSEMultiplayer.Test' } 60 'Native test pipe did not become available'
Wait-Condition { try { (Invoke-RestMethod 'http://127.0.0.1:8384/json' -TimeoutSec 1).Count -gt 0 } catch { $false } } 60 'CEF DevTools did not become available'

$nextId = 0
$events = [Collections.Generic.List[object]]::new()

function Invoke-Native([string]$Command, [hashtable]$Arguments = @{}) {
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', 'SkyrimSEMultiplayer.Test', [IO.Pipes.PipeDirection]::InOut)
    $pipe.Connect(10000)
    $reader = [IO.StreamReader]::new($pipe)
    $writer = [IO.StreamWriter]::new($pipe)
    $writer.AutoFlush = $true
    $script:nextId++
    try {
        $request = @{ id = $script:nextId; command = $Command }
        foreach ($entry in $Arguments.GetEnumerator()) { $request[$entry.Key] = [string]$entry.Value }
        $writer.WriteLine(($request | ConvertTo-Json -Compress))
        $line = $reader.ReadLine()
        if (-not $line) { throw "No response for native command $Command" }
        $response = $line | ConvertFrom-Json
        $events.Add([pscustomobject]@{ time = (Get-Date).ToString('o'); kind = 'native'; request = $request; response = $response })
        if (-not $response.ok) { throw "Native command $Command failed: $($response.error)" }
        return $response
    }
    finally {
        $writer.Dispose(); $reader.Dispose(); $pipe.Dispose()
    }
}

function Invoke-Cdp([string]$Action, [string]$OutputPath = '') {
    $arguments = @((Join-Path $scriptRoot 'cef-cdp.mjs'), $Action)
    if ($OutputPath) { $arguments += $OutputPath }
    $text = & node @arguments
    if ($LASTEXITCODE -ne 0) { throw "CEF action $Action failed" }
    $result = $text | ConvertFrom-Json
    $events.Add([pscustomobject]@{ time = (Get-Date).ToString('o'); kind = 'cef'; action = $Action; result = $result })
    return $result
}

function Save-Checkpoint([string]$Name) {
    $native = Invoke-Native 'snapshot'
    $game = Invoke-Native 'game_snapshot'
    $cef = Invoke-Cdp 'snapshot'
    $bundle = Invoke-Native 'capture_bundle'
    Invoke-Cdp 'screenshot' (Join-Path $artifactRoot "$Name-cef.png") | Out-Null
    [pscustomobject]@{
        native = $native
        game = $game
        cef = $cef
        gameScreenshot = $bundle.screenshotPath
        gameState = $bundle.gameStatePath
    } |
        ConvertTo-Json -Depth 12 | Set-Content (Join-Path $artifactRoot "$Name.json") -Encoding UTF8
    return [pscustomobject]@{ native = $native; game = $game; cef = $cef }
}

function Send-MouseDelta([int]$X, [int]$Y) {
    $mouse = [SkyrimTestInput+MOUSEINPUT]::new()
    $mouse.dx = $X
    $mouse.dy = $Y
    $mouse.dwFlags = 1
    $union = [SkyrimTestInput+InputUnion]::new()
    $union.mi = $mouse
    $input = [SkyrimTestInput+INPUT]::new()
    $input.type = 0
    $input.U = $union
    [SkyrimTestInput]::SendInput(1, @($input), [Runtime.InteropServices.Marshal]::SizeOf([type][SkyrimTestInput+INPUT])) | Out-Null
}

function Send-Key([uint16]$VirtualKey) {
    $downKey = [SkyrimTestInput+KEYBDINPUT]::new(); $downKey.vk = $VirtualKey
    $downUnion = [SkyrimTestInput+InputUnion]::new(); $downUnion.ki = $downKey
    $down = [SkyrimTestInput+INPUT]::new(); $down.type = 1; $down.U = $downUnion
    $upKey = [SkyrimTestInput+KEYBDINPUT]::new(); $upKey.vk = $VirtualKey; $upKey.flags = 2
    $upUnion = [SkyrimTestInput+InputUnion]::new(); $upUnion.ki = $upKey
    $up = [SkyrimTestInput+INPUT]::new(); $up.type = 1; $up.U = $upUnion
    [SkyrimTestInput]::SendInput(2, @($down, $up), [Runtime.InteropServices.Marshal]::SizeOf([type][SkyrimTestInput+INPUT])) | Out-Null
}

# The pipe and CEF are created several seconds before the Scaleform title menu
# finishes loading. Testing earlier produces false failures for every menu action.
Wait-Condition { (Invoke-Native 'snapshot').mainMenuOpen } 90 'Skyrim main menu did not become ready'

$failures = [Collections.Generic.List[string]]::new()
try {
    Invoke-Native 'ping' | Out-Null
    Save-Checkpoint '00-initial' | Out-Null

    Invoke-Native 'open_options' | Out-Null
    Start-Sleep -Seconds 1
    $opened = Save-Checkpoint '01-options-open'
    if (-not $opened.native.overlay.active) { $failures.Add('Native overlay did not become active') }
    if (-not $opened.cef.settingsVisible) { $failures.Add('CEF settings component is not visible') }

    Invoke-Cdp 'focus-display-mode' | Out-Null
    Invoke-Native 'controller' @{ button = 'a' } | Out-Null
    Start-Sleep -Milliseconds 250
    $dropdownOpen = Invoke-Cdp 'snapshot'
    $display = $dropdownOpen.dropdowns | Where-Object id -eq 'displayMode'
    if (-not $display.open) { $failures.Add('Controller A did not open the display-mode dropdown') }
    $originalIndex = $display.selectedIndex

    Invoke-Native 'controller' @{ button = 'down' } | Out-Null
    Start-Sleep -Milliseconds 200
    $moved = Invoke-Cdp 'snapshot'
    $movedDisplay = $moved.dropdowns | Where-Object id -eq 'displayMode'
    if ($movedDisplay.selectedIndex -eq $originalIndex) { $failures.Add('Controller Down did not move dropdown selection') }

    Invoke-Native 'controller' @{ button = 'up' } | Out-Null
    Start-Sleep -Milliseconds 200
    Invoke-Native 'controller' @{ button = 'a' } | Out-Null
    Start-Sleep -Milliseconds 300
    $restored = Invoke-Cdp 'snapshot'
    $restoredDisplay = $restored.dropdowns | Where-Object id -eq 'displayMode'
    if ($restoredDisplay.open) { $failures.Add('Controller A did not confirm/close the dropdown') }
    if ($restoredDisplay.selectedIndex -ne $originalIndex) { $failures.Add('Dropdown did not return to its original selection') }
    Invoke-Native 'confirm_display' | Out-Null
    Start-Sleep -Milliseconds 300

    $gameWindow = [IntPtr]::new([int64]$opened.native.window.hwnd)
    [SkyrimTestInput]::SetForegroundWindow($gameWindow) | Out-Null
    Wait-Condition { (Invoke-Native 'snapshot').window.foreground } 3 'Could not foreground the Skyrim window for mouse injection'
    Send-MouseDelta 8 5
    Start-Sleep -Milliseconds 300
    $mouseState = Invoke-Native 'snapshot'
    if (-not $mouseState.overlay.cursorVisible) { $failures.Add('Physical mouse movement did not restore the CEF cursor') }

    Invoke-Native 'controller' @{ button = 'down' } | Out-Null
    Start-Sleep -Milliseconds 200
    $controllerState = Invoke-Native 'snapshot'
    if ($controllerState.overlay.cursorVisible) { $failures.Add('Controller navigation did not hide the CEF cursor') }

    Save-Checkpoint '02-input-handoff' | Out-Null

    Send-Key 0x5B
    Start-Sleep -Milliseconds 700
    [SkyrimTestInput]::SetCursorPos(
        [int]($controllerState.window.x + ($controllerState.window.outerWidth / 2)),
        [int]($controllerState.window.y + ($controllerState.window.outerHeight / 2))) | Out-Null
    Start-Sleep -Milliseconds 300
    $desktopCursor = Invoke-Native 'snapshot'
    if ($desktopCursor.window.foreground) { $failures.Add('Windows key did not release foreground ownership') }
    if (-not $desktopCursor.systemCursor.visible) { $failures.Add('Windows cursor is hidden while hovering over unfocused Skyrim') }
    Send-Key 0x1B
    [SkyrimTestInput]::SetForegroundWindow($gameWindow) | Out-Null
    Start-Sleep -Milliseconds 300
    Save-Checkpoint '03-desktop-cursor' | Out-Null

    Invoke-Native 'close_options' | Out-Null
    $beforeToggle = Invoke-Native 'snapshot'
    Invoke-Native 'toggle_window' | Out-Null
    Start-Sleep -Seconds 2
    $alternateMode = Invoke-Native 'snapshot'
    if ($alternateMode.window.clientWidth -eq $beforeToggle.window.clientWidth -and
        $alternateMode.window.clientHeight -eq $beforeToggle.window.clientHeight -and
        $alternateMode.window.style -eq $beforeToggle.window.style) {
        $failures.Add('Window toggle produced no observable mode/size/style change')
    }
    Invoke-Native 'toggle_window' | Out-Null
    Start-Sleep -Seconds 2
    $returnedMode = Invoke-Native 'snapshot'
    if ($returnedMode.window.clientWidth -ne $beforeToggle.window.clientWidth -or
        $returnedMode.window.clientHeight -ne $beforeToggle.window.clientHeight -or
        $returnedMode.window.style -ne $beforeToggle.window.style) {
        $failures.Add('Second window toggle did not restore the original window state')
    }
    Save-Checkpoint '04-window-toggle-returned' | Out-Null

    # Renderer failures can occur a few frames after a resize appears correct.
    # Keep observing before declaring the run successful.
    Start-Sleep -Seconds 5
    Invoke-Native 'ping' | Out-Null
    $stable = Invoke-Native 'snapshot'
    if (-not $stable.mainMenuOpen) { $failures.Add('Main menu disappeared during the post-test stability window') }
    if ($stable.window.clientWidth -ne $returnedMode.window.clientWidth -or
        $stable.window.clientHeight -ne $returnedMode.window.clientHeight) {
        $failures.Add('Window dimensions changed during the post-test stability window')
    }
}
catch {
    $failures.Add($_.Exception.Message)
}
finally {
    try { Invoke-Native 'close_options' | Out-Null } catch { }
    $events | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $artifactRoot 'events.json') -Encoding UTF8
    $summary = [pscustomobject]@{
        passed = $failures.Count -eq 0
        failures = @($failures)
        artifactDirectory = $artifactRoot
        finished = (Get-Date).ToString('o')
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $artifactRoot 'summary.json') -Encoding UTF8
}

Get-Content (Join-Path $artifactRoot 'summary.json')
if ($failures.Count) { exit 1 }
