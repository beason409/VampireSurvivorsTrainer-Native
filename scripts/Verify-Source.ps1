[CmdletBinding()]
param(
    [switch]$IncludeDesktop,
    [switch]$IncludeProbeIntegration,
    [switch]$SkipRestore
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $projectRoot
if (-not $IsWindows) { throw 'This source check includes Windows filesystem fixtures. Use the Docker portable checks on a non-Windows host.' }
$env:DOTNET_CLI_HOME = Join-Path $projectRoot '.dotnet'
$env:NUGET_PACKAGES = Join-Path $projectRoot 'dependencies/nuget'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$reportDirectory = Join-Path $projectRoot 'artifacts/source-check'
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$checks = [Collections.Generic.List[object]]::new()

function Invoke-DotnetCheck([string]$Name, [string[]]$DotnetArguments) {
    $log = Join-Path $reportDirectory ($Name + '.txt')
    & dotnet @DotnetArguments 2>&1 | Tee-Object -FilePath $log | Out-Host
    $code = $LASTEXITCODE
    $checks.Add([pscustomobject]@{ name=$Name; exitCode=$code; log=($Name + '.txt') })
    if ($code -ne 0) { throw "Source check failed: $Name (exit $code). See $log" }
}

try {
    # Build explicit source-only projects. Do not compile a GAME_INTEROP-free
    # plugin and mistake it for a working game plugin or a redistributable.
    $projects = @(
        'tests/Trainer.Core.Tests/Trainer.Core.Tests.csproj',
        'tests/Trainer.Runtime.Tests/Trainer.Runtime.Tests.csproj',
        'tests/Trainer.Probe.Tests/Trainer.Probe.Tests.csproj'
    )
    if ($IncludeProbeIntegration) { $projects += 'tools/Trainer.Probe/Trainer.Probe.csproj' }
    if ($IncludeDesktop) {
        $projects += 'src/Trainer.Desktop/Trainer.Desktop.csproj'
        $projects += 'tests/Trainer.Desktop.Tests/Trainer.Desktop.Tests.csproj'
        $projects += 'tests/Trainer.Desktop.Layout.Tests/Trainer.Desktop.Layout.Tests.csproj'
        $projects += 'tests/Trainer.Security.Tests/Trainer.Security.Tests.csproj'
        $projects += 'tests/Trainer.Desktop.Maintenance.Tests/Trainer.Desktop.Maintenance.Tests.csproj'
        $projects += 'tests/Trainer.Desktop.About.Tests/Trainer.Desktop.About.Tests.csproj'
        $projects += 'tests/Trainer.Pipe.Tests/Trainer.Pipe.Tests.csproj'
    }
    foreach ($project in $projects) {
        $name = [IO.Path]::GetFileNameWithoutExtension($project)
        $arguments = @('build', $project, '-c', 'Release', '-m:1', '-p:BuildInParallel=false', '/nodeReuse:false')
        if ($SkipRestore) { $arguments += '--no-restore' }
        Invoke-DotnetCheck ('build-' + $name) $arguments
    }
    Invoke-DotnetCheck 'core-fixtures' @('tests/Trainer.Core.Tests/bin/Release/net10.0/Trainer.Core.Tests.dll')
    Invoke-DotnetCheck 'runtime-fixtures' @('tests/Trainer.Runtime.Tests/bin/Release/net10.0/Trainer.Runtime.Tests.dll')
    $probeArguments = @('tests/Trainer.Probe.Tests/bin/Release/net10.0/Trainer.Probe.Tests.dll')
    if (-not $IncludeProbeIntegration) { $probeArguments += '--offline-only' }
    Invoke-DotnetCheck 'probe-fixtures' $probeArguments

    if ($IncludeDesktop) {
        Invoke-DotnetCheck 'desktop-fixtures' @('tests/Trainer.Desktop.Tests/bin/Release/net10.0-windows/Trainer.Desktop.Tests.dll')
        Invoke-DotnetCheck 'desktop-layout-fixtures' @('tests/Trainer.Desktop.Layout.Tests/bin/Release/net10.0-windows/Trainer.Desktop.Layout.Tests.dll')
        Invoke-DotnetCheck 'security-fixtures' @('tests/Trainer.Security.Tests/bin/Release/net10.0/Trainer.Security.Tests.dll')
        Invoke-DotnetCheck 'desktop-maintenance-fixtures' @('tests/Trainer.Desktop.Maintenance.Tests/bin/Release/net10.0-windows/Trainer.Desktop.Maintenance.Tests.dll')
        Invoke-DotnetCheck 'desktop-about-fixtures' @('tests/Trainer.Desktop.About.Tests/bin/Release/net10.0-windows/Trainer.Desktop.About.Tests.dll')
        Invoke-DotnetCheck 'pipe-fixtures' @('tests/Trainer.Pipe.Tests/bin/Release/net10.0-windows/Trainer.Pipe.Tests.dll')
        $executable = Join-Path $projectRoot 'src/Trainer.Desktop/bin/Release/net10.0-windows/VampireSurvivorsTrainer.exe'
        # This is the product's explicit diagnostic entry point: no timers,
        # game connection, install, settings writes or real save operations.
        $process = Start-Process -FilePath $executable -ArgumentList '--smoke-test' -WindowStyle Hidden -PassThru
        try {
            if (-not $process.WaitForExit(20000)) { $process.Kill(); throw 'Desktop smoke test timed out.' }
            $process.Refresh()
            if ($null -eq $process.ExitCode) { throw 'Desktop exit code could not be read; smoke result is unknown.' }
            $checks.Add([pscustomobject]@{ name='desktop-smoke'; exitCode=$process.ExitCode; log=$null })
            if ($process.ExitCode -ne 0) { throw "Desktop smoke test failed: $($process.ExitCode)" }
        }
        finally { $process.Dispose() }
    }
}
finally {
    [ordered]@{
        createdUtc=[DateTime]::UtcNow.ToString('O')
        platform=[Runtime.InteropServices.RuntimeInformation]::OSDescription
        commit=$env:GITHUB_SHA
        checks=@($checks.ToArray())
        probeIntegration=[bool]$IncludeProbeIntegration
        desktopSmoke=[bool]$IncludeDesktop
        gamePluginBuilt=$false
        gameEffectsVerified=$false
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $reportDirectory 'summary.json') -Encoding utf8
}
Write-Output 'Source checks passed. This does not validate live game effects or create a release package.'
