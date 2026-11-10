<#
.SYNOPSIS
  Regenerate (or verify) the WebCore IDL bindings for one code generator.

.DESCRIPTION
  WebCore's bindings are generated from the *.idl files under src/webcore by
  bindings/scripts/generate-bindings.pl + CodeGenerator<Backend>.pm. The outputs
  are signed in (buildKJS/ = JS backend, buildQJS/ = QJS backend) and compiled
  directly, exactly like the CSS grammar and the UA stylesheets: the build never
  runs perl. This script is the reproducible entry point for that step.

  It needs perl (MSYS2's is used when present) and nothing else. Generation is
  deterministic: with the same defines the output must be byte-identical to what
  is signed in, which is what -Compare checks. That check is the regression gate
  for any change to a generator, an IDL or the defines list - and the baseline
  the ant backend (CodeGeneratorAnt) has to hit before its output is signed in.

  The defines list is the one WebKit 2008 shipped: SVG, database, video, workers,
  datalist and filters on, XPath off (XPath has hand-written bindings in
  bindings/js|qjs; enabling it generates an extra file the signed-in tree does
  not have).

  Backends: QJS is the reproducible one (483 of the 490 signed-in files are
  byte-identical; the other 7 are historical mixed-config artifacts, see
  docs/ref-code-index.md). JS is the dead KJS-era backend: 484 files match, but
  page/DOMWindow.idl cannot be generated any more because CodeGeneratorJS.pm
  still expects LegacyParent=KJS::Window. ANT is the one this tool exists for
  (A1 of issues/wip/ant-dom-bindings.md).

  One perl process per IDL, run in parallel: the generator keeps per-run state in
  package variables, so interfaces are not batched, and a full run is ~95 s on a
  16-core box instead of ~12 min serially.

.EXAMPLE
  pwsh -File tools/gen_bindings.ps1 -Generator QJS -Compare
  # regenerate every QJS binding into %TEMP% and prove it matches buildQJS/

.EXAMPLE
  pwsh -File tools/gen_bindings.ps1 -Generator QJS -Compare -Strict -Skip QJSDOMParser.h,QJSDOMWindow.h,QJSDOMWindow.cpp,QJSHTMLEmbedElement.h,QJSHTMLEmbedElement.cpp,QJSHTMLObjectElement.h,QJSHTMLObjectElement.cpp
  # the gate: 0 differences once the known-historical files are excluded

.EXAMPLE
  pwsh -File tools/gen_bindings.ps1 -Generator ANT -OutDir src/webcore/buildANT
  # generate the ant backend's bindings into the signed-in directory

.EXAMPLE
  pwsh -File tools/gen_bindings.ps1 -Generator QJS -Idl Node.idl -OutDir $env:TEMP\gen
  # one interface only, for reading the generator output

.EXAMPLE
  pwsh -File tools/gen_bindings.ps1 -Generator QJS -Idl svg -Throttle 4 -Verbose
  # a slice of the tree, one line per interface
#>
param(
    # Backend = CodeGenerator<Generator>.pm. ANT needs
    # src/webcore/bindings/scripts/CodeGeneratorAnt.pm (A1 of
    # issues/wip/ant-dom-bindings.md); until it exists this script refuses.
    [ValidateSet('QJS', 'JS', 'ANT')]
    [string]$Generator = 'QJS',

    # Where the generated files go. Default: a temporary directory (nothing is
    # written into the checkout unless -OutDir says so).
    [string]$OutDir = '',

    # Compare against the signed-in build<Generator>/ (the reproducibility gate).
    [switch]$Compare,

    # Compare against this directory instead of the signed-in build<Generator>/.
    [string]$CompareDir = '',

    # Fail the run when a generated file differs from the signed-in one. Without
    # it, differences are reported but only a generation failure exits non-zero.
    # Use it once a backend's output is signed in and the known-historical files
    # are listed in -Skip.
    [switch]$Strict,

    # File names to leave out of the comparison (known-historical artifacts).
    [string[]]$Skip = @(),

    # Substring filter over the .idl path (e.g. 'Node.idl', 'svg', 'html/').
    [string]$Idl = '',

    # IDL #define list passed to the generator (WebKit 2008 shipped set).
    [string]$Defines = '',

    # Concurrent perl processes (each IDL is independent).
    [int]$Throttle = 0,

    # Print one line per generated interface instead of only the summary.
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$scripts = Join-Path $root 'src\webcore\bindings\scripts'
$idlRoot = Join-Path $root 'src\webcore'

if (-not $Defines) {
    $Defines = 'ENABLE_SVG=1 ENABLE_SVG_EXPERIMENTAL_FEATURES=1 ENABLE_DATABASE=1 ' +
               'ENABLE_VIDEO=1 ENABLE_WORKERS=1 ENABLE_DATALIST=1 ENABLE_FILTERS=1 ' +
               'LANGUAGE_JAVASCRIPT=1'
}
if ($Throttle -le 0) {
    $Throttle = [Math]::Min(16, [Math]::Max(2, [Environment]::ProcessorCount))
}

# perl: the MSYS2 one first (the generator uses forward-slash paths and POSIX
# behaviour MSYS2 has by default), then whatever is on PATH.
$perl = 'C:\msys64\usr\bin\perl.exe'
if (-not (Test-Path $perl)) {
    $cmd = Get-Command perl -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw 'perl not found: install MSYS2 (C:\msys64\usr\bin\perl.exe) or put perl on PATH'
    }
    $perl = $cmd.Source
}

# CodeGenerator::ScanDirectory honours SOURCE_ROOT and would then look for IDLs
# somewhere else than the checkout this script was started from.
Remove-Item Env:SOURCE_ROOT -ErrorAction SilentlyContinue

$generatorModule = Join-Path $scripts "CodeGenerator$Generator.pm"
if (-not (Test-Path $generatorModule)) {
    throw "CodeGenerator$Generator.pm is missing from $scripts (the $Generator backend is not implemented yet)"
}

if (-not $OutDir) {
    $OutDir = Join-Path ([System.IO.Path]::GetTempPath()) "agb-bindings-$($Generator.ToLower())"
}
if ($Compare -and -not $CompareDir) {
    # The signed-in directory names follow the historical backends: the JS
    # (KJS-era) output is buildKJS/, QJS is buildQJS/, ant will be buildANT/.
    $CompareDir = switch ($Generator) { "JS" { "buildKJS" } default { "build$Generator" } }
}
if ($CompareDir -and -not [System.IO.Path]::IsPathRooted($CompareDir)) {
    $CompareDir = Join-Path (Join-Path $root 'src\webcore') $CompareDir
}

$outFull = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $outFull | Out-Null
$cmpFull = if ($CompareDir) { [System.IO.Path]::GetFullPath($CompareDir) } else { '' }

$idlFiles = @(Get-ChildItem -Recurse -Path $idlRoot -Filter *.idl | Sort-Object FullName)
if ($Idl) {
    $idlFiles = @($idlFiles | Where-Object { $_.FullName.Replace('\', '/') -like "*$Idl*" })
}
if ($idlFiles.Count -eq 0) {
    throw "no .idl file matched '$Idl' under $idlRoot"
}

$scriptsSlash = $scripts.Replace('\', '/')
$idlRootSlash = $idlRoot.Replace('\', '/')
$outSlash = $outFull.Replace('\', '/')

Write-Host "generator : $Generator ($(Split-Path -Leaf $generatorModule))"
Write-Host "perl      : $perl"
Write-Host "idl files : $($idlFiles.Count)"
Write-Host "out       : $outFull"
if ($cmpFull) { Write-Host "compare   : $cmpFull" }
Write-Host "workers   : $Throttle"

# -Skip is documented as a comma list: `pwsh -File` hands over one string per
# parameter, so split it here (`-Skip A.h,B.cpp`).
$skipNames = @()
foreach ($s in $Skip) {
    foreach ($part in ([string]$s).Split(',')) {
        $name = $part.Trim()
        if ($name) { $skipNames += $name }
    }
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()

# One perl process per IDL. generate-bindings.pl takes exactly one file and the
# generator caches state per process, so this is also the isolation boundary.
$results = $idlFiles | ForEach-Object -ThrottleLimit $Throttle -Parallel {
    $file = $_
    $args = @(
        '-I', $using:scriptsSlash,
        (Join-Path $using:scriptsSlash 'generate-bindings.pl'),
        '--include', $using:idlRootSlash,
        '--outputdir', $using:outSlash,
        '--generator', $using:Generator,
        '--defines', $using:Defines,
        $file.FullName.Replace('\', '/')
    )
    $text = & $using:perl @args 2>&1
    $code = $LASTEXITCODE
    if ($using:Verbose) {
        Write-Host ("[{0}] {1}" -f $(if ($code -eq 0) { 'ok' } else { 'FAIL' }),
            $file.FullName.Substring($using:root.Length + 1))
    }
    [pscustomobject]@{
        Idl    = $file.FullName.Substring($using:root.Length + 1)
        Ok     = ($code -eq 0)
        Output = ($text -join "`n")
    }
}

$sw.Stop()
$failed = @($results | Where-Object { -not $_.Ok })

# ---- compare against what is signed in ------------------------------------
$same = 0
$diff = New-Object System.Collections.ArrayList
$extra = New-Object System.Collections.ArrayList
$skipped = New-Object System.Collections.ArrayList
$generated = @(Get-ChildItem -Path $outFull -File)

if ($cmpFull -and (Test-Path $cmpFull)) {
    foreach ($f in $generated) {
        if ($skipNames -contains $f.Name) {
            [void]$skipped.Add($f.Name)
            continue
        }
        $ref = Join-Path $cmpFull $f.Name
        if (-not (Test-Path $ref)) {
            [void]$extra.Add($f.Name)
            continue
        }
        if ((Get-FileHash -Algorithm SHA256 $f.FullName).Hash -eq
            (Get-FileHash -Algorithm SHA256 $ref).Hash) {
            $same++
        } else {
            [void]$diff.Add($f.Name)
        }
    }
}

Write-Host ''
Write-Host ("generated : {0} files from {1} idl in {2:N1} s" -f $generated.Count, $results.Count, $sw.Elapsed.TotalSeconds)

if ($failed.Count) {
    Write-Host ("FAILED    : {0} idl file(s)" -f $failed.Count) -ForegroundColor Red
    foreach ($f in $failed) {
        Write-Host ("  {0}" -f $f.Idl) -ForegroundColor Red
        foreach ($line in (($f.Output -split "`n") | Select-Object -First 3)) {
            if ($line.Trim()) { Write-Host ("    {0}" -f $line.Trim()) -ForegroundColor DarkGray }
        }
    }
}

if ($cmpFull) {
    if (-not (Test-Path $cmpFull)) {
        Write-Host "compare   : $cmpFull does not exist (nothing to compare against)"
    } else {
        Write-Host ("identical : {0}" -f $same)
        Write-Host ("different : {0}" -f $diff.Count) -ForegroundColor $(if ($diff.Count -and $Strict) { 'Red' } elseif ($diff.Count) { 'Yellow' } else { 'Green' })
        foreach ($d in $diff) { Write-Host "  != $d" -ForegroundColor Yellow }
        if ($skipped.Count) {
            Write-Host ("skipped   : {0} (known-historical)" -f $skipped.Count) -ForegroundColor DarkGray
        }
        if ($extra.Count) {
            # Interfaces the shipped build never listed (the signed-in tree has
            # 255 of the 261 IDLs; the rest are per-backend or ObjC-only).
            Write-Host ("only here : {0} (not in {1})" -f $extra.Count, (Split-Path -Leaf $cmpFull)) -ForegroundColor DarkGray
            foreach ($e in $extra) { Write-Host "  +  $e" -ForegroundColor DarkGray }
        }
    }
}

if ($failed.Count) { exit 1 }
if ($Strict -and $diff.Count) { exit 1 }
exit 0


