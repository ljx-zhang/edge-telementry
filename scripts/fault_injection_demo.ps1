param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutputDir = (Join-Path $PSScriptRoot ('..\fault-demo-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [int]$BasePort = 19120
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
    throw 'sqlite3 must be available on PATH for verification.'
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

# Scenario 1: the receiver commits every event but deliberately drops every fifth ACK.
$ackDir = Join-Path $OutputDir 'ack-loss'
New-Item -ItemType Directory -Path $ackDir | Out-Null
$ackEdgeDb = Join-Path $ackDir 'edge.db'
$ackReceiverDb = Join-Path $ackDir 'receiver.db'
$ackReceiverOut = Join-Path $ackDir 'receiver.out.txt'
$ackReceiverErr = Join-Path $ackDir 'receiver.err.txt'
$ackAgentOut = Join-Path $ackDir 'agent.out.txt'
$ackAgentErr = Join-Path $ackDir 'agent.err.txt'

$receiver = Start-Process -FilePath $receiverExe `
    -ArgumentList '--db',$ackReceiverDb,'--port',$BasePort,'--runtime-sec','8',
                  '--drop-ack-every','5' `
    -RedirectStandardOutput $ackReceiverOut -RedirectStandardError $ackReceiverErr `
    -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 700
$agent = Start-Process -FilePath $agentExe `
    -ArgumentList '--db',$ackEdgeDb,'--host','127.0.0.1','--port',$BasePort,
                  '--device','device-fault','--interval-ms','50','--runtime-sec','3',
                  '--drain-sec','4','--max-pending','200' `
    -RedirectStandardOutput $ackAgentOut -RedirectStandardError $ackAgentErr `
    -WindowStyle Hidden -PassThru
$agent.WaitForExit()
$receiver.WaitForExit()

$ackAgentText = Get-Content -LiteralPath $ackAgentOut -Raw
$ackReceiverText = Get-Content -LiteralPath $ackReceiverOut -Raw
Write-Output $ackAgentText.Trim()
Write-Output $ackReceiverText.Trim()
if ($agent.ExitCode -ne 0 -or $receiver.ExitCode -ne 0) {
    throw "ACK-loss scenario process failure: agent=$($agent.ExitCode), receiver=$($receiver.ExitCode)"
}

$ackEdgeCounts = & $sqlite.Source $ackEdgeDb `
    "select (select value-1 from edge_meta where key='next_sequence'), (select value from edge_meta where key='acknowledged_total'), count(*) from edge_events;"
$ackReceiverCounts = & $sqlite.Source $ackReceiverDb `
    'select count(*), count(distinct event_id) from received_events;'
$ackEdgeParts = $ackEdgeCounts -split '\|'
$ackReceiverParts = $ackReceiverCounts -split '\|'
if ($ackEdgeParts[0] -ne $ackEdgeParts[1] -or $ackEdgeParts[2] -ne '0') {
    throw 'ACK-loss scenario left pending edge events.'
}
if ($ackReceiverParts[0] -ne $ackReceiverParts[1] -or $ackReceiverParts[0] -ne $ackEdgeParts[0]) {
    throw 'ACK-loss scenario produced duplicate or missing receiver rows.'
}
if ($ackReceiverText -notmatch 'duplicates_this_run=([1-9][0-9]*)' -or
    $ackReceiverText -notmatch 'dropped_acks=([1-9][0-9]*)') {
    throw 'ACK-loss scenario did not exercise duplicate delivery.'
}
Write-Output "ACK-loss scenario passed: $ackEdgeCounts edge, $ackReceiverCounts receiver"

# Scenario 2: no receiver exists. The pending queue must stop at the configured limit.
$pressureDir = Join-Path $OutputDir 'backpressure'
New-Item -ItemType Directory -Path $pressureDir | Out-Null
$pressureDb = Join-Path $pressureDir 'edge.db'
$pressureOut = Join-Path $pressureDir 'agent.out.txt'
$pressureErr = Join-Path $pressureDir 'agent.err.txt'
$pressure = Start-Process -FilePath $agentExe `
    -ArgumentList '--db',$pressureDb,'--host','127.0.0.1','--port',($BasePort + 1),
                  '--device','device-pressure','--interval-ms','20','--runtime-sec','2',
                  '--drain-sec','1','--max-pending','10' `
    -RedirectStandardOutput $pressureOut -RedirectStandardError $pressureErr `
    -WindowStyle Hidden -PassThru
$pressure.WaitForExit()

$pressureText = Get-Content -LiteralPath $pressureOut -Raw
Write-Output $pressureText.Trim()
$pressureCounts = & $sqlite.Source $pressureDb `
    'select count(*), count(*) from edge_events where acknowledged=0;'
$pressureParts = $pressureCounts -split '\|'
if ($pressure.ExitCode -ne 2) {
    throw "Backpressure scenario expected exit code 2, got $($pressure.ExitCode)."
}
if ($pressureParts[0] -ne '10' -or $pressureParts[1] -ne '10') {
    throw "Backpressure queue exceeded its bound: $pressureCounts"
}
if ($pressureText -notmatch 'backpressure=([1-9][0-9]*)') {
    throw 'Backpressure scenario never activated the pressure gate.'
}
Write-Output "Backpressure scenario passed: rows|pending=$pressureCounts"
Write-Output "fault injection demo passed: $OutputDir"
