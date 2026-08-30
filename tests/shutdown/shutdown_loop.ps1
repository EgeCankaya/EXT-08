# EXT-08 - the twenty-cycle interrupt-and-verify loop (BTB-SD-1).
#
# "Twenty scripted interrupt-and-verify cycles produce twenty valid captures, each ending in a
# well-formed trailer, exit code 0 each time."
#
# A capture whose last line is not a trailer is unparseable by a strict reader, and a lost tail
# silently discards the most recent - often the most interesting - part of a run. Both failures
# are invisible without a check like this one, which is why it is scripted rather than done by
# hand once.
#
# The interrupt is a real console Ctrl-C, not a kill: we attach to the bridge's own console and
# raise CTRL_C_EVENT there, so the path exercised is exactly the operator's. Attaching means
# briefly detaching from our own console, which is why the parent console is re-attached
# immediately afterwards.
#
#   powershell -ExecutionPolicy Bypass -File tests\shutdown\shutdown_loop.ps1 `
#       -Cycles 20 -OutDir C:\some\scratch\dir -SimWorkDir C:\some\scratch\simrun
#
# Each cycle runs its own simulator, bridge first. That is slower than sharing one host across
# all twenty, and it is the only way the test means anything: the entity_created burst fires
# once at scenario load, so a bridge started after the host records nothing but orphans and the
# capture whose tail we are checking would have no tail. `n8ro-sim-local` is launched from
# -SimWorkDir because it drops a test_artifacts\ tree into whatever directory it starts in.

param(
    [int]    $Cycles     = 20,
    [string] $OutDir     = "$env:TEMP\ext08-shutdown",
    [string] $Bridge     = "build\x64\Release\n8ro-bridge.exe",
    [string] $Config     = "SimEngineClient_SharedMemory",
    [string] $ModelPath  = "C:\N8RO\data\db",
    [string] $SchemaFile = "N8roSimSchema",
    [int]    $RunSeconds = 9,
    [string] $SimWorkDir = "$env:TEMP\ext08-shutdown-sim",
    [string] $Scenario   = "Atacama Air Defense",
    [int]    $SimRunMs   = 60000
)

$ErrorActionPreference = "Stop"

Add-Type -Namespace Ext08 -Name Console -MemberDefinition @'
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool AttachConsole(uint dwProcessId);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool FreeConsole();
    [DllImport("kernel32.dll")]
    public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll")]
    public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);
'@

$ATTACH_PARENT = [uint32]"0xFFFFFFFF"
$CTRL_C_EVENT  = [uint32]0

function Send-CtrlC([int] $ProcessId) {
    # Detach from our console, attach to the target's, and raise Ctrl-C there. Our own handler
    # is disabled across the window so the event does not take this script down with it.
    [void][Ext08.Console]::FreeConsole()
    if (-not [Ext08.Console]::AttachConsole([uint32]$ProcessId)) {
        [void][Ext08.Console]::AttachConsole($ATTACH_PARENT)
        return $false
    }
    [void][Ext08.Console]::SetConsoleCtrlHandler([IntPtr]::Zero, $true)
    $sent = [Ext08.Console]::GenerateConsoleCtrlEvent($CTRL_C_EVENT, 0)
    Start-Sleep -Milliseconds 150
    [void][Ext08.Console]::FreeConsole()
    [void][Ext08.Console]::AttachConsole($ATTACH_PARENT)
    [void][Ext08.Console]::SetConsoleCtrlHandler([IntPtr]::Zero, $false)
    return $sent
}

