@echo off
REM MSVC build of the gap-short engine on the VPS. Auto-locates vcvars64.
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\Omega\third_party\twsapi\client
del *.obj 2>/dev/null
cl /nologo /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /c *.cpp
lib /nologo *.obj /OUT:twsclient.lib
cd /d C:\Omega\ibkr
cl /nologo /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /I..\third_party\twsapi\client GapShortEngine.cpp bid64_stub.cpp ..\third_party\twsapi\client\twsclient.lib ws2_32.lib /Fe:gapshort_engine.exe
echo BUILD_RESULT=%errorlevel%
