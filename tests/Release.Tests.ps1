#requires -Version 7.4
[CmdletBinding()]
param([string]$VerifierScript = (Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts/Verify-Release.ps1'))
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$VerifierScript = [IO.Path]::GetFullPath($VerifierScript)
if (!(Test-Path -LiteralPath $VerifierScript -PathType Leaf)) { throw 'Release verification script was not found.' }
$runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)
$fixtureRoot = Join-Path $projectRoot ('artifacts/tests/release-' + $runId)
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()

function New-Fixture([string]$Name) {
    $directory = Join-Path $fixtureRoot $Name
    New-Item -ItemType Directory -Path (Join-Path $directory 'payload') | Out-Null
    [IO.File]::WriteAllText((Join-Path $directory 'application.bin'), 'original application')
    [IO.File]::WriteAllText((Join-Path $directory 'payload/library.bin'), 'original library')
    $files = @('application.bin','payload/library.bin') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $directory $_)
        [pscustomobject]@{ path=$_; length=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    }
    Write-Manifest $directory $files
    return $directory
}
function Write-Manifest([string]$Directory, [object[]]$Files) {
    [ordered]@{ formatVersion=1; product='Release verifier fixture'; version='0.0.0-test'; author='Fixture'; files=@($Files) } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Directory 'release-manifest.json') -Encoding utf8
}
function Assert-Verification([string]$Name, [string]$Directory, [bool]$MustReject, [string]$RejectionPattern = '') {
    $rejected = $false
    $detail = $null
    $response = @()
    try { $response = @(& $VerifierScript -Directory $Directory) }
    catch { $rejected = $true; $detail = $_.Exception.Message }
    $passed = $rejected -eq $MustReject
    if ($MustReject -and $RejectionPattern -and $detail -notmatch $RejectionPattern) { $passed = $false }
    if (!$MustReject -and (!$response -or $response[-1].Matches -ne $true)) { $passed = $false }
    $results.Add([pscustomobject]@{ name=$Name; passed=$passed; rejected=$rejected; expectedRejection=$MustReject; detail=$detail })
    Write-Host ($(if ($passed) { 'PASS ' } else { 'FAIL ' }) + $Name)
}

$valid = New-Fixture 'valid'
New-Item -ItemType Directory -Path (Join-Path $valid 'data') | Out-Null
[IO.File]::WriteAllText((Join-Path $valid 'data/local-settings.json'), '{"localFixture":true}')
Assert-Verification 'valid package allows mutable user data' $valid $false

$changed = New-Fixture 'changed'
[IO.File]::WriteAllText((Join-Path $changed 'application.bin'), 'modified application')
Assert-Verification 'same-length application change is rejected' $changed $true

$missing = New-Fixture 'missing'
[IO.File]::Delete((Join-Path $missing 'payload/library.bin'))
Assert-Verification 'missing payload file is rejected' $missing $true

$extra = New-Fixture 'extra'
[IO.File]::WriteAllText((Join-Path $extra 'unlisted.bin'), 'unexpected')
Assert-Verification 'unlisted release file is rejected' $extra $true

$duplicate = New-Fixture 'duplicate-case'
$duplicateManifest = Get-Content -LiteralPath (Join-Path $duplicate 'release-manifest.json') -Raw | ConvertFrom-Json
$duplicateManifest.files += [pscustomobject]@{
    path='APPLICATION.BIN'; length=$duplicateManifest.files[0].length; sha256=$duplicateManifest.files[0].sha256
}
Write-Manifest $duplicate $duplicateManifest.files
Assert-Verification 'case-insensitive duplicate paths are rejected' $duplicate $true

$escape = New-Fixture 'path-escape'
$outside = Join-Path $fixtureRoot 'outside.bin'
[IO.File]::WriteAllText($outside, 'fixture-only outside file')
$outsideInfo = Get-Item -LiteralPath $outside
Write-Manifest $escape @([pscustomobject]@{ path='../outside.bin'; length=$outsideInfo.Length; sha256=(Get-FileHash -LiteralPath $outside).Hash })
Assert-Verification 'existing file outside release root cannot be declared' $escape $true

$absolute = New-Fixture 'absolute-path'
Write-Manifest $absolute @([pscustomobject]@{ path=$outside; length=$outsideInfo.Length; sha256=(Get-FileHash -LiteralPath $outside).Hash })
Assert-Verification 'absolute manifest paths are rejected' $absolute $true

