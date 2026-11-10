# tools/TestParallel.psm1
#
# Shared plumbing for the run_*.ps1 suites: every case is one headless probe
# process, cases inside a suite run at the same time, and every case gets its
# own disposable resources.
#
# Why: the suites used to run one probe after another (two seconds of startup
# and --wait per case), and the whole batch serialised the suites that could
# not share the machine. Per-case isolation removes those conflicts:
#
#   AGB_USERDATA        <case>\profile    cookie/cache/localStorage are private
#   working directory   <case>            agb_crash.txt lands here, not in root
#   AGB_DIAG_DIR        <case>\diag       agb_watchdog.txt / agb_busy_*.txt
#   AGB_CLIPBOARD_FILE  <case>\clip.txt   mock clipboard, no Win32 contention
#   AGB_RENDER_TAG      <suite>-<case>    "no orphaned renderer" is per case
#
# Concurrency is bounded across *processes* with a slot directory: suites start
# in parallel as separate pwsh processes, so a PowerShell semaphore would not
# span them, and enumerating agave_shell.exe processes on every start was the
# single largest cost in the refactored suites (Get-Process dominated a
# trivial probe's wall time). A slot file is created atomically (New-Item),
# the directory is counted, and the file is removed when the probe is reaped.
# The slot name carries the owning suite pid, so a killed suite's slots are
# pruned on the next start instead of deadlocking the batch.
#
# Usage (in a suite):
#   Import-Module (Join-Path $PSScriptRoot 'TestParallel.psm1') -Force
#   $run = New-TestRun -Suite 'xhr'
#   $jobs = @(
#       Start-TestProbe -Run $run -Name 'a' -Url $url -EvalFile $f -Wait 1500
#       Start-TestProbe -Run $run -Name 'b' -Url $url2 -Eval 'probeExit(1)'
#   )
#   $results = Complete-TestProbes -Jobs $jobs
#   ...assert on $results[i].Text / .Code...
#   Remove-TestRun $run            # keeps the dir when $script:failures > 0

$script:TestGateMax = 0
$script:TestGateDir = ''
$script:TestGateWarned = $false

function Get-TestGateMax {
    if ($script:TestGateMax -le 0) {
        $n = 0
        [void][int]::TryParse([string]$env:AGB_TEST_MAX_PROBES, [ref]$n)
        # 24 = 1.5 probes per core on the reference machine. Most probes spend
        # their time in fixed --wait sleeps, so a bit of oversubscription raises
        # throughput; the timing probes use load-tolerant comparisons.
        if ($n -le 0) { $n = 24 }
        $script:TestGateMax = [math]::Max(1, [math]::Min(64, $n))
    }
    return $script:TestGateMax
}

function Get-TestRoot {
    param([string]$Root = '')

    if ($Root) { return $Root }
    # tools/TestParallel.psm1 -> repository root
    return (Split-Path -Parent $PSScriptRoot)
}

# A throwaway directory for one suite run. Everything the probes write (profiles,
# logs, screenshots, crash files) lives below it.
function New-TestRun {
    param(
        [Parameter(Mandatory)][string]$Suite,
        [string]$Root = ''
    )

    $root = Get-TestRoot $Root
    $base = Join-Path $root 'build-zig-native\test-runs'
    New-Item -ItemType Directory -Force -Path $base | Out-Null
    $id = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $dir = Join-Path $base ($Suite + '-' + $id)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    # stdout/stderr/eval files go to %TEMP%, not the workspace: the engine's
    # own binaries are in the build tree, but redirecting 2 files per probe
    # into the workspace made a batch of 16 trivial probes take ~2.8 s instead
    # of ~0.6 s (file-system filter / real-time scan). Profiles and other files
    # the probe itself writes must stay in the workspace (its sandbox can only
    # write there).
    $logDir = Join-Path ([IO.Path]::GetTempPath()) ('agb-test-logs\' + $Suite + '-' + $id)
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    return [pscustomobject]@{
        Suite  = $Suite
        Dir    = $dir
        LogDir = $logDir
        Root   = $root
        Gate   = (Join-Path $base '.gate')
    }
}

# Removes the run directory on success; keeps it (and prints the path) when the
# suite failed, so the per-case out/err files survive for diagnosis.
function Remove-TestRun {
    param($Run, [int]$Failures = 0)

    if (-not $Run) { return }
    if ($Failures -gt 0) {
        Write-Host ("logs kept under {0}" -f $Run.Dir)
        if ($Run.LogDir) { Write-Host ("             and {0}" -f $Run.LogDir) }
        return
    }
    try { if ($Run.Dir) { Remove-Item -Recurse -Force $Run.Dir -ErrorAction SilentlyContinue } } catch { }
    try { if ($Run.LogDir) { Remove-Item -Recurse -Force $Run.LogDir -ErrorAction SilentlyContinue } } catch { }
}

function Get-TestFreePort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return [int]$port
}

