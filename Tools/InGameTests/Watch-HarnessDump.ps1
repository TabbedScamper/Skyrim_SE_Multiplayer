param([Parameter(Mandatory)][string]$SessionPath)
$ErrorActionPreference = 'Stop'
$seenGame = $false
$generation = 0
$deadline = (Get-Date).AddHours(2)
$stop = Join-Path $SessionPath 'monitor.stop'
while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $stop)) {
    $game = Get-Process SkyrimTogether -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($game) { $seenGame = $true }
    elseif ($seenGame) { break }
    if ($game -and -not (Get-Process procdump64 -ErrorAction SilentlyContinue)) {
        if ($generation -ge 4) {
            'ProcDump rearm limit reached' | Set-Content -LiteralPath (Join-Path $SessionPath 'monitor.failed')
            break
        }
        # ProcDump's second-chance informational exceptions ignore -f/-fx and
        # can exhaust its count during startup. Rearm without killing a target.
        $generation++
        $folder = Join-Path $SessionPath ('monitor-' + $generation)
        New-Item -ItemType Directory -Path $folder -Force | Out-Null
        $args = @('-accepteula','-ma','-e','1','-f','C0000005','-fx','406D1388','-fx','E06D7363','-fx','000006BA','-n','2',[string]$game.Id,('"' + $folder + '"'))
        $monitor = Start-Process C:/Tools/diag/procdump/procdump64.exe -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $folder 'procdump.log') -RedirectStandardError (Join-Path $folder 'procdump.err')
        @{time=(Get-Date).ToString('o');target=$game.Id;monitor=$monitor.Id;generation=$generation} | ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $SessionPath 'monitor.jsonl')
    }
    Start-Sleep -Milliseconds 500
}
