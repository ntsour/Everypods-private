@echo off
REM Build the standalone bt_wake_test.exe (NOT part of the Propods CMake build).
REM Uses the VS 2022 Build Tools cl.exe + cppwinrt headers from the Windows SDK.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%"
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /await bt_wake_test.cpp /Fe:bt_wake_test.exe ^
   /link windowsapp.lib Ole32.lib
endlocal
