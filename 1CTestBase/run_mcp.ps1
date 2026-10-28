# run_mcp.ps1 -- start 1C on this test base, wait for __READY__ (1cmcp server started by the test .ert),
# run the external MCP client test, then kill 1C. ASCII-only (WinPS 5.1 reads BOM-less .ps1 as ANSI).
param([int]$ClientTimeout = 180, [switch]$NoClient, [switch]$Keep, [switch]$Stop)
# -Keep: leave 1C running with the server after __READY__ (for a manual / Claude Code session)
# -Stop: only stop a 1C left running on this test base by -Keep
# NEVER kill all 1cv7 processes: the user may have other 1C sessions open. Only processes whose
# command line points at THIS test base are stopped.
$ErrorActionPreference = 'SilentlyContinue'
$base = $PSScriptRoot
$proj = Split-Path $base
$exe = 'C:\Program Files (x86)\1Cv77\BIN\1cv7.exe'
$log = Join-Path $base 'test_log.txt'
$mlg = Join-Path $base 'SYSLOG\1cv7.mlg'
function Stop-TestBase1C {
  $n = 0
  Get-CimInstance Win32_Process -Filter "Name='1cv7.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($base) } | ForEach-Object {
    Stop-Process -Id $_.ProcessId -Force; $n++
  }
  if ($n) { Start-Sleep -Seconds 1 }
  Get-ChildItem $base -Filter '*.LCK' | ForEach-Object { [IO.File]::Delete($_.FullName) }
  return $n
}
if ($Stop) { Write-Output ('stopped test 1C processes: ' + (Stop-TestBase1C)); exit 0 }
Stop-TestBase1C | Out-Null
Get-ChildItem $base -Filter '*.LCK' | ForEach-Object { [IO.File]::Delete($_.FullName) }
$usr = Join-Path $base '1SUSERS.DBF'; if (Test-Path $usr) { [IO.File]::Delete($usr) }
if (Test-Path $log) { [IO.File]::Delete($log) }
Copy-Item (Join-Path $proj 'build\1cmcp.dll') $base -Force
$mlgBefore = 0; if (Test-Path $mlg) { $mlgBefore = (Get-Content $mlg -Encoding Default).Count }
$a = 'ENTERPRISE /M /D"' + $base + '"'
$p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
Write-Output ('1C PID ' + $p.Id)
$ready = $false; $why = 'timeout'
for ($i = 0; $i -lt 120; $i++) {
  Start-Sleep -Milliseconds 500
  if (Test-Path $log) {
    $t = Get-Content $log -Encoding Default -Raw
    if ($t -match '__READY__') { $ready = $true; $why = 'ready'; break }
    if ($t -match '__DONE__') { $why = 'done (no server)'; break }
  }
  if ($p.HasExited) { $why = '1C exited ' + $p.ExitCode; break }
  if (Test-Path $mlg) { $c = (Get-Content $mlg -Encoding Default); if ($c.Count -gt $mlgBefore -and ($c[$mlgBefore..($c.Count-1)] -match ';E;Grbg|Err:')) { $why = '1C error in MLG -> kill'; break } }
}
Write-Output ('stage1: ' + $why)
Write-Output '===== 1C LOG ====='
if (Test-Path $log) { Get-Content $log -Encoding Default } else { Write-Output '(no log)' }
if ($ready -and -not $NoClient) {
  Write-Output '===== MCP CLIENT ====='
  $env:PYTHONIOENCODING = 'utf-8'
  foreach ($t in 'mcp_client_test.py','forms_test.py') {
    $job = Start-Job -ScriptBlock { param($s) python -I $s 2>&1 } -ArgumentList (Join-Path $proj "test\$t")
    if (Wait-Job $job -Timeout $ClientTimeout) { Receive-Job $job } else { Write-Output "CLIENT TIMEOUT ($t)"; Stop-Job $job }
    Remove-Job $job -Force
  }
}
if ($Keep -and $ready) { Write-Output 'KEEP: 1C left running, server on http://127.0.0.1:18777/mcp'; exit 0 }
Stop-TestBase1C | Out-Null
Write-Output '===== 1C MLG (new errors) ====='
if (Test-Path $mlg) { $all = Get-Content $mlg -Encoding Default; if ($all.Count -gt $mlgBefore) { $all[$mlgBefore..($all.Count-1)] | Where-Object { $_ -match ';E;Grbg|Err:' } | ForEach-Object { $_ } } }
