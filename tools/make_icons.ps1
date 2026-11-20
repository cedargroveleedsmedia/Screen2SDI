# Generates the tray/app icons (res\*.ico) as anti-aliased status dots.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File tools\make_icons.ps1
$ErrorActionPreference = 'Stop'
$resDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'res'
New-Item -ItemType Directory -Force $resDir | Out-Null

$sizes = 16, 20, 24, 32, 40, 48, 64

function Clamp01([double]$v) { if ($v -lt 0) { 0 } elseif ($v -gt 1) { 1 } else { $v } }

function New-DotImage([int]$size, [int[]]$rgb) {
    # Returns BGRA bytes, bottom-up, for a filled circle with a darker rim and a soft highlight.
    $px = New-Object byte[] ($size * $size * 4)
    $c = ($size - 1) / 2.0
    $R = $size / 2.0 - 0.5
    $rim = [Math]::Max(1.0, $size / 14.0)
    for ($y = 0; $y -lt $size; $y++) {
        for ($x = 0; $x -lt $size; $x++) {
            $dx = $x - $c; $dy = $y - $c
            $d = [Math]::Sqrt($dx * $dx + $dy * $dy)
            $cov = Clamp01 ($R - $d + 0.5)
            if ($cov -le 0) { continue }
            # rim: darken near the edge
            $rimT = Clamp01 (($d - ($R - $rim)) + 0.5)
            # highlight: lighten towards the upper-left
            $hl = Clamp01 (1.0 - [Math]::Sqrt(($dx + $R * 0.35) * ($dx + $R * 0.35) + ($dy + $R * 0.35) * ($dy + $R * 0.35)) / ($R * 0.9))
            $col = @(0, 0, 0)
            for ($i = 0; $i -lt 3; $i++) {
                $v = $rgb[$i] + (255 - $rgb[$i]) * 0.45 * $hl
                $v = $v * (1 - 0.45 * $rimT)
                $col[$i] = [int][Math]::Round([Math]::Min(255, [Math]::Max(0, $v)))
            }
            $row = $size - 1 - $y   # bottom-up
            $o = ($row * $size + $x) * 4
            $a = [int][Math]::Round(255 * $cov)
            $px[$o + 0] = [byte]$col[2]
            $px[$o + 1] = [byte]$col[1]
            $px[$o + 2] = [byte]$col[0]
            $px[$o + 3] = [byte]$a
        }
    }
    return , $px
}

function Write-Ico([string]$path, [int[]]$rgb) {
    $images = @()
    foreach ($s in $sizes) {
        $pixels = New-DotImage $s $rgb
        $maskRow = [int]([Math]::Ceiling($s / 32.0) * 4)
        $ms = New-Object System.IO.MemoryStream
        $bw = New-Object System.IO.BinaryWriter($ms)
        $bw.Write([int]40); $bw.Write([int]$s); $bw.Write([int]($s * 2))
        $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
        $bw.Write([int]($pixels.Length + $maskRow * $s)); $bw.Write([int]0); $bw.Write([int]0)
        $bw.Write([int]0); $bw.Write([int]0)
        $bw.Write($pixels)
        $bw.Write((New-Object byte[] ($maskRow * $s)))
        $bw.Flush()
        $images += , @($s, $ms.ToArray())
    }
    $fs = [System.IO.File]::Create($path)
    $w = New-Object System.IO.BinaryWriter($fs)
    $w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($img in $images) {
        $s = $img[0]; $data = $img[1]
        $dim = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([int16]1); $w.Write([int16]32); $w.Write([int]$data.Length); $w.Write([int]$offset)
        $offset += $data.Length
    }
    foreach ($img in $images) { $w.Write($img[1]) }
    $w.Close()
    Write-Host "wrote $path"
}

Write-Ico (Join-Path $resDir 'green.ico')  @(40, 190, 70)
Write-Ico (Join-Path $resDir 'yellow.ico') @(240, 190, 20)
Write-Ico (Join-Path $resDir 'red.ico')    @(215, 45, 40)
Write-Ico (Join-Path $resDir 'app.ico')    @(40, 110, 215)
