param(
  [switch]$FrameworkDependent,
  [string]$InteropDirectory = $env:VS_TRAINER_INTEROP,
  [string]$RepositoryUrl = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $projectRoot
$env:DOTNET_CLI_HOME = Join-Path $projectRoot '.dotnet'
$env:NUGET_PACKAGES = if ($env:VS_TRAINER_NUGET_PACKAGES) { $env:VS_TRAINER_NUGET_PACKAGES } else { Join-Path $projectRoot 'dependencies\nuget' }
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
if (!$InteropDirectory) { $InteropDirectory = Join-Path $projectRoot 'dependencies\interop' }
$InteropDirectory = [IO.Path]::GetFullPath($InteropDirectory)
foreach ($interface in @('UnityEngine.CoreModule.dll','UnityEngine.ImageConversionModule.dll','VampireSurvivors.Runtime.dll','Assembly-CSharp.dll','PhaserPort.dll','Il2Cppmscorlib.dll','Unity.Mathematics.dll')) {
  if (-not (Test-Path -LiteralPath (Join-Path $InteropDirectory $interface) -PathType Leaf)) { throw "Missing generated game interface $interface. Set -InteropDirectory before packaging." }
}
# Build the plugin being packaged instead of silently reusing an older Release DLL.
& (Join-Path $PSScriptRoot 'Prepare-Dependencies.ps1') | Out-Host
& dotnet build 'src\Trainer.Plugin\Trainer.Plugin.csproj' -c Release -t:Rebuild -m:1 -p:BuildInParallel=false /nodeReuse:false -p:DebugType=None -p:DebugSymbols=false "-p:InteropDirectory=$InteropDirectory" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Plugin build failed.' }
$pluginVersion = ([xml](Get-Content -LiteralPath 'src\Trainer.Plugin\Trainer.Plugin.csproj' -Raw)).Project.PropertyGroup.Version
$desktopVersion = ([xml](Get-Content -LiteralPath 'src\Trainer.Desktop\Trainer.Desktop.csproj' -Raw)).Project.PropertyGroup.Version
$deploymentMode = if ($FrameworkDependent) { 'framework-dependent' } else { 'self-contained' }
$runtimeRequirement = if ($FrameworkDependent) { '.NET 10 Windows Desktop Runtime x64' } else { $null }
$packageVariant = if ($FrameworkDependent) { 'lite' } else { 'full' }
$packageId = 'VampireSurvivorsTrainer-' + $desktopVersion + '-' + $packageVariant + '-test-win-x64-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)
$release = Join-Path (Join-Path $projectRoot 'artifacts\packages') $packageId
# The trusted input set is complete before Core or Desktop is compiled.
# Neither Core nor the catalog itself is a payload input, avoiding self-hashes.
$payload = Join-Path $release 'payload'
New-Item -ItemType Directory -Path $payload | Out-Null
Expand-Archive -LiteralPath (Join-Path $projectRoot 'dependencies\bepinex-793.zip') -DestinationPath $payload
$pluginDir = Join-Path $payload 'BepInEx\plugins\SurvivorsTrainer'
New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null
foreach ($assembly in @('Trainer.Plugin.dll','Trainer.Shared.dll')) {
  Copy-Item -LiteralPath (Join-Path $projectRoot ('src\Trainer.Plugin\bin\Release\net6.0\' + $assembly)) -Destination $pluginDir
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'config\supported-game.json') -Destination $release
Copy-Item -LiteralPath (Join-Path $release 'supported-game.json') -Destination $pluginDir
$catalogPath = Join-Path $projectRoot ('artifacts/build-inputs/' + $packageId + '/trusted-payload-catalog.json')
$catalog = & (Join-Path $PSScriptRoot 'Generate-TrustedPayloadCatalog.ps1') -PayloadDirectory $payload -SupportedGamePath (Join-Path $release 'supported-game.json') -OutputPath $catalogPath
# Explicitly rebuild the referenced net10 libraries. A preceding developer or
# test build can otherwise leave cached CodeView/PDB paths in project-reference
# outputs, even when the desktop publish itself requests DebugType=None.
& dotnet build 'src\Trainer.Core\Trainer.Core.csproj' -c Release -t:Rebuild -m:1 -p:BuildInParallel=false /nodeReuse:false -p:DebugType=None -p:DebugSymbols=false "-p:TrustedPayloadCatalogPath=$catalogPath" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Core release rebuild failed.' }
$selfContained = if ($FrameworkDependent) { 'false' } else { 'true' }
& dotnet publish 'src\Trainer.Desktop\Trainer.Desktop.csproj' -c Release -r win-x64 --self-contained $selfContained -o $release -m:1 -p:BuildInParallel=false -p:PublishSingleFile=false -p:DebugType=None -p:DebugSymbols=false "-p:TrustedPayloadCatalogPath=$catalogPath" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Desktop publish failed.' }
$ownedDesktopAssemblies = @('VampireSurvivorsTrainer.dll','Trainer.Core.dll','Trainer.Shared.dll')
Add-Type -AssemblyName System.Reflection.Metadata
function Assert-NoOwnedDebugPaths([string]$AssemblyPath) {
    $assemblyStream = [IO.File]::OpenRead($AssemblyPath)
    $pe = [Reflection.PortableExecutable.PEReader]::new($assemblyStream)
    try {
        foreach ($entry in $pe.ReadDebugDirectory()) {
            if ($entry.Type -in @([Reflection.PortableExecutable.DebugDirectoryEntryType]::CodeView,[Reflection.PortableExecutable.DebugDirectoryEntryType]::EmbeddedPortablePdb)) {
                throw ('Release blocked: an application assembly contains debug symbols. Rebuild with DebugType=None: ' + [IO.Path]::GetFileName($AssemblyPath))
            }
        }
    }
    finally { $pe.Dispose(); $assemblyStream.Dispose() }
}
foreach ($name in $ownedDesktopAssemblies) { Assert-NoOwnedDebugPaths (Join-Path $release $name) }
foreach ($assembly in @('Trainer.Plugin.dll','Trainer.Shared.dll')) {
  Assert-NoOwnedDebugPaths (Join-Path $pluginDir $assembly)
}
$after = & (Join-Path $PSScriptRoot 'Generate-TrustedPayloadCatalog.ps1') -PayloadDirectory $payload -SupportedGamePath (Join-Path $release 'supported-game.json') -OutputPath $catalogPath
if ($after.Sha256 -ne $catalog.Sha256) { throw 'The prepared payload changed after the trusted catalog was embedded.' }
$coreStream = [IO.File]::OpenRead((Join-Path $release 'Trainer.Core.dll'))
$corePe = [Reflection.PortableExecutable.PEReader]::new($coreStream)
try {
  $metadata = [Reflection.Metadata.PEReaderExtensions]::GetMetadataReader($corePe)
  $embedded = $false
  foreach ($handle in $metadata.ManifestResources) {
    $resource = $metadata.GetManifestResource($handle)
    if ($metadata.GetString($resource.Name) -ne 'Trainer.Core.TrustedPayloadCatalog.json') { continue }
    $reader = $corePe.GetSectionData($corePe.PEHeaders.CorHeader.ResourcesDirectory.RelativeVirtualAddress).GetReader()
    $reader.Offset = [int]$resource.Offset
    $length = $reader.ReadInt32()
    $digest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($reader.ReadBytes($length))).ToLowerInvariant()
    if ($digest -ne $catalog.Sha256) { throw 'Core contains a different trusted payload catalog.' }
    $embedded = $true
  }
  if (!$embedded) { throw 'Core is missing the required embedded trusted payload catalog.' }
}
finally { $corePe.Dispose(); $coreStream.Dispose() }
$integrity = Start-Process -FilePath (Join-Path $release 'VampireSurvivorsTrainer.exe') -ArgumentList '--verify-companion-files' -WindowStyle Hidden -PassThru
try {
  if (!$integrity.WaitForExit(30000)) { $integrity.Kill(); throw 'Embedded companion-file verification timed out.' }
  $integrity.Refresh()
  if ($null -eq $integrity.ExitCode -or $integrity.ExitCode -ne 0) { throw ('Embedded companion-file verification failed (exit ' + $integrity.ExitCode + ').') }
}
finally { $integrity.Dispose() }
foreach ($document in @('LICENSE','THIRD-PARTY-NOTICES.md','docs\BUILDING.md','docs\COMPACT-ARCHITECTURE.md')) {
  if (Test-Path -LiteralPath (Join-Path $projectRoot $document)) {
    $documentTarget = Join-Path $release $document
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($documentTarget)) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot $document) -Destination $documentTarget -Force
  }
}
foreach ($name in @('README.md','VALIDATION.md','PRIVACY.md','STAT-EDITOR.md','SECURITY.md')) {
  $source = Join-Path $projectRoot ('docs\public\' + $name)
  if (!(Test-Path -LiteralPath $source)) { $source = Join-Path $projectRoot $(if ($name -in @('README.md','VALIDATION.md')) { $name } else { 'docs\' + $name }) }
  if (Test-Path -LiteralPath $source) {
    $destination = Join-Path $release $(if ($name -in @('README.md','VALIDATION.md')) { $name } else { 'docs\' + $name })
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
  }
}
$readme = Join-Path $release 'README.md'
if (Test-Path -LiteralPath $readme -PathType Leaf) {
  $notice = if ($FrameworkDependent) {
    "> $desktopVersion · 轻量版：需要 .NET 10 Windows Desktop Runtime x64。`r`n> Lite build: requires .NET 10 Windows Desktop Runtime x64. https://dotnet.microsoft.com/download/dotnet/10.0"
  } else {
    "> $desktopVersion · 完整版：内置桌面运行时，解压后直接运行。`r`n> Full build: the desktop runtime is included. Extract the complete ZIP and run the app."
  }
  [IO.File]::WriteAllText($readme, $notice + "`r`n`r`n" + [IO.File]::ReadAllText($readme), [Text.UTF8Encoding]::new($false))
}
$previewSource = Join-Path $projectRoot 'docs\public\images\features.png'
if (!(Test-Path -LiteralPath $previewSource)) { $previewSource = Join-Path $projectRoot 'docs\images\features.png' }
if (Test-Path -LiteralPath $previewSource -PathType Leaf) {
    $previewTarget = Join-Path $release 'docs\images\features.png'
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($previewTarget)) -Force | Out-Null
    Copy-Item -LiteralPath $previewSource -Destination $previewTarget
}
if (!(Test-Path -LiteralPath (Join-Path $projectRoot 'licenses\components.json'))) { throw 'Third-party license inventory is required before a public package.' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'licenses') -Destination (Join-Path $release 'licenses') -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Verify-Release.ps1') -Destination $release
$sourceCommit = $null
if (Test-Path -LiteralPath (Join-Path $projectRoot '.git')) {
  $sourceCommit = (& git -C $projectRoot rev-parse HEAD 2>$null | Select-Object -First 1)
  if ($LASTEXITCODE -ne 0) { $sourceCommit = $null }
  $dirty = @(& git -C $projectRoot status --porcelain --untracked-files=no)
  if ($dirty.Count -gt 0) { throw 'Commit reviewed source changes before publishing a package with a source revision.' }
}
@{author='Beason9527';product='Vampire Survivors Trainer';repositoryUrl=$RepositoryUrl;version=$desktopVersion;desktopVersion=$desktopVersion;pluginVersion=$pluginVersion;deploymentMode=$deploymentMode;runtimeRequirement=$runtimeRequirement} | ConvertTo-Json | Set-Content (Join-Path $release 'project-info.json') -Encoding utf8
$signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $release 'VampireSurvivorsTrainer.exe')
$inventory = @(Get-ChildItem -LiteralPath $release -Recurse -File -Force | Sort-Object FullName | ForEach-Object { @{path=[IO.Path]::GetRelativePath($release,$_.FullName).Replace('\','/');length=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()} })
@{formatVersion=1;product='Vampire Survivors Trainer';author='Beason9527';version=$desktopVersion;repositoryUrl=$RepositoryUrl;build=@{sourceCommit=$sourceCommit;builtUtc=[DateTime]::UtcNow.ToString('o');pluginInteropRequired=$true;origin='Maintainer local build; no hosted build attestation claimed';desktopVersion=$desktopVersion;pluginVersion=$pluginVersion;deploymentMode=$deploymentMode;runtimeRequirement=$runtimeRequirement;trustedPayloadFiles=$catalog.Files;trustedCatalogSha256=$catalog.Sha256};signature=@{status=$signature.Status.ToString();subject=$signature.SignerCertificate.Subject};files=$inventory} | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $release 'release-manifest.json') -Encoding utf8
& (Join-Path $PSScriptRoot 'Verify-Release.ps1') -Directory $release | Out-Host
# Only this new package's files are included. Existing release/data is never touched.
$archive = $release + '.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($release, $archive, [IO.Compression.CompressionLevel]::SmallestSize, $false)
$checksums = $archive + '.sha256'
((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($archive)) | Set-Content -LiteralPath $checksums -Encoding ascii
[pscustomobject]@{ Directory=$release; Archive=$archive; Checksums=$checksums; SourceCommit=$sourceCommit; SignatureStatus=$signature.Status.ToString(); ArchiveLength=(Get-Item -LiteralPath $archive).Length; FrameworkDependent=[bool]$FrameworkDependent; DeploymentMode=$deploymentMode; RuntimeRequirement=$runtimeRequirement; TrustedCatalog=$catalogPath; TrustedCatalogSha256=$catalog.Sha256; TrustedPayloadFiles=$catalog.Files }
