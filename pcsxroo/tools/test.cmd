@echo off
rem Run the PCSXROO ctest suites.
rem
rem Usage:
rem   test.cmd                       run every suite
rem   test.cmd -R core_test          pass any extra arguments through to ctest
rem
rem core_test links the emulator core, which pulls runtime DLLs from deps\bin and
rem 3rdparty\winpixeventruntime\bin. CMake copies only SDL3.dll next to the test binary, so
rem without these on PATH the process dies at load time with 0xc0000135 (DLL not found).
rem ctest reports that as "Exit code 0xc0000135 ***Exception" with no further explanation,
rem which looks like a broken test rather than a missing search path.

setlocal

for %%I in ("%~dp0..\..") do set "REPO=%%~fI"

call "%~dp0find-vs.cmd" || exit /b 1

set "CTEST=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

if not exist "%REPO%\build\tests\ctest" (
  echo ERROR: no test build found. Run pcsxroo\tools\build.cmd unittests first.
  exit /b 1
)

set "PATH=%REPO%\deps\bin;%REPO%\3rdparty\winpixeventruntime\bin;%PATH%"

cd /d "%REPO%\build\tests\ctest" || exit /b 1
"%CTEST%" --output-on-failure %*
endlocal
