# Image decoding: budget, progressive slices, and pixel identity.
#
# Verifies the per-paint decode budget (R136 / docs/design-async-image-decode.md):
#
#   1. a big image decoded in slices must produce exactly the same pixels as
#      the same image decoded in one shot (catches a decoder that loses rows
#      when it is asked to continue);
#   2. a big image must no longer be decoded inside a single paint: no paint
#      over PAINT_LIMIT_MS, and the frame loop must keep running (rAF gaps);
#   3. AGB_IMG_DECODE_MS=0 still restores the old one-shot behaviour, so the
#      knob itself is tested;
#   4. the sampling cap (AGB_IMG_SAMPLE_MAX) must actually change the output,
#      otherwise the scaled-draw path is dead and the numbers above are lying.
#
# The fixture is generated with System.Drawing (seeded, so two runs are
# byte-identical) instead of committing a 550 KB JPEG.
#
# Usage: pwsh -File tools/run_image_tests.ps1 [-KeepArtifacts]

param(
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'
$tmp = Join-Path $root 'build-zig-native\img_test'
$pixel = Join-Path $root 'tools\pixel_check.py'

foreach ($p in @($exe, $pixel)) {
    if (-not (Test-Path $p)) { throw "not found: $p - run tools/build_project.ps1 first" }
}

New-Item -ItemType Directory -Force -Path $tmp | Out-Null

# The probe writes into its own directory: keep everything inside the workspace
# (the harness sandbox denies writes to %TEMP%).
$env:TEMP = Join-Path $tmp 'tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

$failures = 0

function Fail($what) {
    $script:failures++
    Write-Host "FAIL  $what"
}

function Pass($what) {
    Write-Host "PASS  $what"
}

# ---------------------------------------------------------------- fixture
$jpg = Join-Path $tmp 'big_image.jpg'
if (-not (Test-Path $jpg)) {
    Write-Host '== generating a 4000x3000 JPEG fixture'
    Add-Type -AssemblyName System.Drawing
    $w = 4000; $h = 3000
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $rnd = New-Object System.Random(7)
    for ($i = 0; $i -lt 4000; $i++) {
        $c = [System.Drawing.Color]::FromArgb($rnd.Next(256), $rnd.Next(256), $rnd.Next(256))
        $b = New-Object System.Drawing.SolidBrush($c)
        $g.FillEllipse($b, $rnd.Next($w), $rnd.Next($h), $rnd.Next(200, 900), $rnd.Next(200, 900))
        $b.Dispose()
    }
    $g.Dispose()
    $enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
        Where-Object { $_.MimeType -eq 'image/jpeg' }
    $params = New-Object System.Drawing.Imaging.EncoderParameters(1)
    $params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
        [System.Drawing.Imaging.Encoder]::Quality, 85)
    $bmp.Save($jpg, $enc, $params)
    $bmp.Dispose()
}

