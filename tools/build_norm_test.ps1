# tools/build_norm_test.ps1
#
# Builds debug\norm_test.exe: the address-bar URL normalizer (normalizeUrl and
# every helper it needs) sliced out of examples\browser_shell\main.c and driven
# from a small main(). It exists because normalizeUrl is a static function in
# the GUI shell - the only other way to exercise it is to open a window, type
# into the address bar and click Go.
#
# The regression it was written for (R105): a local path went through
# fileUrlFromLocalPath() as "file://C:/..." (two slashes), normalizeUrl()
# classified that as an unnormalized drive path again, and every Go/Enter click
# percent-encoded the '%' one more time ("%20" -> "%2520" -> ...).
#
# Usage:
#   pwsh -File tools\build_norm_test.ps1
#   .\debug\norm_test.exe "C:\dir\a b.html" "file:///C:/dir/a%20b.html"
#
# Each argument is printed three times: as given, normalizeUrl(x), and
# normalizeUrl(normalizeUrl(x)). The last line gets "<-- GROWS" when the
# normalizer is not idempotent.

param(
    [string]$Compiler = 'C:\msys64\mingw64\bin\gcc.exe'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'examples\browser_shell\main.c'
$outC = Join-Path $root 'debug\norm_test.c'
$outExe = Join-Path $root 'debug\norm_test.exe'

if (-not (Test-Path $Compiler)) {
    throw "compiler not found: $Compiler (pass -Compiler)"
}
if (-not (Test-Path $source)) {
    throw "missing $source"
}

$lines = Get-Content $source

function Slice([string]$startPattern, [string]$endPattern, [switch]$LastEnd) {
    $start = ($lines | Select-String -Pattern $startPattern | Select-Object -First 1).LineNumber
    $endMatch = if ($LastEnd) {
        $lines | Select-String -Pattern $endPattern | Select-Object -Last 1
    } else {
        $lines | Select-String -Pattern $endPattern | Select-Object -First 1
    }
    $end = $endMatch.LineNumber

    if (-not $start -or -not $end -or $end -le $start) {
        throw ("bad slice: {0} (line {1}) .. {2} (line {3})" -f $startPattern, $start, $endPattern, $end)
    }

    ($lines[($start - 1)..($end - 2)] -join "`n")
}

$head = "#include <windows.h>`n#include <stdio.h>`n#include <string.h>`n#define MAX_PATH_CHARS 1024`n"
$utf8Helpers = Slice '^static void utf8ToWide' '^/\* Reads a control back as UTF-8'
$urlHelpers = Slice '^static int startsWithNoCase' '^static void createTab' -LastEnd

$driver = @'

int main(int argc, char** argv)
{
    char once[MAX_PATH_CHARS], twice[MAX_PATH_CHARS];
    int i;

    for (i = 1; i < argc; ++i) {
        normalizeUrl(argv[i], once, sizeof(once));
        normalizeUrl(once, twice, sizeof(twice));
        printf("IN   : %s\n", argv[i]);
        printf("N    : %s\n", once);
        printf("N(N) : %s%s\n\n", twice, strcmp(once, twice) ? "   <-- GROWS" : "");
    }
    return 0;
}
'@

($head + $utf8Helpers + "`n" + $urlHelpers + $driver) | Set-Content $outC -Encoding utf8

& $Compiler -O0 -o $outExe $outC | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "compile failed with $LASTEXITCODE"
}

Write-Host "built $outExe"
