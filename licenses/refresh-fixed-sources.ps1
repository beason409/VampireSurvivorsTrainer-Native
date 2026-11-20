$ErrorActionPreference = 'Stop'
$licenseRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$sources=@(
 @{id='BepInEx';version='6.0.0-be.793';repo='BepInEx/BepInEx';commit='dbc82f05b8ea34833de5b6a723eb60dd8398e0b1';paths=@('LICENSE')},
 @{id='Cpp2IL';version='2022.1.0-development.1452';repo='SamboyCoding/Cpp2IL';commit='558ddd98642010897d54316b51fbaa7889fda093';paths=@('LICENSE')},
 @{id='UnityDoorstop';version='4.6.0';repo='NeighTools/UnityDoorstop';commit='1c3ece1f1e6c20f4aa74d5402baabf8a0098d282';paths=@('LICENSE')},
 @{id='Dobby';version='1.0.5';repo='BepInEx/Dobby';commit='888d971214900374edbca6206fad6ded8a2c1311';paths=@('LICENSE')},
 @{id='BepInEx-mini-coreclr';version='6.0.7';repo='BepInEx/dotnet-runtime';commit='e10df436edd020835a630a40796ba48222acb1d5';paths=@('LICENSE.TXT','THIRD-PARTY-NOTICES.TXT')}
)
$records=@()
foreach($source in $sources) {
 $folder=Join-Path $licenseRoot ($source.id+'/'+$source.version)
 [IO.Directory]::CreateDirectory($folder)|Out-Null
 $record=[ordered]@{id=$source.id;version=$source.version;repository='https://github.com/'+$source.repo;sourceRef=$source.commit;sourceArchive='https://github.com/'+$source.repo+'/archive/'+$source.commit+'.zip';licenseFiles=@();status='license-text-collected';errors=@()}
 foreach($path in $source.paths) {
  $url='https://raw.githubusercontent.com/'+$source.repo+'/'+$source.commit+'/'+$path
  $target=Join-Path $folder $path
  try {
   Invoke-WebRequest -Uri $url -OutFile $target
   $record.licenseFiles+=@{path=[IO.Path]::GetRelativePath($licenseRoot,$target).Replace('\','/');uri=$url;sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash}
  } catch { $record.status='unresolved';$record.errors+=$url+': '+$_.Exception.Message }
 }
 $records+=$record
 Write-Host ($source.id+' '+$source.version+': '+$record.status)
}
$records|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $licenseRoot 'fixed-source-components.json') -Encoding utf8