function Write-Page($path, $imageName, $withTimer) {
    $timer = ''
    if ($withTimer) {
        $timer = @'
<pre id="out">running</pre>
<script>
var gaps = [], last = 0, frames = 0;
function tick(t) {
    if (last) gaps.push(t - last);
    last = t;
    frames++;
    if (frames < 90) requestAnimationFrame(tick);
    else {
        var max = 0;
        for (var i = 0; i < gaps.length; i++) if (gaps[i] > max) max = gaps[i];
        document.getElementById('out').textContent =
            'frames=' + frames + ' maxgap=' + max.toFixed(1);
    }
}
requestAnimationFrame(tick);
</script>
'@
    }
    $html = @"
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>image decode</title>
<style>html,body{margin:0;padding:0;background:#fff}img{width:400px;height:300px;display:block}</style>
</head><body>
<img src="$imageName" alt="">
$timer
</body></html>
"@
    Set-Content -Path $path -Value $html -Encoding utf8
}


# Big PNG: the PNG decoder now feeds libpng in byte chunks, so it needs the
# same "sliced == one-shot" check as JPEG.
$png = Join-Path $tmp 'big_image.png'
if (-not (Test-Path $png)) {
    Write-Host '== generating a 3000x2000 PNG fixture'
    Add-Type -AssemblyName System.Drawing
    $w = 3000; $h = 2000
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $rnd = New-Object System.Random(11)
    for ($i = 0; $i -lt 3000; $i++) {
        $c = [System.Drawing.Color]::FromArgb($rnd.Next(256), $rnd.Next(256), $rnd.Next(256))
        $b = New-Object System.Drawing.SolidBrush($c)
        $g.FillEllipse($b, $rnd.Next($w), $rnd.Next($h), $rnd.Next(100, 600), $rnd.Next(100, 600))
        $b.Dispose()
    }
    $g.Dispose()
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
$staticPage = Join-Path $tmp 'static.html'
$timerPage = Join-Path $tmp 'timer.html'
$pngPage = Join-Path $tmp 'static_png.html'
Write-Page $staticPage 'big_image.jpg' $false
Write-Page $timerPage 'big_image.jpg' $true
Write-Page $pngPage 'big_image.png' $false
$staticUrl = "file:///$($staticPage -replace '\\', '/')"
$timerUrl = "file:///$($timerPage -replace '\\', '/')"
$pngUrl = "file:///$($pngPage -replace '\\', '/')"

function Run-Probe($url, $shot, $wait, [hashtable]$envs) {
    foreach ($k in $envs.Keys) { Set-Item -Path "Env:$k" -Value $envs[$k] }
    try {
        $log = Join-Path $tmp ('run_' + [guid]::NewGuid().ToString('N') + '.log')
        $args = @('--probe', $url, '--wait', "$wait", '--settle', '400', '--timeout', '60000')
        if ($shot) { $args += @('--shot', $shot) }
        Push-Location (Split-Path -Parent $exe)
        try {
            & $exe @args *> $log
        } finally {
            Pop-Location
        }
        return @{ Exit = $LASTEXITCODE; Log = $log }
    } finally {
        foreach ($k in $envs.Keys) { Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue }
    }
}

function Perf-Paints($log) {
    $paints = @()
    $decodes = @()
    foreach ($m in [regex]::Matches((Get-Content -Raw $log), '\[perf\] layout=(\d+)ms paint=(\d+)ms total=\d+ms[^\n]*decode=([\d.]+)ms')) {
        $paints += [int]$m.Groups[2].Value
        $decodes += [double]$m.Groups[3].Value
    }
    return @{ Paints = $paints; Decodes = $decodes }
}

function Max-Gap($log) {
    $m = [regex]::Match((Get-Content -Raw $log), 'maxgap=([\d.]+)')
    if ($m.Success) { return [double]$m.Groups[1].Value }
    return $null
}

# 1. sliced decode must be pixel-identical to a one-shot decode, and the
#    off-thread decode (L2) must produce the very same pixels again.
$oneShot = Join-Path $tmp 'oneshot.bmp'
$sliced = Join-Path $tmp 'sliced.bmp'
$async = Join-Path $tmp 'async.bmp'
$r1 = Run-Probe $staticUrl $oneShot 2500 @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '0'; AGB_IMG_SAMPLE_MAX = '0' }
$r2 = Run-Probe $staticUrl $sliced 2500 @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '12'; AGB_IMG_SAMPLE_MAX = '0' }
Run-Probe $staticUrl $async 2500 @{ AGB_IMG_SAMPLE_MAX = '0' } | Out-Null
if (-not (Test-Path $oneShot) -or -not (Test-Path $sliced) -or -not (Test-Path $async)) {
    Fail 'progressive decode: screenshot missing'
} else {
    $h1 = (Get-FileHash $oneShot -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $sliced -Algorithm SHA256).Hash
    $h3 = (Get-FileHash $async -Algorithm SHA256).Hash
    if ($h1 -eq $h2) { Pass 'JPEG: sliced decode draws the same pixels as one-shot' }
    else { Fail 'JPEG: progressive decode changed the pixels' }
    if ($h1 -eq $h3) { Pass 'JPEG: off-thread decode draws the same pixels' }
    else { Fail 'JPEG: off-thread decode changed the pixels' }
}

# 1b. same for PNG (its decoder feeds libpng in byte chunks)
$pngOne = Join-Path $tmp 'png_oneshot.bmp'
$pngSliced = Join-Path $tmp 'png_sliced.bmp'
Run-Probe $pngUrl $pngOne 2500 @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '0'; AGB_IMG_SAMPLE_MAX = '0' } | Out-Null
Run-Probe $pngUrl $pngSliced 2500 @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '4'; AGB_IMG_SAMPLE_MAX = '0' } | Out-Null
if (-not (Test-Path $pngOne) -or -not (Test-Path $pngSliced)) {
    Fail 'PNG: screenshot missing'
} else {
    $h1 = (Get-FileHash $pngOne -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $pngSliced -Algorithm SHA256).Hash
    if ($h1 -eq $h2) { Pass 'PNG: sliced decode draws the same pixels as one-shot' }
    else { Fail 'PNG: progressive decode changed the pixels' }
}

# 1c. L2: with the decode service on, a paint must not decode at all, and the
#     image still has to appear (compare against the inline-decode render).
$asyncShot = Join-Path $tmp 'async_paint.bmp'
$syncShot = Join-Path $tmp 'sync_paint.bmp'
$rAsync = Run-Probe $staticUrl $asyncShot 2500 @{ AGB_PERF_LOG = '1' }
Run-Probe $staticUrl $syncShot 2500 @{ AGB_IMG_ASYNC = '0'; AGB_IMG_SAMPLE_MAX = '0' } | Out-Null
$asyncPerf = Perf-Paints $rAsync.Log
$asyncDecode = ($asyncPerf.Decodes | Measure-Object -Sum).Sum
if ($null -eq $asyncDecode) { $asyncDecode = 0 }
if ($asyncDecode -gt 2.0) {
    Fail ("L2: paint still decoded {0:N1}ms (should be off-thread)" -f $asyncDecode)
} else {
    Pass ("L2: paint decoded {0:N1}ms (off-thread)" -f $asyncDecode)
}
if ((Test-Path $asyncShot) -and (Test-Path $syncShot)) {
    if ((Get-FileHash $asyncShot -Algorithm SHA256).Hash -eq (Get-FileHash $syncShot -Algorithm SHA256).Hash) {
        Pass 'L2: off-thread frame is painted'
    } else {
        Fail 'L2: off-thread frame differs from the inline render'
    }
} else {
    Fail 'L2: screenshot missing'
}

