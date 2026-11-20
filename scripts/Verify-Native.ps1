#requires -Version 7.4
[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$SkipBuild,
    [switch]$SizeOptimization,
    [string]$GameDirectory = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'artifacts/native/release' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
$allowed = Join-Path $projectRoot 'artifacts/native'
if (!$output.StartsWith($allowed + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Native checks require an output below artifacts/native.' }
if (!$SkipBuild) { & (Join-Path $PSScriptRoot 'Build-Native.ps1') -OutputDirectory $output -SizeOptimization:$SizeOptimization | Out-Host }
$build = Get-Content -LiteralPath (Join-Path $output 'native-build.json') -Raw | ConvertFrom-Json
$nativeVersion = Get-Content -LiteralPath (Join-Path $projectRoot 'native/version.json') -Raw | ConvertFrom-Json
if ($build.version -cne $nativeVersion.productVersion -or $build.fileVersion -cne $nativeVersion.fileVersion) { throw 'Native build version does not match version.json. Rebuild the current native sources.' }
$app = Join-Path $output 'VampireSurvivorsTrainer.Native.exe'
$entry = @($build.outputs | Where-Object { $_.file -eq 'VampireSurvivorsTrainer.Native.exe' })
if (!(Test-Path -LiteralPath $app -PathType Leaf)) { throw 'Native client was not built.' }
if ($entry.Count -ne 1 -or (Get-Item -LiteralPath $app).Length -ne $entry[0].length -or (Get-FileHash -LiteralPath $app -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry[0].sha256) { throw 'Native client does not match the build inventory.' }
$reportDirectory = Join-Path $output 'verification'
[IO.Directory]::CreateDirectory($reportDirectory) | Out-Null
$standalone = Join-Path $reportDirectory 'standalone'
[IO.Directory]::CreateDirectory($standalone) | Out-Null
$standaloneApp = Join-Path $standalone 'VampireSurvivorsTrainer.Native.exe'
Copy-Item -LiteralPath $app -Destination $standaloneApp -Force
if (@(Get-ChildItem -LiteralPath $standalone -File -Force).Count -ne 1) { throw 'Standalone verification requires a directory containing only the native EXE.' }
$results = [Collections.Generic.List[object]]::new()
$imageMetrics = [Collections.Generic.List[object]]::new()
function Read-PreviewContent([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 54 -or $bytes.Length -gt 134217728 -or $bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4d) { throw 'Invalid native BMP preview header.' }
    $offset = [BitConverter]::ToUInt32($bytes,10)
    $dib = [BitConverter]::ToUInt32($bytes,14)
    $width = [BitConverter]::ToInt32($bytes,18)
    $rawHeight = [BitConverter]::ToInt32($bytes,22)
    $planes = [BitConverter]::ToUInt16($bytes,26)
    $bits = [BitConverter]::ToUInt16($bytes,28)
    $compression = [BitConverter]::ToUInt32($bytes,30)
    if ($dib -lt 40 -or $offset -lt 14+$dib -or $width -lt 1 -or $width -gt 8192 -or $rawHeight -eq [int]::MinValue -or $planes -ne 1 -or $bits -notin @(24,32) -or $compression -ne 0) { throw 'Unsupported native BMP preview layout.' }
    $height = [Math]::Abs($rawHeight)
    if ($height -lt 1 -or $height -gt 8192) { throw 'Invalid native BMP preview dimensions.' }
    $stride = [long]([Math]::Floor(($width * $bits + 31) / 32)) * 4
    if ([long]$offset + $stride * $height -gt $bytes.Length) { throw 'Truncated native BMP preview pixels.' }
    $colors = [Collections.Generic.Dictionary[int,int]]::new()
    $stepX = [Math]::Max(1,[int][Math]::Floor($width / 128))
    $stepY = [Math]::Max(1,[int][Math]::Floor($height / 96))
    $sampled = 0; $nonzero = 0; $dominant = 0
    for ($y=0; $y -lt $height; $y+=$stepY) {
        for ($x=0; $x -lt $width; $x+=$stepX) {
            $position = [int]($offset + $stride * $y + $x * ($bits / 8))
            $color = [int]$bytes[$position] -bor ([int]$bytes[$position+1] -shl 8) -bor ([int]$bytes[$position+2] -shl 16)
            $count = 0
            if ($colors.TryGetValue($color,[ref]$count)) { $count++ } else { $count = 1 }
            $colors[$color] = $count
            if ($count -gt $dominant) { $dominant = $count }
            if ($color -ne 0) { $nonzero++ }
            $sampled++
        }
    }
    $nonzeroRatio = $nonzero / [double]$sampled
    $dominantRatio = $dominant / [double]$sampled
    return [pscustomobject]@{width=$width;height=$height;bits=$bits;samples=$sampled;uniqueColors=$colors.Count;nonzeroRatio=$nonzeroRatio;dominantColorRatio=$dominantRatio;passed=($nonzeroRatio -gt 0.01 -and $dominantRatio -lt 0.99 -and $colors.Count -ge 3)}
}
function Invoke-Diagnostic([string]$Name, [string[]]$Arguments, [int]$Expected = 0, [string]$Executable = $standaloneApp) {
    $stdout = Join-Path $reportDirectory ($Name + '.stdout.txt')
    $stderr = Join-Path $reportDirectory ($Name + '.stderr.txt')
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $out = $process.StandardOutput.ReadToEndAsync()
        $err = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(30000)) { $process.Kill(); throw ('Native diagnostic timed out: ' + $Name) }
        $text = $out.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($stdout,$text,[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($stderr,$err.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
        $passed = $process.ExitCode -eq $Expected
        $results.Add([pscustomobject]@{name=$Name;exitCode=$process.ExitCode;expectedExitCode=$Expected;passed=$passed})
        if (!$passed) { throw ('Native diagnostic failed: ' + $Name + '. See its bounded output in artifacts/native.') }
        return $text
    }
    finally { $process.Dispose() }
}
try {
    $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($app)
    $versionMatches = $versionInfo.ProductVersion -ceq $nativeVersion.productVersion -and $versionInfo.FileVersion -ceq $nativeVersion.fileVersion
    $results.Add([pscustomobject]@{name='native-version';exitCode=0;expectedExitCode=0;passed=$versionMatches})
    if (!$versionMatches) { throw 'Native PE product/file versions differ from the build version.' }
    $bundle = Invoke-Diagnostic 'embedded-backend' @('--check-bundle') | ConvertFrom-Json
    if (!$bundle.ok -or $bundle.backendBytes -le 0 -or $bundle.backendSha256 -notmatch '^[a-f0-9]{64}$' -or
        $bundle.payloadBytes -le 0 -or $bundle.payloadBytes -ge $bundle.backendBytes -or $bundle.payloadSha256 -notmatch '^[a-f0-9]{64}$') { throw 'The embedded backend diagnostic is invalid.' }
    $backend = Join-Path $output 'VampireSurvivorsTrainer.NativeBackend.dll'
    if ((Get-Item -LiteralPath $backend).Length -ne $bundle.backendBytes -or (Get-FileHash -LiteralPath $backend -Algorithm SHA256).Hash.ToLowerInvariant() -ne $bundle.backendSha256) { throw 'Embedded backend differs from the built DLL.' }
    Invoke-Diagnostic 'locked-backend-file' @('--check-backend-file', $backend) | Out-Null
    $changedBackend = Join-Path $reportDirectory 'changed-backend.dll'
    $changedDllBytes = [IO.File]::ReadAllBytes($backend)
    $changedDllBytes[-1] = $changedDllBytes[-1] -bxor 1
    [IO.File]::WriteAllBytes($changedBackend,$changedDllBytes)
    Invoke-Diagnostic 'locked-backend-modified-file' @('--check-backend-file', $changedBackend) 2 | Out-Null
    if (!('NativePayloadSearch' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativePayloadSearch {
    public static int Find(byte[] data, byte[] payload) {
        for (int i = 0; i <= data.Length - payload.Length; i++) {
            if (data[i] == payload[0] && data.AsSpan(i, payload.Length).SequenceEqual(payload)) return i;
        }
        return -1;
    }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate uint Start(IntPtr unused);
    public static uint RejectWrongHost(string path) {
        IntPtr module = NativeLibrary.Load(path);
        bool safeToUnload = false;
        try {
            var start = Marshal.GetDelegateForFunctionPointer<Start>(NativeLibrary.GetExport(module, "StartNativeTrainer"));
            uint code = start(IntPtr.Zero);
            safeToUnload = code == 10; // ERROR_BAD_ENVIRONMENT, before any worker/hook.
            return code;
        } finally {
            if (safeToUnload) NativeLibrary.Free(module);
        }
    }
}
'@
    }
    $payloadPath = Join-Path $output 'generated/backend-payload.bin'
    $payloadBytes = [IO.File]::ReadAllBytes($payloadPath)
    if ($build.embeddedBackend.encoding -cne 'LZMS' -or $build.embeddedBackend.length -ne $bundle.payloadBytes -or
        $build.embeddedBackend.sha256 -cne $bundle.payloadSha256 -or $build.embeddedBackend.uncompressedLength -ne $bundle.backendBytes -or
        $payloadBytes.Length -ne $bundle.payloadBytes -or (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $bundle.payloadSha256) {
        throw 'Compressed payload differs from the build inventory.'
    }
    $changedBytes = [IO.File]::ReadAllBytes($app)
    $offset = [NativePayloadSearch]::Find($changedBytes,$payloadBytes)
    if ($offset -lt 0 -or [NativePayloadSearch]::Find($changedBytes,[IO.File]::ReadAllBytes($backend)) -ge 0) { throw 'The client must contain only the compressed copy of the backend.' }
    $changedBytes[$offset + $payloadBytes.Length - 1] = $changedBytes[$offset + $payloadBytes.Length - 1] -bxor 1
    $changedApp = Join-Path $reportDirectory 'changed-embedded-payload.exe'
    [IO.File]::WriteAllBytes($changedApp,$changedBytes)
    Invoke-Diagnostic 'changed-embedded-payload' @('--check-bundle') 2 $changedApp | Out-Null
    $hostCode = [NativePayloadSearch]::RejectWrongHost($backend)
    $results.Add([pscustomobject]@{name='wrong-host-bootstrap';exitCode=$hostCode;expectedExitCode=10;passed=($hostCode -eq 10)})
    if ($hostCode -ne 10) { throw 'The native backend did not refuse the isolated non-game verifier host.' }
    $self = Invoke-Diagnostic 'client-boundaries' @('--self-test') | ConvertFrom-Json
    if (!$self.ok -or $self.checks -ne 13 -or $self.gameConnected -or $self.userDataWritten) { throw 'Native client self-check failed or omitted a required policy group.' }
    $smoke = Invoke-Diagnostic 'ui-smoke' @('--ui-smoke') | ConvertFrom-Json
    if (!$smoke.ok -or $smoke.gameConnected -or $smoke.userDataWritten) { throw 'Native UI smoke check failed.' }
    $previews = @(
        @{name='zh-CN';language='zh-CN';scenario=''},
        @{name='en';language='en';scenario=''},
        @{name='detected-zh-CN';language='zh-CN';scenario='detected'},
        @{name='detected-en';language='en';scenario='detected'},
        @{name='old-loader-zh-CN';language='zh-CN';scenario='old-loader'},
        @{name='old-native-zh-CN';language='zh-CN';scenario='old-native'},
        @{name='connected-zh-CN';language='zh-CN';scenario='connected'},
        @{name='stats-zh-CN';language='zh-CN';scenario='stats'},
        @{name='stats-en';language='en';scenario='stats'},
        @{name='stats-bottom-zh-CN';language='zh-CN';scenario='stats-bottom'},
        @{name='stats-bottom-en';language='en';scenario='stats-bottom'},
        @{name='hotkeys-zh-CN';language='zh-CN';scenario='hotkeys'},
        @{name='hotkeys-en';language='en';scenario='hotkeys'},
        @{name='hotkeys-bottom-zh-CN';language='zh-CN';scenario='hotkeys-bottom'},
        @{name='hotkeys-bottom-en';language='en';scenario='hotkeys-bottom'},
        @{name='gold-zh-CN';language='zh-CN';scenario='gold'},
        @{name='gold-en';language='en';scenario='gold'},
        @{name='gold-needs-backup-zh-CN';language='zh-CN';scenario='gold-needs-backup'},
        @{name='gold-adventure-zh-CN';language='zh-CN';scenario='gold-adventure'},
        @{name='gold-adventure-en';language='en';scenario='gold-adventure'},
        @{name='maintenance-zh-CN';language='zh-CN';scenario='maintenance'},
        @{name='maintenance-en';language='en';scenario='maintenance'},
        @{name='backup-zh-CN';language='zh-CN';scenario='backup'},
        @{name='backup-en';language='en';scenario='backup'},
        @{name='rewards-zh-CN';language='zh-CN';scenario='rewards'},
        @{name='rewards-en';language='en';scenario='rewards'},
        @{name='rewards-offer-zh-CN';language='zh-CN';scenario='rewards-offer'},
        @{name='rewards-offer-en';language='en';scenario='rewards-offer'},
        @{name='rewards-search-zh-CN';language='zh-CN';scenario='rewards-search'},
        @{name='rewards-search-en';language='en';scenario='rewards-search'},
        @{name='rewards-chest-only-zh-CN';language='zh-CN';scenario='rewards-chest-only'},
        @{name='rewards-chest-only-en';language='en';scenario='rewards-chest-only'},
        @{name='rewards-error-zh-CN';language='zh-CN';scenario='rewards-error'},
        @{name='rewards-error-en';language='en';scenario='rewards-error'}
    )
    foreach ($preview in $previews) {
        $image = Join-Path $reportDirectory ('preview-' + $preview.name + '.bmp')
        $arguments = @('--render-preview',$image,$preview.language)
        if ($preview.scenario) { $arguments += $preview.scenario }
        Invoke-Diagnostic ('ui-preview-' + $preview.name) $arguments | Out-Null
        if (!(Test-Path -LiteralPath $image -PathType Leaf) -or (Get-Item -LiteralPath $image).Length -lt 10000) { throw 'Missing native UI preview.' }
        $content = $null
        try {
            $content = Read-PreviewContent $image
            $imageMetrics.Add([pscustomobject]@{language=$preview.language;scenario=$preview.scenario;metrics=$content})
        }
        finally { $results.Add([pscustomobject]@{name=('ui-content-' + $preview.name);exitCode=0;expectedExitCode=0;passed=($null -ne $content -and $content.passed)}) }
        if (!$content.passed) { throw 'Native UI preview is black or virtually flat.' }
    }
    $licenses = Invoke-Diagnostic 'embedded-licenses' @('--licenses')
    foreach ($marker in @('Beason9527','MinHook','yyjson','LLVM','Mingw')) {
        if (!$licenses.Contains($marker,[StringComparison]::OrdinalIgnoreCase)) { throw ('Missing native license text: ' + $marker) }
    }
    $licenseHeader = [IO.File]::ReadAllText((Join-Path $output 'generated/embedded_licenses.hpp'))
    $licenseStart = 'R"vstlic('
    $licenseEnd = ')vstlic";'
    $begin = $licenseHeader.IndexOf($licenseStart,[StringComparison]::Ordinal) + $licenseStart.Length
    $end = $licenseHeader.LastIndexOf($licenseEnd,[StringComparison]::Ordinal)
    if ($begin -lt $licenseStart.Length -or $end -le $begin) { throw 'Missing generated native license text.' }
    $expectedLicenses = $licenseHeader.Substring($begin,$end-$begin) + "`n"
    $licensesMatch = ($expectedLicenses -replace '\r\n?',"`n") -ceq ($licenses -replace '\r\n?',"`n")
    $results.Add([pscustomobject]@{name='license-source-text';exitCode=0;expectedExitCode=0;passed=$licensesMatch})
    if (!$licensesMatch) { throw 'Native license output differs from the complete pinned license texts.' }
    Invoke-Diagnostic 'unknown-cli' @('--arbitrary-dll','forbidden.dll') 1 | Out-Null
    if ($GameDirectory) {
        Invoke-Diagnostic 'read-only-game-fingerprint' @('--inspect',$GameDirectory) | Out-Null
    }
    $markers = @($projectRoot,[IO.Path]::GetDirectoryName($projectRoot),[Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile))
    $variants = @($markers | Where-Object { $_ -and $_.Length -gt 3 } | ForEach-Object { $_; $_.Replace('\','/'); $_.Replace('\','\\') } | Select-Object -Unique)
    foreach ($binary in @($app,$backend)) {
        $bytes = [IO.File]::ReadAllBytes($binary)
        foreach ($text in @([Text.Encoding]::UTF8.GetString($bytes),[Text.Encoding]::Unicode.GetString($bytes),[Text.Encoding]::Unicode.GetString($bytes,1,$bytes.Length-1))) {
            foreach ($marker in $variants) { if ($text.Contains($marker,[StringComparison]::OrdinalIgnoreCase)) { throw 'A private build path remains in a native output.' } }
        }
    }
    $results.Add([pscustomobject]@{name='private-build-paths';exitCode=0;expectedExitCode=0;passed=$true})
}
finally {
    [ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o');version=$build.version;fileVersion=$build.fileVersion;sourceCommit=$build.sourceCommit;checks=$results.ToArray();imageMetrics=$imageMetrics.ToArray();passed=@($results | Where-Object passed).Count;failed=@($results | Where-Object { !$_.passed }).Count;standaloneFiles=1;gameStarted=$false;gameLoaded=$false;gameEffectsVerified=$false;clientBytes=(Get-Item -LiteralPath $app).Length} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $reportDirectory 'summary.json') -Encoding utf8
}
# Expected rejection fixtures must not leave an inherited native exit status
# as the result of this successful PowerShell verification script.
$global:LASTEXITCODE = 0
[pscustomobject]@{Passed=$true;Checks=$results.Count;ClientBytes=(Get-Item -LiteralPath $app).Length;Report=(Join-Path $reportDirectory 'summary.json')}
