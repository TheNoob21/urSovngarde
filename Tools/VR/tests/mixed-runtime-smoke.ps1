<#
.SYNOPSIS
Runs a game-free, two-bot server routing smoke test.
.DESCRIPTION
The server listens on wildcard interfaces, not just loopback. The bots connect to
127.0.0.1 using a randomly generated per-run password. Public listing and status
publication are disabled. This test does not run or validate SE or VR clients.
Synthetic VR poses traverse the actual server between two headless bots; this
does not prove visible game animation, real tracking, or engine node placement.
No firewall or network settings are changed. Logs and a receipt are retained in
one new directory under build. Use matching protocol builds of all three binaries.
#>
[CmdletBinding()]
param(
    [string]$BinaryDir = '',
    [ValidateRange(60, 150)][int]$TimeoutSeconds = 150
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BinaryDir) { $BinaryDir = Join-Path $root 'build\windows\x64\releasedbg' }
$work = Join-Path $root ('build\mixed-runtime-smoke-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $work 'config') -Force | Out-Null
$receiptPath = Join-Path $work 'receipt.json'
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$processes = @()
$receipt = [ordered]@{ result = 'failed'; startedUtc = [DateTime]::UtcNow.ToString('o'); workDir = $work; binaryDir = $BinaryDir; port = $null; checks = @(); error = $null }

function Set-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
    $sectionPattern = '(?ms)^\[' + [regex]::Escape($Section) + '\][ \t]*\r?\n(?<body>.*?)(?=^\[|\z)'
    $sections = [regex]::Matches($Text, $sectionPattern)
    if ($sections.Count -ne 1) { throw "Expected exactly one [$Section] section" }
    $sectionMatch = $sections[0]
    $body = $sectionMatch.Groups['body'].Value
    $keyPattern = '(?m)^[ \t]*' + [regex]::Escape($Key) + '[ \t]*=[^\r\n]*'
    $keys = [regex]::Matches($body, $keyPattern)
    if ($keys.Count -gt 1) { throw "Duplicate $Section/$Key" }
    if ($keys.Count -eq 1) {
        $body = [regex]::Replace($body, $keyPattern, "$Key=$Value")
    } else {
        $body = "$Key=$Value`r`n" + $body
    }
    $group = $sectionMatch.Groups['body']
    return $Text.Substring(0, $group.Index) + $body + $Text.Substring($group.Index + $group.Length)
}

function Read-Log([string]$Path) {
    if (Test-Path -LiteralPath $Path) { return Get-Content -LiteralPath $Path -Raw }
    return ''
}
function Wait-ForLog([string]$Path, [string]$Pattern, $Process, [string]$Description) {
    while ([DateTime]::UtcNow -lt $deadline) {
        $Process.Refresh()
        if ($Process.HasExited) { throw "$Description process exited before readiness (exit $($Process.ExitCode))" }
        if ((Read-Log $Path) -match $Pattern) { return }
        Start-Sleep -Milliseconds 200
    }
    throw "Timed out waiting for $Description"
}
function Assert-Log([string]$Text, [string]$Pattern, [string]$Description) {
    if ($Text -notmatch $Pattern) { throw "Missing evidence: $Description" }
    $receipt.checks += $Description
}

