# make-icon.ps1 -- draw the NSP's desktop icon.
#
# WHY THE ICON IS DRAWN HERE RATHER THAN SHIPPED AS A PICTURE SOMEONE FOUND.
#
# The handover's §3.5 says it plainly: draw our own, and ask the user before using anything of the upstream
# brand. This script is that "draw our own": it is the artwork's source, it is reproducible, and nothing in it
# comes from the service or from any other application.
#
# WHAT hacBrewPack REQUIRES (from its own README, which ships in third_party/hacbrewpack):
#
#   * the file goes in the control romfs as  icon_<Language>.dat,  with icon_AmericanEnglish.dat as the
#     default. Those .dat files "are just renamed jpg files";
#   * it must be a JPEG, 256x256;
#   * and it is "highly recommended to delete unnecessary exif data", because an icon with an EXIF block
#     shows up as a PLACEHOLDER after installing the NSP -- which is exactly the kind of silent failure this
#     project tries to avoid, so the script below verifies the absence of an EXIF marker before it finishes.
#
# Usage:  powershell -File scripts/make-icon.ps1 [-Out assets\icon-256.jpg]

param(
    [string]$Out = 'assets\icon-256.jpg'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$size = 256
$bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# ---- the tile: a rounded square, dark, with the app's own accent ------------
# Rounded by drawing a filled path (four arcs + four lines) rather than a rectangle: the console shows these
# icons on a light background in some menus and a dark one in others, so the corners matter.
$margin = 6
$radius = 46
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$d = $radius * 2
$path.AddArc($margin, $margin, $d, $d, 180, 90)
$path.AddArc($size - $margin - $d, $margin, $d, $d, 270, 90)
$path.AddArc($size - $margin - $d, $size - $margin - $d, $d, $d, 0, 90)
$path.AddArc($margin, $size - $margin - $d, $d, $d, 90, 90)
$path.CloseFigure()

$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.Point(0, 0)),
    (New-Object System.Drawing.Point($size, $size)),
    [System.Drawing.Color]::FromArgb(255, 32, 26, 34),
    [System.Drawing.Color]::FromArgb(255, 58, 26, 30))
$g.FillPath($bg, $path)

# A thin accent border, so the tile reads as an object rather than as a hole.
$pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 214, 62, 74), 6)
$pen.Alignment = [System.Drawing.Drawing2D.PenAlignment]::Inset
$g.DrawPath($pen, $path)

# ---- the mark: a play triangle --------------------------------------------
# The one glyph that says "this plays video" without borrowing anyone's logo.
$accent = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 236, 74, 86))
$tri = @(
    (New-Object System.Drawing.PointF(100, 74)),
    (New-Object System.Drawing.PointF(100, 182)),
    (New-Object System.Drawing.PointF(184, 128))
)
$g.FillPolygon($accent, $tri)

# ---- the name, in the icon's own language ---------------------------------
# Text is drawn last and in white because the tile behind it is dark in both themes. The font is asked for by
# name and falls back to whatever the system has: this script is a build-time tool, not shipped code, so a
# missing font is a cosmetic difference here rather than a crash on the console.
try {
    $font = New-Object System.Drawing.Font('Microsoft YaHei', 30, [System.Drawing.FontStyle]::Bold)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(240, 255, 255, 255))
    # THE TWO CHARACTERS ARE BUILT FROM CODE POINTS, not typed into this file.
    #
    # The first version had the literal text here and it came out as mojibake: Windows PowerShell 5.1 reads a
    # .ps1 with no BOM as ANSI, so a UTF-8 CJK literal is destroyed before Graphics::DrawString ever sees it.
    # U+77ED "短" and U+5267 "剧" are the same characters without depending on the script's own encoding.
    $label = [string][char]0x77ED + [string][char]0x5267
    $g.DrawString($label, $font, $white, (New-Object System.Drawing.RectangleF(0, 188, $size, 52)), $fmt)
} catch {
    Write-Host "icon: no CJK font available, drawing the mark only ($($_.Exception.Message))"
}

$g.Dispose()

# ---- save as JPEG with NO exif ---------------------------------------------
# The quality encoder is the only parameter passed, so what lands on disk is a plain JFIF JPEG: no EXIF
# block, no thumbnail, nothing for the console to choke on.
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$encParams = New-Object System.Drawing.Imaging.EncoderParameters(1)
$encParams.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
    [System.Drawing.Imaging.Encoder]::Quality, 92L)
$bmp.Save((Resolve-Path -LiteralPath (Split-Path -Parent $Out) | ForEach-Object { Join-Path $_ (Split-Path -Leaf $Out) }),
          $codec, $encParams)
$bmp.Dispose()

# ---- verify what was written, because "the icon is a placeholder" is the reported symptom of a bad file ----
$full = Join-Path (Resolve-Path (Split-Path -Parent $Out)) (Split-Path -Leaf $Out)
$bytes = [System.IO.File]::ReadAllBytes($full)
$soi = ($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xD8)
$eoi = ($bytes[$bytes.Length - 2] -eq 0xFF -and $bytes[$bytes.Length - 1] -eq 0xD9)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
$hasExif = $text.Contains('Exif')

Write-Host "icon: $full"
Write-Host "  size      : $($bytes.Length) bytes, $size x $size"
Write-Host "  JPEG SOI  : $soi   EOI: $eoi"
Write-Host "  Exif block: $hasExif   (must be False -- see the note at the top of this script)"

if (-not ($soi -and $eoi)) { throw "the icon is not a complete JPEG" }
if ($hasExif) { throw "the icon carries an EXIF block, which the console renders as a PLACEHOLDER" }
Write-Host "icon: OK"