function Get-TestGateDir {
    param($Run)

    if ($Run -and $Run.Gate) { return $Run.Gate }
    $base = Join-Path (Get-TestRoot) 'build-zig-native\test-runs'
    return (Join-Path $base '.gate')
}

# Removes slots left by a dead probe or a dead suite. A slot is a small file
# named slot-<suite pid>-<guid>; immediately after the spawn its content is the
# probe pid. Tying the slot's life to the process (not to the suite reaching
# Complete-TestProbe) is what keeps the gate deadlock-free: a suite can be
# blocked waiting for the next slot while its finished probe still "holds" one.
function Remove-StaleTestSlots {
    param([string]$Dir)

    try {
        $slots = @(Get-ChildItem $Dir -Filter 'slot-*' -ErrorAction SilentlyContinue)
        if (-not $slots) { return }
        $now = Get-Date
        $alive = @{}
        foreach ($slot in $slots) {
            try {
                $age = ($now - $slot.LastWriteTime).TotalSeconds
                $m = [regex]::Match($slot.Name, '^slot-(\d+)-')
                if ($age -gt 900 -or -not $m.Success) {
                    Remove-Item $slot.FullName -Force -ErrorAction SilentlyContinue
                    continue
                }

                $owner = [int]$m.Groups[1].Value
                if (-not $alive.ContainsKey($owner)) {
                    $alive[$owner] = [bool](Get-Process -Id $owner -ErrorAction SilentlyContinue)
                }
                if (-not $alive[$owner]) {
                    Remove-Item $slot.FullName -Force -ErrorAction SilentlyContinue
                    continue
                }

                $pidText = ''
                $pidText = (Get-Content $slot.FullName -Raw -ErrorAction SilentlyContinue)
                if ($null -eq $pidText) { $pidText = '' }
                $probePid = 0
                if ([int]::TryParse($pidText.Trim(), [ref]$probePid) -and $probePid -gt 0) {
                    if (-not $alive.ContainsKey($probePid)) {
                        $alive[$probePid] = [bool](Get-Process -Id $probePid -ErrorAction SilentlyContinue)
                    }
                    if (-not $alive[$probePid]) {
                        Remove-Item $slot.FullName -Force -ErrorAction SilentlyContinue
                    }
                } elseif ($age -gt 30) {
                    # Slot created but the spawn never wrote a pid (crash
                    # between the two): only give it the benefit of the doubt
                    # briefly.
                    Remove-Item $slot.FullName -Force -ErrorAction SilentlyContinue
                }
            } catch { }
        }
    } catch { }
}

