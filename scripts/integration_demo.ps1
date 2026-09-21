param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutputDir = (Join-Path $PSScriptRoot ('..\demo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [int]$Port = 19110,
    [int]$AgentRuntimeSeconds = 4,
    [int]$IntervalMilliseconds = 100
)

$ErrorActionPreference = 'Stop'
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$receiverExe = Join-Path $BuildDir 'telemetry_receiver.exe'
$agentExe = Join-Path $BuildDir 'edge_agent.exe'

if (-not (Test-Path -LiteralPath $receiverExe) -or -not (Test-Path -LiteralPath $agentExe)) {
    throw "Build artifacts not found in $BuildDir"
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$edgeDb = Join-Path $OutputDir 'edge.db'
$receiverDb = Join-Path $OutputDir 'receiver.db'
$receiverOut = Join-Path $OutputDir 'receiver.out.txt'
$receiverErr = Join-Path $OutputDir 'receiver.err.txt'
$agentOut = Join-Path $OutputDir 'agent.out.txt'
$agentErr = Join-Path $OutputDir 'agent.err.txt'

$receiver = Start-Process -FilePath $receiverExe `
    -ArgumentList '--db',$receiverDb,'--port',$Port,'--runtime-sec',($AgentRuntimeSeconds + 5) `
    -RedirectStandardOutput $receiverOut -RedirectStandardError $receiverErr `
    -WindowStyle Hidden -PassThru

Start-Sleep -Milliseconds 700

$agent = Start-Process -FilePath $agentExe `
    -ArgumentList '--db',$edgeDb,'--host','127.0.0.1','--port',$Port,
                  '--device','device-demo','--interval-ms',$IntervalMilliseconds,
                  '--runtime-sec',$AgentRuntimeSeconds,'--drain-sec','3' `
    -RedirectStandardOutput $agentOut -RedirectStandardError $agentErr `
    -WindowStyle Hidden -PassThru

$agent.WaitForExit()
$receiver.WaitForExit()

Get-Content -LiteralPath $agentOut
Get-Content -LiteralPath $agentErr
Get-Content -LiteralPath $receiverOut
Get-Content -LiteralPath $receiverErr

if ($agent.ExitCode -ne 0 -or $receiver.ExitCode -ne 0) {
    throw "Demo failed: agent=$($agent.ExitCode), receiver=$($receiver.ExitCode)"
}

$sqlite = Get-Command sqlite3 -ErrorAction SilentlyContinue
if ($sqlite) {
    $edgeCounts = & $sqlite.Source $edgeDb `
        "select (select value-1 from edge_meta where key='next_sequence'), (select value from edge_meta where key='acknowledged_total'), count(*) from edge_events;"
    $receiverCounts = & $sqlite.Source $receiverDb `
        'select count(*), count(distinct event_id) from received_events;'
    Write-Output "edge rows|acked|pending: $edgeCounts"
    Write-Output "receiver rows|unique IDs: $receiverCounts"

    $edgeParts = $edgeCounts -split '\|'
    $receiverParts = $receiverCounts -split '\|'
    if ($edgeParts[0] -ne $edgeParts[1] -or $edgeParts[2] -ne '0') {
        throw 'Edge database contains unacknowledged events.'
    }
    if ($receiverParts[0] -ne $receiverParts[1] -or $receiverParts[0] -ne $edgeParts[0]) {
        throw 'Receiver database does not exactly match the edge event set.'
    }
}

Write-Output "demo passed: $OutputDir"
