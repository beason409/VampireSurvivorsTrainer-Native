#requires -Version 7.4
param([string]$Directory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($Directory).TrimEnd([IO.Path]::DirectorySeparatorChar)
if (((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Release verification refuses a linked root.' }
$manifestPath = Join-Path $root 'release-manifest.json'
if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Missing release-manifest.json. This check requires an official release directory.' }
if (((Get-Item -LiteralPath $manifestPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Release verification refuses a linked manifest.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.formatVersion -ne 1 -or !$manifest.files -or $manifest.files.Count -gt 10000) { throw 'Unsupported or empty release manifest.' }
$known = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$issues = [Collections.Generic.List[string]]::new()
foreach ($entry in $manifest.files) {
    $relative = [string]$entry.path
    if (!$relative -or $relative.Contains('\') -or $relative.StartsWith('/') -or $relative.Contains(':') -or @($relative.Split('/') | Where-Object { $_ -in @('','.','..') }).Count -gt 0 -or !$known.Add($relative) -or $entry.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid or duplicate manifest file path/hash.' }
    if ($relative -ieq 'release-manifest.json' -or $relative -ieq 'data' -or $relative.StartsWith('data/',[StringComparison]::OrdinalIgnoreCase) -or [long]$entry.length -lt 0) { throw 'Manifest cannot declare itself or user data.' }
    $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (!$path.StartsWith($root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escapes release directory.' }
    $parent = $path
    while ($parent.Length -ge $root.Length) {
        if (Test-Path -LiteralPath $parent) {
            if (((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Release verification refuses reparse points.' }
        }
        if ($parent -eq $root) { break }
        $parent = [IO.Path]::GetDirectoryName($parent)
    }
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { $issues.Add('Missing: ' + $relative); continue }
    if ((Get-Item -LiteralPath $path).Length -ne [long]$entry.length -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { $issues.Add('Changed: ' + $relative) }
}
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($root)
while ($pending.Count -gt 0) {
    foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
        $relative = [IO.Path]::GetRelativePath($root,$item.FullName).Replace('\','/')
        # User data is deliberately outside the release inventory. Never follow
        # its children, including links, when inspecting the release itself.
        if ($relative -ieq 'data' -and $item.PSIsContainer) { continue }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Release verification refuses unlisted reparse points.' }
        if ($item.PSIsContainer) { $pending.Push($item.FullName); continue }
        if ($relative -ieq 'release-manifest.json') { continue }
        if (!$known.Contains($relative)) { $issues.Add('Unlisted: ' + $relative) }
    }
}
$result = [pscustomobject]@{ Product=$manifest.product; Version=$manifest.version; Author=$manifest.author; Files=$known.Count; Matches=($issues.Count -eq 0); LocalManifestOnly=$true; Issues=@($issues) }
$result
if ($issues.Count -gt 0) { throw 'Release files do not match the local manifest. Compare with the published SHA256SUMS and download source.' }
