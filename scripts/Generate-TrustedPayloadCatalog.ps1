#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PayloadDirectory,
    [Parameter(Mandatory)][string]$SupportedGamePath,
    [Parameter(Mandatory)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$payload = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($PayloadDirectory))
$supported = [IO.Path]::GetFullPath($SupportedGamePath)
$output = [IO.Path]::GetFullPath($OutputPath)
if (!(Test-Path -LiteralPath $payload -PathType Container)) { throw 'The prepared payload directory is missing.' }
if (!(Test-Path -LiteralPath $supported -PathType Leaf)) { throw 'The supported-game configuration is missing.' }
if ($output.Equals($supported,[StringComparison]::OrdinalIgnoreCase) -or $output.Equals($payload,[StringComparison]::OrdinalIgnoreCase) -or
    $output.StartsWith($payload + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'The generated catalog must be outside the payload and cannot replace the supported-game configuration.'
}
function Assert-NoLinkedPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Catalog generation refuses symbolic links and directory junctions.'
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
Assert-NoLinkedPath $payload
Assert-NoLinkedPath $supported
Assert-NoLinkedPath $output
function Get-FileFingerprint([string]$Path) {
    $before = Get-Item -LiteralPath $Path -Force
    if (($before.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Catalog generation refuses linked files.' }
    $length = $before.Length
    $stamp = $before.LastWriteTimeUtc
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $after = Get-Item -LiteralPath $Path -Force
    if ($after.Length -ne $length -or $after.LastWriteTimeUtc -ne $stamp) { throw 'An input file changed while generating the trusted catalog.' }
    [ordered]@{ length=[long]$length; sha256=$hash }
}
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($payload)
$paths = [Collections.Generic.List[string]]::new()
$unique = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($pending.Count -gt 0) {
    foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($pending.Pop())) {
        $attributes = [IO.File]::GetAttributes($entry)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Payload inventory refuses linked files or folders.' }
        if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) { $pending.Push($entry); continue }
        $relative = [IO.Path]::GetRelativePath($payload,$entry).Replace('\','/')
        if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains(':') -or $relative.Contains('\') -or
            @($relative.Split('/') | Where-Object { $_ -in @('','.','..') }).Count -gt 0 -or !$unique.Add($relative)) {
            throw 'Payload inventory contains an invalid or case-insensitive duplicate relative path.'
        }
        $paths.Add($relative)
    }
}
if ($paths.Count -eq 0) { throw 'An empty payload cannot be trusted.' }
$paths.Sort([StringComparer]::Ordinal)
$files = @(foreach ($relative in $paths) {
    $fingerprint = Get-FileFingerprint (Join-Path $payload $relative)
    [ordered]@{ path=$relative; length=$fingerprint.length; sha256=$fingerprint.sha256 }
})
$catalog = [ordered]@{ formatVersion=1; files=$files; supportedGame=(Get-FileFingerprint $supported) }
# Stable UTF-8/LF bytes contain only relative names, lengths and digests. No
# build time, developer path, account name, certificate or private key is used.
$json = ($catalog | ConvertTo-Json -Depth 5).Replace("`r`n","`n") + "`n"
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output)) | Out-Null
$temporary = $output + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
try {
    [IO.File]::WriteAllText($temporary,$json,[Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $output -Force
}
finally { if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary } }
[pscustomobject]@{ CatalogPath=$output; Files=$files.Count; Sha256=(Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant(); SupportedGameSha256=$catalog.supportedGame.sha256 }
