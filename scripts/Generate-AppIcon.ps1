#requires -Version 7.4
[CmdletBinding()]
param(
    [string]$Output = (Join-Path $PSScriptRoot '..\src\Trainer.Desktop\Assets\App.ico'),
    [string]$SourceSvg = (Join-Path $PSScriptRoot '..\src\Trainer.Desktop\Assets\App.svg'),
    [string]$PngDirectory = (Join-Path $PSScriptRoot '..\artifacts\design\app-icon'),
    [string]$InkscapePath = ''
)
$ErrorActionPreference = 'Stop'
if (!$IsWindows) { throw 'Icon regeneration uses Inkscape and Windows System.Drawing. The generated icon is checked into source.' }
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourcePath = [IO.Path]::GetFullPath($SourceSvg)
$outputPath = [IO.Path]::GetFullPath($Output)
$pngPath = [IO.Path]::GetFullPath($PngDirectory)
if (!(Test-Path -LiteralPath $sourcePath -PathType Leaf)) { throw 'The editable source SVG is missing.' }

# Use the installed application without embedding a maintainer's local install path.
function Find-Inkscape {
    if ($InkscapePath) {
        if (!(Test-Path -LiteralPath $InkscapePath -PathType Leaf)) { throw 'The specified Inkscape executable was not found.' }
        return [IO.Path]::GetFullPath($InkscapePath)
    }
    foreach ($name in @('inkscape.com','inkscape.exe')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return $command.Source }
    }
    foreach ($hive in @('HKLM:','HKCU:')) {
        try {
            $key = Get-Item -LiteralPath ($hive + '\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\inkscape.exe') -ErrorAction Stop
            $registered = [string]$key.GetValue('')
            if ($registered -and (Test-Path -LiteralPath $registered -PathType Leaf)) {
                $console = Join-Path ([IO.Path]::GetDirectoryName($registered)) 'inkscape.com'
                return $(if (Test-Path -LiteralPath $console -PathType Leaf) { $console } else { $registered })
            }
        } catch { }
    }
    foreach ($root in @([Environment]::GetFolderPath('ProgramFiles'),[Environment]::GetFolderPath('ProgramFilesX86'))) {
        if (!$root) { continue }
        $candidate = Join-Path $root 'Inkscape\bin\inkscape.com'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw 'Inkscape was not found. Install it or pass -InkscapePath. End users do not need Inkscape to run the trainer.'
}

# Artwork is a self-contained vector: no fonts, external images, scripts or DTDs.
$xmlSettings = [Xml.XmlReaderSettings]::new()
$xmlSettings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
$xmlSettings.XmlResolver = $null
$reader = [Xml.XmlReader]::Create($sourcePath,$xmlSettings)
try { $document = [Xml.XmlDocument]::new(); $document.XmlResolver = $null; $document.Load($reader) } finally { $reader.Dispose() }
foreach ($element in $document.SelectNodes('//*')) {
    if ($element.LocalName -notin @('svg','g','path','rect','circle','ellipse','polygon','polyline','line','title','desc','defs','linearGradient','radialGradient','stop','clipPath')) {
        throw 'The icon SVG must use self-contained vector shapes. Convert text to paths and remove external assets.'
    }
    foreach ($attribute in $element.Attributes) {
        if ($attribute.LocalName -eq 'href' -or $attribute.LocalName -match '^on' -or $attribute.Value -match '(?i)url\((?!#)') {
            throw 'The icon SVG cannot reference external assets or executable content.'
        }
    }
}

$inkscape = Find-Inkscape
Add-Type -AssemblyName System.Drawing
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
[IO.Directory]::CreateDirectory($pngPath) | Out-Null
$scratch = Join-Path $projectRoot ('artifacts\icon-build\' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$oldProfile = $env:INKSCAPE_PROFILE_DIR
$oldXdgData = $env:XDG_DATA_HOME
$oldXdgConfig = $env:XDG_CONFIG_HOME
$env:INKSCAPE_PROFILE_DIR = Join-Path $scratch 'profile'
$env:XDG_DATA_HOME = Join-Path $scratch 'data'
$env:XDG_CONFIG_HOME = Join-Path $scratch 'config'
foreach ($directory in @($env:INKSCAPE_PROFILE_DIR,$env:XDG_DATA_HOME,$env:XDG_CONFIG_HOME)) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
$frames = [Collections.Generic.List[object]]::new()
try {
    foreach ($size in @(16,24,32,48,64,128,256,512)) {
        $raw = Join-Path $scratch ('raw-' + $size + '.png')
        $log = & $inkscape $sourcePath '--export-area-page' '--export-type=png' ("--export-filename=$raw") ("--export-width=$size") ("--export-height=$size") 2>&1
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $raw -PathType Leaf)) { throw ('Inkscape could not export icon size ' + $size + '.') }
        $log | Set-Content -LiteralPath (Join-Path $scratch ('inkscape-' + $size + '.log')) -Encoding utf8
        $image = [Drawing.Image]::FromFile($raw)
        $bitmap = [Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            if ($image.Width -ne $size -or $image.Height -ne $size) { throw 'Inkscape exported an unexpected image size.' }
            $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            # SVG exporters record a size-dependent DPI. Explicit pixel units
            # prevent GDI+ from expanding a 16px export back to its document size.
            $graphics.DrawImage($image,[Drawing.Rectangle]::new(0,0,$size,$size),0,0,$size,$size,[Drawing.GraphicsUnit]::Pixel)
            # A fresh bitmap keeps exported file paths or editor metadata out of PNG/ICO.
            $memory = [IO.MemoryStream]::new()
            try {
                $bitmap.Save($memory,[Drawing.Imaging.ImageFormat]::Png)
                $bytes = $memory.ToArray()
                [IO.File]::WriteAllBytes((Join-Path $pngPath ('App-' + $size + '.png')),$bytes)
                if ($size -le 256) { $frames.Add([pscustomobject]@{size=$size;bytes=$bytes}) }
            } finally { $memory.Dispose() }
        } finally { $graphics.Dispose(); $bitmap.Dispose(); $image.Dispose() }
    }
} finally {
    $env:INKSCAPE_PROFILE_DIR = $oldProfile
    $env:XDG_DATA_HOME = $oldXdgData
    $env:XDG_CONFIG_HOME = $oldXdgConfig
}

$temporary = $outputPath + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
$stream = [IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.size -eq 256) { 0 } else { $frame.size }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]$frame.bytes.Length); $writer.Write([uint32]$offset)
        $offset += $frame.bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.bytes) }
} finally { $writer.Dispose(); $stream.Dispose() }
[IO.File]::Move($temporary,$outputPath,$true)
[pscustomobject]@{ Icon=$outputPath; SourceSvg=$sourcePath; PngDirectory=$pngPath; Frames=@($frames | ForEach-Object size); LargestPng=512 }
