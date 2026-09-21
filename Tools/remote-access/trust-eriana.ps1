$ErrorActionPreference = 'Stop'
$logPath = 'C:\Users\mwalt\SkyrimSeamlessCoop\runtime\remote-access\trust-eriana.log'

try {
    Import-Module Microsoft.WSMan.Management
    Start-Service -Name WinRM

    # Permit authenticated WinRM negotiation only to the known second Skyrim
    # rig. This deliberately avoids the insecure wildcard TrustedHosts value.
    Set-Item -Path 'WSMan:\localhost\Client\TrustedHosts' -Value 'Eriana_3080,192.168.50.103' -Force
    "TrustedHosts=$((Get-Item -Path 'WSMan:\localhost\Client\TrustedHosts').Value)" |
        Set-Content -LiteralPath $logPath
    exit 0
}
catch {
    $_ | Out-String | Set-Content -LiteralPath $logPath
    exit 1
}
