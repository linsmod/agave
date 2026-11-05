<#
.SYNOPSIS
  Capture a symbolized stack from agave_shell, or symbolize an existing
  "[shell] atexit" dump. One command instead of hand-building cdb arguments.

.DESCRIPTION
  Three modes:

    -Url ...            run the probe under cdb, break on the first AV /
                        fail-fast, dump kb + all threads + lm, save a log.
    -Attach <pid>       dump all threads of a running process (for hangs).
    -Hang -Url ...      start the probe, wait -HangAfter seconds, attach,
                        dump, then kill it.
    -Symbolize <log>    map "#NN exe|libagave|antjsb+0x..." frames from a
                        saved log (e.g. the atexit dump) to function/file:line.

  Symbol path is built from build-zig-native + H:\ant-v16\build-zig automatically.
  antjsb is always built -O2 -g (release + debug info) and its PDB ships
  next to the DLL; -DebugBuild forces a rebuild of it.

  When the run ends through exit(), the shell prints its own atexit stack; this
  script detects it in the log and symbolizes it automatically.

.EXAMPLE
  pwsh -File tools/cdb_stack.ps1 -Url https://chat.deepseek.com/sign_in `
       -ScriptFile debug/done.js -Wait 3000

.EXAMPLE
  pwsh -File tools/cdb_stack.ps1 -Url https://chat.deepseek.com/sign_in `
       -ScriptFile debug/done.js -DebugBuild

.EXAMPLE
  pwsh -File tools/cdb_stack.ps1 -Hang -Url https://chat.deepseek.com/sign_in -HangAfter 8

.EXAMPLE
  pwsh -File tools/cdb_stack.ps1 -Attach 12345

.EXAMPLE
  pwsh -File tools/cdb_stack.ps1 -Symbolize debug/wxstyle.out
