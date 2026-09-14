@echo off
rem Sets VS to the Visual Studio installation that carries the x64 C++ toolset.
rem Called with "call find-vs.cmd" from the other scripts in this directory.
rem
rem Resolved at top level on purpose: %ProgramFiles(x86)% contains a closing parenthesis,
rem which ends an enclosing if/for block early unless delayed expansion is used. Keeping
rem this out of any block avoids that whole class of breakage.

if defined VSINSTALLDIR goto :from_env

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :not_found

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"

if not defined VS goto :not_found
exit /b 0

:from_env
rem VSINSTALLDIR from vcvars carries a trailing backslash; the callers join paths themselves.
set "VS=%VSINSTALLDIR:~0,-1%"
exit /b 0

:not_found
echo ERROR: no Visual Studio installation with the x64 C++ toolset was found.
echo Install the Visual Studio 2022 Build Tools with the "Desktop development with C++"
echo workload and the "C++ CMake tools for Windows" component.
exit /b 1
