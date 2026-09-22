#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$targetUser = 'eflem'
$publicKey = 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIO+hkE8aSFgUlMjAkV8SgtYzcmYQ9uznHGrlFdNv0gaM skyrim-se-multiplayer@MASON-OFFICE'

if (-not (Get-Service -Name sshd -ErrorAction SilentlyContinue)) {
    # A partially removed MSI can leave ssh-agent running from Program Files,
    # which locks that directory and makes both MSI repair and ZIP fallback
    # fail. Stop only OpenSSH-owned services/processes before repairing it.
    foreach ($serviceName in @('sshd', 'ssh-agent')) {
        $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
        if ($service -and $service.Status -ne 'Stopped') {
            Stop-Service -Name $serviceName -Force -ErrorAction Stop
            $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(10))
        }
    }
    foreach ($processName in @('sshd', 'ssh-agent', 'ssh-shellhost')) {
        Get-Process -Name $processName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction Stop
    }

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
        -ArgumentList @('/i', "`"$msiPath`"", 'ADDLOCAL=Server', '/qn', '/norestart', '/L*v', "`"$env:TEMP\OpenSSH-MSI.log`"") `
        -Wait -PassThru
    if ($installer.ExitCode -notin @(0, 3010)) {
        Write-Warning "OpenSSH MSI returned $($installer.ExitCode); using Microsoft's documented ZIP installer fallback. MSI log: $env:TEMP\OpenSSH-MSI.log"

        $zipUrl = 'https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-Win64.zip'
        $expectedZipHash = '23f50f3458c4c5d0b12217c6a5ddfde0137210a30fa870e98b29827f7b43aba5'
        $zipPath = Join-Path $env:TEMP 'OpenSSH-Win64.zip'
        $extractRoot = Join-Path $env:TEMP 'SkyrimSEMultiplayer-OpenSSH'
        Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath
        $actualZipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualZipHash -ne $expectedZipHash) {
            throw "OpenSSH ZIP hash mismatch. Expected $expectedZipHash, received $actualZipHash."
        }

        if (Test-Path -LiteralPath $extractRoot) {
            $resolvedExtractRoot = (Resolve-Path -LiteralPath $extractRoot).Path
            $expectedExtractRoot = [IO.Path]::GetFullPath((Join-Path $env:TEMP 'SkyrimSEMultiplayer-OpenSSH'))
            if ($resolvedExtractRoot -ne $expectedExtractRoot) { throw "Refusing to replace unexpected extraction path: $resolvedExtractRoot" }
            Remove-Item -LiteralPath $resolvedExtractRoot -Recurse -Force
        }
        Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot -Force

        $archiveDirectory = Join-Path $extractRoot 'OpenSSH-Win64'
        $installDirectory = Join-Path $env:ProgramFiles 'OpenSSH'
        if (-not (Test-Path -LiteralPath (Join-Path $archiveDirectory 'install-sshd.ps1'))) {
            throw 'The verified OpenSSH archive did not contain install-sshd.ps1.'
        }
        if (Test-Path -LiteralPath $installDirectory) {
            $resolvedInstall = (Resolve-Path -LiteralPath $installDirectory).Path
            $expectedInstall = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'OpenSSH'))
            if ($resolvedInstall -ne $expectedInstall) { throw "Refusing to move unexpected OpenSSH path: $resolvedInstall" }
            $backupDirectory = "$expectedInstall.backup-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            $moveDeadline = (Get-Date).AddSeconds(10)
            do {
                try {
                    Move-Item -LiteralPath $resolvedInstall -Destination $backupDirectory -ErrorAction Stop
                    $moved = $true
                }
                catch [IO.IOException] {
                    Start-Sleep -Milliseconds 500
                }
            } while (-not $moved -and (Get-Date) -lt $moveDeadline)
            if (-not $moved) {
                throw "OpenSSH remains locked after its services were stopped. Close any Explorer window or terminal opened in $resolvedInstall and rerun this script."
            }
            Write-Host "Preserved the previous OpenSSH directory at $backupDirectory"
        }
        New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
        Copy-Item -Path (Join-Path $archiveDirectory '*') -Destination $installDirectory -Recurse -Force
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installDirectory 'install-sshd.ps1')
        if ($LASTEXITCODE -ne 0 -or -not (Get-Service -Name sshd -ErrorAction SilentlyContinue)) {
            throw "The verified OpenSSH ZIP installer did not create the sshd service. MSI log: $env:TEMP\OpenSSH-MSI.log"
        }
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
