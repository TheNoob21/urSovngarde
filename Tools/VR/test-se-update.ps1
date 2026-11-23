param([Parameter(Mandatory = $true)][string]$Scratch, [string]$LegacyClient = '')
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Scratch) { throw 'Scratch must not already exist.' }
New-Item -ItemType Directory -Path $Scratch | Out-Null
$Scratch = (Resolve-Path -LiteralPath $Scratch).Path
$source = Join-Path $Scratch 'archive'
$client = Join-Path $Scratch 'client'
$mod = Join-Path $Scratch 'mod'
$server = Join-Path $Scratch 'server'
foreach ($dir in $source, $client, $mod, $server) { New-Item -ItemType Directory -Path $dir | Out-Null }
function Put([string]$Root, [string]$Name, [string]$Text) {
    $path = Join-Path $Root $Name
    New-Item -ItemType Directory -Force (Split-Path $path -Parent) | Out-Null
    Set-Content -LiteralPath $path -Value $Text -Encoding ASCII
}
function Assert-Text([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path) -or (Get-Content -LiteralPath $Path -Raw).Trim() -ne $Expected) { throw "Unexpected content: $Path" }
}
function Assert-Refused([string]$Archive, [string]$Reason, [string[]]$Options = @()) {
    $ErrorActionPreference = 'Continue'
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $updater -Zip $Archive -ClientFolder $client @Options 2>&1
    if ($LASTEXITCODE -eq 0 -or ($output -join ' ') -notlike "*$Reason*") {
        throw "Expected refusal: $Reason"
    }
}
function Assert-BatchRefused([string]$Archive) {
    $ErrorActionPreference = 'Continue'
    $output = ' ' | & (Join-Path $client 'update.bat') $Archive 2>&1
    if ($LASTEXITCODE -eq 0 -or ($output -join ' ') -notlike '*No update files were replaced*') {
        throw 'The batch entry point failed to refuse a running sibling server.'
    }
}
$files = @('urSovngardeSE.exe', 'SkyrimTogether.exe', 'client-payload\TPProcess.exe',
    'client-payload\EarlyLoad.dll', 'client-payload\libcef.dll', 'client-payload\dxcompiler.dll', 'client-payload\dxil.dll', 'client-payload\icudtl.dat',
    'client-payload\resources.pak', 'client-payload\UI\index.html', 'client-payload\assets\cursor.dds',
    'client-payload\locales\en-US.pak', 'urSovngarde SE mod\SkyrimTogether.esp',
    'urSovngarde SE mod\scripts\test.pex')
