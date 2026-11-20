param([switch]$IncludeRuntime,[switch]$OnlyUnresolved)
$ErrorActionPreference = 'Stop'
$licenseRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$client = [Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(90)
$client.DefaultRequestHeaders.UserAgent.ParseAdd('VampireSurvivorsTrainer-LicenseAudit/1.0')
$packageSpecs = @(
  @('HarmonyX','2.10.2'), @('AsmResolver','6.0.0-beta.5'),
  @('AsmResolver.DotNet','6.0.0-beta.5'), @('AsmResolver.PE','6.0.0-beta.5'), @('AsmResolver.PE.File','6.0.0-beta.5'),
  @('AssetRipper.CIL','1.2.2'), @('AssetRipper.Primitives','3.2.0'),
  @('Disarm','2022.1.0-master.99'), @('AssetRipper.Gee.External.Capstone','2.3.2'), @('Iced','1.21.0'),
  @('Il2CppInterop.Common','1.5.3'), @('Il2CppInterop.Generator','1.5.3'),
  @('Il2CppInterop.HarmonySupport','1.5.3'), @('Il2CppInterop.Runtime','1.5.3'),
  @('Mono.Cecil','0.11.4'), @('MonoMod.Backports','1.1.2'), @('MonoMod.ILHelpers','1.1.0'),
  @('MonoMod.RuntimeDetour','22.7.31.1'), @('MonoMod.Utils','22.7.31.1'), @('SemanticVersioning','2.0.2'),
  @('Microsoft.Bcl.AsyncInterfaces','6.0.0'), @('Microsoft.Extensions.DependencyInjection.Abstractions','6.0.0'),
  @('Microsoft.Extensions.DependencyInjection','6.0.0'), @('Microsoft.Extensions.Logging.Abstractions','6.0.0'),
  @('Microsoft.Extensions.Logging','6.0.0'), @('Microsoft.Extensions.Options','6.0.0'), @('Microsoft.Extensions.Primitives','6.0.0')
)
if ($IncludeRuntime) { $packageSpecs += ,@('Microsoft.NETCore.App.Runtime.win-x64','6.0.7') }
$sourceRefs = @{
  'HarmonyX'='v2.10.2'; 'Il2CppInterop.Common'='dbda1cb353b0f4253345dc45136d170b9e50a5a0';
  'Il2CppInterop.Generator'='dbda1cb353b0f4253345dc45136d170b9e50a5a0';
  'Il2CppInterop.HarmonySupport'='dbda1cb353b0f4253345dc45136d170b9e50a5a0';
  'Il2CppInterop.Runtime'='dbda1cb353b0f4253345dc45136d170b9e50a5a0';
  'Mono.Cecil'='0.11.4'; 'MonoMod.RuntimeDetour'='v22.07.31.01'; 'MonoMod.Utils'='v22.07.31.01';
  'SemanticVersioning'='c52c9d4c5d34a6a3bb6ff45f7276d8b6e951a42d'
}
$repositories=@{
 'Il2CppInterop.Common'='https://github.com/BepInEx/Il2CppInterop'; 'Il2CppInterop.Generator'='https://github.com/BepInEx/Il2CppInterop';
 'Il2CppInterop.HarmonySupport'='https://github.com/BepInEx/Il2CppInterop'; 'Il2CppInterop.Runtime'='https://github.com/BepInEx/Il2CppInterop';
 'Mono.Cecil'='https://github.com/jbevain/cecil'; 'MonoMod.RuntimeDetour'='https://github.com/MonoMod/MonoMod'; 'MonoMod.Utils'='https://github.com/MonoMod/MonoMod'
}
function DownloadBytes([string]$Uri) { return ,$client.GetByteArrayAsync($Uri).GetAwaiter().GetResult() }
function ChildText($Node,[string]$Name) {
  $child=$Node.SelectSingleNode("*[local-name()='$Name']")
  if ($child) { return [string]$child.InnerText }
  return ''
}
$records = [Collections.Generic.List[object]]::new()
if($OnlyUnresolved -and (Test-Path (Join-Path $licenseRoot 'nuget-components.json'))) {
  $previous=Get-Content (Join-Path $licenseRoot 'nuget-components.json') -Raw|ConvertFrom-Json
  foreach($item in $previous|Where-Object status -eq 'license-text-collected') { $records.Add($item) }
  $resolved=@($records|ForEach-Object id)
  $packageSpecs=@($packageSpecs|Where-Object { $_[0] -notin $resolved })
}
$extraFeeds=@()
foreach($feed in @('https://nuget.bepinex.dev/v3/index.json','https://nuget.samboy.dev/v3/index.json')) {
 try {
  $index=[Text.Encoding]::UTF8.GetString((DownloadBytes $feed))|ConvertFrom-Json
  $resource=$index.resources|Where-Object { $_.'@type' -like 'PackageBaseAddress*' }|Select-Object -First 1
  if($resource) { $extraFeeds+=([string]$resource.'@id').TrimEnd('/')+'/' }
 } catch { Write-Host ('Optional upstream feed unavailable: '+$feed) }
}
try {
  foreach ($spec in $packageSpecs) {
    $packageId=$spec[0]; $version=$spec[1]
    $folder=Join-Path $licenseRoot ($packageId+'/'+$version)
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    $baseUri='https://api.nuget.org/v3-flatcontainer/'+$packageId.ToLowerInvariant()+'/'+$version.ToLowerInvariant()+'/'+$packageId.ToLowerInvariant()
    $packageUri=$baseUri+'.'+$version.ToLowerInvariant()+'.nupkg'
    $record=[ordered]@{ id=$packageId;version=$version;metadataUri=$baseUri+'.nuspec';packageUri=$packageUri;license='UNKNOWN';repository='';sourceRef='';licenseFiles=@();status='unresolved';error=$null }
    try {
      $metadataBytes=$null
      foreach($feedBase in @('https://api.nuget.org/v3-flatcontainer/')+$extraFeeds) {
        $baseUri=$feedBase+$packageId.ToLowerInvariant()+'/'+$version.ToLowerInvariant()+'/'+$packageId.ToLowerInvariant()
        try { $metadataBytes=DownloadBytes ($baseUri+'.nuspec');break } catch { }
      }
      if(!$metadataBytes) { throw 'Exact package metadata unavailable from the configured official feeds.' }
      $record.metadataUri=$baseUri+'.nuspec';$packageUri=$baseUri+'.'+$version.ToLowerInvariant()+'.nupkg';$record.packageUri=$packageUri
      [IO.File]::WriteAllBytes((Join-Path $folder 'package.nuspec'),$metadataBytes)
      $metadataXml=[xml][Text.Encoding]::UTF8.GetString($metadataBytes)
      $metadata=$metadataXml.SelectSingleNode("//*[local-name()='metadata']")
      $licenseNode=$metadata.SelectSingleNode("*[local-name()='license']")
      if ($licenseNode) { $record.license=[string]$licenseNode.InnerText }
      $repository=$metadata.SelectSingleNode("*[local-name()='repository']")
      if ($repository) { $record.repository=[string]$repository.GetAttribute('url'); $record.sourceRef=[string]$repository.GetAttribute('commit') }
      if ($repositories.ContainsKey($packageId)) { $record.repository=$repositories[$packageId] }
      if ($sourceRefs.ContainsKey($packageId)) { $record.sourceRef=$sourceRefs[$packageId] }
      $record.copyright=ChildText $metadata 'copyright'
      $packageBytes=DownloadBytes $packageUri
      $record.packageSha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($packageBytes))
      $stream=[IO.MemoryStream]::new($packageBytes,$false)
      $zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Read)
      try {
        foreach($entry in $zip.Entries) {
          if ($entry.Name -notmatch '^(LICENSE(?:[._-].*)?|COPYING(?:[._-].*)?|NOTICE(?:[._-].*)?|THIRD[-_]?PARTY[-_]?NOTICES?(?:[._-].*)?)$') { continue }
          $destination=Join-Path $folder $entry.Name
          $inputStream=$entry.Open();$outputStream=[IO.File]::Create($destination)
          try {$inputStream.CopyTo($outputStream)} finally {$outputStream.Dispose();$inputStream.Dispose()}
          $record.licenseFiles+=([IO.Path]::GetRelativePath($licenseRoot,$destination).Replace('\','/'))
        }
      } finally {$zip.Dispose();$stream.Dispose()}
      if ($record.licenseFiles.Count -eq 0 -and $record.repository -match '^https://github.com/([^/]+/[^/]+?)(?:\.git)?/?$' -and $record.sourceRef) {
        $repo=$Matches[1]
        foreach($name in @('LICENSE','LICENSE.txt','LICENSE.md','LICENSE.TXT','license.md','License.md','LICENSE-MIT')) {
          $uri='https://raw.githubusercontent.com/'+$repo+'/'+$record.sourceRef+'/'+$name
          try {
            $bytes=DownloadBytes $uri
            $destination=Join-Path $folder $name
            [IO.File]::WriteAllBytes($destination,$bytes)
            $record.licenseFiles+=([IO.Path]::GetRelativePath($licenseRoot,$destination).Replace('\','/'))
            $record.licenseUri=$uri
            break
          } catch [Net.Http.HttpRequestException] { }
        }
      }
      if ($record.licenseFiles.Count -gt 0) { $record.status='license-text-collected' }
      else { $record.error='No license text found in this package or its pinned source root. Release review required.' }
      $dependencies=$metadata.SelectNodes(".//*[local-name()='dependency']")
      $record.dependencies=@($dependencies|ForEach-Object{[ordered]@{id=$_.GetAttribute('id');version=$_.GetAttribute('version')}}|Sort-Object { $_.id+' '+$_.version } -Unique)
    } catch { $record.error=$_.Exception.Message }
    $records.Add($record)
    Write-Host ($packageId+' '+$version+': '+$record.status)
  }
} finally { $client.Dispose() }
$records|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $licenseRoot 'nuget-components.json') -Encoding utf8
[pscustomobject]@{packages=$records.Count;unresolved=@($records|Where-Object status -ne 'license-text-collected'|ForEach-Object id)}|ConvertTo-Json -Compress