# 2. the inline path (L1) must not stall for the whole decode either
$paintLimit = 90      # was ~467ms before the budget
$gapLimit = 70        # was ~100ms (the whole decode landing in one frame)
$r3 = Run-Probe $timerUrl $null 3000 @{ AGB_PERF_LOG = '1'; AGB_IMG_ASYNC = '0' }
$perf = Perf-Paints $r3.Log
$gap = Max-Gap $r3.Log
if ($perf.Paints.Count -lt 1) {
    Fail 'no decoding frame at all (inline path)'
} else {
    Pass ("inline decode finished in {0} frame(s)" -f $perf.Paints.Count)
}
$maxPaint = ($perf.Paints | Measure-Object -Maximum).Maximum
if ($maxPaint -gt $paintLimit) {
    Fail ("a paint took {0}ms (limit {1}ms)" -f $maxPaint, $paintLimit)
} else {
    Pass ("worst paint {0}ms (limit {1}ms)" -f $maxPaint, $paintLimit)
}
if ($null -eq $gap) {
    Fail 'no rAF gap reported by the page'
} elseif ($gap -gt $gapLimit) {
    Fail ("rAF gap {0}ms (limit {1}ms)" -f $gap, $gapLimit)
} else {
    Pass ("rAF gap {0}ms (limit {1}ms)" -f $gap, $gapLimit)
}

# 2b. L3: decoding at display size must be much cheaper than decoding natural size
$rScaled = Run-Probe $timerUrl $null 3000 @{ AGB_PERF_LOG = '1'; AGB_IMG_ASYNC = '0' }
$rNatural = Run-Probe $timerUrl $null 3000 @{ AGB_PERF_LOG = '1'; AGB_IMG_NO_SCALE = '1'; AGB_IMG_ASYNC = '0' }
$scaledMs = (Perf-Paints $rScaled.Log).Decodes | Measure-Object -Sum
$naturalMs = (Perf-Paints $rNatural.Log).Decodes | Measure-Object -Sum
if (-not $scaledMs.Count -or -not $naturalMs.Count) {
    Fail 'L3: no decode timings'
} elseif ($scaledMs.Sum -gt $naturalMs.Sum / 3) {
    Fail ("L3: scaled decode {0:N0}ms is not much cheaper than natural {1:N0}ms" -f $scaledMs.Sum, $naturalMs.Sum)
} else {
    Pass ("L3: scaled decode {0:N0}ms vs natural {1:N0}ms" -f $scaledMs.Sum, $naturalMs.Sum)
}

# 3. AGB_IMG_DECODE_MS=0 restores the one-shot path (with L3 disabled, otherwise
#    the frame is small enough to be cheap even in one shot)
$r4 = Run-Probe $timerUrl $null 3000 @{ AGB_PERF_LOG = '1'; AGB_IMG_DECODE_MS = '0'; AGB_IMG_NO_SCALE = '1'; AGB_IMG_ASYNC = '0' }
$perfOld = Perf-Paints $r4.Log
$oldMax = 0
if ($perfOld.Paints.Count) { $oldMax = ($perfOld.Paints | Measure-Object -Maximum).Maximum }
if ($perfOld.Paints.Count -le 2 -and $oldMax -gt 200) {
    Pass ("AGB_IMG_DECODE_MS=0 decodes in one frame ({0}ms)" -f $oldMax)
} else {
    Fail ("AGB_IMG_DECODE_MS=0 did not restore the one-shot path (frames={0} max={1}ms)" -f $perfOld.Paints.Count, $oldMax)
}

# 4. the sampling cap must be live (otherwise the scaled-draw path is not used)
$nearest = Join-Path $tmp 'nearest.bmp'
$exact = Join-Path $tmp 'exact.bmp'
Run-Probe $staticUrl $nearest 2500 @{ AGB_IMG_SAMPLE_MAX = '1'; AGB_IMG_DECODE_MS = '12'; AGB_IMG_ASYNC = '0' } | Out-Null
Run-Probe $staticUrl $exact 2500 @{ AGB_IMG_SAMPLE_MAX = '0'; AGB_IMG_DECODE_MS = '12'; AGB_IMG_ASYNC = '0' } | Out-Null
if ((Test-Path $nearest) -and (Test-Path $exact)) {
    $hn = (Get-FileHash $nearest -Algorithm SHA256).Hash
    $he = (Get-FileHash $exact -Algorithm SHA256).Hash
    if ($hn -ne $he) { Pass 'scaled sampling cap changes the output (path is live)' }
    else { Fail 'AGB_IMG_SAMPLE_MAX had no effect - scaled draw not used?' }
} else {
    Fail 'sampling-cap screenshots missing'
}

if (-not $KeepArtifacts) {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Host "IMAGE=FAIL ($failures)"
    exit 1
}

Write-Host 'IMAGE=PASS'
exit 0