$declaredData = New-Fixture 'declared-data'
New-Item -ItemType Directory -Path (Join-Path $declaredData 'data') | Out-Null
$dataPath = Join-Path $declaredData 'data/local-save.bin'
[IO.File]::WriteAllText($dataPath, 'mutable fixture-only user data')
$dataInfo = Get-Item -LiteralPath $dataPath
$dataManifest = Get-Content -LiteralPath (Join-Path $declaredData 'release-manifest.json') -Raw | ConvertFrom-Json
$dataManifest.files += [pscustomobject]@{ path='data/local-save.bin'; length=$dataInfo.Length; sha256=(Get-FileHash -LiteralPath $dataPath).Hash }
Write-Manifest $declaredData $dataManifest.files
Assert-Verification 'manifest cannot declare mutable user data' $declaredData $true 'user data'

$selfDeclared = New-Fixture 'manifest-self'
Write-Manifest $selfDeclared @([pscustomobject]@{ path='release-manifest.json'; length=1L; sha256=('0' * 64) })
Assert-Verification 'manifest cannot declare itself' $selfDeclared $true 'itself'

$reparseChecks = 0
$reparseSkip = $null
try {
    $target = Join-Path $fixtureRoot 'link-target'
    New-Item -ItemType Directory -Path $target | Out-Null
    [IO.File]::WriteAllText((Join-Path $target 'linked.bin'), 'fixture-only link content')
    $declaredLink = New-Fixture 'declared-link'
    $linkType = if ($IsWindows) { 'Junction' } else { 'SymbolicLink' }
    New-Item -ItemType $linkType -Path (Join-Path $declaredLink 'linked') -Target $target -ErrorAction Stop | Out-Null
}
catch {
    $reparseSkip = $_.Exception.Message
    Write-Host ('SKIP unavailable reparse fixture: ' + $reparseSkip)
}
if ($null -eq $reparseSkip) {
    $linkInfo = Get-Item -LiteralPath (Join-Path $target 'linked.bin')
    Write-Manifest $declaredLink @([pscustomobject]@{ path='linked/linked.bin'; length=$linkInfo.Length; sha256=(Get-FileHash -LiteralPath $linkInfo.FullName).Hash })
    Assert-Verification 'declared file through reparse directory is rejected' $declaredLink $true
    $reparseChecks++

    $unlistedLink = New-Fixture 'unlisted-link'
    New-Item -ItemType $linkType -Path (Join-Path $unlistedLink 'linked') -Target $target -ErrorAction Stop | Out-Null
    Assert-Verification 'unlisted reparse directory is rejected' $unlistedLink $true
    $reparseChecks++

    $rootLinkTarget = New-Fixture 'linked-root-target'
    $linkedRoot = Join-Path $fixtureRoot 'linked-root'
    New-Item -ItemType $linkType -Path $linkedRoot -Target $rootLinkTarget -ErrorAction Stop | Out-Null
    Assert-Verification 'linked release root is rejected' $linkedRoot $true
    $reparseChecks++
}

$manifestLinkSkip = $null
try {
    $linkedManifest = New-Fixture 'linked-manifest'
    $manifestTarget = Join-Path $valid 'release-manifest.json'
    [IO.File]::Delete((Join-Path $linkedManifest 'release-manifest.json'))
    New-Item -ItemType SymbolicLink -Path (Join-Path $linkedManifest 'release-manifest.json') -Target $manifestTarget -ErrorAction Stop | Out-Null
}
catch {
    $manifestLinkSkip = $_.Exception.Message
    Write-Host ('SKIP unavailable manifest file-link fixture: ' + $manifestLinkSkip)
}
if ($null -eq $manifestLinkSkip) {
    Assert-Verification 'linked manifest is rejected before reading it' $linkedManifest $true 'linked manifest'
    $reparseChecks++
}

$report = [ordered]@{
    createdUtc=[DateTime]::UtcNow.ToString('O')
    total=$results.Count
    passed=@($results | Where-Object passed).Count
    failed=@($results | Where-Object { !$_.passed }).Count
    reparseChecks=$reparseChecks
    reparseSkip=$reparseSkip
    manifestLinkSkip=$manifestLinkSkip
    checks=@($results.ToArray())
}
$reportPath = Join-Path $fixtureRoot 'report.json'
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
[pscustomobject]@{ ReportPath=$reportPath; Passed=$report.passed; Total=$report.total; ReparseChecks=$reparseChecks; SkippedReparse=($null -ne $reparseSkip); SkippedManifestLink=($null -ne $manifestLinkSkip) }
if ($report.failed -gt 0) { throw ('Release fixtures failed: ' + $report.failed + '. See ' + $reportPath) }
