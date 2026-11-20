#requires -Version 7.4
[CmdletBinding()]
param(
    [string]$ZigPath = $env:VS_TRAINER_ZIG,
    [switch]$CheckToolchain,
    [switch]$NoDownload,
    [switch]$SkipTests,
    [switch]$SizeOptimization,
    [ValidateSet('All','Tests','Backend','Client')][string]$Target = 'All',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
if (!$IsWindows -or ![Environment]::Is64BitProcess) { throw 'Build-Native requires 64-bit PowerShell on Windows.' }
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$nativeRoot = Join-Path $projectRoot 'native'
$toolsRoot = Join-Path $projectRoot 'dependencies/native-tools'
$artifactsRoot = Join-Path $projectRoot 'artifacts/native'
$lock = Get-Content -LiteralPath (Join-Path $nativeRoot 'toolchain.json') -Raw | ConvertFrom-Json
$nativeVersion = Get-Content -LiteralPath (Join-Path $nativeRoot 'version.json') -Raw | ConvertFrom-Json
if ($nativeVersion.formatVersion -ne 1 -or $nativeVersion.productVersion -notmatch '^(?:native-pilot\.[1-9][0-9]*|[1-9][0-9]*\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))$' -or $nativeVersion.fileVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' -or @($nativeVersion.fileVersion.Split('.') | Where-Object { [long]$_ -gt 65535 }).Count -ne 0) { throw 'Invalid native version metadata.' }
if ($nativeVersion.productVersion -notlike 'native-pilot.*' -and $nativeVersion.fileVersion -cne ($nativeVersion.productVersion + '.0')) { throw 'Stable product and file versions must match.' }
$backendVersion = $nativeVersion.backendVersion ?? $nativeVersion.productVersion
if ($backendVersion -isnot [string] -or $backendVersion -notmatch '^native-pilot\.[1-9][0-9]*$') { throw 'Invalid native backend version.' }
if ($lock.formatVersion -ne 1 -or $lock.version -ne '0.15.2' -or $lock.target -ne 'x86_64-windows-gnu' -or
    $lock.archiveSha256 -notmatch '^[a-f0-9]{64}$' -or $lock.archiveUrl -cne 'https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip') { throw 'Unsupported native toolchain lock.' }

function Assert-Under([string]$Path, [string]$Root) {
    $full = [IO.Path]::GetFullPath($Path)
    $base = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($Root))
    if (!$full.StartsWith($base + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Native output path escapes its allowed directory.' }
    $current = $full
    while ($current.Length -gt $base.Length) {
        if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Native build refuses linked output or toolchain paths.' }
        $current = [IO.Path]::GetDirectoryName($current)
    }
    if ((Test-Path -LiteralPath $base) -and ((Get-Item -LiteralPath $base -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Native build refuses a linked output root.' }
    return $full
}
function Test-Archive([string]$Path) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and (Get-Item -LiteralPath $Path).Length -eq [long]$lock.archiveBytes -and (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $lock.archiveSha256
}

[IO.Directory]::CreateDirectory($toolsRoot) | Out-Null
if (!$ZigPath) {
    $ZigPath = Assert-Under (Join-Path $toolsRoot $lock.executableRelativePath) $toolsRoot
    $archive = Assert-Under (Join-Path $toolsRoot ([IO.Path]::GetFileName($lock.archiveUrl))) $toolsRoot
    if (!(Test-Path -LiteralPath $ZigPath -PathType Leaf)) {
        if (!(Test-Archive $archive)) {
            if ($NoDownload) { throw 'The fixed Zig compiler is unavailable. Supply -ZigPath or allow the verified portable download.' }
            $partial = $archive + '.partial-' + [Guid]::NewGuid().ToString('N')
            Write-Host ('Downloading portable Zig ' + $lock.version + ' for native builds.')
            $priorProgress = $ProgressPreference
            try { $ProgressPreference = 'SilentlyContinue'; Invoke-WebRequest -Uri $lock.archiveUrl -OutFile $partial }
            finally { $ProgressPreference = $priorProgress }
            if (!(Test-Archive $partial)) { throw 'Downloaded Zig archive failed the fixed size/SHA-256 check.' }
            Move-Item -LiteralPath $partial -Destination $archive -Force
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        try {
            foreach ($entry in $zip.Entries) { Assert-Under (Join-Path $toolsRoot $entry.FullName) $toolsRoot | Out-Null }
        }
        finally { $zip.Dispose() }
        Expand-Archive -LiteralPath $archive -DestinationPath $toolsRoot -Force
    }
    elseif ((Test-Path -LiteralPath $archive) -and !(Test-Archive $archive)) { throw 'Cached Zig archive differs from the fixed official checksum.' }
}
$ZigPath = [IO.Path]::GetFullPath($ZigPath)
if (!(Test-Path -LiteralPath $ZigPath -PathType Leaf)) { throw 'ZigPath must point to the fixed zig.exe.' }
$version = (& $ZigPath version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $version -cne $lock.version) { throw ('Expected Zig ' + $lock.version + '.') }
if ($CheckToolchain) {
    [pscustomobject]@{Compiler='Zig';Version=$version;Target=$lock.target;CompilerPath=$ZigPath;ArchiveSha256=$lock.archiveSha256;RequiresVisualStudio=$false}
    return
}

if (!$OutputDirectory) { $OutputDirectory = Join-Path $artifactsRoot 'release' }
$output = Assert-Under $OutputDirectory $artifactsRoot
$generated = Join-Path $output 'generated'
$objects = Join-Path $output 'obj'
$logs = Join-Path $output 'logs'
foreach ($directory in @($output,$generated,$objects,$logs)) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
$utf8 = [Text.UTF8Encoding]::new($false)
$supported = Get-Content -LiteralPath (Join-Path $projectRoot 'config/supported-game.json') -Raw | ConvertFrom-Json
if ($supported.gameAssemblySha256 -notmatch '^[a-fA-F0-9]{64}$' -or $supported.metadataSha256 -notmatch '^[a-fA-F0-9]{64}$' -or $supported.gameExecutableSha256 -notmatch '^[a-fA-F0-9]{64}$' -or $supported.buildId -notmatch '^[0-9]+$') { throw 'Invalid supported-game identity.' }
$identityHeader = @"
#pragma once
namespace vst::supported {
inline constexpr char game_sha256[] = "$($supported.gameAssemblySha256.ToLowerInvariant())";
inline constexpr char metadata_sha256[] = "$($supported.metadataSha256.ToLowerInvariant())";
inline constexpr char game_exe_sha256[] = "$($supported.gameExecutableSha256.ToLowerInvariant())";
inline constexpr char build_id[] = "$($supported.buildId)";
}
"@
[IO.File]::WriteAllText((Join-Path $generated 'supported_game.hpp'),$identityHeader,$utf8)
$versionHeader = @"
#pragma once
namespace vst::version {
inline constexpr char product[] = "$($nativeVersion.productVersion)";
inline constexpr char file[] = "$($nativeVersion.fileVersion)";
}
"@
[IO.File]::WriteAllText((Join-Path $generated 'native_version.hpp'),$versionHeader,$utf8)

$vendors = Get-Content -LiteralPath (Join-Path $nativeRoot 'vendor/manifest.json') -Raw | ConvertFrom-Json
foreach ($vendor in $vendors) {
    foreach ($file in $vendor.files) {
        $path = Assert-Under (Join-Path $projectRoot $file.path) (Join-Path $nativeRoot 'vendor')
        if ($file.sha256 -notmatch '^[a-f0-9]{64}$' -or !(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw ('Pinned vendor source changed: ' + $file.path) }
    }
}
$runtimeLicenses = Get-Content -LiteralPath (Join-Path $nativeRoot 'vendor/runtime-licenses.json') -Raw | ConvertFrom-Json
if ($runtimeLicenses.formatVersion -ne 1 -or $runtimeLicenses.toolchainVersion -cne $lock.version -or $runtimeLicenses.sourceArchiveSha256 -cne $lock.archiveSha256 -or !$runtimeLicenses.files) { throw 'Native runtime license inventory does not match the fixed compiler.' }
$licenseParts = [Collections.Generic.List[object]]::new()
$licenseParts.Add([ordered]@{component='Vampire Survivors Trainer';path='LICENSE'})
foreach ($vendor in $vendors) {
    $license = @($vendor.files | Where-Object { [IO.Path]::GetFileName($_.path) -match '^(?i:LICENSE)(?:\.txt)?$' })
    if ($license.Count -ne 1) { throw 'Expected one pinned license file for each native vendor.' }
    $licenseParts.Add([ordered]@{component=$vendor.name;path=$license[0].path})
}
foreach ($file in $runtimeLicenses.files) {
    $path = Assert-Under (Join-Path $projectRoot $file.path) (Join-Path $nativeRoot 'vendor/licenses')
    if ($file.sha256 -notmatch '^[a-f0-9]{64}$' -or !(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne [long]$file.length -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw ('Pinned runtime license changed: ' + $file.path) }
    $licenseParts.Add([ordered]@{component=$file.component;path=$file.path})
}
$licenseText = [Text.StringBuilder]::new()
foreach ($part in $licenseParts) {
    [void]$licenseText.Append('===== ').Append($part.component).Append(' (').Append($part.path).AppendLine(') =====')
    [void]$licenseText.AppendLine([IO.File]::ReadAllText((Join-Path $projectRoot $part.path))).AppendLine()
}
if ($licenseText.ToString().Contains(')vstlic"',[StringComparison]::Ordinal)) { throw 'A license contains the reserved C++ raw-string delimiter.' }
$licenseHeader = "#pragma once`nnamespace vst::embedded {`ninline constexpr char licenses[] = R`"vstlic(" + $licenseText.ToString() + ")vstlic`";`n}`n"
[IO.File]::WriteAllText((Join-Path $generated 'embedded_licenses.hpp'),$licenseHeader,$utf8)
$includePaths = @((Join-Path $nativeRoot 'common'),(Join-Path $nativeRoot 'backend'),(Join-Path $nativeRoot 'vendor/yyjson'),(Join-Path $nativeRoot 'vendor/minhook/include'),$generated)
$optimization = if ($SizeOptimization) { '-Oz' } else { '-Os' }
$compilerFlags = @('-target',$lock.target,$optimization,'-g0','-DUNICODE','-D_UNICODE','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-D_WIN32_WINNT=0x0a00','-DNDEBUG','-DMH_STATIC')
$compilerFlags += '-DVST_NATIVE_VERSION="' + $backendVersion + '"'
if ($SizeOptimization) { $compilerFlags += @('-flto=thin','-ffunction-sections','-fdata-sections') }
foreach ($path in $includePaths) { $compilerFlags += '-I' + $path }
foreach ($prefix in @($projectRoot,$projectRoot.Replace('\','/'),(Split-Path $ZigPath -Parent),(Split-Path $ZigPath -Parent).Replace('\','/')) | Select-Object -Unique) {
    $compilerFlags += '-ffile-prefix-map=' + $prefix + '=.'
    $compilerFlags += '-fmacro-prefix-map=' + $prefix + '=.'
}
$libraries = @('-ladvapi32','-lbcrypt','-luser32','-lshell32','-lpsapi','-lgdi32','-lole32','-lcomctl32')
$linkFlags = @('-target',$lock.target,$optimization,'-g0','-s','-static','-Wl,--dynamicbase,--nxcompat,--high-entropy-va')
if ($SizeOptimization) { $linkFlags += '-flto=thin' }
$compiled = @{}
$commands = [Collections.Generic.List[object]]::new()
function Invoke-Zig([string[]]$Arguments, [string]$Step) {
    $log = Join-Path $logs ($Step + '.txt')
    & $ZigPath @Arguments 2>&1 | Tee-Object -FilePath $log | Out-Host
    if ($LASTEXITCODE -ne 0) { throw ('Native step failed: ' + $Step + '. See artifacts/native build logs.') }
}

function Write-EmbeddedBackend($BackendInfo) {
    if (!('NativeBackendCompression' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
public static class NativeBackendCompression {
    private const uint Lzms = 5;

    [DllImport("cabinet.dll", SetLastError = true)]
    private static extern bool CreateCompressor(uint algorithm, IntPtr allocator, out IntPtr compressor);

    [DllImport("cabinet.dll", SetLastError = true)]
    private static extern bool Compress(IntPtr compressor, byte[] source, nuint length,
        [Out] byte[] buffer, nuint capacity, out nuint written);

    [DllImport("cabinet.dll")]
    private static extern bool CloseCompressor(IntPtr compressor);

    public static byte[] Encode(byte[] source) {
        if (!CreateCompressor(Lzms, IntPtr.Zero, out var compressor)) throw new Win32Exception();
        try {
            var buffer = new byte[source.Length];
            if (!Compress(compressor, source, (nuint)source.Length, buffer, (nuint)buffer.Length, out var written))
                throw new Win32Exception();
            if (written == 0 || written >= (nuint)source.Length) throw new InvalidOperationException("Expected a smaller backend payload.");
            Array.Resize(ref buffer, checked((int)written));
            return buffer;
        } finally { CloseCompressor(compressor); }
    }
}
'@
    }
    $raw = [IO.File]::ReadAllBytes($backend)
    $packed = [NativeBackendCompression]::Encode($raw)
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($packed)).ToLowerInvariant()
    $header = [Text.StringBuilder]::new($packed.Length * 6)
    [void]$header.AppendLine('#pragma once').AppendLine('#include <cstddef>').AppendLine('namespace vst::embedded {').AppendLine('inline constexpr unsigned char backend_payload[] = {')
    for ($offset=0; $offset -lt $packed.Length; $offset+=24) {
        for ($column=$offset; $column -lt [Math]::Min($offset+24,$packed.Length); $column++) { [void]$header.Append('0x').Append($packed[$column].ToString('x2')).Append(',') }
        [void]$header.AppendLine()
    }
    [void]$header.AppendLine('};').Append('inline constexpr std::size_t backend_size = ').Append($raw.Length).AppendLine(';')
    [void]$header.Append('inline constexpr char backend_sha256[] = "').Append($BackendInfo.sha256).AppendLine('";')
    [void]$header.Append('inline constexpr char payload_sha256[] = "').Append($hash).AppendLine('";').AppendLine('}')
    [IO.File]::WriteAllText((Join-Path $generated 'embedded_backend.hpp'),$header.ToString(),$utf8)
    [IO.File]::WriteAllBytes((Join-Path $generated 'backend-payload.bin'),$packed)
    return [ordered]@{encoding='LZMS';length=$packed.Length;sha256=$hash;uncompressedLength=$raw.Length}
}
function Compile-Source([string]$Relative) {
    if ($compiled.ContainsKey($Relative)) { return $compiled[$Relative] }
    $source = Join-Path $projectRoot $Relative
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw ('Native source is missing: ' + $Relative) }
    $name = $Relative.Replace('/','_').Replace('\','_')
    $object = Join-Path $objects ($name + '.obj')
    $driver = if ([IO.Path]::GetExtension($source) -eq '.c') { 'cc' } else { 'c++' }
    $standard = if ($driver -eq 'cc') { '-std=c11' } else { '-std=c++20' }
    Invoke-Zig (@($driver,$standard) + $compilerFlags + @('-c',$source,'-o',$object)) ('compile-' + $name)
    $commands.Add([ordered]@{source=$Relative;sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant();language=$driver})
    $compiled[$Relative] = $object
    return $object
}
function Read-PeInfo([string]$Path) {
    Add-Type -AssemblyName System.Reflection.Metadata
    $stream = [IO.File]::OpenRead($Path)
    $pe = [Reflection.PortableExecutable.PEReader]::new($stream)
    try {
        if ($pe.PEHeaders.CoffHeader.Machine -ne [Reflection.PortableExecutable.Machine]::Amd64 -or $pe.PEHeaders.PEHeader.Magic -ne [Reflection.PortableExecutable.PEMagic]::PE32Plus -or $pe.PEHeaders.CorHeader) { throw 'A native output is not unmanaged Windows x64.' }
        $features = $pe.PEHeaders.PEHeader.DllCharacteristics
        foreach ($required in @([Reflection.PortableExecutable.DllCharacteristics]::DynamicBase,[Reflection.PortableExecutable.DllCharacteristics]::NxCompatible,[Reflection.PortableExecutable.DllCharacteristics]::HighEntropyVirtualAddressSpace)) {
            if (($features -band $required) -eq 0) { throw 'A native output is missing ASLR/DEP mitigation flags.' }
        }
        foreach ($entry in $pe.ReadDebugDirectory()) {
            if ($entry.Type -in @([Reflection.PortableExecutable.DebugDirectoryEntryType]::CodeView,[Reflection.PortableExecutable.DebugDirectoryEntryType]::EmbeddedPortablePdb)) { throw 'A native output contains debug paths or embedded PDB data.' }
        }
        $imports = [Collections.Generic.List[string]]::new()
        $rva = $pe.PEHeaders.PEHeader.ImportTableDirectory.RelativeVirtualAddress
        if ($rva) {
            $reader = $pe.GetSectionData($rva).GetReader()
            for ($index=0; $index -lt 256; $index++) {
                $original=$reader.ReadUInt32();$time=$reader.ReadUInt32();$forward=$reader.ReadUInt32();$nameRva=$reader.ReadInt32();$thunk=$reader.ReadUInt32()
                if (($original -bor $time -bor $forward -bor $nameRva -bor $thunk) -eq 0) { break }
                $nameReader=$pe.GetSectionData($nameRva).GetReader();$bytes=[Collections.Generic.List[byte]]::new()
                for ($character=0; $character -lt 512; $character++) { $value=$nameReader.ReadByte();if (!$value) { break };$bytes.Add($value) }
                $name=[Text.Encoding]::ASCII.GetString($bytes.ToArray())
                if ($name -notmatch '^(?i:(?:api-ms-win-|ext-ms-win-)[a-z0-9-]+\.dll|kernel32\.dll|ntdll\.dll|advapi32\.dll|bcrypt\.dll|user32\.dll|shell32\.dll|psapi\.dll|msvcrt\.dll|ucrtbase\.dll|ole32\.dll|oleaut32\.dll|shlwapi\.dll|ws2_32\.dll|winmm\.dll|version\.dll|rpcrt4\.dll|secur32\.dll|normaliz\.dll|gdi32\.dll|comctl32\.dll|uxtheme\.dll|comdlg32\.dll|imm32\.dll)$') { throw ('Unexpected non-system native import: ' + $name) }
                $imports.Add($name)
            }
        }
        $exports = [Collections.Generic.List[string]]::new()
        $exportRva = $pe.PEHeaders.PEHeader.ExportTableDirectory.RelativeVirtualAddress
        if ($exportRva) {
            $exportReader = $pe.GetSectionData($exportRva).GetReader()
            $exportReader.Offset = 24
            $exportCount = $exportReader.ReadInt32()
            $functionTableRva = $exportReader.ReadInt32()
            $namesRva = $exportReader.ReadInt32()
            if ($exportCount -lt 0 -or $exportCount -gt 4096) { throw 'Invalid native export count.' }
            $nameTable = $pe.GetSectionData($namesRva).GetReader()
            for ($index=0; $index -lt $exportCount; $index++) {
                $nameReader=$pe.GetSectionData($nameTable.ReadInt32()).GetReader();$bytes=[Collections.Generic.List[byte]]::new()
                for ($character=0; $character -lt 512; $character++) { $value=$nameReader.ReadByte();if (!$value) { break };$bytes.Add($value) }
                $exports.Add([Text.Encoding]::ASCII.GetString($bytes.ToArray()))
            }
        }
        return [ordered]@{file=[IO.Path]::GetFileName($Path);length=(Get-Item -LiteralPath $Path).Length;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();machine='Amd64';imports=$imports.ToArray();mitigations=$features.ToString();debugPaths=$false;exports=$exports.ToArray()}
    }
    finally { $pe.Dispose();$stream.Dispose() }
}

$previousGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$previousLocalCache = $env:ZIG_LOCAL_CACHE_DIR
$previousDirectory = $PWD
$outputs = [Collections.Generic.List[object]]::new()
$backend = Join-Path $output 'VampireSurvivorsTrainer.NativeBackend.dll'
$client = Join-Path $output 'VampireSurvivorsTrainer.Native.exe'
$tests = Join-Path $output 'VampireSurvivorsTrainer.Native.Tests.exe'
$testsPassed = $null
$embeddedBackend = $null
try {
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $toolsRoot 'zig-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $output '.zig-cache'
    Set-Location -LiteralPath $projectRoot
    $json = Compile-Source 'native/vendor/yyjson/yyjson.c'
    if ($Target -ne 'Tests') {
        $backendObjects = @($json)
        foreach ($source in @('native/common/win_util.cpp','native/common/backup_proof.cpp','native/common/runtime.cpp','native/common/protocol.cpp','native/backend/game.cpp','native/backend/pipe_server.cpp','native/backend/entry.cpp','native/vendor/minhook/src/buffer.c','native/vendor/minhook/src/hook.c','native/vendor/minhook/src/trampoline.c','native/vendor/minhook/src/hde/hde64.c')) { $backendObjects += Compile-Source $source }
        Invoke-Zig (@('c++') + $linkFlags + @('-shared') + $backendObjects + $libraries + @('-o',$backend)) 'link-backend'
        $backendInfo = Read-PeInfo $backend
        if ($backendInfo.exports.Count -eq 0) { throw 'The native backend must expose its bootstrap API.' }
        $outputs.Add($backendInfo)
        if ($Target -in @('All','Client')) {
            $embeddedBackend = Write-EmbeddedBackend $backendInfo
            $clientObjects = @($json,(Compile-Source 'native/common/win_util.cpp'),(Compile-Source 'native/common/backup_proof.cpp'),(Compile-Source 'native/client/maintenance.cpp'),(Compile-Source 'native/client/main.cpp'))
            $resource = Join-Path $nativeRoot 'client/app.rc'
            if (Test-Path -LiteralPath $resource -PathType Leaf) {
                $resourceText = [IO.File]::ReadAllText($resource)
                $numericVersion = [regex]::Escape($nativeVersion.fileVersion.Replace('.',','))
                if ($resourceText -notmatch ('(?m)^FILEVERSION\s+' + $numericVersion + '\s*$') -or $resourceText -notmatch ('(?m)^PRODUCTVERSION\s+' + $numericVersion + '\s*$') -or !$resourceText.Contains('"' + $nativeVersion.productVersion + '\0"',[StringComparison]::Ordinal)) { throw 'Native resource version differs from version.json.' }
                $appManifest = [xml]([IO.File]::ReadAllText((Join-Path $nativeRoot 'client/app.manifest')))
                if ($appManifest.assembly.assemblyIdentity.version -cne $nativeVersion.fileVersion) { throw 'Native application manifest version differs from version.json.' }
                $compiledResource = Join-Path $objects 'client.res'
                Invoke-Zig @('rc','/c','65001','/:auto-includes','gnu','/i',(Join-Path $nativeRoot 'client'),'/fo',$compiledResource,$resource) 'compile-resource'
                $clientObjects += $compiledResource
            }
            Invoke-Zig (@('c++') + $linkFlags + @('-municode','-mwindows') + $clientObjects + $libraries + @('-o',$client)) 'link-client'
            $outputs.Add((Read-PeInfo $client))
        }
    }
    if ($Target -in @('All','Tests')) {
        $testObjects = @($json,(Compile-Source 'native/common/runtime.cpp'),(Compile-Source 'native/common/protocol.cpp'),(Compile-Source 'native/common/backup_proof.cpp'),(Compile-Source 'native/client/maintenance.cpp'),(Compile-Source 'native/backend/pipe_server.cpp'),(Compile-Source 'native/tests/policy_tests.cpp'))
        if (Test-Path -LiteralPath (Join-Path $nativeRoot 'common/win_util.cpp') -PathType Leaf) { $testObjects += Compile-Source 'native/common/win_util.cpp' }
        Invoke-Zig (@('c++') + $linkFlags + $testObjects + $libraries + @('-o',$tests)) 'link-tests'
        $outputs.Add((Read-PeInfo $tests))
        if (!$SkipTests) {
            & $tests 2>&1 | Tee-Object -FilePath (Join-Path $logs 'policy-tests.txt') | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'Native policy tests failed.' }
            $testsPassed = $true
        }
    }
    $commit = $null; $dirty = $null
    if ((Test-Path -LiteralPath (Join-Path $projectRoot '.git')) -and (Get-Command git -ErrorAction SilentlyContinue)) {
        $commit = (& git -C $projectRoot rev-parse HEAD 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw 'Could not read the native source revision.' }
        $dirty = @(& git -C $projectRoot status --porcelain).Count -ne 0
        if ($LASTEXITCODE -ne 0) { throw 'Could not read the native source status.' }
    }
    [ordered]@{formatVersion=1;createdUtc=[DateTime]::UtcNow.ToString('o');version=$nativeVersion.productVersion;fileVersion=$nativeVersion.fileVersion;sourceCommit=$commit;sourceDirty=$dirty;target=$Target;platform=$lock.target;optimization=@{mode=$optimization;thinLto=[bool]$SizeOptimization;functionSections=[bool]$SizeOptimization;dataSections=[bool]$SizeOptimization};compiler=@{name='Zig';version=$version;archiveSha256=$lock.archiveSha256};sources=$commands.ToArray();outputs=$outputs.ToArray();embeddedBackend=$embeddedBackend;embeddedLicenseFiles=$licenseParts.Count;embeddedLicenseBytes=[Text.Encoding]::UTF8.GetByteCount($licenseText.ToString());testsPassed=$testsPassed;gameStarted=$false;gameEffectsVerified=$false} | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $output 'native-build.json') -Encoding utf8
    [pscustomobject]@{Directory=$output;Backend=$(if(Test-Path -LiteralPath $backend){$backend}else{$null});Client=$(if(Test-Path -LiteralPath $client){$client}else{$null});Tests=$(if(Test-Path -LiteralPath $tests){$tests}else{$null});Report=(Join-Path $output 'native-build.json');TestsPassed=$testsPassed;Target=$Target}
}
finally {
    $env:ZIG_GLOBAL_CACHE_DIR = $previousGlobalCache
    $env:ZIG_LOCAL_CACHE_DIR = $previousLocalCache
    Set-Location -LiteralPath $previousDirectory
}
