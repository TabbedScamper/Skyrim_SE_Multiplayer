#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$targetUser = 'eflem'
$publicKey = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIO+hkE8aSFgUlMjAkV8SgtYzcmYQ9uznHGrlFdNv0gaM skyrim-se-multiplayer@MASON-OFFICE'

$capability = Get-WindowsCapability -Online -Name 'OpenSSH.Server~~~~0.0.1.0'
if ($capability.State -ne 'Installed') {
    Add-WindowsCapability -Online -Name $capability.Name | Out-Null
}

Set-Service -Name sshd -StartupType Automatic
Start-Service -Name sshd

function Add-AuthorizedKey {
    param(
        [Parameter(Mandatory)] [string] $Path
    )

    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType File -Path $Path -Force | Out-Null
    }
    if ($publicKey -notin @(Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)) {
        Add-Content -LiteralPath $Path -Value $publicKey -Encoding ascii
    }
}

# Windows OpenSSH uses the ProgramData file for administrators and the profile
# file for standard users. Populate both so the setup remains valid if the
# account's local group membership changes.
$adminKeys = 'C:\ProgramData\ssh\administrators_authorized_keys'
Add-AuthorizedKey -Path $adminKeys
& icacls.exe $adminKeys /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F' | Out-Null

$userKeys = "C:\Users\$targetUser\.ssh\authorized_keys"
Add-AuthorizedKey -Path $userKeys
& icacls.exe (Split-Path -Parent $userKeys) /inheritance:r /grant "${env:COMPUTERNAME}\${targetUser}:(OI)(CI)F" /grant 'SYSTEM:(OI)(CI)F' | Out-Null

$ruleName = 'SkyrimSEMultiplayer-SSH-LAN'
if (-not (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -Name $ruleName -DisplayName 'Skyrim SE Multiplayer deployment (LAN only)' `
        -Direction Inbound -Action Allow -Protocol TCP -LocalPort 22 -Profile Private `
        -RemoteAddress LocalSubnet | Out-Null
}
else {
    Set-NetFirewallRule -Name $ruleName -Enabled True -Profile Private -RemoteAddress LocalSubnet
}

Write-Host 'Eriana_3080 is paired with MASON-OFFICE for key-based Skyrim SE Multiplayer deployment.'
