@echo off
rem EXT-08 - build and run every suite, in one command.
rem
rem Until this file existed the README asked a reader to paste six compile lines out of prose,
rem which is the same defect the golden --help files exist to prevent: a command nobody executes
rem is a command that rots. The build lines below are the ones the suites' own header comments
rem carry, and they are now executed rather than quoted.
rem
rem TWO TIERS, AND THE SPLIT IS DELIBERATE (docs/clean-room.md section 4):
rem
rem   headers tier    entity_picture_test, referee_test, determinism_test - 213 checks. They link
rem                   NO import library and start no simulator, but they do include six SDK
rem                   headers, so they need C:\N8RO present. They cannot run on a clean runner.
rem   zero-install    capture_reader and its mutation harness, plus the schema digest - 23
rem                   mutations and the conformance pass. These link nothing and need no install
rem                   at all, which is why CI runs exactly this half on a machine that has never
rem                   had C:\N8RO on it (.github/workflows/zero-install-tier.yml).
rem
rem Without an install the headers tier is SKIPPED with a named message and the zero-install tier
rem still runs, so this script is useful in both places. It fails on the first failure.

setlocal enabledelayedexpansion
set ROOT=%~dp0..
set OUT=%ROOT%\build\tests
set CHECKS=0
set RAN=0
set SKIPPED=

