$ErrorActionPreference = 'Stop'
$collector = Join-Path $PSScriptRoot '..\collect-logs.ps1'
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $collector), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Collector has PowerShell parse errors.' }
$function = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-BinaryIdentityManifest' }, $true)
if (-not $function) { throw 'Identity function missing.' }
Invoke-Expression $function.Extent.Text

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$dir = Join-Path $tempRoot ('binary-identities-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $dir | Out-Null
try {
    $file = Join-Path $dir 'urSovngarde.exe'
    [IO.File]::WriteAllText($file, 'first')
    $first = Get-BinaryIdentityManifest $dir
    if (@($first.binaries).Count -ne 6 -or ($first.binaries | Where-Object filename -eq 'SkyrimTogether.exe').status -ne 'missing') { throw 'SE launcher identity is missing.' }
    $entry = $first.binaries | Where-Object filename -eq 'urSovngarde.exe'
    if ($entry.status -ne 'present' -or $entry.size -ne 5 -or $entry.sha256 -ne (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant()) { throw 'Present identity is incorrect.' }
    if (($first.binaries | Where-Object filename -eq 'STServer.dll').status -ne 'missing') { throw 'Missing file was not reported.' }
    [IO.File]::WriteAllText($file, 'second')
    $second = Get-BinaryIdentityManifest $dir
    $changed = $second.binaries | Where-Object filename -eq 'urSovngarde.exe'
    if ($changed.size -ne 6 -or $changed.sha256 -eq $entry.sha256) { throw 'Changed file was not detected.' }
    $locked = [IO.File]::Open($file, 'Open', 'ReadWrite', 'None')
    try {
        $unreadable = (Get-BinaryIdentityManifest $dir).binaries | Where-Object filename -eq 'urSovngarde.exe'
        if ($unreadable.status -ne 'unreadable' -or $null -ne $unreadable.sha256 -or $null -ne $unreadable.size) { throw 'Locked file was not reported as unreadable.' }
    } finally { $locked.Dispose() }
    $json = $second | ConvertTo-Json -Depth 4
    if ($json.Contains($dir) -or $json -notmatch 'do not establish source-to-binary provenance') { throw 'Manifest privacy or provenance statement failed.' }
    Write-Host 'Binary identity tests passed.'
} finally {
    $cleanupPath = [IO.Path]::GetFullPath($dir)
    if ([IO.Path]::GetDirectoryName($cleanupPath) -ne $tempRoot -or
        [IO.Path]::GetFileName($cleanupPath) -notmatch '^binary-identities-test-[0-9a-f]{32}$' -or
        ((Get-Item -LiteralPath $cleanupPath).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Refusing cleanup outside the temporary test directory.'
    }
    Remove-Item -LiteralPath $cleanupPath -Recurse -Force
}
