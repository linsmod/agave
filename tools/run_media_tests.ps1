# tools/run_media_tests.ps1
#
# Media element regression: DOM/API surface, geometry against Chrome's UA
# layout, and the transport (play -> timeupdate -> ended).
#
# The transport assertions hold for both paths: with ffmpeg installed the
# element is driven by the real decoder (see run_media_real_tests.ps1), and
# only when it is missing does the internal ticker simulate the clock. The
# assertions therefore never depend on which one ran.
#
# The reference values are Chrome's, taken with tools/ua_style_dump.py
# (see docs/arch-media.md section 3): <video> defaults to 300x150,
# <audio> without controls is display:none, <audio controls> is 300x54.
#
# Usage:
#   pwsh -File tools/run_media_tests.ps1            # run everything
#   pwsh -File tools/run_media_tests.ps1 -SkipState # skip the timing test
#
# Exit code 0 when every assertion passes.

param(
    [switch]$SkipState
)

$ErrorActionPreference = 'Stop'

$root   = Split-Path -Parent $PSScriptRoot
$shell  = Join-Path $root 'build-zig-native\agave_shell.exe'
$pages  = Join-Path $root 'tools\pages'
$work   = Join-Path $root 'debug'

if (-not (Test-Path $shell)) {
    Write-Host "missing $shell (build first: pwsh -File tools/build_project.ps1)"
    exit 1
}

# The headless probe writes its BMP / scratch next to the exe when the target
# directory lacks the AppContainer write ACE; keep our own scratch inside the
# build tree, which always has it.
if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work | Out-Null }

$failures = 0

function Invoke-Probe {
    param([string]$Url, [string]$Script, [string]$EvalArgs)

    $args = @('--probe', $Url, '--wait', '900')

    if ($Script) { $args += @('--eval-file', $Script) }
    if ($EvalArgs) { $args += @('--eval', $EvalArgs) }

    $args += @('--settle', '3000')

    $output = & $shell @args 2>&1 | Out-String
    $line = ($output -split "`r?`n" | Where-Object { $_ -match '\[exit\]' } | Select-Object -First 1)

    if (-not $line) { return $null }
    return ($line -replace '^.*\[exit\]\s*', '')
}

function Assert-Contains {
    param([string]$Name, [string]$Actual, [string]$Expected)

    if ($null -eq $Actual) {
        Write-Host ("  FAIL {0}: probe produced no [exit] line" -f $Name)
        $script:failures++
        return
    }

    if ($Actual -match [regex]::Escape($Expected)) {
        Write-Host ("  ok   {0} ({1})" -f $Name, $Expected)
    } else {
        Write-Host ("  FAIL {0}: expected '{1}' in:" -f $Name, $Expected)
        Write-Host ("       {0}" -f $Actual)
        $script:failures++
    }
}

Write-Host '== media element surface + geometry (Chrome-parity)'
$result = Invoke-Probe -Url (Join-Path $pages 'cap_media.html') -EvalArgs 'probeExit(window.reportMedia())'

Assert-Contains 'tag names'        $result 'tag=VIDEO/AUDIO'
Assert-Contains 'video default box' $result 'defaultBox=300x150'
Assert-Contains 'mounted box'      $result 'mountedBox=320x180'
Assert-Contains 'offset box'       $result 'offset=320x180'
Assert-Contains 'audio[controls]'  $result 'audioControls=300x54'
Assert-Contains 'audio no controls' $result 'audioNoControls=none'
Assert-Contains 'methods'          $result 'methods=function,function,function,function,function'
Assert-Contains 'canPlayType mp4'  $result 'canPlay=probably|probably|maybe|maybe||'
Assert-Contains 'textTracks'       $result 'tracks=1:subtitles/English/en'

Write-Host '== resource selection (<source> fallback, new Audio)'
$result = Invoke-Probe -Url (Join-Path $pages 'cap_media_source.html') -EvalArgs 'probeExit(window.reportSource())'
Assert-Contains 'skips unsupported type' $result 'picked=clip640x360.mp4'
Assert-Contains 'new Audio()'            $result 'newAudio=AUDIO/1'

if (-not $SkipState) {
    Write-Host '== transport (play -> timeupdate -> ended)'
    $state = Invoke-Probe -Url (Join-Path $pages 'cap_media_state.html') `
                          -Script (Join-Path $pages 'cap_media_state.js')

    Assert-Contains 'metadata'     $state 'dur=5.00 dim=640x360'
    Assert-Contains 'play call'    $state 'afterPlayCall: paused=0'
    Assert-Contains 'clock moves'  $state 'readPlayback: later: paused=0'
    Assert-Contains 'timeupdate'   $state 'play,playing,timeupdate'
    Assert-Contains 'seek'         $state 'seeking'
    Assert-Contains 'ended'        $state 'ended=1'
}

Write-Host ''
if ($failures -eq 0) {
    Write-Host 'MEDIA=PASS'
    exit 0
}

Write-Host ("MEDIA=FAIL ({0} assertion(s))" -f $failures)
exit 1
