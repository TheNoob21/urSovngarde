# Exercise the collector's discovery block with disposable client and overwrite folders.
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ("collector-test-" + [guid]::NewGuid())
$client = Join-Path $root 'tools\client'
$logs = Join-Path $client 'logs'
$run1 = Join-Path $logs 'crash-run-one'
$run2 = Join-Path $logs 'crash-run-two'
$overwrite = Join-Path $root 'overwrite\Root'
[void](New-Item -ItemType Directory -Path $root -ErrorAction Stop)
try {
    foreach ($dir in @($run1, $run2, $overwrite)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    function Make-Input($path, $minutesAgo) {
        Set-Content -LiteralPath $path -Value 'test'
        (Get-Item -LiteralPath $path).LastWriteTime = (Get-Date).AddMinutes(-$minutesAgo)
    }
    Make-Input (Join-Path $logs 'crash_UTC_legacy.small.dmp') 30
    Make-Input (Join-Path $run1 'crash_UTC_old.small.dmp') 20
    Make-Input (Join-Path $run2 'crash_UTC_new.small.dmp') 10
    Make-Input (Join-Path $overwrite 'crash_UTC_full.dmp') 5
    Make-Input (Join-Path $logs 'freeze.small.dmp') 30
    Make-Input (Join-Path $run1 'freeze.small.dmp') 10
    Make-Input (Join-Path $logs 'process-end.log') 30
    Make-Input (Join-Path $run2 'process-end.log') 10
    $run3 = Join-Path $logs 'crash-run-empty'
    New-Item -ItemType Directory -Force $run3 | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $run3 'process-end.log'), [byte[]]@())
    Make-Input (Join-Path $run2 'crash_UTC_expired.small.dmp') 1500
    $script:collected = @()
    function Add-File([string]$Path, [string]$Name, [switch]$SharedSource) { $script:collected += $Path; $script:names += $Name }
    $ClientFolder = $client
    $source = Get-Content -Raw (Join-Path $PSScriptRoot 'collect-logs.ps1')
    $start = $source.IndexOf('# Newest crash dump from the last day.')
    $end = $source.IndexOf('# Build of the client,', $start)
    if ($start -lt 0 -or $end -lt 0) { throw 'Collector discovery block not found' }
    . ([scriptblock]::Create($source.Substring($start, $end - $start)))
    $expected = @((Join-Path $run1 'freeze.small.dmp'), (Join-Path $run2 'process-end.log'),
        (Join-Path $run2 'crash_UTC_new.small.dmp'), (Join-Path $run1 'crash_UTC_old.small.dmp'))
    if ($script:collected.Count -ne $expected.Count -or (Compare-Object $expected $script:collected)) {
        throw "Unexpected collected paths: $($script:collected -join ', ')"
    }
    # Equal names in separate runs must not overwrite each other in the archive.
    Make-Input (Join-Path $run1 'crash_UTC_same.small.dmp') 2
    Make-Input (Join-Path $run2 'crash_UTC_same.small.dmp') 1
    $script:collected = @(); $script:names = @()
    . ([scriptblock]::Create($source.Substring($start, $end - $start)))
    if (($script:names | Where-Object { $_ -like 'crash_UTC_same*' } | Select-Object -Unique).Count -ne 2) {
        throw 'Duplicate dump names would overwrite each other'
    }
    # Verify legacy-only inputs and the full-dump fallback remain readable.
    Remove-Item -LiteralPath $run1, $run2 -Recurse -Force
    Remove-Item -LiteralPath (Join-Path $logs 'crash_UTC_legacy.small.dmp')
    $script:collected = @()
    . ([scriptblock]::Create($source.Substring($start, $end - $start)))
    $expected = @((Join-Path $logs 'freeze.small.dmp'), (Join-Path $logs 'process-end.log'), (Join-Path $overwrite 'crash_UTC_full.dmp'))
    if ($script:collected.Count -ne $expected.Count -or (Compare-Object $expected $script:collected)) {
        throw "Unexpected legacy paths: $($script:collected -join ', ')"
    }
    # Exercise the real copy helper while a writer holds a source open as the client does.
    $functionStart = $source.IndexOf('function Add-File(')
    $functionEnd = $source.IndexOf('# Observe only files', $functionStart)
    if ($functionStart -lt 0 -or $functionEnd -lt 0) { throw 'Add-File function not found' }
    . ([scriptblock]::Create($source.Substring($functionStart, $functionEnd - $functionStart)))
    $staging = Join-Path $root 'staging'
    New-Item -ItemType Directory -Force $staging | Out-Null
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class CollectorTestNative {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
}
'@
    foreach ($name in @('live.small.dmp', 'process-end.log')) {
        $path = Join-Path $root $name
        [IO.File]::WriteAllText($path, 'live diagnostic')
        $handle = [CollectorTestNative]::CreateFile($path, 0x40010000, 1, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
        if ($handle.IsInvalid) { throw "Unable to open live test handle: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
        try { Add-File $path -SharedSource } finally { $handle.Dispose() }
        if ([IO.File]::ReadAllText((Join-Path $staging $name)) -ne 'live diagnostic') { throw "Live copy failed: $name" }
    }
    Write-Host 'Collector discovery and live sharing checks passed.'
} finally {
    if (Test-Path -LiteralPath $root) {
        $actual = (Resolve-Path -LiteralPath $root).Path
        $tempRoot = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).Path.TrimEnd('\')
        if ($actual -ne $root -or (Split-Path $actual -Parent) -ne $tempRoot) {
            throw 'Unexpected collector test directory'
        }
        Remove-Item -LiteralPath $actual -Recurse -Force
    }
}