#>
param(
    [string]$Url,
    [string]$Script,
    [string]$ScriptFile,
    [int]$Wait = 3000,
    [int]$Settle = 1000,
    [int]$Timeout = 60000,
    [int]$Attach = 0,
    [switch]$Hang,
    [int]$HangAfter = 10,
    [string]$Symbolize,
    [string]$Log,
    [switch]$DebugBuild,
    [string]$Exe,
    [string[]]$AppArgs,
    [int]$Frames = 60
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$cdb     = Join-Path $root 'tools\cdb.exe'
$build   = Join-Path $root 'build-zig-native'
$default = Join-Path $build 'agave_shell.exe'
$symDirs = @($build, 'H:\ant-v16\build-zig')
$symPath = ($symDirs -join ';')
$stamp   = Get-Date -Format 'yyyyMMdd_HHmmss'
$debugDir = Join-Path $root 'debug'
if (-not (Test-Path $debugDir)) { New-Item -ItemType Directory -Path $debugDir | Out-Null }
if (-not $Log) { $Log = Join-Path $debugDir ("cdb_stack_{0}.log" -f $stamp) }
$Log = [IO.Path]::GetFullPath($Log)
$logDir = Split-Path -Parent $Log
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

function Save-Log {
    param([string[]]$Lines, [string]$Path)
    $Lines | Set-Content -Path $Path -Encoding utf8
}

function New-CmdFile {
    param([string[]]$Lines)
    $p = Join-Path $debugDir ("cdb_cmds_{0}_{1}.txt" -f $stamp, [guid]::NewGuid().ToString('N').Substring(0, 6))
    $Lines | Set-Content -Path $p -Encoding utf8
    return $p
}

function Show-SymbolStatus {
    param([string[]]$Lines)
    $mods = 'antjsb|libagave|agave_shell'
    $interesting = $Lines | Where-Object { $_ -match "^\S+\s+\S+\s+($mods)\s" }
    foreach ($l in $interesting) {
        if ($l -match '\s(antjsb|libagave|agave_shell)\s+.*\(private pdb symbols\)') {
            Write-Host ("  [sym] " + $l.Trim())
        } elseif ($l -match '\s(antjsb|libagave|agave_shell)\s') {
            Write-Host ("  [sym] " + $l.Trim()) -ForegroundColor Yellow
        }
    }
    $ant = $interesting | Where-Object { $_ -match '\santjsb\s' }
    if ($ant -and -not ($ant | Where-Object { $_ -match '\santjsb\s+.*private pdb symbols' })) {
        Write-Host "  [!] antjsb has no PDB: its frames will stay module+offset." -ForegroundColor Yellow
        Write-Host "      rebuild: pwsh -File tools/build_antjsb.ps1 -Force   (or -DebugBuild)" -ForegroundColor Yellow
    }
}

function Invoke-AtexitSymbolize {
    param([string]$LogPath)
    $txt = Get-Content -Raw -Path $LogPath
    if ($txt -notmatch 'atexit fired') { return }
    Write-Host ""
    Write-Host "== atexit stack (from the log) =="
    Invoke-Symbolize -LogPath $LogPath
}

function Invoke-Symbolize {
    param([string]$LogPath)

    $text = Get-Content -Raw -Path $LogPath
    $frames = [regex]::Matches($text, '(?m)^\s*#(\d+)\s+(\?|[A-Za-z0-9_]+)\+0x([0-9a-fA-F]+)\s*$')
    if ($frames.Count -eq 0) {
        Write-Host "no '#NN module+0x...' frames found in $LogPath"
        return
    }

    $modules = @{
        'exe'       = @{ File = 'agave_shell.exe'; Name = 'agave_shell' }
        'libagave'  = @{ File = 'libagave.dll';    Name = 'libagave' }
        'antjsb' = @{ File = 'antjsb.dll';   Name = 'antjsb' }
    }

    $groups = @{}
    $meta = @()
    foreach ($m in $frames) {
        $idx = [int]$m.Groups[1].Value
        $mod = $m.Groups[2].Value
        $off = $m.Groups[3].Value
        $meta += [pscustomobject]@{ Index = $idx; Mod = $mod; Off = $off }
        if (-not $modules.ContainsKey($mod)) { continue }
        if (-not $groups.ContainsKey($mod)) { $groups[$mod] = @() }
        $groups[$mod] += [pscustomobject]@{ Index = $idx; Off = $off }
    }

    $resolved = @{}
    foreach ($mod in $groups.Keys) {
        $info = $modules[$mod]
        $file = Join-Path $build $info.File
        if (-not (Test-Path $file)) { continue }

        $cmd = @('.lines')
        foreach ($d in $symDirs) { $cmd += ".sympath+ `"$d`"" }
        $n = 0
        foreach ($f in $groups[$mod]) {
            $cmd += (".echo @@{0}" -f $f.Index)
            $cmd += ("ln {0}+0x{1}" -f $info.Name, $f.Off)
            $cmd += ("u {0}+0x{1} L1" -f $info.Name, $f.Off)
        }
        $cmd += 'q'
        $cf = New-CmdFile $cmd
        $out = & $cdb -y $symPath -z $file -cf $cf 2>&1 | ForEach-Object { "$_" }
        Remove-Item $cf -ErrorAction SilentlyContinue

        $current = -1
        foreach ($line in $out) {
            if ($line -match '^@@(\d+)') { $current = [int]$Matches[1]; continue }
            if ($current -lt 0) { continue }
            if ($line -match '^\s*(?:\([0-9a-f`]+\)\s+)?[A-Za-z0-9_]+!\s*([^+\s\[\]]+)\+0x([0-9a-fA-F]+)\s*(?:\[(.+?) @ (\d+)\])?') {
                $sym = $Matches[1]
                $src = $null
                if ($Matches[3]) { $src = "{0}:{1}" -f $Matches[3], $Matches[4] }
                if (-not $resolved.ContainsKey($current) -or $Matches[3]) {
                    $resolved[$current] = [pscustomobject]@{ Sym = $sym; Src = $src }
                }
            }
        }
    }

    Write-Host ("{0,-5} {1,-22} {2}" -f '#', 'frame', 'symbol')
    foreach ($m in $meta | Sort-Object Index) {
        $tag = "#{0:D2}" -f $m.Index
        $frame = "$($m.Mod)+0x$($m.Off)"
        if ($resolved.ContainsKey($m.Index)) {
            $r = $resolved[$m.Index]
            $sym = "$($m.Mod)!$($r.Sym)"
            if ($r.Src) { $sym += "  [$($r.Src)]" }
            Write-Host ("{0,-5} {1,-22} {2}" -f $tag, $frame, $sym)
        } elseif ($modules.ContainsKey($m.Mod)) {
            Write-Host ("{0,-5} {1,-22} ? (no symbol; PDB missing/mismatched)" -f $tag, $frame) -ForegroundColor DarkYellow
        } else {
            Write-Host ("{0,-5} {1,-22} ? (unknown module)" -f $tag, $frame) -ForegroundColor DarkGray
        }
    }
    Write-Host ""
}