rem --- the toolchain, discovered rather than hard-coded --------------------------------------
rem C:\N8RO\dev\setup-dev.cmd would do this, and requires the install by construction - so it is
rem unavailable in exactly the case the zero-install tier exists to cover. vswhere ships with
rem every Visual Studio installer since 2017. Anything 17.x or 18.x with the x64 tools compiles
rem everything below; n8ro-bridge.sln itself pins toolset v145 and needs 18.x, which is a
rem separate constraint and is not exercised here.
where cl >nul 2>&1
if not errorlevel 1 goto :have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo FAIL: vswhere.exe not found. Every Visual Studio installer since 2017 ships it.
  exit /b 1
)
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -version [17.0^,18.99] -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if "%VSDIR%"=="" (
  echo FAIL: no Visual Studio with the C++ x64 toolset was found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:have_cl

if not exist "%OUT%" mkdir "%OUT%"

rem --- which tier can run here ---------------------------------------------------------------
if "%N8RO_RELEASE%"=="" set "N8RO_RELEASE=C:\N8RO"
set SDK=
if exist "%N8RO_RELEASE%\include\n8ro-sim" set SDK=1
if not defined SDK (
  echo No N8RO install at "%N8RO_RELEASE%" - the headers tier will be skipped.
  echo Set N8RO_RELEASE if it lives elsewhere. The zero-install tier below still runs.
  echo.
  set SKIPPED=the headers tier ^(213 checks^)
  goto :zero_install
)

rem --- headers tier --------------------------------------------------------------------------
rem The roster and ADR-6: an entity's identity is (name, occupancy) and never name, which is the
rem invariant every downstream statistic rests on.
echo === entity_picture_test - the roster and ADR-6
cl /nologo /std:c++17 /EHsc /W4 /O2 ^
   /I "%N8RO_RELEASE%\include\n8ro-core" /I "%N8RO_RELEASE%\include\n8ro-sim" ^
   /Fe:"%OUT%\entity_picture_test.exe" /Fo:"%OUT%\\" /Fd:"%OUT%\\" ^
   "%ROOT%\tests\entity-picture\entity_picture_test.cpp" "%ROOT%\src\EntityPicture.cpp"
if errorlevel 1 exit /b 1
call :run "%OUT%\entity_picture_test.exe" entity_picture_test
if errorlevel 1 exit /b 1

echo.
echo === referee_test - the three condition kinds, and the loader's rejections
cl /nologo /std:c++17 /EHsc /W4 /O2 ^
   /I "%N8RO_RELEASE%\include\n8ro-core" /I "%N8RO_RELEASE%\include\n8ro-sim" ^
   /Fe:"%OUT%\referee_test.exe" /Fo:"%OUT%\\" /Fd:"%OUT%\\" ^
   "%ROOT%\tests\referee\referee_test.cpp" "%ROOT%\src\Referee.cpp" "%ROOT%\src\Conditions.cpp" ^
   "%ROOT%\src\Geodesy.cpp" "%ROOT%\src\JsonParse.cpp" "%ROOT%\src\Json.cpp"
if errorlevel 1 exit /b 1
call :run "%OUT%\referee_test.exe" referee_test
if errorlevel 1 exit /b 1

rem R4's three hazards - unordered iteration, locale-sensitive float formatting, and any
rem unordered container on an output path - each tested by running it rather than by reading it.
echo.
echo === determinism_test - R4's hazards, the locale, and BP-4's structural reserve
cl /nologo /std:c++17 /EHsc /W4 /O2 ^
   /I "%N8RO_RELEASE%\include\n8ro-core" /I "%N8RO_RELEASE%\include\n8ro-sim" ^
   /Fe:"%OUT%\determinism_test.exe" /Fo:"%OUT%\\" /Fd:"%OUT%\\" ^
   "%ROOT%\tests\determinism\determinism_test.cpp" "%ROOT%\src\CaptureFormat.cpp" ^
   "%ROOT%\src\Json.cpp" "%ROOT%\src\Referee.cpp" "%ROOT%\src\Conditions.cpp" ^
   "%ROOT%\src\Geodesy.cpp" "%ROOT%\src\JsonParse.cpp" "%ROOT%\src\RecordQueue.cpp"
if errorlevel 1 exit /b 1
call :run "%OUT%\determinism_test.exe" determinism_test
if errorlevel 1 exit /b 1

:zero_install
rem --- zero-install tier ---------------------------------------------------------------------
rem BTB-CAP-5's claim is that docs/capture-format-v1.md is complete enough to write a reader
rem from. The proof is this reader, written from the specification and not from this program's
rem source: no /I, no /LIBPATH, no .lib. A compile line anyone can read in five seconds is a
rem better proof of that than an argument about translation units.
echo.
echo === capture_reader - the format spec, checked against a real file
cl /nologo /std:c++17 /EHsc /W4 /O2 ^
   /Fe:"%OUT%\capture_reader.exe" /Fo:"%OUT%\\" /Fd:"%OUT%\\" ^
   "%ROOT%\tests\capture-reader\capture_reader.cpp"
if errorlevel 1 exit /b 1

"%OUT%\capture_reader.exe" ^
    "%ROOT%\docs\sample-capture\capture-atacama-air-defense-sample.n8rocap.jsonl" ^
    --spec "%ROOT%\docs\capture-format-v1.md"
if errorlevel 1 (
  echo TESTS FAILED: the shipped sample does not conform to n8ro-capture/1.
  exit /b 1
)
set /a RAN+=1

rem A reader that has never rejected anything has not been shown to work. Twenty-three
rem deliberate defects, each of which the reader must catch; a survivor fails the run.
where python >nul 2>&1
if errorlevel 1 (
  echo.
  echo SKIPPED: the mutation harness and the schema digest need python on PATH.
  set SKIPPED=!SKIPPED! + the python checks
  goto :summary
)

echo.
echo === mutation harness - 23 deliberate defects, 0 survivors permitted
python "%ROOT%\tests\capture-reader\mutate.py" ^
    "%ROOT%\docs\sample-capture\capture-atacama-air-defense-sample.n8rocap.jsonl" ^
    "%OUT%\capture_reader.exe" "%ROOT%\docs\capture-format-v1.md"
if errorlevel 1 (
  echo TESTS FAILED: a deliberate defect survived the reader.
  exit /b 1
)
set /a RAN+=1

rem docs/condition-file-schema.md is vendored BY IDENTITY into EXT-17's contract/ (its E-5).
rem This is the check that stops it drifting from the README section it was cut from.
echo.
echo === the vendorable condition digest still matches the README
python "%ROOT%\tests\referee\check_schema_digest.py"
if errorlevel 1 (
  echo TESTS FAILED: the condition schema digest drifted from the README.
  exit /b 1
)
set /a RAN+=1

:summary
echo.
echo tests: !CHECKS! check(s) across !RAN! suite(s)/harness(es), 0 failure(s).
if not "!SKIPPED!"=="" echo        skipped: !SKIPPED!.
echo tests: all passed.
exit /b 0

rem --- run one suite and add its printed check count to the total -----------------------------
rem The count is SUMMED from what the suite actually printed rather than typed here, for the
rem same reason the --help files are golden: a number in prose is a number that rots.
:run
"%~1" > "%OUT%\%~2.out" 2>&1
set TESTRC=%errorlevel%
type "%OUT%\%~2.out"
if not "%TESTRC%"=="0" (
  echo TESTS FAILED: %~2
  exit /b 1
)
for /f "tokens=1" %%N in ('findstr /r /c:"^[0-9][0-9]* checks," "%OUT%\%~2.out"') do set /a CHECKS+=%%N
set /a RAN+=1
exit /b 0
