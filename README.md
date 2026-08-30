# EXT-08 — Bus Telemetry Bridge

A standalone C++17 console program that attaches to a running N8RO simulation over the
message bus. The contract is [`docs/prd.md`](docs/prd.md); observations from the bus are in
[`notes.md`](notes.md).

**Status: M1 + M2.** The bridge creates a `SimulationEngineClient`, starts the message pump,
and prints the engine state, frame number, simulation time and scenario name once a second
from the client's local getters. It does not subscribe, capture, or judge anything yet —
that is M3 onward.

## Requirements

- Windows 10/11, x64
- N8RO runtime **2.1.328** installed at `C:\N8RO` (SDK component `com.n8ro.dev` 2.1.328)
- Visual Studio 2026 (v18.x) with the C++ desktop workload — toolset **v145**

Nothing else. The program links four import libraries from the release tree and no
third-party dependency.

## Build

From a plain `cmd` prompt:

```cmd
call C:\N8RO\setup.cmd
call C:\N8RO\dev\setup-dev.cmd
cd /d C:\Projects\EXT-08
msbuild n8ro-bridge.sln /p:Configuration=Release /p:Platform=x64
```

Output: `build\x64\Release\n8ro-bridge.exe`.

`setup.cmd` exports `N8RO_RELEASE` and puts `C:\N8RO\bin` on `PATH`; the project reads
`N8RO_RELEASE` for its include and library paths and falls back to `C:\N8RO`. If the release
lives elsewhere, pass it explicitly:

```cmd
msbuild n8ro-bridge.sln /p:Configuration=Release /p:Platform=x64 /p:N8roRelease=D:\N8RO
```

A missing or mistyped release path fails with one named error rather than a wall of missing
includes.

`C:\N8RO\bin` must be on `PATH` at **run** time too, for `n8ro-sim.dll` and friends —
`setup.cmd` does that.

## Run

The bridge is a passive client. Something has to be hosting an engine for it to attach to;
the shipped `n8ro-sim-local.exe` will host one and run a scenario:

```cmd
n8ro-sim-local.exe --scenario "Atacama Air Defense" --model-path C:\N8RO\data\db --run-ms 300000
```

Then, in a second prompt that has also run `setup.cmd`:

```cmd
build\x64\Release\n8ro-bridge.exe ^
    --config      SimEngineClient_SharedMemory ^
    --model-path  C:\N8RO\data\db ^
    --schema-file N8roSimSchema
```

```
[INFO] (n8ro-bridge) creating client: config=SimEngineClient_SharedMemory modelPath=C:\N8RO\data\db schemaFile=N8roSimSchema
[INFO] (n8ro-bridge) message pump started; reporting engine state once a second from the local getters (Ctrl-C to stop)
engine=running      frame=249        simTime=    12.450 scenario=Atacama Air Defense
engine=running      frame=269        simTime=    13.450 scenario=Atacama Air Defense
```

Every printed value is a local read on the client. Nothing on the print path touches the
bus.

### Options

| option | meaning |
|---|---|
| `--config` | client-side engine config entry, e.g. `SimEngineClient_SharedMemory`. Must be a `SimEngineClient_*` entry — a `SimEngineHost_*` one names the wrong side and will not connect |
| `--model-path` | directory holding the schema and instance database, e.g. `C:\N8RO\data\db` |
| `--schema-file` | schema name inside that database, e.g. `N8roSimSchema` |

All three are required; none are compiled in.

### Exit codes

| code | meaning |
|---:|---|
| 0 | clean exit (`--help`) |
| 2 | bad invocation — unknown option, missing value, missing required option |
| 3 | `SimulationEngineClient::create()` returned `nullopt`; all three configuration values are echoed back as resolved |
| 4 | an exception reached `main` — it is caught and logged, never propagated |

Configuration is where the difficulty lives. A wrong `--schema-file`, for example, names
the file it could not open and then names all three values back:

```
[ERROR] (BinaryDbFileIO) cannot open file: C:/N8RO/data/db/NoSuchSchema/Config/SimEngine/SimEngineClient_SharedMemory.n8ro.instance
[ERROR] (n8ro-bridge) SimulationEngineClient::create returned nullopt; no client was constructed and nothing was subscribed
[ERROR] (n8ro-bridge)   --config      = SimEngineClient_SharedMemory
[ERROR] (n8ro-bridge)   --model-path  = C:\N8RO\data\db
[ERROR] (n8ro-bridge)   --schema-file = NoSuchSchema
```

## Determinism probe

`tests/float-format/float_format_probe.cpp` settles which double-to-text format is
round-trip exact **and** locale independent on this toolchain. It is standalone:

```cmd
cl /std:c++17 /O2 /EHsc tests\float-format\float_format_probe.cpp
float_format_probe.exe
```

Exit code 0 if at least one candidate passes both axes. The result and what it means for
the capture format are in [`notes.md`](notes.md).

## Layout

```
docs/prd.md                              the contract — 27 FRs prefixed BTB-
notes.md                                 what the bus actually carries (graded deliverable)
src/main.cpp                             the bridge
tests/float-format/                      the OQ-5 determinism probe
n8ro-bridge.sln / .vcxproj               Release|x64, v145, stdcpp17
```

## Notes on handling captures

Anything this program eventually records inherits the classification of the scenario it
records — entity names, positions, teams and outcomes originate from an Arkheon
Technologies proprietary platform. Treat captures accordingly.

This install is missing `C:\N8RO\data\geoid\earth_geoid_05m_g.n8grid`, so runs log a
terrain-datum warning and fall back to the ellipsoid. Altitudes observed on the bus are
ellipsoidal, not orthometric.
