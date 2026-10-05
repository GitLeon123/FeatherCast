# Generates the FeatherCast icon (build/icon.ico, build/icon.png)
# from the checked-in source artwork.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

Add-Type -AssemblyName System.Drawing

$buildDir = Join-Path $PSScriptRoot "..\build"
$assetPath = Join-Path $PSScriptRoot "..\native\assets\icon.png"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

if (-not (Test-Path $assetPath)) {
  throw "Icon source not found: $assetPath"
}

function New-IconBitmap([System.Drawing.Image]$source, [int]$size) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
  $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $g.DrawImage($source, 0, 0, $size, $size)
  $g.Dispose()
  return $bmp
}

$source = [System.Drawing.Image]::FromFile((Resolve-Path $assetPath))

# 256px PNG (for ICO and window icon)
$big = New-IconBitmap $source 256
$pngPath = Join-Path $buildDir "icon.png"
$big.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)

# PNG bytes for the ICO container
$ms = New-Object System.IO.MemoryStream
$big.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
$pngBytes = $ms.ToArray()
$ms.Dispose()

# ICO with embedded PNG (Vista+ supports PNG-in-ICO)
$icoPath = Join-Path $buildDir "icon.ico"
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)      # reserved
$bw.Write([uint16]1)      # type = icon
$bw.Write([uint16]1)      # image count
$bw.Write([byte]0)        # width 0 => 256
$bw.Write([byte]0)        # height 0 => 256
$bw.Write([byte]0)        # colors
$bw.Write([byte]0)        # reserved
$bw.Write([uint16]1)      # planes
$bw.Write([uint16]32)     # bits per pixel
$bw.Write([uint32]$pngBytes.Length)
$bw.Write([uint32]22)     # offset (6 + 16)
$bw.Write($pngBytes)
$bw.Flush(); $bw.Dispose(); $fs.Dispose()

# The tray icon uses the same embedded application icon resource.
$big.Dispose(); $source.Dispose()
Write-Host "Icons generated in $buildDir : icon.ico, icon.png"

# Android launcher icons for the FeatherCast Phone app (android/app).
$resDir = Join-Path $PSScriptRoot "..\android\app\src\main\res"
$source = [System.Drawing.Bitmap]::FromFile((Resolve-Path $assetPath))
# Bounds of the rounded-square artwork inside the transparent source margin.
$content = New-Object System.Drawing.Rectangle(35, 30, 1184, 1194)
$densities = @{ "mdpi" = 1.0; "hdpi" = 1.5; "xhdpi" = 2.0; "xxhdpi" = 3.0; "xxxhdpi" = 4.0 }
foreach ($density in $densities.Keys) {
  $scale = $densities[$density]
  $dir = Join-Path $resDir "mipmap-$density"
  New-Item -ItemType Directory -Force -Path $dir | Out-Null

  $legacy = New-IconBitmap $source ([int](48 * $scale))
  $legacy.Save((Join-Path $dir "ic_launcher.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $legacy.Dispose()

  # Adaptive foreground: 108dp canvas, artwork centered at 76dp so the
  # launcher mask trims the rounded corners onto the matching background.
  $size = [int](108 * $scale)
  $inner = [int](76 * $scale)
  $offset = [int](($size - $inner) / 2)
  $fg = New-Object System.Drawing.Bitmap($size, $size)
  $g = [System.Drawing.Graphics]::FromImage($fg)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $g.DrawImage($source, (New-Object System.Drawing.Rectangle($offset, $offset, $inner, $inner)),
               $content, [System.Drawing.GraphicsUnit]::Pixel)
  $g.Dispose()
  $fg.Save((Join-Path $dir "ic_launcher_foreground.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $fg.Dispose()
}
$source.Dispose()
Write-Host "Android launcher icons generated in $resDir"
