# Image decoding: budget, progressive slices, and pixel identity.
#
# Verifies the per-paint decode budget (R136 / docs/design-async-image-decode.md):
#
#   1. a big image decoded in slices must produce exactly the same pixels as
#      the same image decoded in one shot (catches a decoder that loses rows
#      when it is asked to continue);
#   2. a big image must no longer be decoded inside a single paint: no paint
#      over the one-shot decode, and the frame loop must keep running (rAF gaps);
#   3. AGB_IMG_DECODE_MS=0 still restores the old one-shot behaviour, so the
#      knob itself is tested;
#   4. the sampling cap (AGB_IMG_SAMPLE_MAX) must actually change the output,
#      otherwise the scaled-draw path is dead and the numbers above are lying;
#   5. an image whose resource completes with no body at all (HTTP 204,
#      Content-Length: 0, a truncated header) must be a broken image, not a
#      crash - and the async path must not re-decode it on every paint.
#
# Parallel by design: every probe has its own profile/cwd, so the timed cases
# run next to the pixel cases. The timing assertions are relative (the sliced
# decode is compared with a one-shot control started at the same time), which
# keeps them meaningful on a loaded machine.
#
# The fixture is generated with System.Drawing (seeded, so two runs are
# byte-identical) instead of committing a 550 KB JPEG, and it is cached under
# build-zig-native/img_test between runs (first run pays the generation cost).
#
# Usage: pwsh -File tools/run_image_tests.ps1 [-KeepArtifacts]
# Expected wall time: ~6 s after the fixture exists (~10 s on a cold build).

