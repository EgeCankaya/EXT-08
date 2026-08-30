# EXT-08 - BTB-CAP-3's harness: ten replays of one stored capture, hashed (M7).
#
# The requirement, as PRD rev 5 restated it:
#
#   "Ten consecutive runs REPLAYED FROM ONE STORED CAPTURE hash identically. Live-run pairs
#    are not a valid check of this requirement on a wall-clock-paced host, because they vary
#    for a reason outside the system."
#
# That distinction was measured, not assumed: two live runs of one scenario differ because
# `n8ro-sim-local` skips a different ~1% of frames each time, so a live pair measures the
# host's repeatability rather than the recorder's. Replaying one stored file removes the host
# from the experiment entirely, which is the only way the answer is about us.
#
# This is the end-to-end half of the determinism check. The other half - that the emission
# path itself introduces no variation, under a comma-decimal locale, with payload fields
# inserted in any order - is tests\determinism\determinism_test.cpp, and it is the one that
# would actually localise a regression.
#
#   powershell -ExecutionPolicy Bypass -File tests\determinism\replay_hashes.ps1 `
#       -Capture captures\capture-atacama-air-defense-000.n8rocap.jsonl `
#       -Conditions conditions\atacama.conditions.json

param(
    [string] $Capture    = "captures\capture-atacama-air-defense-000.n8rocap.jsonl",
    [string] $Conditions = "conditions\atacama.conditions.json",
    [string] $Bridge     = "build\x64\Release\n8ro-bridge.exe",
    [string] $OutDir     = "$env:TEMP\ext08-determinism",
    [int]    $Runs       = 10
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Capture))    { Write-Host "no such capture: $Capture";       exit 2 }
if (-not (Test-Path $Conditions)) { Write-Host "no such condition file: $Conditions"; exit 2 }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "EXT-08 determinism harness - $Runs replays of one stored capture"
Write-Host "  capture     $Capture"
Write-Host "  conditions  $Conditions"
Write-Host ""

$hashes = @()
for ($run = 1; $run -le $Runs; $run++) {
    Get-ChildItem -Path $OutDir -Filter "verdicts-*.jsonl" -ErrorAction SilentlyContinue |
        Remove-Item -Force

    & $Bridge --replay $Capture --conditions $Conditions --out-dir $OutDir | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("  run {0,2}  FAIL  replay exited {1}" -f $run, $LASTEXITCODE)
        exit 1
    }

    $verdicts = Get-ChildItem -Path $OutDir -Filter "verdicts-*.jsonl" | Select-Object -First 1
    if (-not $verdicts) {
        Write-Host ("  run {0,2}  FAIL  replay wrote no verdict file" -f $run)
        exit 1
    }

    $hash = (Get-FileHash -LiteralPath $verdicts.FullName -Algorithm SHA256).Hash
    $hashes += $hash
    Write-Host ("  run {0,2}  {1}  {2} bytes" -f $run, $hash.Substring(0, 16), $verdicts.Length)
}

Write-Host ""
# @() forces an array even when every hash is the same, so .Count and [0] mean what they look
# like. Without it a single unique string indexes to its first character.
$distinct = @($hashes | Select-Object -Unique)
if ($distinct.Count -eq 1) {
    Write-Host "$Runs of $Runs replays hash identically:"
    Write-Host "  $($distinct[0])"
    exit 0
}
Write-Host "FAILED - $($distinct.Count) distinct hashes across $Runs replays:"
$distinct | ForEach-Object { Write-Host "  $_" }
exit 1
