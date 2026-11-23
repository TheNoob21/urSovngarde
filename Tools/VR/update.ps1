# Applies an urSovngarde update zip without closing Mod Organizer 2 or the server window's folder.
# Keep this next to urSovngarde.exe, or flat Skyrim's urSovngardeSE.exe (update.bat does the rest). Drag the update zip onto update.bat,
# or run:  update.bat "path\to\urSovngarde-...-update.zip"
#
# Client files (urSovngarde.exe, urSovngarde.pdb; flat Skyrim's urSovngardeSE.exe, urSovngardeSE.pdb) go into this folder. Server files (urSovngardeServer.exe, its
# .manifest, STServer.dll) go into the folder that holds the server: this one, a "Server" or "urSovngarde Server"
# folder next to it, or -ServerFolder.
#
# The programs were renamed on 2026-10-09: SkyrimTogetherVR.exe and SkyrimTogetherServer.exe before. The update zip
# of that release also carries the new build under the old names, for an older update.bat and for an MO2 executable
# that still points at an old name. Here a file under an old name only replaces one that is already there; it is
# never added to a folder that does not have it.
#
# Each file is replaced by renaming the old one aside first: a file that is open (MO2 keeps handles on the
# tools folder) can still be renamed, while overwriting it fails. The old copies are removed at the end.
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [string]$ClientFolder = $PSScriptRoot,
    [string]$ServerFolder = '',
    [string]$ModFolder = '',
    [switch]$PromptForModFolder
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path $Zip)) {
    Write-Host "Zip not found: $Zip"
    exit 1
}
if (Get-Process urSovngarde, SkyrimTogetherVR, SkyrimTogether, SkyrimVR, urSovngardeSE, SkyrimSE -ErrorAction SilentlyContinue) {
    Write-Host "The game is running. Close it, then run the update again."
    exit 1
}

$ClientFolder = (Resolve-Path -LiteralPath $ClientFolder).Path
$serverExes = 'urSovngardeServer.exe', 'SkyrimTogetherServer.exe'
if ($ServerFolder -eq '') {
    $parent = Split-Path $ClientFolder -Parent
    foreach ($candidate in $ClientFolder, (Join-Path $ClientFolder 'Server'), (Join-Path $parent 'Server'), (Join-Path $parent 'urSovngarde Server')) {
        if ($serverExes | Where-Object { Test-Path (Join-Path $candidate $_) }) { $ServerFolder = $candidate; break }
    }
}

$clientFiles = 'urSovngarde.exe', 'urSovngarde.pdb'
# Flat Skyrim's client, placed only in a folder that holds it: each game's update goes to its own tools folder.
$flatClientFiles = 'urSovngardeSE.exe', 'urSovngardeSE.pdb'
$legacyFlatFiles = 'SkyrimTogether.exe', 'SkyrimTogether.pdb'
$isFlatFolder = ((Test-Path (Join-Path $ClientFolder 'urSovngardeSE.exe')) -or (Test-Path (Join-Path $ClientFolder 'SkyrimTogether.exe'))) -and -not (Test-Path (Join-Path $ClientFolder 'urSovngarde.exe'))
$legacyClientFiles = 'SkyrimTogetherVR.exe', 'SkyrimTogetherVR.pdb'
$serverFiles = 'urSovngardeServer.exe', 'urSovngardeServer.exe.manifest', 'STServer.dll'
$legacyServerFiles = 'SkyrimTogetherServer.exe', 'SkyrimTogetherServer.exe.manifest'

function Test-ServerRunning {
    return [bool](Get-Process urSovngardeServer, SkyrimTogetherServer -ErrorAction SilentlyContinue)
}
if ($ServerFolder -and (Test-ServerRunning)) {
    $archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
    try {
        foreach ($entry in $archive.Entries) {
            $name = [IO.Path]::GetFileName($entry.FullName)
            if ($entry.FullName -match '^(client-payload|urSovngarde(?: SE)? mod)[\\/]') { continue }
            if ($serverFiles -contains $name -or
                ($legacyServerFiles -contains $name -and (Test-Path -LiteralPath (Join-Path $ServerFolder $name)))) {
                throw 'The server is running. Close it and retry. No update files were replaced.'
            }
        }
    } finally { $archive.Dispose() }
}