param(
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-zig-native\agave_shell.exe'
$fixtures = Join-Path $root 'build-zig-native\img_test'
$pixel = Join-Path $root 'tools\pixel_check.py'

foreach ($p in @($exe, $pixel)) {
    if (-not (Test-Path $p)) { throw "not found: $p - run tools/build_project.ps1 first" }
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'image'

New-Item -ItemType Directory -Force -Path $fixtures | Out-Null

$failures = 0

function Fail($what) {
    $script:failures++
    Write-Host "FAIL  $what"
}

function Pass($what) {
    Write-Host "PASS  $what"
}

# ---------------------------------------------------------------- fixtures
$jpg = Join-Path $fixtures 'big_image.jpg'
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

# Big PNG: the PNG decoder now feeds libpng in byte chunks, so it needs the
# same "sliced == one-shot" check as JPEG.
$png = Join-Path $fixtures 'big_image.png'
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

$staticUrl = 'file:///' + ((Join-Path $fixtures 'static.html') -replace '\\', '/')
$timerUrl = 'file:///' + ((Join-Path $fixtures 'timer.html') -replace '\\', '/')
$pngUrl = 'file:///' + ((Join-Path $fixtures 'static_png.html') -replace '\\', '/')

# Teardown page: six asynchronous decodes in flight when the document is torn
# down. The copies stay in the cached fixture directory.
$tdPage = Join-Path $fixtures 'td.html'
$tdHtml = @"
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>teardown</title>
<style>html,body{margin:0;padding:0}img{width:400px;height:260px;display:inline-block;margin:2px}</style>
</head><body>
<script>
for (var i = 1; i <= 6; i++) {
    var img = document.createElement('img');
    img.src = 'td_img' + i + '.png';
    document.body.appendChild(img);
}
setTimeout(function () { location.href = 'td_target.html'; }, 120);
</script>
</body></html>
"@
Set-Content -Path $tdPage -Value $tdHtml -Encoding utf8
$tdTarget = Join-Path $fixtures 'td_target.html'
Write-Page $tdTarget 'big_image.jpg' $false
$tdUrl = 'file:///' + ($tdPage -replace '\\', '/')

function Ensure-Fixtures {
    Write-Page (Join-Path $fixtures 'static.html') 'big_image.jpg' $false
    Write-Page (Join-Path $fixtures 'timer.html') 'big_image.jpg' $true
    Write-Page (Join-Path $fixtures 'static_png.html') 'big_image.png' $false
    1..6 | ForEach-Object {
        $copy = Join-Path $fixtures ("td_img{0}.png" -f $_)
        if (-not (Test-Path $copy)) { Copy-Item $png $copy -Force }
    }
}
Ensure-Fixtures

function Perf-Paints($log) {
    $paints = @()
    $decodes = @()
    if (-not $log) { return @{ Paints = $paints; Decodes = $decodes } }
    foreach ($m in [regex]::Matches($log, '\[perf\] layout=(\d+)ms paint=(\d+)ms total=\d+ms[^\n]*decode=([\d.]+)ms')) {
        $paints += [int]$m.Groups[2].Value
        $decodes += [double]$m.Groups[3].Value
    }
    return @{ Paints = $paints; Decodes = $decodes }
}

function Max-Gap($log) {
    if (-not $log) { return $null }
    $m = [regex]::Match($log, 'maxgap=([\d.]+)')
    if ($m.Success) { return [double]$m.Groups[1].Value }
    return $null
}

# ---------------------------------------------------------------- empty image
$pageDir = Join-Path $root 'tools/pages'
$emptyPort = Get-TestFreePort
$emptyServer = Start-Process -WindowStyle Hidden python -PassThru `
    -ArgumentList (Join-Path $root 'tools\empty_image_server.py'), "$emptyPort"

try {
    $ready = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 100
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$emptyPort/cap_empty_image.html" `
                -UseBasicParsing -TimeoutSec 2
            if ($r.StatusCode -eq 200) { $ready = $true; break }
        } catch { }
    }
    if (-not $ready) { Fail "empty-image: test server did not come up on port $emptyPort" }

    # ------------------------------------------------------------ start probes
    $jobs = [ordered]@{}

    $jobs.jpegOne = Start-TestProbe -Run $run -Name 'jpeg-oneshot' -Url $staticUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $staticUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'jpeg-oneshot\shot.bmp')) `
        -Env @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '0'; AGB_IMG_SAMPLE_MAX = '0' }
    $jobs.jpegSliced = Start-TestProbe -Run $run -Name 'jpeg-sliced' -Url $staticUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $staticUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'jpeg-sliced\shot.bmp')) `
        -Env @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '12'; AGB_IMG_SAMPLE_MAX = '0' }
    $jobs.jpegAsync = Start-TestProbe -Run $run -Name 'jpeg-async' -Url $staticUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $staticUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'jpeg-async\shot.bmp')) `
        -Env @{ AGB_IMG_SAMPLE_MAX = '0' }

    $jobs.pngOne = Start-TestProbe -Run $run -Name 'png-oneshot' -Url $pngUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $pngUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'png-oneshot\shot.bmp')) `
        -Env @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '0'; AGB_IMG_SAMPLE_MAX = '0' }
    $jobs.pngSliced = Start-TestProbe -Run $run -Name 'png-sliced' -Url $pngUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $pngUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'png-sliced\shot.bmp')) `
        -Env @{ AGB_IMG_ASYNC = '0'; AGB_IMG_DECODE_MS = '4'; AGB_IMG_SAMPLE_MAX = '0' }

    # L2: with the decode service on, a paint must not decode at all, and the
    # image still has to appear (compare against the inline-decode render).
    $jobs.l2Async = Start-TestProbe -Run $run -Name 'l2-async' -Url $staticUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $staticUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'l2-async\shot.bmp')) `
        -Env @{ AGB_PERF_LOG = '1' }
    $jobs.l2Sync = Start-TestProbe -Run $run -Name 'l2-sync' -Url $staticUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $staticUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'l2-sync\shot.bmp')) `
        -Env @{ AGB_IMG_ASYNC = '0'; AGB_IMG_SAMPLE_MAX = '0' }

    # Teardown mid-decode.
    $jobs.teardown = Start-TestProbe -Run $run -Name 'teardown' -Url $tdUrl -Wait 3000 -Settle 400 `
        -RawArgs @('--probe', $tdUrl, '--wait', '3000', '--settle', '400', '--timeout', '60000') `
        -Env @{ AGB_IMG_ASYNC_DEBUG = '1' }

    # Sampling cap (nearest vs exact).
    $jobs.nearest = Start-TestProbe -Run $run -Name 'sample-nearest' -Url $staticUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $staticUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'sample-nearest\shot.bmp')) `
        -Env @{ AGB_IMG_SAMPLE_MAX = '1'; AGB_IMG_DECODE_MS = '12'; AGB_IMG_ASYNC = '0' }
    $jobs.exact = Start-TestProbe -Run $run -Name 'sample-exact' -Url $staticUrl -Wait 2500 -Settle 400 `
        -RawArgs @('--probe', $staticUrl, '--wait', '2500', '--settle', '400', '--timeout', '60000',
                   '--shot', (Join-Path $run.Dir 'sample-exact\shot.bmp')) `
        -Env @{ AGB_IMG_SAMPLE_MAX = '0'; AGB_IMG_DECODE_MS = '12'; AGB_IMG_ASYNC = '0' }

    if ($ready) {
        $jobs.empty = Start-TestProbe -Run $run -Name 'empty-image' `
            -Url "http://127.0.0.1:$emptyPort/cap_empty_image.html" `
            -EvalFile (Join-Path $pageDir 'check_empty_image.js') -Wait 1500 -Settle 400 -Timeout 30000 `
            -Env @{ AGB_IMG_ASYNC_DEBUG = '1' }
    }

    # Wave 1: pixel identity / teardown / empty-image. They all decode the big
    # fixture, so the timing probes do not start until this wave is done - a
    # paint measured next to ten other 12-megapixel decoders says nothing.
    $names = @($jobs.Keys)
    $res = @{}
    $done = Complete-TestProbes -Jobs @($jobs.Values)
    for ($i = 0; $i -lt $names.Count; $i++) { $res[$names[$i]] = $done[$i] }

    # Wave 2: inline decode timings + L3, four probes on an otherwise idle
    # suite. The one-shot control is captured in the same wave, so the
    # comparison stays meaningful on a busy machine.
    $timing = [ordered]@{}
    $timing.inline = Start-TestProbe -Run $run -Name 'inline-sliced' -Url $timerUrl -Wait 3000 -Settle 400 `
        -RawArgs @('--probe', $timerUrl, '--wait', '3000', '--settle', '400', '--timeout', '60000') `
        -Env @{ AGB_PERF_LOG = '1'; AGB_IMG_ASYNC = '0' }
    $timing.control = Start-TestProbe -Run $run -Name 'inline-oneshot' -Url $timerUrl -Wait 3000 -Settle 400 `
        -RawArgs @('--probe', $timerUrl, '--wait', '3000', '--settle', '400', '--timeout', '60000') `
        -Env @{ AGB_PERF_LOG = '1'; AGB_IMG_DECODE_MS = '0'; AGB_IMG_NO_SCALE = '1'; AGB_IMG_ASYNC = '0' }
    $timing.scaled = Start-TestProbe -Run $run -Name 'l3-scaled' -Url $timerUrl -Wait 3000 -Settle 400 `
        -RawArgs @('--probe', $timerUrl, '--wait', '3000', '--settle', '400', '--timeout', '60000') `
        -Env @{ AGB_PERF_LOG = '1'; AGB_IMG_ASYNC = '0' }
    $timing.natural = Start-TestProbe -Run $run -Name 'l3-natural' -Url $timerUrl -Wait 3000 -Settle 400 `
        -RawArgs @('--probe', $timerUrl, '--wait', '3000', '--settle', '400', '--timeout', '60000') `
        -Env @{ AGB_PERF_LOG = '1'; AGB_IMG_NO_SCALE = '1'; AGB_IMG_ASYNC = '0' }

    $timingNames = @($timing.Keys)
    $timingDone = Complete-TestProbes -Jobs @($timing.Values)
    for ($i = 0; $i -lt $timingNames.Count; $i++) { $res[$timingNames[$i]] = $timingDone[$i] }

    function ShotPath([string]$key) { return (Join-Path $run.Dir "$key\shot.bmp") }
    function Hash([string]$path) {
        if (Test-Path $path) { return (Get-FileHash $path -Algorithm SHA256).Hash }
        return $null
    }

    # 1. sliced decode must be pixel-identical to a one-shot decode, and the
    #    off-thread decode (L2) must produce the very same pixels again.
    $hOne = Hash (ShotPath 'jpeg-oneshot')
    $hSliced = Hash (ShotPath 'jpeg-sliced')
    $hAsync = Hash (ShotPath 'jpeg-async')
    if (-not $hOne -or -not $hSliced -or -not $hAsync) {
        Fail 'progressive decode: screenshot missing'
    } else {
        if ($hOne -eq $hSliced) { Pass 'JPEG: sliced decode draws the same pixels as one-shot' }
        else { Fail 'JPEG: progressive decode changed the pixels' }
        if ($hOne -eq $hAsync) { Pass 'JPEG: off-thread decode draws the same pixels' }
        else { Fail 'JPEG: off-thread decode changed the pixels' }
    }

    # 1b. same for PNG (its decoder feeds libpng in byte chunks).
    $pngOne = Hash (ShotPath 'png-oneshot')
    $pngSliced = Hash (ShotPath 'png-sliced')
    if (-not $pngOne -or -not $pngSliced) {
        Fail 'PNG: screenshot missing'
    } else {
        if ($pngOne -eq $pngSliced) { Pass 'PNG: sliced decode draws the same pixels as one-shot' }
        else { Fail 'PNG: progressive decode changed the pixels' }
    }

    # 1c. L2: no decode in the paint, same frame as the inline render.
    $asyncPerf = Perf-Paints $res.l2Async.Text
    $asyncDecode = ($asyncPerf.Decodes | Measure-Object -Sum).Sum
    if ($null -eq $asyncDecode) { $asyncDecode = 0 }
    if ($asyncDecode -gt 2.0) {
        Fail ("L2: paint still decoded {0:N1}ms (should be off-thread)" -f $asyncDecode)
    } else {
        Pass ("L2: paint decoded {0:N1}ms (off-thread)" -f $asyncDecode)
    }
    if ((Hash (ShotPath 'l2-async')) -and (Hash (ShotPath 'l2-sync')) -and
        (Hash (ShotPath 'l2-async')) -eq (Hash (ShotPath 'l2-sync'))) {
        Pass 'L2: off-thread frame is painted'
    } else {
        Fail 'L2: off-thread frame differs from the inline render'
    }

    # 2. the inline path (L1) must not stall for the whole decode either. The
    #    limits are relative to a one-shot control captured concurrently: the
    #    absolute 90 ms values used to be exact only on an idle machine.
    $perf = Perf-Paints $res.inline.Text
    $gap = Max-Gap $res.inline.Text
    $controlPerf = Perf-Paints $res.control.Text
    $controlGap = Max-Gap $res.control.Text
    $maxPaint = 0
    if ($perf.Paints.Count) { $maxPaint = ($perf.Paints | Measure-Object -Maximum).Maximum }
    $controlPaint = 0
    if ($controlPerf.Paints.Count) { $controlPaint = ($controlPerf.Paints | Measure-Object -Maximum).Maximum }

    if ($perf.Paints.Count -lt 1) {
        Fail 'no decoding frame at all (inline path)'
    } else {
        Pass ("inline decode finished in {0} frame(s)" -f $perf.Paints.Count)
    }
    if ($perf.Paints.Count -lt 3) {
        Fail ("inline decode used {0} frame(s), want at least 3 slices" -f $perf.Paints.Count)
    }
    if ($controlPerf.Paints.Count -eq 0 -or $controlPaint -lt 150) {
        # AGB_IMG_DECODE_MS=0 must restore the one-shot hole (this used to be
        # the only proof that the knob still works).
        Fail ("AGB_IMG_DECODE_MS=0 did not restore the one-shot path (frames={0} max={1}ms)" -f $controlPerf.Paints.Count, $controlPaint)
    } else {
        Pass ("AGB_IMG_DECODE_MS=0 decodes in one frame ({0}ms)" -f $controlPaint)
    }
    if ($controlPaint -gt 0 -and $maxPaint -gt [math]::Max(120, $controlPaint / 2)) {
        Fail ("a sliced paint took {0}ms (one-shot control {1}ms)" -f $maxPaint, $controlPaint)
    } else {
        Pass ("worst sliced paint {0}ms (one-shot control {1}ms)" -f $maxPaint, $controlPaint)
    }
    # On a quiet machine the derived budget is ~70-90 ms. In the full batch the
    # frame loop itself is scheduled late, so an absolute gap says nothing;
    # what still holds is that the sliced run is not dramatically worse than
    # the one-shot control captured in the same wave. The per-paint maximum
    # above carries the real "the whole decode is not in one paint" evidence.
    if ($null -eq $gap) {
        Fail 'no rAF gap reported by the page'
    } elseif ($null -ne $controlGap -and $controlGap -gt 300 -and $gap -gt $controlGap * 1.5) {
        Fail ("rAF gap {0}ms (one-shot control {1}ms)" -f $gap, $controlGap)
    } elseif (($null -eq $controlGap -or $controlGap -le 300) -and $gap -gt [math]::Max(80, $controlGap / 2)) {
        Fail ("rAF gap {0}ms (one-shot control {1}ms)" -f $gap, $controlGap)
    } else {
        Pass ("rAF gap {0}ms (one-shot control {1}ms)" -f $gap, $controlGap)
    }

    # 2b. L3: decoding at display size must be much cheaper than natural size.
    $scaledMs = (Perf-Paints $res.scaled.Text).Decodes | Measure-Object -Sum
    $naturalMs = (Perf-Paints $res.natural.Text).Decodes | Measure-Object -Sum
    if (-not $scaledMs.Count -or -not $naturalMs.Count) {
        Fail 'L3: no decode timings'
    } elseif ($scaledMs.Sum -gt $naturalMs.Sum / 2.5) {
        Fail ("L3: scaled decode {0:N0}ms is not much cheaper than natural {1:N0}ms" -f $scaledMs.Sum, $naturalMs.Sum)
    } else {
        Pass ("L3: scaled decode {0:N0}ms vs natural {1:N0}ms" -f $scaledMs.Sum, $naturalMs.Sum)
    }

    # 2c. tearing the document down while workers are decoding must not crash
    #     and must not leave jobs stuck.
    $rTd = $res.teardown
    $tdLog = $rTd.Text
    $tdCrash = $tdLog -match '\[crash\]'
    $tdLive = [regex]::Matches($tdLog, 'live=(\d+)') | ForEach-Object { [int]$_.Groups[1].Value }
    if ($rTd.Code -ne 0 -and $rTd.Code -ne 4) {
        Fail ("teardown mid-decode: probe exit {0}" -f $rTd.Code)
    } elseif ($tdCrash) {
        Fail 'teardown mid-decode crashed'
    } elseif ($tdLive.Count -and ($tdLive | Select-Object -Last 1) -ne 0) {
        Fail ("teardown mid-decode left jobs in flight (live trail: {0})" -f ($tdLive -join ','))
    } else {
        Pass 'teardown mid-decode: no crash, decode queue drained'
    }

    # 2d. an image resource that completes with no body at all is a broken
    #     image, not a crash; the valid image next to the broken ones must still
    #     decode, and the async path must not re-request it forever.
    if ($ready) {
        $rEmpty = $res.empty
        $emptyLog = $rEmpty.Text
        $payload = [regex]::Match($emptyLog, '\[exit\] ([^\r\n]*)').Groups[1].Value.Trim()
        $crashFile = Join-Path $rEmpty.Job.Dir 'agb_crash.txt'
        if ($rEmpty.Code -ne 0) {
            Fail ("empty-image: probe exit {0} (payload '{1}')" -f $rEmpty.Code, $payload)
        } elseif ($emptyLog -match '\[crash\]' -or (Test-Path $crashFile)) {
            Fail 'empty-image: crashed on a resource with no body'
        } elseif ($payload -ne 'empty=1/0 nocontent=1/0 truncated=1/0 ok=1/1') {
            Fail ("empty-image: broken images not reported as broken ('{0}')" -f $payload)
        } else {
            $requests = [regex]::Matches($emptyLog, 'requests=(\d+)') |
                ForEach-Object { [int]$_.Groups[1].Value }
            $maxRequests = 0
            if ($requests.Count) { $maxRequests = ($requests | Measure-Object -Maximum).Maximum }
            if ($maxRequests -gt 4) {
                Fail ("empty-image: async decode re-requested {0} times (repaint loop)" -f $maxRequests)
            } else {
                Pass ("empty-image: no-body images are broken, not fatal (async requests={0})" -f $maxRequests)
            }
        }
    }

    # 4. the sampling cap must be live (otherwise the scaled-draw path is not used)
    $hn = Hash (ShotPath 'sample-nearest')
    $he = Hash (ShotPath 'sample-exact')
    if (-not $hn -or -not $he) {
        Fail 'sampling-cap screenshots missing'
    } elseif ($hn -ne $he) {
        Pass 'scaled sampling cap changes the output (path is live)'
    } else {
        Fail 'AGB_IMG_SAMPLE_MAX had no effect - scaled draw not used?'
    }
} finally {
    if ($emptyServer -and -not $emptyServer.HasExited) {
        Stop-Process -Id $emptyServer.Id -Force -ErrorAction SilentlyContinue
    }
    if (-not $KeepArtifacts) { Remove-TestRun $run $failures }
    else { Write-Host ("artifacts kept under {0}" -f $run.Dir) }
}

if ($failures -gt 0) {
    Write-Host "IMAGE=FAIL ($failures)"
    exit 1
}

Write-Host 'IMAGE=PASS'
exit 0
