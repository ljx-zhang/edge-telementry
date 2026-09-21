param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutputDir = (Join-Path $PSScriptRoot ('..\benchmark-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [int]$Port = 19150,
    [int]$RuntimeSeconds = 3,
    [int]$IntervalMilliseconds = 1
)

$ErrorActionPreference = 'Stop'
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$receiverExe = Join-Path $BuildDir 'telemetry_receiver.exe'
$agentExe = Join-Path $BuildDir 'edge_agent.exe'
$sqlite = Get-Command sqlite3 -ErrorAction SilentlyContinue

if (-not (Test-Path -LiteralPath $receiverExe) -or -not (Test-Path -LiteralPath $agentExe)) {
    throw "Build artifacts not found in $BuildDir"
}
if (-not $sqlite) {
    throw 'sqlite3 must be available on PATH for benchmark verification.'
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$edgeDb = Join-Path $OutputDir 'edge.db'
$receiverDb = Join-Path $OutputDir 'receiver.db'
$receiverOut = Join-Path $OutputDir 'receiver.out.txt'
$receiverErr = Join-Path $OutputDir 'receiver.err.txt'
$agentOut = Join-Path $OutputDir 'agent.out.txt'
$agentErr = Join-Path $OutputDir 'agent.err.txt'

$receiver = Start-Process -FilePath $receiverExe `
    -ArgumentList '--db',$receiverDb,'--port',$Port,'--runtime-sec',($RuntimeSeconds + 8),
                  '--workers','4','--connection-queue','64' `
    -RedirectStandardOutput $receiverOut -RedirectStandardError $receiverErr `
    -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 700

$timer = [System.Diagnostics.Stopwatch]::StartNew()
$agent = Start-Process -FilePath $agentExe `
    -ArgumentList '--db',$edgeDb,'--host','127.0.0.1','--port',$Port,
                  '--device','device-benchmark','--interval-ms',$IntervalMilliseconds,
                  '--runtime-sec',$RuntimeSeconds,'--drain-sec','7','--batch-size','256',
                  '--max-pending','100000','--max-pending-bytes','134217728' `
    -RedirectStandardOutput $agentOut -RedirectStandardError $agentErr `
    -WindowStyle Hidden -PassThru
$agent.WaitForExit()
$timer.Stop()
$receiver.WaitForExit()

$agentText = Get-Content -LiteralPath $agentOut -Raw
$receiverText = Get-Content -LiteralPath $receiverOut -Raw
Write-Output $agentText.Trim()
Write-Output $receiverText.Trim()
if ($agent.ExitCode -ne 0 -or $receiver.ExitCode -ne 0) {
    throw "Benchmark process failure: agent=$($agent.ExitCode), receiver=$($receiver.ExitCode)"
}

$edgeCounts = & $sqlite.Source $edgeDb `
    "select (select value-1 from edge_meta where key='next_sequence'), (select value from edge_meta where key='acknowledged_total'), count(*) from edge_events;"
$receiverCount = & $sqlite.Source $receiverDb 'select count(*) from received_events;'
$parts = $edgeCounts -split '\|'
if ($parts[0] -ne $parts[1] -or $parts[2] -ne '0' -or $receiverCount -ne $parts[0]) {
    throw "Benchmark data mismatch: edge=$edgeCounts receiver=$receiverCount"
}

$throughput = [math]::Round(([double]$receiverCount / $timer.Elapsed.TotalSeconds), 2)
Write-Output "benchmark result: events=$receiverCount elapsed_s=$([math]::Round($timer.Elapsed.TotalSeconds, 3)) acknowledged_events_per_second=$throughput"
Write-Output "benchmark artifacts: $OutputDir"