function Test-Capture([string] $Path) {
    # The check the requirement actually makes: the file's LAST line is a well-formed trailer
    # saying the run was stopped by the operator. Reading only the tail keeps this cheap on a
    # capture of any size.
    if (-not (Test-Path $Path)) { return "no capture file" }
    $last = Get-Content -LiteralPath $Path -Tail 1
    if (-not $last) { return "capture is empty" }
    try { $record = $last | ConvertFrom-Json } catch { return "last line is not JSON" }
    if ($record.type -ne "trailer") { return "last line is a '$($record.type)', not a trailer" }
    if ($record.end_reason -ne "shutdown") { return "end_reason is '$($record.end_reason)'" }
    if ($null -eq $record.counts) { return "trailer has no counts" }
    return $null
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $SimWorkDir | Out-Null
Get-ChildItem -Path $OutDir -Filter "capture-*.n8rocap.jsonl" -ErrorAction SilentlyContinue |
    Remove-Item -Force

Write-Host "EXT-08 shutdown loop - $Cycles cycles, $RunSeconds s each, into $OutDir"
Write-Host ""

$failures = 0
for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    $label = "sd{0:d3}" -f $cycle
    $log   = Join-Path $OutDir "bridge-$label.log"

    # Bridge first, always. See the header comment.
    $process = Start-Process -FilePath $Bridge -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -ArgumentList @("--config", $Config,
                        "--model-path", ('"' + $ModelPath + '"'),
                        "--schema-file", $SchemaFile,
                        "--out-dir", ('"' + $OutDir + '"'),
                        "--run-label", $label)

    # Touch the handle before waiting. Start-Process -PassThru hands back a Process object
    # whose ExitCode is null unless the handle has been cached first - a well-known PowerShell
    # trap, and one that would otherwise make every cycle look like a failure.
    $null = $process.Handle

    # Quotes are embedded by hand: Start-Process -ArgumentList joins its array with spaces and
    # quotes nothing, so "Atacama Air Defense" arrives as three arguments and the host loads a
    # scenario called "Atacama" - which fails, and leaves a capture named capture-unknown-*.
    $sim = Start-Process -FilePath "n8ro-sim-local.exe" -PassThru -WindowStyle Hidden `
        -WorkingDirectory $SimWorkDir `
        -RedirectStandardOutput (Join-Path $OutDir "sim-$label.log") `
        -RedirectStandardError  (Join-Path $OutDir "sim-$label.err") `
        -ArgumentList @("--scenario", ('"' + $Scenario + '"'),
                        "--model-path", ('"' + $ModelPath + '"'),
                        "--run-ms", $SimRunMs)
    $null = $sim.Handle

    # Long enough for the scenario to load and the stream to start; the interrupt then lands
    # mid-run, which is the case the requirement is about. Scenario load alone takes about
    # 4.5 s on the reference machine, so anything under ~6 s interrupts a bridge that has not
    # recorded a thing yet - which passes the trailer check and tests nothing.
    Start-Sleep -Seconds $RunSeconds

    if (-not (Send-CtrlC $process.Id)) {
        Write-Host ("  cycle {0,2}  FAIL  could not raise Ctrl-C in the bridge's console" -f $cycle)
        $failures++
        try { $process.Kill() } catch { }
        try { $sim.Kill() } catch { }
        continue
    }

    if (-not $process.WaitForExit(20000)) {
        Write-Host ("  cycle {0,2}  FAIL  did not exit within 20 s of the interrupt" -f $cycle)
        $failures++
        try { $process.Kill() } catch { }
        try { $sim.Kill() } catch { }
        continue
    }
    $process.WaitForExit()      # settles the exit code as well as the wait
    try { $sim.Kill(); [void]$sim.WaitForExit(5000) } catch { }

    $exit = $process.ExitCode
    $capture = Get-ChildItem -Path $OutDir -Filter "capture-*-$label.n8rocap.jsonl" |
               Select-Object -First 1
    $problem = if ($capture) { Test-Capture $capture.FullName } else { "no capture file" }

    $samples = 0
    if ($capture -and -not $problem) {
        $trailer = Get-Content -LiteralPath $capture.FullName -Tail 1 | ConvertFrom-Json
        $samples = [int]$trailer.counts.samples
        # Every record enqueued before the signal must be in the file, so the trailer's own
        # count has to agree with the records present. Counting them is the check.
        $actual = (Select-String -LiteralPath $capture.FullName -Pattern '"type":"sample"' `
                   -SimpleMatch -AllMatches | Measure-Object).Count
        if ($actual -ne $samples) {
            $problem = "trailer says $samples samples, file holds $actual"
        }
    }

    if (-not $problem -and $samples -le 0) {
        # The requirement is about not losing a tail. A capture with no records has no tail to
        # lose, so an empty one is a failed cycle rather than a passed one - almost certainly a
        # bridge that attached after the scenario had already loaded.
        $problem = "capture has no samples, so it proves nothing about the tail"
    }

    if ($exit -ne 0 -or $problem) {
        $why = if ($problem) { $problem } else { "exit code $exit" }
        Write-Host ("  cycle {0,2}  FAIL  {1}" -f $cycle, $why)
        $failures++
    } else {
        Write-Host ("  cycle {0,2}  ok    exit 0, trailer end_reason=shutdown, {1} samples" -f `
                    $cycle, $samples)
    }
}

Write-Host ""
if ($failures -eq 0) {
    Write-Host "$Cycles of $Cycles cycles clean: exit 0, well-formed trailer, end_reason=shutdown"
    exit 0
}
Write-Host "$failures of $Cycles cycles FAILED"
exit 1
