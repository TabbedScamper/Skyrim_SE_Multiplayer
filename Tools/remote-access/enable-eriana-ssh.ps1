#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$targetUser = 'eflem'
$publicKey = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIO+hkE8aSFgUlMjAkV8SgtYzcmYQ9uznHGrlFdNv0gaM skyrim-se-multiplayer@MASON-OFFICE'

if (-not (Get-Service -Name sshd -ErrorAction SilentlyContinue)) {
    # Windows Capability installation can hang when its optional-feature source
    # is unavailable. Use Microsoft's signed standalone server MSI instead and
    # verify the release hash published by Microsoft before executing it.
    $msiUrl = 'https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-Win64-v10.0.0.0.msi'
    $expectedHash = 'ddec9c53864280759cf9f74791cefd387100e3946aa849a1c138a4ed1b96b7d9'
    $msiPath = Join-Path $env:TEMP 'OpenSSH-Win64-v10.0.0.0.msi'

    Invoke-WebRequest -Uri $msiUrl -OutFile $msiPath
    $actualHash = (Get-FileHash -LiteralPath $msiPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHash) {
        throw "OpenSSH MSI hash mismatch. Expected $expectedHash, received $actualHash."
    }

    $installer = Start-Process -FilePath 'msiexec.exe' `
        -ArgumentList @('/i', "`"$msiPath`"", 'ADDLOCAL=Server', '/qn', '/norestart') `
        -Wait -PassThru
    if ($installer.ExitCode -notin @(0, 3010)) {
        throw "OpenSSH MSI installation failed with exit code $($installer.ExitCode)."
    }
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

# If the MSI created its standard rule, narrow that rule as well.
$standardRule = Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue
if ($standardRule) {
    Set-NetFirewallRule -Name $standardRule.Name -Enabled True -Profile Private -RemoteAddress LocalSubnet
}
else {
    Set-NetFirewallRule -Name $ruleName -Enabled True -Profile Private -RemoteAddress LocalSubnet
}

Write-Host 'Eriana_3080 is paired with MASON-OFFICE for key-based Skyrim SE Multiplayer deployment.'
