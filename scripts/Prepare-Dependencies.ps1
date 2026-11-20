param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$url = 'https://builds.bepinex.dev/projects/bepinex_be/793/BepInEx-Unity.IL2CPP-win-x64-6.0.0-be.793%2Bdbc82f0.zip'
$expected = '5B63D7D0E92A2448CCD13B738FF95FF1CC1026C6D718555D4DF8E6C824953AC4'
$dependencies = Join-Path $root 'dependencies'
New-Item -ItemType Directory -Path $dependencies -Force | Out-Null
$zip = Join-Path $dependencies 'bepinex-793.zip'
if (!(Test-Path -LiteralPath $zip) -or (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $expected) {
    $download = $zip + '.download'
    Invoke-WebRequest -Uri $url -OutFile $download
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $expected) { throw 'Official loader download SHA-256 mismatch; nothing extracted.' }
    Move-Item -LiteralPath $download -Destination $zip -Force
}
$destination = Join-Path $dependencies 'bepinex'
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($entry in $archive.Entries) {
        $candidate = [IO.Path]::GetFullPath((Join-Path $destination $entry.FullName))
        if (!$candidate.StartsWith([IO.Path]::GetFullPath($destination).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Loader archive contains an unsafe path.' }
    }
} finally { $archive.Dispose() }
Expand-Archive -LiteralPath $zip -DestinationPath $destination -Force
[pscustomobject]@{Directory=$destination;Source=$url;Sha256=$expected;Executed=$false}
