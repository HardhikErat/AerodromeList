@echo off
setlocal
rem Builds AerodromeList.dll (32-bit, as EuroScope is a 32-bit program).

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build
rem If EuroScope has the DLL loaded it is locked, but it can still be renamed.
del /q build\AerodromeList.dll.old 2>nul
if exist build\AerodromeList.dll move /y build\AerodromeList.dll build\AerodromeList.dll.old >nul 2>nul

cl /nologo /c /O2 /MT /EHsc /std:c++17 /W3 /GS /DNDEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Fo"build\\" src\AerodromeList.cpp src\AsrIndex.cpp src\AsrOpener.cpp src\RunwayDialog.cpp || exit /b 1

link /nologo /DLL /OUT:build\AerodromeList.dll build\AerodromeList.obj build\AsrIndex.obj build\AsrOpener.obj build\RunwayDialog.obj ^
   sdk\EuroScopePlugInDll.lib user32.lib gdi32.lib comctl32.lib || exit /b 1

echo.
echo Built build\AerodromeList.dll