# Waits until fewer than AGB_TEST_MAX_PROBES probes hold a slot. The first call
# in a process also staggers 0-300 ms so a batch of suites does not launch its
# first case in lockstep. Returns the slot path (release it in
# Complete-TestProbe; a killed suite's slots are pruned by the next start).
function Wait-TestProbeSlot {
    param($Run, [int]$TimeoutSeconds = 180)

    if (-not $script:TestGateStaggered) {
        $script:TestGateStaggered = $true
        Start-Sleep -Milliseconds (Get-Random -Maximum 300)
    }

    $dir = Get-TestGateDir $Run
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $max = Get-TestGateMax
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $started = Get-Date
    $lastPrune = $started

    while ($true) {
        $slot = Join-Path $dir ("slot-{0}-{1}" -f $PID, [guid]::NewGuid().ToString('N'))
        try {
            New-Item -ItemType File -Path $slot -ErrorAction SilentlyContinue | Out-Null
            $count = @(Get-ChildItem $dir -Filter 'slot-*' -ErrorAction SilentlyContinue).Count
        } catch {
            return $null
        }
        if ($count -le $max) { return $slot }

        try { Remove-Item $slot -Force -ErrorAction SilentlyContinue } catch { }
        $now = Get-Date
        if ($now -gt $deadline) {
            if (-not $script:TestGateWarned) {
                $script:TestGateWarned = $true
                Write-Host ("WARN  probe gate waited {0}s with {1} slots; continuing" -f $TimeoutSeconds, $count)
            }
            return $null
        }

        # Normal operation releases slots in Complete-TestProbe, so no pruning
        # is needed while waiting. Pruning is for the deadlock shape (a suite
        # blocked here while its finished probes still hold slots) and for
        # killed suites; 1.5 s in, pay for it once.
        if (($now - $started).TotalSeconds -gt 1.5 -and
            ($now - $lastPrune).TotalSeconds -gt 1.5) {
            $lastPrune = $now
            Remove-StaleTestSlots $dir
        }
        Start-Sleep -Milliseconds (25 + (Get-Random -Maximum 40))
    }
}

function Release-TestProbeSlot {
    param([string]$Slot)

    if ($Slot) {
        try { Remove-Item $Slot -Force -ErrorAction SilentlyContinue } catch { }
    }
}

function Set-TestEnvValue {
    param([string]$Name, $Value, $Saved)

    if (-not $Saved.ContainsKey($Name)) {
        $Saved[$Name] = [Environment]::GetEnvironmentVariable($Name)
    }
    if ($null -eq $Value) {
        Remove-Item "Env:$Name" -ErrorAction SilentlyContinue
    } else {
        Set-Item -Path "Env:$Name" -Value $Value
    }
}

function Restore-TestEnvValue {
    param([string]$Name, $Saved)

    if ($Saved.ContainsKey($Name) -and $null -ne $Saved[$Name]) {
        Set-Item -Path "Env:$Name" -Value $Saved[$Name]
    } else {
        Remove-Item "Env:$Name" -ErrorAction SilentlyContinue
    }
}

function Get-TestClipboard {
    param($Job)

    if ($Job.ClipFile -and (Test-Path $Job.ClipFile)) {
        return [IO.File]::ReadAllText($Job.ClipFile)
    }
    return ''
}

# Windows command-line quoting for Start-Process. PowerShell's -ArgumentList
# joins an array with spaces without quoting, so an argument containing a space
# would be split (the page URL then swallowed the tail). The shell parses its
# own command line with plain double-quote tokens and does NOT understand
# backslash escapes, so this only wraps whitespace arguments in quotes -
# scripts must therefore use single-quoted JS (or, better, -Eval, which the
# helper writes to a file and passes as --eval-file).
function ConvertTo-TestCommandLine {
    # Not $Args: that is PowerShell's automatic argument array.
    param([string[]]$ArgList)

    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($raw in $ArgList) {
        $s = [string]$raw
        if ($s.Length -eq 0) { $parts.Add('""'); continue }
        if ($s -match '[\s"]') { $parts.Add('"' + $s + '"'); continue }
        $parts.Add($s)
    }
    return ($parts -join ' ')
}

