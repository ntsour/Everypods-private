@echo off
REM Build the standalone ks_connect_spike.exe (also available as CMake target ks_connect_spike).
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%"
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   ks_connect_spike.cpp /Fe:ks_connect_spike.exe /link Ole32.lib windowsapp.lib
endlocal
