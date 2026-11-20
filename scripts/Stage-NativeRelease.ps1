#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Commit,
    [string[]]$ForbiddenText = @()
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$version = Get-Content -LiteralPath (Join-Path $root 'native/version.json') -Raw | ConvertFrom-Json
if ($version.productVersion -notmatch '^[1-9]\d*\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)$' -or $Commit -notmatch '^[0-9a-f]{40}$') {
    throw 'A stable release version and exact source commit are required.'
}
$head = git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -cne $Commit) { throw 'Release commit does not match HEAD.' }
$status = @(git -C $root status --porcelain)
if ($LASTEXITCODE -ne 0 -or $status.Count) { throw 'Release staging requires a clean checkout.' }
$identities = @(git -C $root log HEAD --format='%ae%n%ce')
if ($LASTEXITCODE -ne 0 -or !$identities.Count -or @($identities | Where-Object { $_ -notmatch '^(?:[^@\s]+@users\.noreply\.github\.com|noreply@github\.com)$' }).Count) {
    throw 'Release ancestry includes author or committer information outside the public GitHub email policy.'
}

$export = & (Join-Path $PSScriptRoot 'Export-PublicSource.ps1') -ForbiddenText $ForbiddenText
$inventory = Get-Content -LiteralPath $export.ManifestPath -Raw | ConvertFrom-Json
$tracked = @(git -C $root -c core.quotepath=false ls-files)
if ($LASTEXITCODE -ne 0 -or !$tracked.Count -or !$inventory.privacyScanPassed -or
    @(Compare-Object $tracked @($inventory.files.path) -CaseSensitive).Count -or @($inventory.files | Where-Object normalizedDocumentation).Count) {
    throw 'The committed source tree differs from the reviewed public export.'
}

$output = Join-Path $root 'artifacts/native/release'
$build = Get-Content -LiteralPath (Join-Path $output 'native-build.json') -Raw | ConvertFrom-Json
$checks = Get-Content -LiteralPath (Join-Path $output 'verification/summary.json') -Raw | ConvertFrom-Json
if ($build.sourceCommit -cne $Commit -or $checks.sourceCommit -cne $Commit -or $build.sourceDirty -ne $false -or
    $build.version -cne $version.productVersion -or $checks.version -cne $version.productVersion -or $build.fileVersion -cne $version.fileVersion -or
    !$build.testsPassed -or !$build.optimization.thinLto -or $checks.failed -ne 0 -or $checks.passed -lt 70 -or $checks.standaloneFiles -ne 1) {
    throw 'The release requires the clean optimized build and passing native checks.'
}

$exeName = 'VampireSurvivorsTrainer.Native.exe'
$dllName = 'VampireSurvivorsTrainer.NativeBackend.dll'
$privatePaths = @($root, [IO.Path]::GetDirectoryName($root), [Environment]::GetFolderPath('UserProfile')) + $ForbiddenText
$markers = @($privatePaths | Where-Object { $_ -and $_.Length -ge 4 } | ForEach-Object { $_; $_.Replace('\','/'); $_.Replace('\','\\') })
foreach ($name in @($exeName, $dllName)) {
    $path = Join-Path $output $name
    $entry = @($build.outputs | Where-Object file -CEQ $name)
    $bytes = [IO.File]::ReadAllBytes($path)
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    if ($entry.Count -ne 1 -or $entry[0].length -ne $bytes.Length -or $entry[0].sha256 -cne $hash -or $entry[0].debugPaths -ne $false) { throw 'A release binary differs from the verified build.' }
    $textForms = @([Text.Encoding]::UTF8.GetString($bytes), [Text.Encoding]::Unicode.GetString($bytes), [Text.Encoding]::Unicode.GetString($bytes,1,$bytes.Length-1))
    foreach ($form in $textForms) {
        foreach ($marker in $markers) {
            if ($form.Contains($marker,[StringComparison]::OrdinalIgnoreCase)) { throw 'A release binary contains a local path or private marker.' }
        }
        if ($form -match '(?i)[a-z]:[\\/]+Users[\\/]+[^\\/\x00-\x1F]{2,80}[\\/]|(?:steam[\\/]+)?userdata[\\/]+[0-9]{5,12}[\\/]') {
            throw 'A release binary contains a concrete user profile or Steam account path.'
        }
    }
}

$sameExe = Join-Path $output "verification/standalone/$exeName"
$builtExe = Join-Path $output $exeName
$exeHash = (Get-FileHash -LiteralPath $builtExe -Algorithm SHA256).Hash.ToLowerInvariant()
if ((Get-FileHash -LiteralPath $sameExe -Algorithm SHA256).Hash.ToLowerInvariant() -cne $exeHash) { throw 'The verified standalone EXE differs from the release EXE.' }
$destination = Join-Path $root "artifacts/native/release-assets/v$($version.productVersion)"
[IO.Directory]::CreateDirectory($destination) | Out-Null
if (@(Get-ChildItem -LiteralPath $destination -Force).Count) { throw 'The release asset directory must be empty.' }
Copy-Item -LiteralPath $builtExe -Destination (Join-Path $destination $exeName)
if ((Get-FileHash -LiteralPath (Join-Path $destination $exeName) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $exeHash) { throw 'Staged EXE failed verification.' }
[IO.File]::WriteAllText((Join-Path $destination 'SHA256SUMS.txt'), "$exeHash  $exeName`n", [Text.Encoding]::ASCII)
[pscustomobject]@{Version=$version.productVersion;Commit=$Commit;Bytes=(Get-Item -LiteralPath $builtExe).Length;Sha256=$exeHash;PublicSourceFiles=$tracked.Count;PrivacyScanPassed=$true}
