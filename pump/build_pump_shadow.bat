@echo off
REM Build the standalone C++ pump shadow runner with MSVC (header-only deps, no twsapi).
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\Omega\pump
cl /nologo /std:c++17 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS /I..\include pump_shadow_main.cpp /Fe:pump_shadow.exe
echo BUILD_RESULT=%errorlevel%