foreach ($file in $files) { Put $source $file 'new' }
Put $source 'Server\urSovngardeServer.exe' 'new'
Put $source 'Server\STServer.dll' 'new'
if ($LegacyClient) {
    if (-not (Test-Path -LiteralPath (Join-Path $LegacyClient 'SkyrimTogether.exe') -PathType Leaf)) { throw 'LegacyClient must contain SkyrimTogether.exe.' }
    Copy-Item -Path (Join-Path $LegacyClient '*') -Destination $client -Recurse -Force
} else { Put $client 'SkyrimTogether.exe' 'old' }
Put $client 'settings.json' 'private'
Put $client 'servers.txt' 'saved connection'
Put $client 'Saves\before-update.ess' 'saved game'
Put $client 'SkyrimTogether.exe.old' 'keep'
Put $mod 'SkyrimTogether.esp' 'old'
Put $mod 'user-file.txt' 'keep'
$retired = @('SKSE\Plugins\urSovngardeEssentials.dll', 'scripts\urSovngardeCoopMCM.pex', 'scripts\source\urSovngardeCoopMCM.psc')
foreach ($relative in $retired) { Put $mod $relative "legacy $relative" }
Put $mod 'SKSE\Plugins\user.dll' 'keep'
Put $server 'urSovngardeServer.exe' 'old'
Put $server 'config\STServer.ini' 'private'
$zip = Join-Path $Scratch 'update.zip'
Compress-Archive -Path (Join-Path $source '*') -DestinationPath $zip
$updater = Join-Path $PSScriptRoot 'update.ps1'
# Give an idle fixture process the legacy server name so the real process check runs.
$processFolder = New-Item -ItemType Directory -Path (Join-Path $Scratch 'process')
$processExe = Join-Path $processFolder.FullName 'SkyrimTogetherServer.exe'
Copy-Item (Join-Path $env:SystemRoot 'System32\ping.exe') $processExe
function Get-InstallSnapshot {
    Get-ChildItem -LiteralPath $client, $mod, $server -File -Recurse | Sort-Object FullName | ForEach-Object {
        '{0} {1}' -f $_.FullName, (Get-FileHash -LiteralPath $_.FullName).Hash
    }
}
Copy-Item -LiteralPath $updater, (Join-Path $PSScriptRoot 'update.bat') -Destination $client -Force
$before = Get-InstallSnapshot
$running = Start-Process -FilePath $processExe -ArgumentList '-t', '127.0.0.1' -PassThru -WindowStyle Hidden
try {
    if ($running.HasExited) { throw 'The fixture server exited before preflight.' }
    Assert-Refused $zip 'No update files were replaced' @('-ServerFolder', $server, '-ModFolder', $mod)
    # The drag-and-drop entry point supplies a tools path ending in "\." and discovers its sibling server.
    Assert-BatchRefused $zip
    if (Compare-Object $before (Get-InstallSnapshot)) { throw 'Running-server refusal changed the installation.' }
} finally {
    if (-not $running.HasExited) { $running.Kill(); $running.WaitForExit() }
    $running.Dispose()
}
Assert-Refused $zip 'Pass -ModFolder'
if (Compare-Object $before (Get-InstallSnapshot)) { throw 'Missing mod root changed the installation.' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $updater -Zip $zip -ClientFolder $client -ModFolder $mod -ServerFolder $server
if ($LASTEXITCODE -ne 0) { throw 'Upgrade failed.' }
foreach ($file in 'SkyrimTogether.exe', 'urSovngardeSE.exe', 'TPProcess.exe', 'dxcompiler.dll', 'dxil.dll', 'UI\index.html') { Assert-Text (Join-Path $client $file) 'new' }
Assert-Text (Join-Path $client 'settings.json') 'private'
Assert-Text (Join-Path $client 'servers.txt') 'saved connection'
Assert-Text (Join-Path $client 'Saves\before-update.ess') 'saved game'
Assert-Text (Join-Path $client 'SkyrimTogether.exe.old') 'keep'
Assert-Text (Join-Path $mod 'scripts\test.pex') 'new'
Assert-Text (Join-Path $mod 'user-file.txt') 'keep'
Assert-Text (Join-Path $mod 'SKSE\Plugins\user.dll') 'keep'
$backups = @(Get-ChildItem -LiteralPath $client -Directory -Filter 'retired-mod-files.*')
if ($backups.Count -ne 1) { throw 'Expected one recovery directory.' }
foreach ($relative in $retired) {
    if (Test-Path -LiteralPath (Join-Path $mod $relative)) { throw "Retired file still installed: $relative" }
    Assert-Text (Join-Path $backups[0].FullName ($relative + '.retired')) "legacy $relative"
}
Assert-Text (Join-Path $server 'urSovngardeServer.exe') 'new'
Assert-Text (Join-Path $server 'STServer.dll') 'new'
Assert-Text (Join-Path $server 'config\STServer.ini') 'private'
# A missing companion and an invalid mod root must fail before replacing any files.
$bad = Join-Path $Scratch 'bad'
New-Item -ItemType Directory -Path $bad | Out-Null
Put $bad 'urSovngardeSE.exe' 'bad'
$badZip = Join-Path $Scratch 'bad.zip'
Compress-Archive -Path (Join-Path $bad '*') -DestinationPath $badZip
Assert-Refused $badZip 'Incomplete SE update'
Assert-Text (Join-Path $client 'urSovngardeSE.exe') 'new'
Assert-Refused $zip 'ModFolder must be the existing urSovngarde mod root' @('-ModFolder', $client)
Assert-Text (Join-Path $client 'settings.json') 'private'
Write-Host "SE update fixture passed: $Scratch"
