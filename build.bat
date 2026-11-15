@echo off
rem ---------------------------------------------------------------------------
rem  ftviz one-click Windows build: produces a standalone, statically linked
rem  dist\ftviz.exe (MSVC /MT, static OpenSSL, all shaders and data embedded).
rem
rem  Needs: Visual Studio 2022 (or Build Tools) with the C++ workload, which
rem  also provides CMake; Git; Python 3. vcpkg is used from %VCPKG_ROOT% or
rem  cloned into .\vcpkg on first run.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"

rem --- 1. Visual Studio developer environment --------------------------------
rem (No parenthesised blocks here: "%ProgramFiles(x86)%" contains a ")".)
where cl >nul 2>nul
if not errorlevel 1 goto :havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto :novs
:havecl

where cmake >nul 2>nul || (echo [build] CMake not found. Install the "C++ CMake tools for Windows" VS component. & goto :fail)
where git   >nul 2>nul || (echo [build] Git not found. Install Git for Windows. & goto :fail)

rem --- 2. Python + jinja2 (glad generates its GL loader at build time) --------
set "PY=python"
where python >nul 2>nul || set "PY=py -3"
%PY% -c "import jinja2" >nul 2>nul
if errorlevel 1 (
  echo [build] installing jinja2 for the glad loader generator...
  %PY% -m pip install --user jinja2 || (echo [build] Python 3 with pip is required. & goto :fail)
)

rem --- 3. Static OpenSSL via vcpkg --------------------------------------------
if not defined VCPKG_ROOT set "VCPKG_ROOT=%CD%\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  if not exist "%VCPKG_ROOT%\bootstrap-vcpkg.bat" (
    echo [build] cloning vcpkg into %VCPKG_ROOT% ...
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%" || goto :fail
  )
  call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics || goto :fail
)
echo [build] vcpkg install openssl:x64-windows-static (first run takes a few minutes)
"%VCPKG_ROOT%\vcpkg.exe" install openssl:x64-windows-static || goto :fail

rem --- 4. Configure, build, test ----------------------------------------------
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 ^
  -DFTVIZ_STATIC=ON -DFTVIZ_BUILD_TOOLS=OFF ^
  -DOPENSSL_ROOT_DIR="%VCPKG_ROOT%\installed\x64-windows-static" || goto :fail
cmake --build build-win --config Release --parallel || goto :fail
ctest --test-dir build-win -C Release --output-on-failure || echo [build] WARNING: some tests failed, see above.

if not exist dist mkdir dist
copy /y build-win\Release\ftviz.exe dist\ftviz.exe >nul || goto :fail
echo.
echo [build] done: %CD%\dist\ftviz.exe  (double-click it, or run "ftviz.exe --help")
endlocal
exit /b 0

:novs
echo [build] Visual Studio 2022 with the "Desktop development with C++" workload was not found.
:fail
echo [build] FAILED
endlocal
exit /b 1
