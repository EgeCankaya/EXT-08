# EXT-08 - the R1 teardown spike (M7).
#
# R1, from the PRD's risk register:
#
#   "A 0xC0000005 host-side teardown access violation has been observed on this platform, with
#    a userPlugins/sim plugin loaded. These projects load no plugins, so it may not apply."
#
# It is carried as a risk because EXT-17 requires twenty or more unattended runs with clean
# teardown, and a host that access-violates on the way out would make that impossible. The
# mitigation the PRD chose was to spike it before EXT-17's acceptance criteria are locked:
# twenty consecutive plugin-free load-run-teardown cycles, exit codes recorded, and whatever
# the result, it goes in the notes and to the mentor.
#
# What this measures is the **host's** exit code, with the bridge attached to it - so it also
# answers the second-order question, which is whether a bus client observing the run changes
# the teardown. A cycle is clean only if both processes exit 0.
#
#   powershell -ExecutionPolicy Bypass -File tests\teardown-spike\teardown_spike.ps1 -Cycles 20
#
# 0xC0000005 arrives as exit code -1073741819. That is the value to look for.

param(
    [int]    $Cycles     = 20,
    [string] $WorkDir    = "$env:TEMP\ext08-teardown",
    [string] $OutDir     = "$env:TEMP\ext08-teardown\captures",
    [string] $Bridge     = "build\x64\Release\n8ro-bridge.exe",
    [string] $Config     = "SimEngineClient_SharedMemory",
    [string] $ModelPath  = "C:\N8RO\data\db",
    [string] $SchemaFile = "N8roSimSchema",
    [string] $Scenario   = "Atacama Air Defense",
    [int]    $SimRunMs   = 6000,
    [switch] $WithoutBridge
)

# Note: the per-cycle process handle below is $bridgeProc, not $bridge. PowerShell variable
# names are case-insensitive, so a local $bridge would be the *same variable* as the $Bridge
# parameter and would blank the executable path on the first iteration.

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir  | Out-Null

$mode = if ($WithoutBridge) { "host alone" } else { "host with the bridge attached" }
Write-Host "EXT-08 R1 teardown spike - $Cycles cycles, $mode"
Write-Host "  scenario  $Scenario, --run-ms $SimRunMs"
Write-Host "  0xC0000005 would appear as exit code -1073741819"
Write-Host ""

$hostCodes   = @{}
$bridgeCodes = @{}
$dirty = 0

for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    $label = "td{0:d3}" -f $cycle
    $bridgeProc = $null

    if (-not $WithoutBridge) {
        # Bridge first, so it witnesses the whole load-run-teardown cycle rather than
        # attaching into the middle of one.
        $bridgeProc = Start-Process -FilePath $Bridge -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $WorkDir "bridge-$label.log") `
            -RedirectStandardError  (Join-Path $WorkDir "bridge-$label.err") `
            -ArgumentList @("--config", $Config,
                            "--model-path", ('"' + $ModelPath + '"'),
                            "--schema-file", $SchemaFile,
                            "--out-dir", ('"' + $OutDir + '"'),
                            "--run-label", $label)
        $null = $bridgeProc.Handle
        Start-Sleep -Milliseconds 800
    }

    # Quotes embedded by hand - Start-Process joins its argument array with spaces and quotes
    # nothing, so a scenario name containing a space would arrive as several arguments.
    $simulator = Start-Process -FilePath "n8ro-sim-local.exe" -PassThru -WindowStyle Hidden `
        -WorkingDirectory $WorkDir `
        -RedirectStandardOutput (Join-Path $WorkDir "sim-$label.log") `
        -RedirectStandardError  (Join-Path $WorkDir "sim-$label.err") `
        -ArgumentList @("--scenario", ('"' + $Scenario + '"'),
                        "--model-path", ('"' + $ModelPath + '"'),
                        "--run-ms", $SimRunMs)
    $null = $simulator.Handle

    if (-not $simulator.WaitForExit(120000)) {
        Write-Host ("  cycle {0,2}  HUNG  the host did not exit within 120 s" -f $cycle)
        try { $simulator.Kill() } catch { }
        $dirty++
        if ($bridgeProc) { try { $bridgeProc.Kill() } catch { } }
        continue
    }
    $simulator.WaitForExit()
    $hostCode = $simulator.ExitCode
    $hostCodes[$hostCode] = 1 + $(if ($hostCodes.ContainsKey($hostCode)) { $hostCodes[$hostCode] } else { 0 })

    $bridgeCode = $null
    if ($bridgeProc) {
        # The bridge ends itself on host loss, within its documented 3 s window.
        if (-not $bridgeProc.WaitForExit(30000)) {
            Write-Host ("  cycle {0,2}  HUNG  the bridge did not notice host loss within 30 s" -f $cycle)
            try { $bridgeProc.Kill() } catch { }
            $dirty++
            continue
        }
        $bridgeProc.WaitForExit()
        $bridgeCode = $bridgeProc.ExitCode
        $bridgeCodes[$bridgeCode] = 1 + $(if ($bridgeCodes.ContainsKey($bridgeCode)) { $bridgeCodes[$bridgeCode] } else { 0 })
    }

    $clean = ($hostCode -eq 0) -and (($null -eq $bridgeCode) -or ($bridgeCode -eq 0))
    if (-not $clean) { $dirty++ }

    $note = if ($hostCode -eq -1073741819) { "  <- 0xC0000005 ACCESS VIOLATION" } else { "" }
    $suffix = if ($null -ne $bridgeCode) { ", bridge $bridgeCode" } else { "" }
    Write-Host ("  cycle {0,2}  {1}  host {2}{3}{4}" -f `
                $cycle, $(if ($clean) { "ok  " } else { "DIRTY" }), $hostCode, $suffix, $note)
}

Write-Host ""
Write-Host "host exit codes:"
$hostCodes.GetEnumerator() | Sort-Object Name | ForEach-Object {
    Write-Host ("  {0,12} x{1}" -f $_.Key, $_.Value)
}
if ($bridgeCodes.Count -gt 0) {
    Write-Host "bridge exit codes:"
    $bridgeCodes.GetEnumerator() | Sort-Object Name | ForEach-Object {
        Write-Host ("  {0,12} x{1}" -f $_.Key, $_.Value)
    }
}
Write-Host ""
if ($dirty -eq 0) {
    Write-Host "$Cycles of $Cycles cycles torn down cleanly. R1 did not reproduce in this configuration."
    exit 0
}
Write-Host "$dirty of $Cycles cycles did NOT tear down cleanly. R1 reproduces - take this to the mentor."
exit 1
