@echo off
rem Configure and build PCSXROO with the Visual Studio 2022 Build Tools toolchain.
rem
rem Usage:
rem   build.cmd                 build everything
rem   build.cmd core_test       build one or more named targets
rem
rem The repository root is derived from this script's location, so the file can be moved
rem or the repository cloned anywhere without editing paths.

setlocal enabledelayedexpansion

for %%I in ("%~dp0..\..") do set "REPO=%%~fI"

call "%~dp0find-vs.cmd" || exit /b 1

set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%CMAKE%" (
  echo ERROR: cmake.exe not found at "%CMAKE%".
  echo Install the "C++ CMake tools for Windows" component.
  exit /b 1
)

if not exist "%REPO%\deps\lib\cmake\Qt6\Qt6Config.cmake" (
  echo ERROR: dependencies are missing from "%REPO%\deps".
  echo Download pcsx2-windows-dependencies.7z from
  echo   https://github.com/PCSX2/pcsx2-windows-dependencies/releases/latest
  echo and extract it so that "%REPO%\deps\lib" exists.
  exit /b 1
)

if not defined VSINSTALLDIR call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

rem The "unittests" target runs ctest as a post-build step, and core_test links the
rem emulator core, which needs runtime DLLs that CMake does not copy next to the test
rem binary. Without these the test process dies at load time with a bare 0xc0000135.
set "PATH=%REPO%\deps\bin;%REPO%\3rdparty\winpixeventruntime\bin;%PATH%"

cd /d "%REPO%" || exit /b 1

rem Read below with !VAR! rather than %VAR%: a parenthesised block is parsed in one go, so
rem %VAR% inside it expands before the block runs, which silently passes an empty
rem -DCMAKE_BUILD_TYPE= and leaves CMake on its own default.
if not defined PCSXROO_BUILD_TYPE set "PCSXROO_BUILD_TYPE=Release"

if not exist "%REPO%\build\CMakeCache.txt" (
  echo === Configuring ===
  rem Release, not Devel. A Devel build asserts its way to a __fastfail (0xC0000409)
  rem during VM boot, right after the game database loads and before the GS opens, so it
  rem can never actually run a game - which makes it useless for debugging one. Set
  rem PCSXROO_BUILD_TYPE=Devel to opt back in for core work that does not need to boot.
  "%CMAKE%" -B build -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=!PCSXROO_BUILD_TYPE! ^
    -DQT_BUILD=ON ^
    -DCMAKE_PREFIX_PATH="%REPO%\deps" ^
    -DDISABLE_ADVANCE_SIMD=ON || exit /b 1
)

if "%~1"=="" (
  echo === Building all targets ===
  "%CMAKE%" --build build --parallel || exit /b 1

  rem The install step is the deployment step: it assembles bin\ with the Qt and third
  rem party DLLs beside the executable plus the resources it loads at runtime. Running
  rem build\pcsx2-qt\pcsx2-qt.exe directly only works if deps\bin happens to be on PATH,
  rem and fails with a bare 0xC0000135 when it is not.
  echo === Installing to bin ===
  "%CMAKE%" --install build || exit /b 1

  rem cmake --install does not copy WinPixEventRuntime.dll. Release builds do not reference
  rem it, so upstream CI never notices, but a Devel build links against it and dies at
  rem startup with a "code execution cannot proceed" message box. That box is invisible to a
  rem non-interactive session, where it looks like the emulator silently hanging instead.
  if exist "%REPO%\3rdparty\winpixeventruntime\bin\WinPixEventRuntime.dll" (
    copy /y "%REPO%\3rdparty\winpixeventruntime\bin\WinPixEventRuntime.dll" "%REPO%\bin\" >nul
  )
) else (
  echo === Building %* ===
  "%CMAKE%" --build build --parallel --target %* || exit /b 1
)

echo === Done ===
endlocal