$temp = Join-Path $env:TEMP ("urs-update-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
[System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $temp)

function Replace-File([string]$source, [string]$folder) {
    $name = Split-Path $source -Leaf
    $target = Join-Path $folder $name
    $old = "$target.old-$([guid]::NewGuid().ToString('N'))"
    if (Test-Path $target) {
        try { Rename-Item $target $old -ErrorAction Stop }
        catch {
            Write-Host "  $name is in use (a running game or server holds it). Close it and run the update again."
            throw "update stopped at $name"
        }
    }
    Copy-Item $source $target
    if (Test-Path $old) { Remove-Item $old -Force -ErrorAction SilentlyContinue }
    Write-Host "  replaced $name in $folder"
}

$done = 0
$payload = Join-Path $temp 'client-payload'
$modName = if ($isFlatFolder) { 'urSovngarde SE mod' } else { 'urSovngarde mod' }
$modPayload = Join-Path $temp $modName
$retired = @('SKSE\Plugins\urSovngardeEssentials.dll', 'scripts\urSovngardeCoopMCM.pex', 'scripts\source\urSovngardeCoopMCM.psc')
$otherMod = if ($isFlatFolder) { 'urSovngarde mod' } else { 'urSovngarde SE mod' }
if (Test-Path -LiteralPath (Join-Path $temp $otherMod)) { throw 'Choose the update for this edition of Skyrim.' }
if ($isFlatFolder -and ((Test-Path (Join-Path $temp 'urSovngardeSE.exe')) -or (Test-Path $payload) -or (Test-Path $modPayload))) {
    if (-not (Test-Path -LiteralPath (Join-Path $temp 'urSovngardeSE.exe') -PathType Leaf)) { throw 'Incomplete SE update: missing urSovngardeSE.exe' }
    foreach ($required in 'client-payload\TPProcess.exe', 'client-payload\EarlyLoad.dll',
        'client-payload\UI', 'client-payload\assets', 'client-payload\locales',
        'client-payload\libcef.dll', 'client-payload\icudtl.dat', 'client-payload\resources.pak',
        'urSovngarde SE mod\SkyrimTogether.esp', 'urSovngarde SE mod\scripts') {
        if (-not (Test-Path -LiteralPath (Join-Path $temp $required))) { throw "Incomplete SE update: missing $required" }
    }
}
if (Test-Path -LiteralPath $modPayload -PathType Container) {
    foreach ($relative in $retired) {
        if (Test-Path -LiteralPath (Join-Path $modPayload $relative)) { throw "Unexpected retired file in Vanilla update: $relative" }
    }
    if (-not $ModFolder -and $PromptForModFolder) {
        $ModFolder = (Read-Host 'Enter the existing urSovngarde mod folder containing SkyrimTogether.esp').Trim('"')
    }
    if (-not $ModFolder) { throw 'Pass -ModFolder with the existing mod root so retired files can be migrated.' }
    if (-not (Test-Path -LiteralPath (Join-Path $ModFolder 'SkyrimTogether.esp') -PathType Leaf)) {
        throw 'ModFolder must be the existing urSovngarde mod root containing SkyrimTogether.esp.'
    }
    $modRoot = (Resolve-Path -LiteralPath $ModFolder).Path.TrimEnd('\')
    $clientRoot = (Resolve-Path -LiteralPath $ClientFolder).Path.TrimEnd('\')
    if ($modRoot -eq $clientRoot -or $modRoot.StartsWith("$clientRoot\", [StringComparison]::OrdinalIgnoreCase) -or $clientRoot.StartsWith("$modRoot\", [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ModFolder and ClientFolder must be separate folders.'
    }
    # Do not write through junctions or symlinks within the selected mod.
    $paths = @($modRoot) + @(Get-ChildItem -LiteralPath $modPayload -Recurse -File | ForEach-Object {
        Join-Path $modRoot $_.FullName.Substring($modPayload.Length).TrimStart('\')
    })
    $paths += @($retired | ForEach-Object { Join-Path $modRoot $_ })
    foreach ($path in $paths) {
        $current = $path
        while ($current -and $current.Length -ge $modRoot.Length) {
            if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "ModFolder contains a link: $current"
            }
            if ($current -eq $modRoot) { break }
            $current = Split-Path $current -Parent
        }
    }
} elseif ($ModFolder) { throw 'ModFolder requires an update with a mod payload.' }
if ($isFlatFolder -and (Test-Path $payload)) {
    Get-ChildItem $payload -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($payload.Length).TrimStart('\')
        $subfolder = Split-Path $relative -Parent
        $folder = if ($subfolder) { Join-Path $ClientFolder $subfolder } else { $ClientFolder }
        New-Item -ItemType Directory -Force $folder | Out-Null
        Replace-File $_.FullName $folder
        $done++
    }
}
$found = Get-ChildItem $temp -Recurse -File | Where-Object {
    -not $_.FullName.StartsWith("$payload\", [StringComparison]::OrdinalIgnoreCase) -and
    -not $_.FullName.StartsWith("$modPayload\", [StringComparison]::OrdinalIgnoreCase)
}
foreach ($file in $found) {
    $legacy = ($legacyClientFiles + $legacyServerFiles) -contains $file.Name
    if ($legacyFlatFiles -contains $file.Name) {
        if ($isFlatFolder -and (Test-Path (Join-Path $ClientFolder $file.Name))) {
            Replace-File $file.FullName $ClientFolder
            $done++
        }
    } elseif ($flatClientFiles -contains $file.Name) {
        if (-not $isFlatFolder) { Write-Host "  $($file.Name) is flat Skyrim's client and this folder holds the VR one; skipped."; continue }
        Replace-File $file.FullName $ClientFolder
        $done++
    } elseif ($clientFiles -contains $file.Name -or $legacyClientFiles -contains $file.Name) {
        if ($isFlatFolder) { Write-Host "  $($file.Name) is the VR client and this folder holds flat Skyrim's; skipped."; continue }
        if ($legacy -and -not (Test-Path (Join-Path $ClientFolder $file.Name))) { continue }
        Replace-File $file.FullName $ClientFolder
        $done++
    } elseif ($serverFiles -contains $file.Name -or $legacyServerFiles -contains $file.Name) {
        if ($ServerFolder -eq '') {
            if (-not $legacy) { Write-Host "  $($file.Name) is in the zip but no server folder was found; skipped (pass -ServerFolder if you host)." }
            continue
        }
        if ($legacy -and -not (Test-Path (Join-Path $ServerFolder $file.Name))) { continue }
        if (Test-ServerRunning) { throw 'Update incomplete. The server started during the update. Close it and retry the entire zip.' }
        Replace-File $file.FullName $ServerFolder
        $done++
    }
}
if ($done -eq 0) {
    Write-Host "Nothing in that zip matched an update file."
    exit 1
}
if (Test-Path (Join-Path $ClientFolder 'SkyrimTogetherVR.exe')) {
    Write-Host "Note: this folder still has SkyrimTogetherVR.exe, the program's old name, now holding the new build. Point your shortcut or MO2 executable at urSovngarde.exe when convenient."
}
if ($modRoot) {
    Get-ChildItem -LiteralPath $modPayload -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($modPayload.Length).TrimStart('\')
        $folder = Split-Path (Join-Path $modRoot $relative) -Parent
        New-Item -ItemType Directory -Force $folder | Out-Null
        Replace-File $_.FullName $folder
    }
    $backup = $null
    foreach ($relative in $retired) {
        $target = Join-Path $modRoot $relative
        if (-not (Test-Path -LiteralPath $target)) { continue }
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Expected a file at $target" }
        if (-not $backup) {
            $backup = Join-Path $ClientFolder ("retired-mod-files." + [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $backup | Out-Null
            Write-Host "Retired mod files are backed up in $backup"
        }
        $destination = Join-Path $backup ($relative + '.retired')
        New-Item -ItemType Directory -Force (Split-Path $destination -Parent) | Out-Null
        Move-Item -LiteralPath $target -Destination $destination
    }
}
if ($isFlatFolder -and (Test-Path (Join-Path $ClientFolder 'SkyrimTogether.exe'))) {
    Write-Host 'Point your shortcut or MO2 executable at urSovngardeSE.exe.'
}
Remove-Item $temp -Recurse -Force
Write-Host "Update done: $done file(s)."
