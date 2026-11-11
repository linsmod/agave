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
# The three probes run at once; expected wall time ~4 s.
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

if (-not (Test-Path $shell)) {
    Write-Host "missing $shell (build first: pwsh -File tools/build_project.ps1)"
    exit 1
}

Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force -DisableNameChecking
$run = New-TestRun -Suite 'media'

$failures = 0

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
$surfaceJob = Start-TestProbe -Run $run -Name 'surface' -Url (Join-Path $pages 'cap_media.html') `
    -Eval 'probeExit(window.reportMedia())' -Wait 900 -Settle 3000 -Timeout 30000

Write-Host '== resource selection (<source> fallback, new Audio)'
$sourceJob = Start-TestProbe -Run $run -Name 'source' -Url (Join-Path $pages 'cap_media_source.html') `
    -Eval 'probeExit(window.reportSource())' -Wait 900 -Settle 3000 -Timeout 30000

$stateJob = $null
if (-not $SkipState) {
    Write-Host '== transport (play -> timeupdate -> ended)'
    # Settle is only the failure bound (a passing run exits via probeExit as
    # soon as the page reports). Under a full parallel batch (24 probes + other
    # suites) the real decoder can start slowly enough that 3 s expired before
    # the page called probeExit, so give it room instead of reporting a timeout
    # as a media defect.
    $stateJob = Start-TestProbe -Run $run -Name 'state' -Url (Join-Path $pages 'cap_media_state.html') `
        -EvalFile (Join-Path $pages 'cap_media_state.js') -Wait 900 -Settle 8000 -Timeout 30000
}

$surface = Complete-TestProbe -Job $surfaceJob
$source = Complete-TestProbe -Job $sourceJob
$state = if ($stateJob) { Complete-TestProbe -Job $stateJob } else { $null }

$result = $surface.Payload
Assert-Contains 'tag names'        $result 'tag=VIDEO/AUDIO'
Assert-Contains 'video default box' $result 'defaultBox=300x150'
Assert-Contains 'mounted box'      $result 'mountedBox=320x180'
Assert-Contains 'offset box'       $result 'offset=320x180'
Assert-Contains 'audio[controls]'  $result 'audioControls=300x54'
Assert-Contains 'audio no controls' $result 'audioNoControls=none'
Assert-Contains 'methods'          $result 'methods=function,function,function,function,function'
Assert-Contains 'canPlayType mp4'  $result 'canPlay=probably|probably|maybe|maybe||'
Assert-Contains 'textTracks'       $result 'tracks=1:subtitles/English/en'

$result = $source.Payload
Assert-Contains 'skips unsupported type' $result 'picked=clip640x360.mp4'
Assert-Contains 'new Audio()'            $result 'newAudio=AUDIO/1'

if (-not $SkipState) {
    $result = $state.Payload
    Assert-Contains 'metadata'     $result 'dur=5.00 dim=640x360'
    Assert-Contains 'play call'    $result 'afterPlayCall: paused=0'
    Assert-Contains 'clock moves'  $result 'readPlayback: later: paused=0'
    Assert-Contains 'timeupdate'   $result 'play,playing,timeupdate'
    Assert-Contains 'seek'         $result 'seeking'
    Assert-Contains 'ended'        $result 'ended=1'
}

Remove-TestRun $run $failures

Write-Host ''
if ($failures -eq 0) {
    Write-Host 'MEDIA=PASS'
    exit 0
}

Write-Host ("MEDIA=FAIL ({0} assertion(s))" -f $failures)
exit 1