# Starts one probe and returns immediately. -RawArgs wins over the Url/Eval
# builder (a few cases need a flag the builder does not know about).
function Start-TestProbe {
    param(
        [Parameter(Mandatory)]$Run,
        [Parameter(Mandatory)][string]$Name,
        [string]$Url = '',
        [string]$Eval = '',
        [string]$EvalFile = '',
        [string[]]$Extra = @(),
        [string[]]$RawArgs = @(),
        [int]$Wait = 300,
        [int]$Settle = 300,
        [int]$Timeout = 30000,
        [string]$Backend = '',
        [hashtable]$Env = @{},
        [string]$ClipboardValue = $null,
        [string]$Exe = '',
        [string]$RenderTag = ''
    )

    if (-not $Exe) {
        $Exe = Join-Path $Run.Root 'build-zig-native\agave_shell.exe'
    }

    $safe = ($Name -replace '[^A-Za-z0-9_.-]', '_')
    $dir = Join-Path $Run.Dir $safe
    $n = 1
    while (Test-Path $dir) {
        $n++
        $dir = Join-Path $Run.Dir ($safe + '-' + $n)
    }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $caseName = Split-Path -Leaf $dir
    $logDir = Join-Path $Run.LogDir $caseName
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    $profile = Join-Path $dir 'profile'
    $diag = Join-Path $dir 'diag'
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    New-Item -ItemType Directory -Force -Path $diag | Out-Null

    # Logs go to %TEMP% (see New-TestRun); the probe only reads the eval file.
    $out = Join-Path $logDir 'out.txt'
    $err = Join-Path $logDir 'err.txt'
    $clip = Join-Path $dir 'clipboard.txt'
    if ($null -ne $ClipboardValue) {
        [IO.File]::WriteAllText($clip, $ClipboardValue, (New-Object System.Text.UTF8Encoding($false)))
    }

    if ($RawArgs.Count -gt 0) {
        $argv = @($RawArgs)
    } else {
        if ($Eval) {
            # Command-line arguments are mangled by the shell's own simple
            # parser as soon as the script contains spaces/quotes; a file is
            # exact and also keeps very long scripts off the command line.
            $EvalFile = Join-Path $logDir 'eval.js'
            Set-Content -Path $EvalFile -Value $Eval -Encoding utf8
        }
        $argv = @('--probe', $Url, '--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout")
        if ($EvalFile) { $argv += @('--eval-file', $EvalFile) }
        if ($Extra.Count -gt 0) { $argv += $Extra }
    }

    $saved = @{}
    try {
        Set-TestEnvValue 'AGB_USERDATA' $profile $saved
        Set-TestEnvValue 'AGB_DIAG_DIR' $diag $saved
        Set-TestEnvValue 'AGB_CLIPBOARD_FILE' $clip $saved
        Set-TestEnvValue 'AGB_VIEW_BACKEND' $(if ($Backend) { $Backend } else { $null }) $saved
        if ($RenderTag) { Set-TestEnvValue 'AGB_RENDER_TAG' $RenderTag $saved }
        foreach ($key in $Env.Keys) {
            Set-TestEnvValue $key $Env[$key] $saved
        }

        $slot = Wait-TestProbeSlot $Run
        try {
            $cmdline = ConvertTo-TestCommandLine $argv
            $proc = Start-Process -FilePath $Exe -NoNewWindow -PassThru `
                -WorkingDirectory $dir -ArgumentList $cmdline `
                -RedirectStandardOutput $out -RedirectStandardError $err
            # The slot lives only as long as this pid (Remove-StaleTestSlots).
            if ($slot) { Set-Content -Path $slot -Value $proc.Id -Encoding ascii }
        } catch {
            Release-TestProbeSlot $slot
            throw
        }
    } finally {
        foreach ($key in @($saved.Keys)) { Restore-TestEnvValue $key $saved }
    }

    return [pscustomobject]@{
        Name    = $Name
        Dir     = $dir
        Profile = $profile
        Diag    = $diag
        ClipFile = $clip
        Out     = $out
        Err     = $err
        Proc    = $proc
        Slot    = $slot
        Started = [Diagnostics.Stopwatch]::StartNew()
    }
}

function Complete-TestProbe {
    param($Job, [int]$HardTimeoutSeconds = 90)

    if (-not $Job) { return $null }

    $deadline = (Get-Date).AddSeconds($HardTimeoutSeconds)
    while (-not $Job.Proc.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 20
    }

    $timedOut = $false
    if (-not $Job.Proc.HasExited) {
        $timedOut = $true
        try { $Job.Proc.Kill() } catch { }
        try { [void]$Job.Proc.WaitForExit(3000) } catch { }
    }

    $Job.Started.Stop()
    Release-TestProbeSlot $Job.Slot

    # Real process wall time. Started/Stopped spans from spawn to *reaping*,
    # which can be seconds later when the suite starts every probe first and
    # waits for them afterwards - the "interrupt aborts in <10 s" style
    # assertions must see the probe's own runtime, not the queue delay.
    $realSeconds = $Job.Started.Elapsed.TotalSeconds
    try {
        $realSeconds = ($Job.Proc.ExitTime - $Job.Proc.StartTime).TotalSeconds
    } catch { }

    $code = -1
    try { $code = $Job.Proc.ExitCode } catch { }

    $stdout = ''
    $stderr = ''
    if (Test-Path $Job.Out) { $stdout = [string](Get-Content $Job.Out -Raw -ErrorAction SilentlyContinue) }
    if (Test-Path $Job.Err) { $stderr = [string](Get-Content $Job.Err -Raw -ErrorAction SilentlyContinue) }

    # The probe's own answer: "[exit] <payload>" on stdout. probeExit payloads
    # are usually one line; keep the whole tail for the multi-line ones.
    $payload = ''
    $m = [regex]::Match($stdout, '\[exit\] ?(.*)')
    if ($m.Success) { $payload = $m.Groups[1].Value.TrimEnd("`r") }

    return [pscustomobject]@{
        Name    = $Job.Name
        Payload = $payload
        Job     = $Job
        Dir     = $Job.Dir
        Diag    = $Job.Diag
        ClipFile = $Job.ClipFile
        Code    = $code
        Stdout  = $stdout
        Stderr  = $stderr
        Text    = ($stdout + $stderr)
        TimedOut = $timedOut
        Seconds = [math]::Round($realSeconds, 1)
    }
}

# Waits for every job (they were started together) and returns the results in
# the same order as -Jobs, so callers can index them. One shared wait loop:
# waiting job-by-job added up to one poll interval per completed probe.
function Complete-TestProbes {
    param([object[]]$Jobs, [int]$HardTimeoutSeconds = 90)

    $deadline = (Get-Date).AddSeconds($HardTimeoutSeconds)
    while ($true) {
        $running = 0
        foreach ($job in $Jobs) {
            if ($job -and -not $job.Proc.HasExited) { $running++ }
        }
        if ($running -eq 0 -or (Get-Date) -gt $deadline) { break }
        Start-Sleep -Milliseconds 20
    }

    $results = @()
    foreach ($job in $Jobs) {
        $results += Complete-TestProbe -Job $job -HardTimeoutSeconds 2
    }
    return $results
}

# Convenience for a one-probe case: start, wait, and return the result.
function Invoke-TestProbe {
    param(
        [Parameter(Mandatory)]$Run,
        [Parameter(Mandatory)][string]$Name,
        [string]$Url = '',
        [string]$Eval = '',
        [string]$EvalFile = '',
        [string[]]$Extra = @(),
        [string[]]$RawArgs = @(),
        [int]$Wait = 300,
        [int]$Settle = 300,
        [int]$Timeout = 30000,
        [string]$Backend = '',
        [hashtable]$Env = @{},
        [string]$ClipboardValue = $null,
        [string]$Exe = '',
        [int]$HardTimeoutSeconds = 90
    )

    $job = Start-TestProbe -Run $Run -Name $Name -Url $Url -Eval $Eval -EvalFile $EvalFile `
        -Extra $Extra -RawArgs $RawArgs -Wait $Wait -Settle $Settle -Timeout $Timeout `
        -Backend $Backend -Env $Env -ClipboardValue $ClipboardValue -Exe $Exe
    return Complete-TestProbe -Job $job -HardTimeoutSeconds $HardTimeoutSeconds
}

Export-ModuleMember -Function *
