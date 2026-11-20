param(
  [switch]$SkipRestore,
  [string]$InteropDirectory = $env:VS_TRAINER_INTEROP
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
  if (-not (Test-Path -LiteralPath (Join-Path $InteropDirectory $interface) -PathType Leaf)) { throw "Missing generated game interface $interface. Set -InteropDirectory to the current game's BepInEx/interop directory." }
}
& (Join-Path $PSScriptRoot 'Prepare-Dependencies.ps1') | Out-Host
if (-not $SkipRestore) {
  & dotnet restore 'VampireSurvivorsTrainer.sln' -p:BuildInParallel=false
  if ($LASTEXITCODE -ne 0) { throw 'Dependency restore failed.' }
}
& dotnet build 'src/Trainer.Plugin/Trainer.Plugin.csproj' -c Release --no-restore -m:1 -p:BuildInParallel=false /nodeReuse:false "-p:InteropDirectory=$InteropDirectory"
if ($LASTEXITCODE -ne 0) { throw 'Game plugin build failed.' }
$buildId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)
$buildRoot = Join-Path $projectRoot ('artifacts/builds/' + $buildId)
$app = Join-Path $buildRoot 'app'
$payload = Join-Path $app 'payload'
New-Item -ItemType Directory -Path $payload | Out-Null
Expand-Archive -LiteralPath (Join-Path $projectRoot 'dependencies/bepinex-793.zip') -DestinationPath $payload
$pluginDirectory = Join-Path $payload 'BepInEx/plugins/SurvivorsTrainer'
New-Item -ItemType Directory -Path $pluginDirectory -Force | Out-Null
foreach ($name in @('Trainer.Plugin.dll','Trainer.Shared.dll')) {
  Copy-Item -LiteralPath (Join-Path $projectRoot ('src/Trainer.Plugin/bin/Release/net6.0/' + $name)) -Destination $pluginDirectory
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'config/supported-game.json') -Destination (Join-Path $app 'supported-game.json')
Copy-Item -LiteralPath (Join-Path $app 'supported-game.json') -Destination $pluginDirectory
$catalog = & (Join-Path $PSScriptRoot 'Generate-TrustedPayloadCatalog.ps1') -PayloadDirectory $payload -SupportedGamePath (Join-Path $app 'supported-game.json') -OutputPath (Join-Path $buildRoot 'trusted-payload-catalog.json')
# Core is outside the payload: rebuilding it with the finished payload catalog
# cannot change any file whose digest is in that catalog.
& dotnet build 'src/Trainer.Core/Trainer.Core.csproj' -c Release -t:Rebuild --no-restore -m:1 -p:BuildInParallel=false /nodeReuse:false "-p:TrustedPayloadCatalogPath=$($catalog.CatalogPath)"
if ($LASTEXITCODE -ne 0) { throw 'Protected Core build failed.' }
& dotnet build 'VampireSurvivorsTrainer.sln' -c Release --no-restore -m:1 -p:BuildInParallel=false /nodeReuse:false "-p:InteropDirectory=$InteropDirectory" "-p:TrustedPayloadCatalogPath=$($catalog.CatalogPath)"
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
foreach ($name in @('Trainer.Plugin.dll','Trainer.Shared.dll')) {
  if ((Get-FileHash -LiteralPath (Join-Path $pluginDirectory $name) -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath (Join-Path $projectRoot ('src/Trainer.Plugin/bin/Release/net6.0/' + $name)) -Algorithm SHA256).Hash) {
    throw 'The game plugin changed after catalog generation. Run the build again from stable source.'
  }
}
$desktopOutput = Join-Path $projectRoot 'src/Trainer.Desktop/bin/Release/net10.0-windows'
foreach ($name in @('VampireSurvivorsTrainer.exe','VampireSurvivorsTrainer.dll','VampireSurvivorsTrainer.deps.json','VampireSurvivorsTrainer.runtimeconfig.json','Trainer.Core.dll','Trainer.Shared.dll')) {
  Copy-Item -LiteralPath (Join-Path $desktopOutput $name) -Destination $app
}
$integrity = Start-Process -FilePath (Join-Path $app 'VampireSurvivorsTrainer.exe') -ArgumentList '--verify-companion-files' -WindowStyle Hidden -PassThru
try {
  if (!$integrity.WaitForExit(30000)) { $integrity.Kill(); throw 'Source-build companion-file verification timed out.' }
  $integrity.Refresh()
  if ($null -eq $integrity.ExitCode -or $integrity.ExitCode -ne 0) { throw ('Source-build companion-file verification failed (exit ' + $integrity.ExitCode + ').') }
}
finally { $integrity.Dispose() }
[pscustomobject]@{ DesktopExecutable=(Join-Path $app 'VampireSurvivorsTrainer.exe'); PayloadDirectory=$payload; TrustedCatalog=$catalog.CatalogPath; TrustedFiles=$catalog.Files; FrameworkDependent=$true }
