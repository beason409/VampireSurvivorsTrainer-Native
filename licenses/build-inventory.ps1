$ErrorActionPreference = 'Stop'
$licenseRoot=[IO.Path]::GetFullPath($PSScriptRoot)
$projectRoot=Split-Path $licenseRoot -Parent
$components=@()
function LicenseFiles($Paths) {
 @($Paths|ForEach-Object {
  $relative=[string]$_
  $full=Join-Path $licenseRoot $relative
  if(!(Test-Path -LiteralPath $full -PathType Leaf)){throw ('Missing license text: '+$relative)}
  @{path='licenses/'+$relative.Replace('\','/');sha256=(Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash}
 })
}
foreach($record in Get-Content (Join-Path $licenseRoot 'nuget-components.json') -Raw|ConvertFrom-Json) {
 if($record.status -ne 'license-text-collected'){throw ('Unresolved license: '+$record.id)}
 $license=$record.license
 if($record.id -in @('HarmonyX','SemanticVersioning')){$license='MIT'}
 $repo=$record.repository
 if($record.id -eq 'HarmonyX'){$repo='https://github.com/BepInEx/HarmonyX'}
 $components+=@{id=$record.id+'@'+$record.version;version=$record.version;license=$license;repository=$repo;sourceRef=$record.sourceRef;metadataUri=$record.metadataUri;packageUri=$record.packageUri;packageSha256=$record.packageSha256;licenseFiles=@(LicenseFiles $record.licenseFiles)}
}
foreach($record in Get-Content (Join-Path $licenseRoot 'fixed-source-components.json') -Raw|ConvertFrom-Json) {
 if($record.status -ne 'license-text-collected'){throw ('Unresolved fixed source: '+$record.id)}
 $license=switch($record.id){'BepInEx'{'LGPL-2.1'};'UnityDoorstop'{'LGPL-2.1'};'Dobby'{'Apache-2.0'};default{'MIT'}}
 $components+=@{id=$record.id+'@'+$record.version;version=$record.version;license=$license;repository=$record.repository;sourceRef=$record.sourceRef;sourceArchive=$record.sourceArchive;licenseFiles=@(LicenseFiles @($record.licenseFiles|ForEach-Object path))}
}
foreach($record in Get-Content (Join-Path $licenseRoot 'desktop-runtime-components.json') -Raw|ConvertFrom-Json) {
 $paths=@($record.licenseFiles|Where-Object {$_ -notmatch '\.nuspec$'}|ForEach-Object {$record.id+'/'+$record.version+'/'+$_})
 if(!$paths.Count){throw ('Desktop runtime license missing: '+$record.id)}
 $components+=@{id=$record.id+'@'+$record.version;version=$record.version;license=$record.licenseDeclaration;repository=$record.repository;sourceRef=$record.sourceRef;metadataUri=$record.metadataUri;licenseFiles=@(LicenseFiles $paths)}
}
$files=@()
$unknown=@()
$bepRoot=Join-Path $projectRoot 'dependencies\bepinex'
foreach($file in Get-ChildItem -LiteralPath $bepRoot -File -Recurse) {
 $relative=[IO.Path]::GetRelativePath($bepRoot,$file.FullName).Replace('\','/')
 $name=$file.Name
 $component=$null
 if($relative -like 'BepInEx/core/*') {
  $component=switch -Wildcard ($name) {
   '0Harmony.dll'{'HarmonyX@2.10.2'}
   'BepInEx.*'{'BepInEx@6.0.0-be.793'}
   'AsmResolver*'{'AsmResolver@6.0.0-beta.5'}
   'AssetRipper.CIL.dll'{'AssetRipper.CIL@1.2.2'}
   'AssetRipper.Primitives.dll'{'AssetRipper.Primitives@3.2.0'}
   'Cpp2IL.Core.dll'{'Cpp2IL@2022.1.0-development.1452'}
   'LibCpp2IL.dll'{'Cpp2IL@2022.1.0-development.1452'}
   'StableNameDotNet.dll'{'Cpp2IL@2022.1.0-development.1452'}
   'WasmDisassembler.dll'{'Cpp2IL@2022.1.0-development.1452'}
   'Disarm.dll'{'Disarm@2022.1.0-master.99'}
   'dobby.dll'{'Dobby@1.0.5'}
   'Gee.External.Capstone.dll'{'AssetRipper.Gee.External.Capstone@2.3.2'}
   'Iced.dll'{'Iced@1.21.0'}
   'Il2CppInterop.*.dll'{[IO.Path]::GetFileNameWithoutExtension($name)+'@1.5.3'}
   'Mono.Cecil*'{'Mono.Cecil@0.11.4'}
   'MonoMod.Backports.dll'{'MonoMod.Backports@1.1.2'}
   'MonoMod.ILHelpers.dll'{'MonoMod.ILHelpers@1.1.0'}
   'MonoMod.RuntimeDetour.dll'{'MonoMod.RuntimeDetour@22.7.31.1'}
   'MonoMod.Utils.dll'{'MonoMod.Utils@22.7.31.1'}
   'SemanticVersioning.dll'{'SemanticVersioning@2.0.2'}
  }
 } elseif($relative -like 'dotnet/*') {
  if($name -match '^(Microsoft\.Bcl\.AsyncInterfaces|Microsoft\.Extensions\.(DependencyInjection(?:\.Abstractions)?|Logging(?:\.Abstractions)?|Options|Primitives))\.dll$') {
   $component=[IO.Path]::GetFileNameWithoutExtension($name)+'@6.0.0'
  } else {$component='BepInEx-mini-coreclr@6.0.7'}
 } elseif($name -in @('winhttp.dll','.doorstop_version')) {$component='UnityDoorstop@4.6.0'}
 elseif($name -in @('changelog.txt','doorstop_config.ini')) {$component='BepInEx@6.0.0-be.793'}
 if(!$component -or $component -notin $components.id){$unknown+=$relative;continue}
 $files+=@{path=$relative;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash;component=$component}
}
if($unknown.Count){throw ('Unattributed dependency files: '+($unknown -join ', '))}
$inventory=[ordered]@{formatVersion=1;scope='License/source attribution for the fixed dependency baseline, not a vulnerability scan or a build attestation';bepinexDistributionSha256='5B63D7D0E92A2448CCD13B738FF95FF1CC1026C6D718555D4DF8E6C824953AC4';components=@($components|Sort-Object id);bepinexDistributionFiles=@($files|Sort-Object path);unresolved=@()}
$inventory|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $licenseRoot 'components.json') -Encoding utf8
[pscustomobject]@{components=$components.Count;baselineFiles=$files.Count;coreFiles=@($files|Where-Object path -like 'BepInEx/core/*').Count;coreDlls=@($files|Where-Object path -like 'BepInEx/core/*.dll').Count;unresolved=$unknown}|ConvertTo-Json -Compress