function Invoke-Run {
    param([string]$ExePath, [string[]]$CliArgs)

    $cmd = @('.lines')
    foreach ($d in $symDirs) { $cmd += ".sympath+ `"$d`"" }
    $cmd += '.reload'
    $h = ".echo ===FAULT===;.lastevent;.exr -1;r;kb $Frames;~*kb 30;lm;q"
    $cmd += ('sxe -c "{0}" av' -f $h)
    $cmd += ('sxe -c "{0}" 0xc0000409' -f $h)
    $cmd += ('sxe -c "{0}" 0xc0000374' -f $h)
    $cmd += ('sxe -c "{0}" 0x80000003' -f $h)
    $cmd += 'g'
    $cmd += 'q'
    $cf = New-CmdFile $cmd

    Write-Host "== cdb run =="
    Write-Host "  exe : $ExePath"
    Write-Host "  sym : $symPath"
    Write-Host "  log : $Log"
    $out = & $cdb -y $symPath -cf $cf $ExePath @CliArgs 2>&1 | ForEach-Object { "$_" }
    Remove-Item $cf -ErrorAction SilentlyContinue
    Save-Log $out $Log

    $text = $out -join "`n"
    $fault = $out | Select-String -Pattern '^===FAULT===\s*$' | Select-Object -First 1
    if ($fault) {
        Write-Host "== fault captured =="
        $i = $fault.LineNumber - 1
        $end = [Math]::Min($out.Count - 1, $i + 120)
        $out[$i..$end] | ForEach-Object { Write-Host $_ }
        Write-Host ""
        Write-Host "== symbols =="
        Show-SymbolStatus $out
    } else {
        Write-Host "== no exception (process exited or timed out) ==" -ForegroundColor Yellow
        $tail = [Math]::Max(0, $out.Count - 25)
        $out[$tail..($out.Count - 1)] | ForEach-Object { Write-Host $_ }
        Write-Host "  for a hang use: -Attach <pid>  or  -Hang -Url <url> -HangAfter N" -ForegroundColor Yellow
    }
    Invoke-AtexitSymbolize $Log
    Write-Host ""
    Write-Host "log: $Log"
}

function Invoke-Attach {
    param([int]$TargetPid)
    $cmd = ".lines; ~*kb $Frames; lm; qd"
    Write-Host "== attach pid $TargetPid =="
    Write-Host "  log : $Log"
    $out = & $cdb -y $symPath -p $TargetPid -c $cmd 2>&1 | ForEach-Object { "$_" }
    Save-Log $out $Log
    $out | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Show-SymbolStatus $out
    Invoke-AtexitSymbolize $Log
    Write-Host "log: $Log"
}

function Invoke-Hang {
    param([string]$ExePath, [string[]]$CliArgs)
    $procLog = Join-Path $debugDir ("probe_{0}.out" -f $stamp)
    $procErr = Join-Path $debugDir ("probe_{0}.err" -f $stamp)
    Write-Host "== start probe, attach after $HangAfter s =="
    $argLine = ($CliArgs | ForEach-Object { Quote-Arg $_ }) -join ' '
    $p = Start-Process -FilePath $ExePath -ArgumentList $argLine -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $procLog -RedirectStandardError $procErr
    Start-Sleep -Seconds $HangAfter
    if ($p.HasExited) {
        Write-Host "process exited before attach (code $($p.ExitCode)); see $procErr" -ForegroundColor Yellow
        return
    }
    try {
        Invoke-Attach -TargetPid $p.Id
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    }
}

function Quote-Arg {
    param([string]$s)
    if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' }
    return $s
}

# ------------------------------------------------------------------ main
if (-not (Test-Path $cdb)) { throw "missing $cdb" }

if ($Symbolize) {
    if (-not (Test-Path $Symbolize)) { throw "missing log $Symbolize" }
    Invoke-Symbolize -LogPath ([IO.Path]::GetFullPath($Symbolize))
    Write-Host "log: $([IO.Path]::GetFullPath($Symbolize))"
    return
}

if ($DebugBuild) {
    Write-Host "== build antjsb (release + debug info, antjsb.pdb) =="
    & pwsh -NoProfile -File (Join-Path $root 'tools\build_antjsb.ps1')
    if ($LASTEXITCODE -ne 0) { throw "build_antjsb failed" }
}

$exe = if ($Exe) { [IO.Path]::GetFullPath($Exe) } else { $default }
if (-not (Test-Path $exe)) { throw "missing $exe" }

if ($Attach -gt 0) {
    Invoke-Attach -TargetPid $Attach
    return
}

if (-not $Url -and -not $Exe) { throw "need -Url (run), -Attach <pid>, -Symbolize <log>" }

$cli = @()
if ($AppArgs) {
    $cli = $AppArgs
} else {
    $cli += @('--probe', $Url)
    if ($ScriptFile) {
        if (-not (Test-Path $ScriptFile)) { throw "missing script file $ScriptFile" }
        $cli += @('--eval-file', ([IO.Path]::GetFullPath($ScriptFile)))
    } elseif ($Script) {
        $cli += @('--eval', $Script)
    }
    $cli += @('--wait', "$Wait", '--settle', "$Settle", '--timeout', "$Timeout")
}

if ($Hang) {
    Invoke-Hang -ExePath $exe -CliArgs $cli
} else {
    Invoke-Run -ExePath $exe -CliArgs $cli
}