try {
    Write-Host 'Game-free routing smoke: this does not test SE or VR game clients.'
    Write-Host 'NOTICE: server listens on wildcard interfaces; bots use 127.0.0.1. Public listing and status are disabled.'
    foreach ($file in @('urSovngardeServer.exe', 'STServer.dll', 'STBot.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $BinaryDir $file) -PathType Leaf)) { throw "Required binary missing: $BinaryDir\$file" }
    }
    $receipt['binaries'] = @(foreach ($file in @('urSovngardeServer.exe', 'STServer.dll', 'STBot.exe')) {
        $binaryPath = Join-Path $BinaryDir $file
        [ordered]@{ filename = $file; size = (Get-Item -LiteralPath $binaryPath).Length; sha256 = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $port = Get-Random -Minimum 40000 -Maximum 60000
    $password = [guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')
    $receipt.port = $port
    $ini = Get-Content -LiteralPath (Join-Path $root 'Tools\VR\STServer.default.ini') -Raw
    $ini = Set-IniValue $ini 'general' 'bConsole' 'false'
    $ini = Set-IniValue $ini 'LiveServices' 'bAnnounceServer' 'false'
    $ini = Set-IniValue $ini 'LiveServices' 'bPublicStatus' 'false'
    $ini = Set-IniValue $ini 'GameServer' 'uPort' "$port"
    $ini = Set-IniValue $ini 'GameServer' 'sPassword' $password
    Set-Content -LiteralPath (Join-Path $work 'config\STServer.ini') -Value $ini -Encoding UTF8
    @('waitfor connected 45','waitfor known other 40',
      'posemark Actor','waitfor pose Actor a 30','expect pose Actor a','wait 2','markpos other',
      'waitfor movedfrommark other >= 200 40','expect movedfrommark other >= 200',
      'posemark Actor','waitfor pose Actor b 30','expect pose Actor b',
      'posemark Actor','waitfor pose Actor none 30','expect pose Actor none',
      'disconnect','wait 2','reconnect','waitfor connected 45','waitfor known other 40',
      'posemark Actor','waitfor pose Actor recovery 40','expect pose Actor recovery',
      'expect known other','expect connected','stop') | Set-Content -LiteralPath (Join-Path $work 'watch.txt') -Encoding ASCII
    @('waitfor connected 45','waitfor known other 40','wait 3',
      'posefixture a','wait 4','walk rel 600 0 200','wait 5',
      'posefixture b','wait 12','posefixture none','wait 12',
      'posefixture recovery','wait 12','expect connected','stop') | Set-Content -LiteralPath (Join-Path $work 'actor.txt') -Encoding ASCII

    $server = Start-Process -FilePath (Join-Path $BinaryDir 'urSovngardeServer.exe') -WorkingDirectory $work -WindowStyle Hidden -PassThru
    $processes += $server
    $serverLog = Join-Path $work 'logs\STServerOut.log'
    Wait-ForLog $serverLog ("started on port " + $port + '(?!\d)') $server 'server on requested port'
    $serverText = Read-Log $serverLog
    $protocolPattern = 'Protocol\s+([0-9a-fA-F]{8})(?=[:\s]|$)'
    Assert-Log $serverText $protocolPattern 'server protocol reported'
    $botExe = Join-Path $BinaryDir 'STBot.exe'
    $serverProtocol = [regex]::Match($serverText, $protocolPattern).Groups[1].Value
    $versionLog = Join-Path $work 'bot-version.txt'
    $versionProcess = Start-Process -FilePath $botExe -WorkingDirectory $work -WindowStyle Hidden -PassThru -ArgumentList '--version' -RedirectStandardOutput $versionLog
    $processes += $versionProcess
    $null = $versionProcess.Handle
    while (-not $versionProcess.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100; $versionProcess.Refresh() }
    if (-not $versionProcess.HasExited -or $versionProcess.ExitCode -ne 0) { throw 'Could not run bot protocol check' }
    $botVersion = Read-Log $versionLog
    if ($botVersion -notmatch $protocolPattern) { throw 'Could not read bot protocol' }
    if ($Matches[1] -cne $serverProtocol) { throw "Protocol mismatch: server=$serverProtocol bot=$($Matches[1])" }
    $receipt['protocol'] = $serverProtocol
    $receipt['botVersion'] = $botVersion.Trim()
    $receipt.checks += 'bot and server protocol match'
    $common = @('--server', "127.0.0.1:$port", '--password', $password, '--standalone', '--host-timeout', '2', '--x', '0', '--y', '0', '--hostlog', 'missing-host.log', '--max-runtime', "$TimeoutSeconds")
    $watch = Start-Process -FilePath $botExe -WorkingDirectory $work -WindowStyle Hidden -PassThru -ArgumentList (@('watch.txt', '--name', 'Watcher') + $common)
    $processes += $watch
    $null = $watch.Handle
    $watchLog = Join-Path $work 'logs\bot-Watcher.log'
    Wait-ForLog $watchLog 'In the world: our character is' $watch 'watcher assignment'
    $actor = Start-Process -FilePath $botExe -WorkingDirectory $work -WindowStyle Hidden -PassThru -ArgumentList (@('actor.txt', '--name', 'Actor') + $common)
    $processes += $actor
    $null = $actor.Handle
    while ([DateTime]::UtcNow -lt $deadline) {
        $watch.Refresh(); $actor.Refresh(); $server.Refresh()
        if ($server.HasExited) { throw 'Server exited during the test' }
        if ($watch.HasExited -and $actor.HasExited) { break }
        Start-Sleep -Milliseconds 200
    }
    $watch.Refresh(); $actor.Refresh()
    if (-not $watch.HasExited -or -not $actor.HasExited) { throw 'Timed out waiting for both bots' }
    if ($watch.ExitCode -ne 0 -or $actor.ExitCode -ne 0) { throw "Bot exit codes: watcher=$($watch.ExitCode), actor=$($actor.ExitCode)" }
    $watchText = Read-Log $watchLog
    $actorText = Read-Log (Join-Path $work 'logs\bot-Actor.log')
    Assert-Log $watchText 'Accepted as player' 'watcher authentication'
    Assert-Log $actorText 'Accepted as player' 'actor authentication'
    Assert-Log $actorText 'In the world: our character is' 'actor assignment'
    if ([regex]::Matches($watchText, 'In the world: our character is').Count -lt 2) { throw 'Watcher was not assigned again after reconnect' }
    $receipt.checks += 'watcher assignment and reassignment'
    Assert-Log $watchText 'ok: movedfrommark other >= 200' 'remote movement relayed'
    Assert-Log $watchText 'disconnecting on purpose' 'intentional disconnect'
    Assert-Log $watchText 'reconnecting \(number 1\)' 'reconnect initiated'
    Assert-Log $watchText '(?s)reconnecting \(number 1\).*ok: known other' 'remote character rediscovered after reconnect'
    foreach ($phase in @('a', 'b', 'none', 'recovery')) {
        Assert-Log $actorText ("\[pose\] sending fixture " + $phase + ' through ClientReferencesMoveRequest') "actor sends synthetic VR pose phase $phase through client movement"
        Assert-Log $watchText ("ok: pose Actor " + $phase + ' \(received Actor player \d+ actor [0-9A-F]+ seq \d+ > mark \d+, age \d+ms;') "watcher receives fresh named Actor VR pose phase $phase through actual server"
    }
    Assert-Log $watchText '(?s)ok: pose Actor a .*ok: pose Actor b .*ok: pose Actor none ' 'distinct head and arm rotations, hips, scale, fingers, hand checks and weapons compared; then HasData=false transition observed'
    $receipt.checks += @('received head quaternion matches each distinct fixture',
        'received left and right arm quaternions match each distinct fixture',
        'received tracked-leg bones and hip offsets match each distinct fixture',
        'received root scale matches each distinct fixture',
        'received left and right hand-check offsets match each distinct fixture',
        'received finger quaternions and weapon transforms match each distinct fixture',
        'no-data requires a prior received VR pose and a newer remote movement snapshot')
    Assert-Log $watchText '(?s)disconnecting on purpose.*\[pose\] receive cache and freshness marks cleared on disconnect.*reconnecting \(number 1\).*ok: pose Actor recovery ' 'received pose cache cleared on disconnect and distinct fresh pose recovered after reconnect'
    $receipt['poseEvidence'] = @($watchText -split '\r?\n' | Where-Object { $_ -match '\[script\] ok: pose Actor (a|b|none|recovery) ' })
    Assert-Log $watchText 'all \d+ checks passed' 'watcher script checks passed'
    Assert-Log $actorText 'all \d+ checks passed' 'actor script checks passed'
    $receipt.result = 'passed'
    Write-Host "PASS: authentication, assignment, movement relay, synthetic VR pose A/B/no-data/recovery, disconnect and reconnect. Receipt: $receiptPath"
}
catch {
    $receipt.error = $_.Exception.Message
    Write-Error -ErrorAction Continue "Smoke failed: $($receipt.error). Receipt: $receiptPath"
}
finally {
    foreach ($process in $processes) {
        try {
            $process.Refresh()
            if (-not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction Stop
                if (-not $process.WaitForExit(3000)) { throw 'Process did not exit within cleanup timeout' }
            }
        } catch { $receipt.result = 'failed'; $receipt.error = "Cleanup failed for PID $($process.Id): $($_.Exception.Message)" }
    }
    $receipt['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
}
if ($receipt.result -ne 'passed') { exit 1 }
